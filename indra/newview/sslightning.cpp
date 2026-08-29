/**
 * @file sslightning.cpp
 * @brief See sslightning.h.
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
#include "llviewerdisplay.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "llsurface.h"
#include "llvector4a.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "pipeline.h"

namespace
{

    const F32 ANTICIPATION_MAX_S = 8.f;

    // Matches the settings.xml default: the anticipation effect ships switched on.
    const F32 ANTICIPATION_DEFAULT_S = 3.f;

    const F32 LEADER_MIN_S = 0.05f;
    const F32 LEADER_MAX_S = 0.18f;
    const F32 LEADER_GLOW = 0.12f;

    const S32 RETURN_STROKES_MIN = 1;
    const S32 RETURN_STROKES_MAX = 4;
    const F32 RESTRIKE_MIN_S = 0.03f;
    const F32 RESTRIKE_MAX_S = 0.09f;
    const F32 STROKE_DECAY_S = 0.055f;

    const F32 THUNDER_SHADOW_ZONE_M = 20000.f;

    const F32 STRIKE_NEAR_M = 50.f;
    const F32 STRIKE_FAR_M = 12000.f;

    const F32 CLOUD_BASE_M = 600.f;
    const F32 CLOUD_TOP_M = 1400.f;

    const F32 ATTACH_SEARCH_M = 120.f;

    const S32 MAX_CHANNEL_NODES = 1000;

    // Ground-strike branch exclusion: a cone about the main line's foot - apex held over
    // the attachment, half-angle rolled per strike - and a floor above the ground.
    const F32 BRANCH_CONE_APEX_M = 80.f;
    const F32 BRANCH_CONE_HALF_ANGLE_MIN_DEG = 20.f;
    const F32 BRANCH_CONE_HALF_ANGLE_MAX_DEG = 40.f;
    const F32 BRANCH_FLOOR_MIN_M = 20.f;
    const F32 BRANCH_FLOOR_MAX_M = 40.f;

    const S32 BRANCH_TRIES = 4;

    const F32 PREPARE_LEAD_S = 10.f;

    // Setting read with a fallback when it does not exist.
    F32 settingF(const char* name, F32 fallback)
    {
        return gSavedSettings.controlExists(name) ? (F32)gSavedSettings.getF32(name) : fallback;
    }

    // Debug strikes spawn as far ahead as the anticipation effect needs, so the slider's
    // charge window fits inside a button-triggered strike the way it does a scheduled one.
    // The floor keeps the buttons feeling deliberate when anticipation is low or zero.
    F32 debugStrikeLeadS()
    {
        const F32 anticipation = llclamp(settingF("SSAtmoLightningAnticipation", ANTICIPATION_DEFAULT_S), 0.f, ANTICIPATION_MAX_S);
        return llmax(3.f, anticipation + 0.25f);
    }
}

// Debug label for a strike kind.
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

// Debug overlay colour per strike kind - the markers and the countdown labels share it, so a glance tells the kinds apart.
const LLColor4& SSLightning::kindDebugColor(SSStrikeKind k)
{
    static const LLColor4 sheet(0.2f, 0.9f, 1.f, 0.75f);
    static const LLColor4 fork(1.f, 0.75f, 0.15f, 0.75f);
    static const LLColor4 ground(1.f, 0.15f, 0.85f, 0.75f);
    switch (k)
    {
        case STRIKE_SHEET:  return sheet;
        case STRIKE_FORK:   return fork;
        default:            return ground;
    }
}

// Exports the brightest live strikes as deferred point lights, positioned at each channel's node nearest the camera.
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

        LLVector3 best = strike.mOrigin;
        F32 best_d2 = (strike.mOrigin - cam).magVecSquared();
        for (const SSStrikeNode& node : strike.mChannel)
        {
            if (node.mReachedAt > strike.mLeaderProgress) continue;
            const F32 d2 = (node.mPos - cam).magVecSquared();
            if (d2 < best_d2) { best_d2 = d2; best = node.mPos; }
        }

        const F32 radius = llclamp(sqrtf(best_d2) * 0.6f, 60.f, 900.f);

        if (sqrtf(best_d2) - radius > MAX_FAR_CLIP) continue;

        out_pos_radius.push_back(LLVector4(best.mV[VX], best.mV[VY], best.mV[VZ], radius));
        out_color.push_back(tint * (b * strength));
    }

    return (S32)out_pos_radius.size();
}

// Kills a strike's debug HUD text.
static void ss_kill_strike_text(SSStrike& strike)
{
    if (strike.mDebugText)
    {
        strike.mDebugText->markDead();
        strike.mDebugText = nullptr;
    }
}

// True when a point falls inside a ground strike's branch exclusion: below the floor held
// over the attachment, or down in the cone about the main line's descent - apex 80m over
// the ground with a half-angle rolled 20-40deg. Branches off the trunk are what this keeps
// away from the strike point; the trunk itself never asks.
static bool ss_branch_forbidden(const SSStrike& strike, const LLVector3& pos)
{
    if (!strike.mBranchLimits) return false;

    if (pos.mV[VZ] < strike.mBranchFloorZ) return true;

    const LLVector3 from_apex = pos - strike.mBranchConeApex;
    const F32 along = from_apex * strike.mBranchConeAxis;
    if (along <= 0.f) return false; // above the apex - the cone only reaches down

    const F32 len = from_apex.magVec();
    if (len < 0.01f) return true;
    return (along / len) >= strike.mBranchConeDot;
}

// Drops all strikes and scheduling - the off switch.
void SSLightning::clear()
{
    for (SSStrike& strike : mStrikes) ss_kill_strike_text(strike);
    mStrikes.clear();
    mNextStrikeAt = -1.0;
    mFlash = 0.f;
    mFlashDir.clear();
}

// Seconds until the next scheduled strike, for the overlay.
F64 SSLightning::nextStrikeIn() const
{
    if (mNextStrikeAt < 0.0) return -1.0;
    return llmax(0.0, mNextStrikeAt - SSAtmoMagic::getInstance()->sharedTime());
}

// Per-frame drive: schedules strikes from convection (or the track's intervals), prepares each a lead early, advances the live ones and gathers the frame's flash.
void SSLightning::idle(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoLightning", true);
    if (!atmo->isEnabled() || !enabled || !atmo->lightningOn())
    {
        if (!mStrikes.empty()) clear();
        return;
    }

    const F64 now = atmo->sharedTime();

    const F32 convection = atmo->turbulence();

    F32 interval_min = atmo->lightningIntervalMin();
    F32 interval_max = atmo->lightningIntervalMax();
    if (interval_max < 0.f)
    {
        interval_min = 0.f;
        interval_max = 0.f;
        if (convection >= 0.75f)        { interval_min = 2.f;  interval_max = 5.f;  }
        else if (convection >= 0.55f)   { interval_min = 30.f; interval_max = 60.f; }
    }

    if (interval_max <= 0.f)
    {
        mNextStrikeAt = -1.0;
    }
    else
    {
        const F32 rate = llclamp(settingF("SSAtmoLightningRate", 1.f), 0.05f, 8.f);
        if (mNextStrikeAt < 0.0)
        {
            SSRandStream rng((U32)(now * 1000.0) ^ atmo->seed());
            mNextStrikeAt = now + rng.frand(interval_min, interval_max) / rate;
        }
        else if (now >= mNextStrikeAt - (F64)PREPARE_LEAD_S && !mPrepared)
        {
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

// Debug button: a strike in front of the camera with a short lead, so it still sounds like a scheduled one.
void SSLightning::triggerNow()
{
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    const F32 bearing = atan2f(at.mV[VY], at.mV[VX]);

    SSRandStream rng((U32)(SSAtmoMagic::getInstance()->sharedTime() * 31.0));
    const F32 vis = MAX_FAR_CLIP * 0.8f;

    const F32 t = rng.frand();
    const F32 dist = 60.f + t * t * (llclamp(vis, 500.f, 2500.f) - 60.f);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    spawn(llmax(atmo->turbulence(), 0.6f), atmo->sharedTime() + debugStrikeLeadS(), bearing, dist);
    mPrepared = false;
}

// Debug button: a ground strike where the camera ray through the lower third of the view meets land or a build, so the mark lands where you are looking rather than wherever the roll put it.
void SSLightning::triggerGroundNow()
{
    if (!gViewerWindow) return;

    const LLRect& view = gViewerWindow->getWorldViewRectScaled();
    const S32 x = (S32)view.getCenterX();
    const S32 y = (S32)(view.mBottom + (F32)view.getHeight() / 3.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3 dir = gViewerWindow->mouseDirectionGlobal(x, y);

    LLVector4a start, end, pos;
    start.load3(cam.mV);
    end.load3((cam + dir * MAX_FAR_CLIP).mV);

    LLVector3 at;
    bool found = false;
    if (gPipeline.lineSegmentIntersectWorldGeometry(start, end, &pos))
    {
        at.set(pos.getF32ptr());
        found = true;
    }
    else if (dir.mV[VZ] < -0.01f)
    {
        // Open sky along the whole ray (the surface is past the far clip): walk it down to where it dips under the terrain.
        for (F32 d = 50.f; d < MAX_FAR_CLIP; d += 32.f)
        {
            const LLVector3 p = cam + dir * d;
            if (p.mV[VZ] <= LLWorld::getInstance()->resolveLandHeightAgent(p))
            {
                at = p;
                found = true;
                break;
            }
        }
    }
    if (!found) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    spawn(llmax(atmo->turbulence(), 0.6f), atmo->sharedTime() + debugStrikeLeadS(), -1.f, -1.f, STRIKE_GROUND, &at);
    mPrepared = false;
}

// Builds a full strike for a future fire time: kind, placement, attachment, channel, and thunder handed to the soundscape with its lead. A forced kind or ground point (debug buttons) skips the rolls for that part.
void SSLightning::spawn(F32 intensity, F64 fire_at, F32 force_bearing, F32 force_dist,
                        SSStrikeKind force_kind, const LLVector3* force_ground)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const F64 now = atmo->sharedTime();

    SSRandStream rng((U32)(fire_at * 4093.0) ^ atmo->seed() ^ 0xb01du);

    SSStrike strike;
    strike.mCreatedAt = now;
    strike.mFireAt = fire_at;
    strike.mIntensity = llclamp(intensity, 0.f, 1.f);

    if (force_kind < STRIKE_KIND_COUNT)
    {
        strike.mKind = force_kind;
    }
    else
    {
        F32 w[STRIKE_KIND_COUNT];
        w[STRIKE_SHEET]  = llmax(settingF("SSAtmoLightningWeightSheet",  6.f), 0.f);
        w[STRIKE_FORK]   = llmax(settingF("SSAtmoLightningWeightFork",   3.f), 0.f);
        w[STRIKE_GROUND] = llmax(settingF("SSAtmoLightningWeightGround", 1.f), 0.f);

        const F32 total = w[0] + w[1] + w[2];
        if (total <= 0.f) return;

        F32 roll = rng.frand(0.f, total);
        strike.mKind = STRIKE_SHEET;
        for (S32 k = 0; k < STRIKE_KIND_COUNT; ++k)
        {
            if (roll < w[k]) { strike.mKind = (SSStrikeKind)k; break; }
            roll -= w[k];
        }
    }

    const LLVector3 cam = gAgent.getPositionAgent();

    F32 band_lo = cam.mV[VZ] + CLOUD_BASE_M;
    F32 band_hi = cam.mV[VZ] + CLOUD_TOP_M;
    SSVolCloud* field = SSVolCloud::getInstance();
    if (!field->empty())
    {
        band_lo = field->cloudBaseZ();
        band_hi = llmax(field->cloudTopZ(), band_lo + 50.f);
    }

    if (force_ground)
    {
        // Debug placement: the bolt anchors at the given point exactly - no terrain resolve, no attach-bias
        // search - with the cloud origin straight above it.
        strike.mGround = *force_ground;
        strike.mOrigin.set(strike.mGround.mV[VX], strike.mGround.mV[VY],
                           band_lo + rng.frand(0.2f, 0.9f) * (band_hi - band_lo));
    }
    else
    {
        const F32 t = rng.frand();
        const F32 dist = (force_dist >= 0.f) ? force_dist
            : STRIKE_NEAR_M + (STRIKE_FAR_M - STRIKE_NEAR_M) * t * t;
        const F32 bearing = (force_bearing > -10.f && force_dist >= 0.f) ? force_bearing
            : rng.frand(0.f, F_TWO_PI);

        strike.mOrigin.set(cam.mV[VX] + cosf(bearing) * dist,
                           cam.mV[VY] + sinf(bearing) * dist,
                           band_lo + rng.frand(0.2f, 0.9f) * (band_hi - band_lo));

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
    }

    strike.mDistanceM = (strike.mOrigin - cam).magVec();

    if (strike.mKind != STRIKE_SHEET)
    {
        buildChannel(strike, strike.mIntensity);
    }

    LLVector3 thunder_pos = strike.mGround;
    F32 thunder_d_sq = (thunder_pos - cam).magVecSquared();
    for (const SSStrikeNode& node : strike.mChannel)
    {
        const F32 d_sq = (node.mPos - cam).magVecSquared();
        if (d_sq < thunder_d_sq) { thunder_d_sq = d_sq; thunder_pos = node.mPos; }
    }
    const F32 thunder_d = sqrtf(thunder_d_sq);
    strike.mAudible = thunder_d < THUNDER_SHADOW_ZONE_M;

    F32 muffle = 0.f;
    {
        SSVolCloud* vol = SSVolCloud::getInstance();
        if (!vol->empty())
        {
            muffle = 1.f - vol->transmittance(cam, thunder_pos, 1.f);
        }
        if (strike.mKind != STRIKE_GROUND) muffle = llmax(muffle, 0.35f);
    }

    if (strike.mAudible)
    {
        SSSoundscape::getInstance()->scheduleThunder(
            thunder_pos, thunder_d, strike.mIntensity, strike.mFireAt, muffle);
    }
    strike.mThunderSent = true;

    mStrikes.push_back(strike);
}

// One midpoint-displaced run of channel between two points - the self-similar kinked geometry everything else hangs off.
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

    const LLVector3 ref = (llabs(dir.mV[VZ]) > 0.9f)
        ? LLVector3(1.f, 0.f, 0.f) : LLVector3(0.f, 0.f, 1.f);
    LLVector3 side_u = dir % ref;
    if (side_u.normalize() <= 0.f) return;
    LLVector3 side_v = dir % side_u;
    if (side_v.normalize() <= 0.f) return;

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

// Recursive branches off a run: count and reach proportional to the run, deviated from the local travel direction.
void SSLightning::growBranches(SSStrike& strike, const std::vector<S32>& along,
                               S32 depth, S32 levels, F32 intensity,
                               SSRandStream& rng, F32 fecundity)
{
    if (depth <= 0 || along.size() < 3) return;
    if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

    const LLVector3& run_a = strike.mChannel[(size_t)along.front()].mPos;
    const LLVector3& run_b = strike.mChannel[(size_t)along.back()].mPos;
    const F32 run_len = (run_b - run_a).magVec();
    const F32 len_gain = llclamp(run_len / 700.f, 1.f, 4.f);

    const S32 count = llmax(1, (S32)(rng.frand(1.6f, 3.4f) * (0.55f + intensity * 0.45f) * len_gain * fecundity));

    for (S32 b = 0; b < count; ++b)
    {
        if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

        // A candidate that would come down inside the trunk cone or under the ground floor
        // is aborted and re-rolled from another node - a few tries, then the branch is dropped.
        for (S32 attempt = 0; attempt < BRANCH_TRIES; ++attempt)
        {
            const S32 pick = 1 + rng.rand((S32)along.size() - 2);
            const S32 from_idx = along[(size_t)pick];
            const SSStrikeNode& from_node = strike.mChannel[(size_t)from_idx];
            if (from_node.mParent < 0) continue;

            LLVector3 travel = from_node.mPos - strike.mChannel[(size_t)from_node.mParent].mPos;
            if (travel.normalize() <= 0.f) continue;

            const F32 vertical = llabs(travel.mV[VZ]);
            LLVector3 dir = travel;
            dir.mV[VX] += rng.frand(-0.55f, 0.55f);
            dir.mV[VY] += rng.frand(-0.55f, 0.55f);
            dir.mV[VZ] += rng.frand(-0.35f, 0.15f) * llmax(vertical, 0.25f);
            if (dir.normalize() <= 0.f) continue;

            const F32 reach = llclamp(rng.frand(0.10f, 0.30f) * run_len, 30.f, 900.f);
            const LLVector3 end = from_node.mPos + dir * reach;

            if (ss_branch_forbidden(strike, end)) continue;

            const F32 t0 = from_node.mReachedAt;
            const F32 t1 = llmin(1.f, t0 + 0.12f * (F32)depth);

            const size_t grown = strike.mChannel.size();

            std::vector<S32> sub;
            growPath(strike, from_idx, from_node.mPos, end,
                     llmax(levels - 1, 1),
                     from_node.mWidth * 0.55f, from_node.mWidth * 0.2f,
                     t0, t1, false, rng, sub);

            // The midpoint wander can still carry the run into the cone or under the floor:
            // roll the grown nodes back and try somewhere else.
            bool rejected = false;
            for (const S32 idx : sub)
            {
                if (ss_branch_forbidden(strike, strike.mChannel[(size_t)idx].mPos))
                {
                    rejected = true;
                    break;
                }
            }
            if (rejected)
            {
                strike.mChannel.resize(grown);
                continue;
            }

            growBranches(strike, sub, depth - 1, llmax(levels - 1, 1),
                         intensity * 0.75f, rng, fecundity);
            break;
        }
    }
}

// Chooses the morphology (spider, bidirectional crawler, cloud-to-air, or ground trunk with a fork-style cloud spread) and grows the whole channel.
void SSLightning::buildChannel(SSStrike& strike, F32 intensity)
{
    SSRandStream rng((U32)(strike.mFireAt * 7919.0) ^ 0xfa11u);

    const bool to_ground = (strike.mKind == STRIKE_GROUND);

    if (to_ground)
    {
        // Hold forked channels off the strike point: a cone about the main line's descent,
        // apex 80m over the attachment with a 20-40deg half-angle, and a floor 20-40m over it.
        LLVector3 axis = strike.mGround - strike.mOrigin;
        if (axis.normalize() <= 0.f) axis.set(0.f, 0.f, -1.f);
        strike.mBranchLimits = true;
        strike.mBranchConeAxis = axis;
        strike.mBranchConeApex = strike.mGround + LLVector3(0.f, 0.f, BRANCH_CONE_APEX_M);
        strike.mBranchConeDot = cosf(rng.frand(BRANCH_CONE_HALF_ANGLE_MIN_DEG, BRANCH_CONE_HALF_ANGLE_MAX_DEG) * DEG_TO_RAD);
        strike.mBranchFloorZ = strike.mGround.mV[VZ] + rng.frand(BRANCH_FLOOR_MIN_M, BRANCH_FLOOR_MAX_M);
    }

    S32 levels = 3;
    if (strike.mDistanceM < 800.f)       levels = 6;
    else if (strike.mDistanceM < 2500.f) levels = 5;
    else if (strike.mDistanceM < 6000.f) levels = 4;

    static LLCachedControl<S32> depth_setting(gSavedSettings, "SSAtmoLightningBranchDepth", 3);
    const S32 depth = llclamp((S32)depth_setting, 0, 5);

    strike.mChannel.reserve(256);

    if (rng.frand() < (to_ground ? 0.25f : 0.35f))
    {
        // All primary geometry grows before any branches - the horizontal spread must not
        // be left to fight the trunk's forks for the last channel nodes.
        std::vector<S32> trunk;
        if (to_ground)
        {
            growPath(strike, -1, strike.mOrigin, strike.mGround, levels,
                     1.f, 0.6f, 0.f, 1.f, true, rng, trunk);
        }

        const S32 arm_levels = llmax(levels - 2, 3);
        const S32 arms = 4 + rng.rand(4);
        const F32 base_bearing = rng.frand(0.f, F_TWO_PI);

        std::vector<std::vector<S32>> arm_nodes((size_t)arms);
        for (S32 a = 0; a < arms; ++a)
        {
            const F32 bearing = base_bearing + (F32)a * F_TWO_PI / (F32)arms + rng.frand(-0.5f, 0.5f);
            const F32 reach = rng.frand(400.f, 1300.f);
            const LLVector3 tip = strike.mOrigin
                + LLVector3(cosf(bearing) * reach, sinf(bearing) * reach, rng.frand(-120.f, 120.f));
            growPath(strike, -1, strike.mOrigin, tip, arm_levels,
                     0.85f, 0.3f, 0.f, 1.f, !to_ground && a == 0, rng, arm_nodes[(size_t)a]);
        }
        if (to_ground)
        {
            growBranches(strike, trunk, depth, levels, intensity, rng);
        }
        for (S32 a = 0; a < arms; ++a)
        {
            growBranches(strike, arm_nodes[(size_t)a], depth, arm_levels, intensity * 0.85f, rng, 2.f);
        }
        return;
    }

    if (!to_ground && rng.frand() < 0.5f)
    {
        const F32 bearing = rng.frand(0.f, F_TWO_PI);

        std::vector<S32> run_a, run_b;
        {
            const F32 reach = rng.frand(600.f, 2400.f);
            const LLVector3 tip = strike.mOrigin
                + LLVector3(cosf(bearing) * reach, sinf(bearing) * reach, rng.frand(-150.f, 150.f));
            growPath(strike, -1, strike.mOrigin, tip, levels,
                     1.f, 0.6f, 0.f, 1.f, true, rng, run_a);
        }
        {
            const F32 back = bearing + F_PI + rng.frand(-0.5f, 0.5f);
            const F32 reach = rng.frand(400.f, 1600.f);
            const LLVector3 tip = strike.mOrigin
                + LLVector3(cosf(back) * reach, sinf(back) * reach, rng.frand(-150.f, 150.f));
            growPath(strike, -1, strike.mOrigin, tip, levels,
                     0.9f, 0.5f, 0.f, 1.f, true, rng, run_b);
        }
        growBranches(strike, run_a, depth, levels, intensity, rng);
        growBranches(strike, run_b, depth, levels, intensity * 0.85f, rng);
        return;
    }

    const LLVector3 tip = to_ground
        ? strike.mGround
        : strike.mOrigin + (strike.mGround - strike.mOrigin) * rng.frand(0.4f, 0.7f);

    std::vector<S32> trunk;
    growPath(strike, -1, strike.mOrigin, tip, levels,
             1.f, 0.6f, 0.f, 1.f, true, rng, trunk);

    if (to_ground)
    {
        // Horizontal spread off the top, fork-style: a run out of the origin in each
        // direction, so a ground bolt carries the cloud-level channel a fork does instead
        // of standing as a bare vertical line.
        const S32 arm_levels = llmax(levels - 2, 3);
        const F32 base_bearing = rng.frand(0.f, F_TWO_PI);

        std::vector<std::vector<S32>> runs(2);
        for (S32 r = 0; r < 2; ++r)
        {
            const F32 run_bearing = base_bearing + (F32)r * F_PI + rng.frand(-0.5f, 0.5f);
            const F32 reach = rng.frand(400.f, 1300.f);
            const LLVector3 run_tip = strike.mOrigin
                + LLVector3(cosf(run_bearing) * reach, sinf(run_bearing) * reach, rng.frand(-120.f, 120.f));
            growPath(strike, -1, strike.mOrigin, run_tip, arm_levels,
                     0.85f, 0.3f, 0.f, 1.f, false, rng, runs[(size_t)r]);
        }
        for (S32 r = 0; r < 2; ++r)
        {
            growBranches(strike, runs[(size_t)r], depth, arm_levels, intensity * 0.85f, rng, 2.f);
        }
    }

    growBranches(strike, trunk, depth, levels, intensity, rng);
}

// Runs one strike through its phases: charge, leader descent, summed return strokes, flash decay, retirement.
void SSLightning::advance(SSStrike& strike, F32 dt)
{
    strike.mT = (F32)(SSAtmoMagic::getInstance()->sharedTime() - strike.mFireAt);

    SSRandStream rng((U32)(strike.mFireAt * 3571.0) ^ 0x11feu);

    const F32 leader_s = rng.frand(LEADER_MIN_S, LEADER_MAX_S);
    const S32 strokes = RETURN_STROKES_MIN + rng.rand(RETURN_STROKES_MAX - RETURN_STROKES_MIN + 1);

    const F32 charge_s = SSAtmoMagic::getInstance()->lightningCharge()
        ? llclamp(settingF("SSAtmoLightningAnticipation", ANTICIPATION_DEFAULT_S), 0.f, ANTICIPATION_MAX_S)
        : 0.f;
    if (charge_s > 0.f && strike.mT < -leader_s)
    {
        const F32 until = -leader_s - strike.mT;
        strike.mCharge = (until < charge_s) ? (1.f - until / charge_s) : 0.f;

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
        strike.mLeaderProgress = 0.f;
    }
    else if (strike.mT < 0.f)
    {
        strike.mLeaderProgress = 1.f - (-strike.mT / leader_s);
        brightness = (strike.mKind == STRIKE_SHEET) ? 0.f : LEADER_GLOW;

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

        F32 at = 0.f;
        for (S32 i = 0; i < strokes; ++i)
        {
            if (strike.mT >= at)
            {
                const F32 since = strike.mT - at;
                const F32 scale = 1.f / (1.f + (F32)i * 0.6f);
                const F32 glow = scale * expf(-since / STROKE_DECAY_S);
                brightness += glow;

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
            brightness = 0.f;
            strike.mDone = true;
        }
    }

    strike.mChannelBrightness = llclamp(brightness, 0.f, 1.f);

    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugStrikeMarkers", false);
    if (markers && strike.mT <= -MARKER_HIDE_S && !strike.mDone)
    {
        if (!strike.mDebugText)
        {
            strike.mDebugText = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            if (strike.mDebugText)
            {
                strike.mDebugText->setDoFade(false);
                strike.mDebugText->setZCompare(false);
                strike.mDebugText->setColor(LLColor4(1.f, 0.25f, 0.9f, 1.f));
            }
        }
        if (strike.mDebugText)
        {
            // Ground strikes are labelled at the attachment; sheet and fork live in the sky, so float the countdown at their centre instead - sheet at the flash origin, fork at the channel's centroid.
            LLVector3 label_pos = strike.mGround + LLVector3(0.f, 0.f, 4.f);
            if (strike.mKind == STRIKE_SHEET)
            {
                label_pos = strike.mOrigin;
            }
            else if (strike.mKind == STRIKE_FORK && !strike.mChannel.empty())
            {
                LLVector3 centre;
                for (const SSStrikeNode& node : strike.mChannel) centre += node.mPos;
                label_pos = centre / (F32)strike.mChannel.size();
            }
            strike.mDebugText->setPositionAgent(label_pos);
            strike.mDebugText->setColor(kindDebugColor(strike.mKind));
            strike.mDebugText->setString(llformat("%s strike in %.1fs%s",
                kindName(strike.mKind), -strike.mT,
                strike.mCharge > 0.f ? llformat("  charge %.0f%%", strike.mCharge * 100.f).c_str() : ""));
        }
    }
    else
    {
        ss_kill_strike_text(strike);
    }

    const F32 falloff = 1.f / (1.f + (strike.mDistanceM / 4000.f) * (strike.mDistanceM / 4000.f));
    strike.mFlash = llclamp(brightness, 0.f, 1.f) * strike.mIntensity * falloff;

}
