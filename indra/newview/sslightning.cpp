/**
 * @file sslightning.cpp
 * @brief Atmo Magic lightning: strike scheduling, channel generation and the
 *        discharge phases. See sslightning.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sslightning.h"

#include "ssatmomagic.h"
#include "sssoundscape.h"
#include "sswindflow.h"
#include "ssvolcloud.h"

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llsurface.h"
#include "llhudobject.h"
#include "llhudtext.h"

namespace
{
    // Discharge phases: faint stepped leader gropes down over ~50-180ms (the visible forking), then 1-4 return strokes fire up the channel ~30-90ms apart, each dimmer - the flicker. Full physics: doc/atmo_magic_lightning.md

    // How long before the strike its own arrival becomes visible. Zero in nature - a bolt is not announced - but this is a weather system for a world with dragons in it, and a charge gathering at
    // the point about to be hit is a good effect. Off by default; the setting is the dial.
    const F32 ANTICIPATION_MAX_S = 8.f;

    const F32 LEADER_MIN_S = 0.05f;
    const F32 LEADER_MAX_S = 0.18f;
    const F32 LEADER_GLOW = 0.12f;      // how bright the leader is, against 1

    const S32 RETURN_STROKES_MIN = 1;
    const S32 RETURN_STROKES_MAX = 4;
    const F32 RESTRIKE_MIN_S = 0.03f;
    const F32 RESTRIKE_MAX_S = 0.09f;
    const F32 STROKE_DECAY_S = 0.055f;  // e-folding time of one stroke

    // Past the refraction shadow zone thunder never arrives - distant storms are correctly silent (doc/atmo_magic_lightning.md#thunder-acoustics).
    const F32 THUNDER_SHADOW_ZONE_M = 20000.f;

    // Strike placement range around the camera, squared-biased far so overhead strikes stay the exception.
    // Near enough to land INSIDE the region: SL worlds are tiny islands, and a strike on your own parcel is the one that exercises everything at once - the flowmap attachment (which only knows
    // in-region geometry), real ground height, sparks, the scene light on wet streets, near-simultaneous thunder. With the squared-bias roll below, roughly one strike in eight lands within 256m.
    const F32 STRIKE_NEAR_M = 50.f;
    const F32 STRIKE_FAR_M = 12000.f;

    // Cloud base and top the channel is drawn between, above the camera.
    const F32 CLOUD_BASE_M = 600.f;
    const F32 CLOUD_TOP_M = 1400.f;

    // How far off its nominal landing point a strike will wander to find something worth hitting. Past this the search is not just expensive but wrong - a bolt that crosses a whole region to reach a
    // tower reads as guided rather than as attracted.
    const F32 ATTACH_SEARCH_M = 120.f;

    // Node cap: recursive branching is exponential and several strikes can be live at once; this keeps a fierce one from becoming a geometry bomb.
    const S32 MAX_CHANNEL_NODES = 700;

    // Strikes are built and their thunder queued this early so assets can fetch/decode and near thunder can start its lead-in BEFORE the flash - see doc/atmo_magic_lightning.md#timing.
    const F32 PREPARE_LEAD_S = 10.f;

    F32 settingF(const char* name, F32 fallback)
    {
        return gSavedSettings.controlExists(name) ? (F32)gSavedSettings.getF32(name) : fallback;
    }
}

// static
const char* SSLightning::kindName(SSStrikeKind k)
{
    switch (k)
    {
        case STRIKE_SHEET:  return "sheet";
        case STRIKE_FORK:   return "fork";
        case STRIKE_GROUND: return "ground";
        default:            return "?";
    }
}

S32 SSLightning::sceneLights(std::vector<LLVector4>& out_pos_radius,
                             std::vector<LLColor3>& out_color, S32 max_count) const
{
    out_pos_radius.clear();
    out_color.clear();
    if (max_count <= 0) return 0;

    static LLCachedControl<F32> scene_light(gSavedSettings, "SSAtmoLightningSceneLight", 1.f);
    const F32 strength = llclamp((F32)scene_light, 0.f, 4.f);
    if (strength <= 0.f) return 0;

    const LLColor3 tint = SSAtmoMagic::getInstance()->lightningCoreColor();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    for (const SSStrike& strike : mStrikes)
    {
        if ((S32)out_pos_radius.size() >= max_count) break;

        const F32 b = strike.mChannelBrightness * strike.mIntensity;
        if (b <= 0.01f) continue;

        // Light sits at the channel node nearest the camera - the part lighting your street is the part beside you, not the origin up in the cloud.
        LLVector3 best = strike.mOrigin;
        F32 best_d2 = (strike.mOrigin - cam).magVecSquared();
        for (const SSStrikeNode& node : strike.mChannel)
        {
            if (node.mReachedAt > strike.mLeaderProgress) continue;
            const F32 d2 = (node.mPos - cam).magVecSquared();
            if (d2 < best_d2) { best_d2 = d2; best = node.mPos; }
        }

        // Big enough to be a sky lighting a street rather than a lamp post. Radius grows with distance so a far strike still reaches whatever is between it and the camera.
        const F32 radius = llclamp(sqrtf(best_d2) * 0.6f, 60.f, 900.f);

        // A light so far out that its radius cannot reach any fragment the gbuffer holds would burn a batcher slot lighting nothing.
        if (sqrtf(best_d2) - radius > LLViewerCamera::getInstance()->getRenderFarPlane()) continue;

        out_pos_radius.push_back(LLVector4(best.mV[VX], best.mV[VY], best.mV[VZ], radius));
        out_color.push_back(tint * (b * strength));
    }

    return (S32)out_pos_radius.size();
}

static void ss_kill_strike_text(SSStrike& strike)
{
    if (strike.mDebugText)
    {
        strike.mDebugText->markDead();
        strike.mDebugText = nullptr;
    }
}

void SSLightning::clear()
{
    for (SSStrike& strike : mStrikes) ss_kill_strike_text(strike);
    mStrikes.clear();
    mNextStrikeAt = -1.0;
    mFlash = 0.f;
    mFlashDir.clear();
}

F64 SSLightning::nextStrikeIn() const
{
    if (mNextStrikeAt < 0.0) return -1.0;
    return llmax(0.0, mNextStrikeAt - SSAtmoMagic::getInstance()->sharedTime());
}

void SSLightning::idle(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    // Lightning IS weather, unlike the footstep voices - so this gates on isEnabled(), which already means "and something is actually running". Two switches, and they mean different things. The
    // viewer setting is "do I want to see this at all", the track's is "does this sky have lightning" - a preference and a property. Either one off is off.
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoLightning", true);
    if (!atmo->isEnabled() || !enabled || !atmo->lightningOn())
    {
        if (!mStrikes.empty()) clear();
        return;
    }

    const F64 now = atmo->sharedTime();

    // How often, from how unstable the air is. Convection is what actually drives this - a stable sky does not strike at any moisture level - and turbulence is the figure the running weather exposes
    // for it. The thresholds mirror the convection phases in ssatmoenvweatherstate.cpp so a v3 environment and a v2 track agree about when a sky is thundery; when the v3 weather state's own
    // mLightningIntervalMin/MaxSeconds are bridged through to here, this should read those instead of rederiving them.
    const F32 convection = atmo->turbulence();

    F32 interval_min = atmo->lightningIntervalMin();
    F32 interval_max = atmo->lightningIntervalMax();
    if (interval_max < 0.f)
    {
        // Nothing said - a v2 notecard track, which has no lightning fields to say it with. Derived from turbulence using the same thresholds the convection phases use in ssatmoenvweatherstate.cpp,
        // so a v2 sky and a v3 sky of the same instability strike at the same rate.
        interval_min = 0.f;
        interval_max = 0.f;
        if (convection >= 0.75f)        { interval_min = 2.f;  interval_max = 5.f;  }
        else if (convection >= 0.55f)   { interval_min = 30.f; interval_max = 60.f; }
    }

    if (interval_max <= 0.f)
    {
        // Nothing thundery about this sky. Strikes already in flight are left to finish - their thunder is still on its way.
        mNextStrikeAt = -1.0;
    }
    else
    {
        const F32 rate = llclamp(settingF("SSAtmoLightningRate", 1.f), 0.05f, 8.f);
        if (mNextStrikeAt < 0.0)
        {
            // Not from zero: a sky that has just turned severe should not fire the instant it does, and one being scrubbed past in the preview should not fire on every frame it crosses a threshold.
            SSRandStream rng((U32)(now * 1000.0) ^ atmo->seed());
            mNextStrikeAt = now + rng.frand(interval_min, interval_max) / rate;
        }
        else if (now >= mNextStrikeAt - (F64)PREPARE_LEAD_S && !mPrepared)
        {
            // Built PREPARE_LEAD_S before firing: thunder is queued against the future fire time and the soundscape works backwards from it (travel + recording onset). doc/atmo_magic_lightning.md#timing.
            const F32 fierce = (atmo->lightningIntensity() >= 0.f)
                ? atmo->lightningIntensity() : convection;
            spawn(fierce, mNextStrikeAt);
            mPrepared = true;
        }

        if (mPrepared && now >= mNextStrikeAt)
        {
            SSRandStream rng((U32)(now * 977.0) ^ atmo->seed() ^ 0x5eed1u);
            mNextStrikeAt = now + rng.frand(interval_min, interval_max) / rate;
            mPrepared = false;
        }
    }

    // Age everything, and gather the frame's flash while walking it.
    mFlash = 0.f;
    mFlashDir.clear();

    F32 brightest = 0.f;
    for (SSStrike& strike : mStrikes)
    {
        advance(strike, dt);

        mFlash += strike.mFlash;
        if (strike.mFlash > brightest)
        {
            brightest = strike.mFlash;
            LLVector3 dir = strike.mOrigin - gAgent.getPositionAgent();
            if (dir.normalize() > 0.f) mFlashDir = dir;
        }
    }
    mFlash = llclamp(mFlash, 0.f, 1.f);

    mStrikes.erase(std::remove_if(mStrikes.begin(), mStrikes.end(),
                                  [](const SSStrike& s) { return s.mDone; }),
                   mStrikes.end());
}

void SSLightning::triggerNow()
{
    // Debug button: placed IN FRONT of the camera at a random distance inside the visible range, and given a short 3s lead rather than the full window - enough for the sound to queue, short enough
    // to not feel like the button is broken. Needs Atmo enabled and lightning on, since idle() clears strikes otherwise.
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    const F32 bearing = atan2f(at.mV[VY], at.mV[VX]);

    SSRandStream rng((U32)(SSAtmoMagic::getInstance()->sharedTime() * 31.0));
    const F32 vis = LLViewerCamera::getInstance()->getRenderFarPlane() * 0.8f;

    // Down to practically overhead - natural spawns bottom out at 300m, but a debug button exists precisely to summon the case you want to look at, and the near case (sparks, scene light, the
    // channel tearing past at full width) is the one worth inspecting most. Rolled as t*t so roughly half the presses land in the near third.
    const F32 t = rng.frand();
    const F32 dist = 60.f + t * t * (llclamp(vis, 500.f, 2500.f) - 60.f);

    // Still given its preparation window rather than fired on the spot, so a hand-triggered strike sounds like a scheduled one. The wait is what buys the sound its lead-in.
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    spawn(llmax(atmo->turbulence(), 0.6f), atmo->sharedTime() + 3.0, bearing, dist);
    mPrepared = false;    // does not consume the scheduled strike's slot
}

void SSLightning::spawn(F32 intensity, F64 fire_at, F32 force_bearing, F32 force_dist)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const F64 now = atmo->sharedTime();

    SSRandStream rng((U32)(fire_at * 4093.0) ^ atmo->seed() ^ 0xb01du);

    SSStrike strike;
    strike.mCreatedAt = now;
    strike.mFireAt = fire_at;
    strike.mIntensity = llclamp(intensity, 0.f, 1.f);

    // Which kind, by weight. Sheet dominates by default because it dominates in reality: most discharges never leave the cloud, and a sky where every flash is a forked bolt to ground reads as a
    // cartoon.
    F32 w[STRIKE_KIND_COUNT];
    w[STRIKE_SHEET]  = llmax(settingF("SSAtmoLightningWeightSheet",  6.f), 0.f);
    w[STRIKE_FORK]   = llmax(settingF("SSAtmoLightningWeightFork",   3.f), 0.f);
    w[STRIKE_GROUND] = llmax(settingF("SSAtmoLightningWeightGround", 1.f), 0.f);

    const F32 total = w[0] + w[1] + w[2];
    if (total <= 0.f) return;    // every kind switched off is a valid answer

    F32 roll = rng.frand(0.f, total);
    strike.mKind = STRIKE_SHEET;
    for (S32 k = 0; k < STRIKE_KIND_COUNT; ++k)
    {
        if (roll < w[k]) { strike.mKind = (SSStrikeKind)k; break; }
        roll -= w[k];
    }

    // Where. Squared so the far half of the range gets most of the strikes - a linear pick would put half of them inside 6km, which is a storm directly overhead every time.
    const F32 t = rng.frand();
    const F32 dist = (force_dist >= 0.f) ? force_dist
        : STRIKE_NEAR_M + (STRIKE_FAR_M - STRIKE_NEAR_M) * t * t;
    const F32 bearing = (force_bearing > -10.f && force_dist >= 0.f) ? force_bearing
        : rng.frand(0.f, F_TWO_PI);

    const LLVector3 cam = gAgent.getPositionAgent();

    // Channel spans SSVolCloud's live band [interaction: volcloud band -> bolt altitude]; the constants only stand in when no field is built.
    F32 band_lo = cam.mV[VZ] + CLOUD_BASE_M;
    F32 band_hi = cam.mV[VZ] + CLOUD_TOP_M;
    SSVolCloud* field = SSVolCloud::getInstance();
    if (!field->empty())
    {
        band_lo = field->cloudBaseZ();
        band_hi = llmax(field->cloudTopZ(), band_lo + 50.f);
    }

    strike.mOrigin.set(cam.mV[VX] + cosf(bearing) * dist,
                       cam.mV[VY] + sinf(bearing) * dist,
                       band_lo + rng.frand(0.2f, 0.9f) * (band_hi - band_lo));

    // A ground strike lands roughly under its origin, wandering as it comes down. Region ground height is only known nearby, so anything past the world simply terminates at the camera's own ground
    // level - at ten kilometres nobody can tell, and pretending otherwise would mean raycasting into regions that are not loaded.
    LLVector3 ground = strike.mOrigin;
    ground.mV[VZ] = cam.mV[VZ];
    if (LLViewerRegion* regionp = gAgent.getRegion())
    {
        const LLVector3 local = ground - regionp->getOriginAgent();
        if (local.mV[VX] >= 0.f && local.mV[VY] >= 0.f &&
            local.mV[VX] < 256.f && local.mV[VY] < 256.f)
        {
            ground.mV[VZ] = regionp->getLand().resolveHeightRegion(local);
        }
    }
    // Attachment [interaction: sswindflow height capture -> landing point]: best (height - lateral*bias) within reach, so a modest roof underfoot beats a spire 100m off - doc/atmo_magic_lightning.md#attachment.
    if (strike.mKind == STRIKE_GROUND)
    {
        const F32 penalty = llclamp(settingF("SSAtmoLightningAttachBias", 1.f), 0.f, 4.f);

        F32 best_score = -1.0e9f;
        LLVector3 best;
        bool found = false;

        SSWindFlowMap::getInstance()->forEachColumn(ground, ATTACH_SEARCH_M,
            [&](const LLVector3& pos, F32 top)
            {
                const F32 dx = pos.mV[VX] - ground.mV[VX];
                const F32 dy = pos.mV[VY] - ground.mV[VY];
                const F32 lateral = sqrtf(dx * dx + dy * dy);

                const F32 score = top - lateral * penalty;
                if (score > best_score)
                {
                    best_score = score;
                    best = pos;
                    found = true;
                }
            });

        if (found) ground = best;
    }

    strike.mGround = ground;

    strike.mDistanceM = (strike.mOrigin - cam).magVec();
    strike.mAudible = strike.mDistanceM < THUNDER_SHADOW_ZONE_M;

    if (strike.mKind != STRIKE_SHEET)
    {
        buildChannel(strike, strike.mIntensity);
    }

    // Thunder handed over at build time with the FUTURE fire time [interaction: -> soundscape]; only that side knows travel time and the recording's own onset.
    if (strike.mAudible)
    {
        SSSoundscape::getInstance()->scheduleThunder(
            strike.mGround, strike.mDistanceM, strike.mIntensity, strike.mFireAt);
    }
    strike.mThunderSent = true;

    mStrikes.push_back(strike);
}

void SSLightning::growPath(SSStrike& strike, S32 parent,
                           const LLVector3& from, const LLVector3& to,
                           S32 levels, F32 width_start, F32 width_end,
                           F32 t_start, F32 t_end, bool trunk,
                           SSRandStream& rng, std::vector<S32>& out_nodes)
{
    out_nodes.clear();

    LLVector3 axis = to - from;
    const F32 len = axis.magVec();
    if (len < 0.5f) return;

    const LLVector3 dir = axis / len;

    // Displacement is in the plane perpendicular to the channel AXIS: correct at any orientation, and gives a horizontal in-cloud fork the vertical wander world-XY jitter gave none of.
    const LLVector3 ref = (fabsf(dir.mV[VZ]) > 0.9f)
        ? LLVector3(1.f, 0.f, 0.f) : LLVector3(0.f, 0.f, 1.f);
    LLVector3 side_u = dir % ref;
    if (side_u.normalize() <= 0.f) return;
    LLVector3 side_v = dir % side_u;
    if (side_v.normalize() <= 0.f) return;

    // Midpoint displacement: self-similar kinks at every scale, deliberately left unsmoothed - straight runs meeting hard angles are the character (doc/atmo_magic_lightning.md#channel-generation).
    std::vector<LLVector3> pts;
    pts.reserve((size_t)1 << (levels + 1));
    pts.push_back(from);
    pts.push_back(to);

    F32 amp = len * 0.09f;
    for (S32 l = 0; l < levels; ++l)
    {
        std::vector<LLVector3> next;
        next.reserve(pts.size() * 2);

        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            next.push_back(pts[k]);

            LLVector3 mid = (pts[k] + pts[k + 1]) * 0.5f;
            mid += side_u * rng.frand(-amp, amp);
            mid += side_v * rng.frand(-amp, amp);

            // ...and a little ALONG the axis as well, which is what makes the step lengths uneven. A stepped leader does not advance in equal jumps, and evenly spaced kinks read as a zigzag ornament
            // rather than as a discharge finding its way.
            mid += dir * rng.frand(-amp * 0.35f, amp * 0.35f);

            next.push_back(mid);
        }
        next.push_back(pts.back());

        pts.swap(next);
        amp *= 0.55f;
    }

    S32 prev = parent;
    const F32 count = (F32)(pts.size() - 1);
    for (size_t k = 0; k < pts.size(); ++k)
    {
        if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

        // The first point of a branch IS its parent node - it starts where it left. Emitting it again would be a zero-length segment.
        if (k == 0 && parent >= 0) continue;

        const F32 f = (F32)k / llmax(count, 1.f);

        SSStrikeNode node;
        node.mPos = pts[k];
        node.mParent = prev;
        node.mTrunk = trunk;
        node.mWidth = lerp(width_start, width_end, f);
        node.mReachedAt = llclamp(lerp(t_start, t_end, f), 0.f, 1.f);
        strike.mChannel.push_back(node);

        prev = (S32)strike.mChannel.size() - 1;
        out_nodes.push_back(prev);
    }
}

void SSLightning::growBranches(SSStrike& strike, const std::vector<S32>& along,
                               S32 depth, S32 levels, F32 intensity,
                               SSRandStream& rng)
{
    if (depth <= 0 || along.size() < 3) return;
    if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

    // Fewer, shorter branches the deeper in this is. A first-generation branch off the trunk is an event; a fourth-generation one is texture.
    const S32 count = llmax(1, (S32)(rng.frand(1.6f, 3.4f) * (0.55f + intensity * 0.45f)));

    for (S32 b = 0; b < count; ++b)
    {
        if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

        // Not from the very start or the very end: a branch at the root is a second trunk, and one at the tip is a frayed end.
        const S32 pick = 1 + rng.rand((S32)along.size() - 2);
        const S32 from_idx = along[(size_t)pick];
        const SSStrikeNode& from_node = strike.mChannel[(size_t)from_idx];
        if (from_node.mParent < 0) continue;

        // Which way the channel was already travelling here. A leader branches in the direction it is going - it does not turn round - so the branch is that direction, deviated.
        LLVector3 travel = from_node.mPos - strike.mChannel[(size_t)from_node.mParent].mPos;
        if (travel.normalize() <= 0.f) continue;

        LLVector3 dir = travel;
        dir.mV[VX] += rng.frand(-0.55f, 0.55f);
        dir.mV[VY] += rng.frand(-0.55f, 0.55f);
        dir.mV[VZ] += rng.frand(-0.35f, 0.15f);   // biased to keep descending
        if (dir.normalize() <= 0.f) continue;

        // Each generation reaches less far than the one it left.
        const F32 reach = rng.frand(25.f, 90.f) * (F32)depth * 0.55f;
        const LLVector3 end = from_node.mPos + dir * reach;

        // It cannot exist before the node it grew from, and it finishes before the parent run does - a branch is a detour, not an extension.
        const F32 t0 = from_node.mReachedAt;
        const F32 t1 = llmin(1.f, t0 + 0.12f * (F32)depth);

        std::vector<S32> sub;
        growPath(strike, from_idx, from_node.mPos, end,
                 llmax(levels - 1, 1),
                 from_node.mWidth * 0.55f, from_node.mWidth * 0.2f,
                 t0, t1, false, rng, sub);

        growBranches(strike, sub, depth - 1, llmax(levels - 1, 1),
                     intensity * 0.75f, rng);
    }
}

void SSLightning::buildChannel(SSStrike& strike, F32 intensity)
{
    SSRandStream rng((U32)(strike.mFireAt * 7919.0) ^ 0xfa11u);

    const bool to_ground = (strike.mKind == STRIKE_GROUND);

    // A fork channel stops in mid-air rather than landing. Two thirds of the way down is far enough to look like it is going somewhere and short enough that it plainly never got there.
    const LLVector3 tip = to_ground
        ? strike.mGround
        : strike.mOrigin + (strike.mGround - strike.mOrigin) * rng.frand(0.4f, 0.7f);

    // How finely to subdivide, by how close this one is. Detail nobody can resolve is detail nobody should pay for - but a strike a few hundred metres off is something a camera can be flown into,
    // now that the channel lives inside a cloud you can enter. Six levels is sixty-five points of trunk; three is nine, which at ten kilometres is more than the handful of pixels it occupies.
    S32 levels = 3;
    if (strike.mDistanceM < 800.f)       levels = 6;
    else if (strike.mDistanceM < 2500.f) levels = 5;
    else if (strike.mDistanceM < 6000.f) levels = 4;

    static LLCachedControl<S32> depth_setting(gSavedSettings, "SSAtmoLightningBranchDepth", 3);
    const S32 depth = llclamp((S32)depth_setting, 0, 5);

    strike.mChannel.reserve(256);

    std::vector<S32> trunk;
    growPath(strike, -1, strike.mOrigin, tip, levels,
             1.f, 0.6f, 0.f, 1.f, true, rng, trunk);

    growBranches(strike, trunk, depth, levels, intensity, rng);
}

void SSLightning::advance(SSStrike& strike, F32 dt)
{
    strike.mT = (F32)(SSAtmoMagic::getInstance()->sharedTime() - strike.mFireAt);

    SSRandStream rng((U32)(strike.mFireAt * 3571.0) ^ 0x11feu);

    const F32 leader_s = rng.frand(LEADER_MIN_S, LEADER_MAX_S);
    const S32 strokes = RETURN_STROKES_MIN + rng.rand(RETURN_STROKES_MAX - RETURN_STROKES_MIN + 1);

    // The anticipation, if it is switched on. Runs up to the leader, not up to the return stroke, because once the leader is on its way down the strike is no longer being anticipated - it is
    // happening.
    const F32 charge_s = SSAtmoMagic::getInstance()->lightningCharge()
        ? llclamp(settingF("SSAtmoLightningAnticipation", 0.f), 0.f, ANTICIPATION_MAX_S)
        : 0.f;
    if (charge_s > 0.f && strike.mT < -leader_s)
    {
        const F32 until = -leader_s - strike.mT;    // seconds still to wait
        strike.mCharge = (until < charge_s) ? (1.f - until / charge_s) : 0.f;

        // The crackle starts once, at the foot of the ramp, and runs to meet the strike. Its own length is the author's business; what matters here is that it begins when the build does.
        if (strike.mCharge > 0.f && !strike.mChargeSent)
        {
            strike.mChargeSent = true;
            SSSoundscape::getInstance()->playCharge(strike.mGround, strike.mIntensity);
        }
    }
    else
    {
        strike.mCharge = 0.f;
    }

    F32 brightness = 0.f;

    strike.mStrokeCount = 0;

    if (strike.mT < -leader_s)
    {
        // Still coming. Nothing of the discharge itself is visible; only the charge above, if anything.
        strike.mLeaderProgress = 0.f;
    }
    else if (strike.mT < 0.f)
    {
        // The leader, on its way down and visible while it goes. A sheet strike has no channel to show, so it simply waits out this phase dark - the cloud does not light up until the return stroke
        // either.
        strike.mLeaderProgress = 1.f - (-strike.mT / leader_s);
        brightness = (strike.mKind == STRIKE_SHEET) ? 0.f : LEADER_GLOW;

        // The leader is drawn as a pseudo-stroke at zero offset: it is the channel being CREATED, so there is nothing for it to have drifted from yet.
        if (brightness > 0.f)
        {
            strike.mStrokeCount = 1;
            strike.mStrokeAt[0] = 0.f;
            strike.mStrokeBright[0] = brightness;
        }
    }
    else
    {
        strike.mLeaderProgress = 1.f;

        // Return strokes, each a spike down the whole channel decaying away before the next arrives. Summed rather than switched between, so strokes that fall close together run into one another the
        // way they do in a flash that reads as one long flicker.
        F32 at = 0.f;
        for (S32 i = 0; i < strokes; ++i)
        {
            if (strike.mT >= at)
            {
                const F32 since = strike.mT - at;
                // Later strokes down an already-ionised channel are dimmer than the first, which is why a flash fades as it flickers.
                const F32 scale = 1.f / (1.f + (F32)i * 0.6f);
                const F32 glow = scale * expf(-since / STROKE_DECAY_S);
                brightness += glow;

                // Kept per-stroke as well as summed: the wind offset the renderer derives from mStrokeAt is what turns strokes into a ribbon.
                if (strike.mStrokeCount < SSStrike::MAX_STROKES)
                {
                    strike.mStrokeAt[strike.mStrokeCount] = at;
                    strike.mStrokeBright[strike.mStrokeCount] = glow;
                    ++strike.mStrokeCount;
                }
            }
            at += rng.frand(RESTRIKE_MIN_S, RESTRIKE_MAX_S);
        }

        if (strike.mT > at + STROKE_DECAY_S * 6.f)
        {
            // Visually finished, and safe to retire: the clap was handed to the soundscape when the strike was prepared, and the soundscape holds it independently of anything here.
            brightness = 0.f;
            strike.mDone = true;
        }
    }

    strike.mChannelBrightness = llclamp(brightness, 0.f, 1.f);

    // Debug marker text: a countdown floating over the attachment point while the strike is pending, so where-and-when can be checked visually before anything fires. The beam/box geometry half
    // lives in sslightningrender.cpp under the same setting.
    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugStrikeMarkers", false);
    if (markers && strike.mT < 0.f && !strike.mDone)
    {
        if (!strike.mDebugText)
        {
            strike.mDebugText = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            if (strike.mDebugText)
            {
                strike.mDebugText->setDoFade(false);
                strike.mDebugText->setColor(LLColor4(1.f, 0.25f, 0.9f, 1.f));
            }
        }
        if (strike.mDebugText)
        {
            strike.mDebugText->setPositionAgent(strike.mGround + LLVector3(0.f, 0.f, 4.f));
            strike.mDebugText->setString(llformat("%s strike in %.1fs%s",
                kindName(strike.mKind), -strike.mT,
                strike.mCharge > 0.f ? llformat("  charge %.0f%%", strike.mCharge * 100.f).c_str() : ""));
        }
    }
    else
    {
        ss_kill_strike_text(strike);
    }

    // The sky lift. Falls off with distance, but nothing like inverse square - a flash ten kilometres off still lights the whole sky, because what you are seeing is the cloud deck itself lit up
    // rather than the channel.
    const F32 falloff = 1.f / (1.f + (strike.mDistanceM / 4000.f) * (strike.mDistanceM / 4000.f));
    strike.mFlash = llclamp(brightness, 0.f, 1.f) * strike.mIntensity * falloff;

}
