/**
 * @file ssprecipitation.cpp
 * @brief Atmo Magic precipitation simulation implementation.
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

#include "ssprecipitation.h"
#include "ssprecipvariants.h"
#include "ssrainshadow.h"
#include "sswindflow.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llrand.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"

#include <algorithm>

// <SS:Nexii> Atmo Magic precipitation simulation

static const U64 MAX_CATCHUP_TICKS = 4;
static const F32 VIS_BAND = 75.f;           // skip drops entirely above/below the camera by this much
static const F32 IMPACT_QUEUE_RADIUS = 32.f;
static const F32 MAX_FRAME_DT = 0.2f;
// Ring and crown are separate entries, and both live for well under a second, so this is landings per second times their life times two. It has to clear what the impact queue can now deliver or the
// ring buffer simply overwrites live splashes with newer ones and the count sits pinned at the cap.
static const size_t RIPPLE_CAP = 4096;
static const F32 MAX_SPAWN_DRIFT = 12.f;    // cap on upwind spawn offset for impacting types
static const U32 GROUND_CHECK_SLICES = 8;   // frames a full ground re-check is spread over

// Travel budget for a non-impacting particle, as a fall distance beyond its own spawn column. These are retired by the floor they are actually over, so the budget is only a backstop, and a mean one
// is what made snow fade out overhead. The base covers the drop from a roof to the street below it; the wind term covers how much ground a flake crosses on the way down, since the harder it is
// pushed the further it can travel over surfaces that have nothing to do with the column it grew from.
static const F32 IMPACT_OVERSHOOT = 2.f;        // metres past the predicted landing
static const F32 DRIFT_FALL_SLACK = 40.f;       // metres, before wind
static const F32 DRIFT_SLACK_PER_WIND = 6.f;    // extra metres per m/s of wind
static const F32 DRIFT_MAX_AGE = 90.f;          // absolute ceiling, seconds

// A particle sitting this far under the surface resolved above it is not landing on anything - it is indoors, or under a bridge or overhang
static const F32 COVER_TOLERANCE = 2.f;

// How quickly the measured air time follows the population it is measuring
static const F32 LIFE_EMA = 0.02f;

static LLTrace::BlockTimerStatHandle FTM_SS_SIM("Atmo Magic Sim");
static LLTrace::BlockTimerStatHandle FTM_SS_SIM_INTEGRATE("Integrate");
static LLTrace::BlockTimerStatHandle FTM_SS_SIM_SPAWN("Spawn");

// Spawn grid per tier: cell size, shared ticks per second, and the fraction of the type's base drop rate each spawned particle stands in for
struct SSTierSpec
{
    F32 mCell;
    F64 mHz;
    F32 mRateScale;
    F32 mCapShare;      // fraction of the total particle budget this tier may hold
};
static const SSTierSpec TIER_SPEC[TIER_COUNT] = {
    {  8.f, 8.0, 1.f,    0.61f }, // TIER_DROPS
    { 16.f, 4.0, 0.14f,  0.31f }, // TIER_CLUSTERS
    { 32.f, 2.0, 0.004f, 0.08f }, // TIER_SHEETS
};

// SSAtmoParticleBudget is the total number of precipitation particles allowed in the air, split between the tiers by the shares above. It used to be a multiplier on three separate hard caps, which
// meant the number it controlled was never written down anywhere the user could see.
static S32 tierCap(SSPrecipTier tier)
{
    static LLCachedControl<U32> budget(gSavedSettings, "SSAtmoParticleBudget", 40000);
    const S32 total = (S32)llclamp((U32)budget, 500u, 200000u);
    return llmax(16, (S32)((F32)total * TIER_SPEC[tier].mCapShare));
}

// LOD handoff radii come from the preset, scaled by the global LOD sliders relative to the rain-family reference distances. Horizontal wind at a point. With the flowmap solved this is the local
// field, so drops funnel down alleys, lean around windward faces and go slack in the lee of a building; without it, the single ambient vector as before.
static LLVector3 windAt(const LLVector3& pos_agent)
{
    static LLCachedControl<bool> advect(gSavedSettings, "SSAtmoWindFlowAdvect", true);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!advect || !SSWindFlowMap::getInstance()->isValid())
    {
        return atmo->windXY();
    }

    const LLVector3 v = SSWindFlowMap::getInstance()->sample(pos_agent);
    return LLVector3(v.mV[VX], v.mV[VY], 0.f);
}

static void tierRadii(const SSPrecipPreset& preset, F32& r0, F32& r1, F32& r2)
{
    r0 = preset.mTiers[TIER_DROPS].mRadius;
    r1 = preset.mTiers[TIER_CLUSTERS].mRadius;
    r2 = preset.mTiers[TIER_SHEETS].mRadius;

    if (!preset.mTiers[TIER_DROPS].mEnabled) r0 = 0.f;
    if (!preset.mTiers[TIER_CLUSTERS].mEnabled) r1 = r0;
    if (!preset.mTiers[TIER_SHEETS].mEnabled) r2 = 0.f;

    static LLCachedControl<F32> lod_drops(gSavedSettings, "SSAtmoLodDrops", 28.f);
    static LLCachedControl<F32> lod_clusters(gSavedSettings, "SSAtmoLodClusters", 96.f);
    static LLCachedControl<F32> lod_sheets(gSavedSettings, "SSAtmoLodSheets", 224.f);
    r0 *= llclamp((F32)lod_drops, 8.f, 96.f) / 28.f;
    r1 *= llclamp((F32)lod_clusters, 24.f, 256.f) / 96.f;
    r2 *= llclamp((F32)lod_sheets, 64.f, 512.f) / 224.f;
    r1 = llmax(r1, r0 + 8.f);
    if (r2 > 0.f) r2 = llmax(r2, r1 + 16.f);
}

// How far above its landing point a particle materializes. Distant sheets span longer columns so the horizon stays busy.
static void fallLength(const SSPrecipPreset& preset, SSPrecipTier tier, F32& lo, F32& hi)
{
    lo = preset.mFallLo;
    hi = preset.mFallHi;
    if (tier == TIER_SHEETS)
    {
        lo *= 2.4f;
        hi *= 2.4f;
    }
}

// Glow, scaled by the global glow master
static F32 presetGlow(const SSPrecipPreset& preset)
{
    static LLCachedControl<F32> glow_scale(gSavedSettings, "SSAtmoGlowScale", 1.f);
    return llclamp(preset.mGlow, 0.f, 1.f) * llclamp((F32)glow_scale, 0.f, 3.f);
}

// The precipitation slider grows liquid drops as well as multiplying them, which is what carries one rain preset from a drizzle to a downpour.
static F32 intensitySizeScale(const SSPrecipPreset& preset, F32 precipitation)
{
    if (preset.mIntensitySize <= 0.f) return 1.f;
    const F32 scale = 0.55f + 0.9f * llclamp(precipitation, 0.f, 1.f);
    return lerp(1.f, scale, llclamp(preset.mIntensitySize, 0.f, 1.f));
}

// static
void SSPrecipSim::tierBands(SSPrecipTier tier, const SSPrecipPreset& preset,
                            F32& in_lo, F32& in_hi, F32& out_lo, F32& out_hi)
{
    F32 r0, r1, r2;
    tierRadii(preset, r0, r1, r2);
    // Proportional bands stay well-ordered whatever the LOD sliders say
    switch (tier)
    {
        case TIER_DROPS:
            in_lo = 0.f;         in_hi = 0.f;
            out_lo = r0 * 0.72f; out_hi = r0;
            break;
        case TIER_CLUSTERS:
            in_lo = r0 * 0.5f;   in_hi = r0 * 0.78f;
            out_lo = r1 * 0.84f; out_hi = r1;
            break;
        default:
            in_lo = r1 * 0.75f;  in_hi = r1 * 0.9f;
            out_lo = r2 * 0.9f;  out_hi = r2;
            break;
    }
}

SSPrecipSim::SSPrecipSim()
{
    for (S32 i = 0; i < TIER_COUNT; ++i)
    {
        mTierCount[i] = 0;
        mTierTarget[i] = 0.f;
        mMeanLife[i] = 0.f;
        mLastTick[i] = 0;
    }
}

LLViewerTexture* SSPrecipSim::texture(U8 index) const
{
    if (index < mTextures.size() && mTextures[index].notNull())
    {
        return mTextures[index];
    }
    return LLViewerFetchedTexture::sDefaultParticleImagep;
}

U8 SSPrecipSim::textureIndex(LLViewerTexture* texturep)
{
    if (!texturep)
    {
        texturep = LLViewerFetchedTexture::sDefaultParticleImagep;
    }
    for (size_t i = 0; i < mTextures.size(); ++i)
    {
        if (mTextures[i].get() == texturep) return (U8)i;
    }
    if (mTextures.size() >= SS_PRECIP_MAX_TEXTURES)
    {
        // Returning slot 0 here silently aliased every new texture onto the first one ever registered, which is why clusters and sheets drew a single drop stretched across their quad and a preset
        // switch kept the previous preset's art. Start the table over instead; live particles have to go with it because their indices point into it.
        resetTextureTable();
    }
    mTextures.push_back(texturep);
    return (U8)(mTextures.size() - 1);
}

void SSPrecipSim::resetTextureTable()
{
    mParticles.clear();
    mRipples.clear();
    mStreams.clear();
    mTextures.clear();
    mRippleCursor = 0;
    mDripCount = 0;
    for (S32 i = 0; i < TIER_COUNT; ++i)
    {
        mTierCount[i] = 0;
        mTierTarget[i] = 0.f;
        mMeanLife[i] = 0.f;
    }
}

void SSPrecipSim::clear()
{
    mParticles.clear();
    mRipples.clear();
    mStreams.clear();
    mTextures.clear();
    mRippleCursor = 0;
    mDripCount = 0;
    for (S32 i = 0; i < TIER_COUNT; ++i)
    {
        mTierCount[i] = 0;
        mTierTarget[i] = 0.f;
        mLastTick[i] = 0;
    }
}

void SSPrecipSim::shift(const LLVector3& offset)
{
    for (SSPrecipParticle& p : mParticles) p.mPos += offset;
    for (SSPrecipParticle& p : mRipples) p.mPos += offset;
    for (SSPrecipParticle& p : mStreams) p.mPos += offset;
}

// Streams are anchored, so nothing integrates: they age, they scroll their texture, and they go when their eave stops asking for them.
void SSPrecipSim::updateStreams(F32 dt)
{
    for (size_t i = 0; i < mStreams.size(); )
    {
        SSPrecipParticle& s = mStreams[i];
        s.mAge += dt;

        if (s.mAge >= s.mMaxAge)
        {
            // Erased in place rather than swapped with the back: the list is held in key order so a stream can be found by binary search, and there are only ever a handful of these a frame
            mStreams.erase(mStreams.begin() + (S32)i);
            continue;
        }

        // Scroll the texture down the stream at the speed the water is running. The rate comes from the repeat count the stream carries, because that is what decides how far one unit of phase moves
        // a feature; working it out separately here had the water crawling on a short fall and racing on a long one.
        s.mPhase += dt * ssStreamScroll(llmax(0.05f, s.mFloorZ), s.mPlaneD);

        // The phase only ever enters the texture coordinate as an offset, and the art tiles, so a whole repeat can come off it without the stream moving. A gutter that runs for an hour would
        // otherwise be scrolling a number too big to hold the fraction that matters.
        if (s.mPhase > 1.f) s.mPhase -= 1.f;

        ++i;
    }
}

void SSPrecipSim::update(F32 dt)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM);

    dt = llclamp(dt, 0.f, MAX_FRAME_DT);

    updateStreams(dt);

    {
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM_INTEGRATE);

    // Ground re-checks are spread over several frames: one slice per frame keeps the shadow map lookups bounded no matter how many are in the air
    static U32 sSlicePhase = 0;
    const U32 slice_phase = sSlicePhase++ % GROUND_CHECK_SLICES;
    const bool slice_check = !SSAtmoMagic::getInstance()->preset().makesImpacts();
    const bool sky_track = SSAtmoMagic::getInstance()->isSkyTrack();

    // Replace particles the moment they land rather than waiting for the next spawn tick, so rain stays continuous instead of arriving in pulses at the tier's tick rate. Collected here and emitted
    // after the loop: emitParticle pushes to the same vector being walked.
    static LLCachedControl<bool> respawn_setting(gSavedSettings, "SSAtmoRespawnOnImpact", true);
    const F32 respawn_env = SSAtmoMagic::getInstance()->gustEnvelopeAt(
        SSAtmoMagic::getInstance()->sharedTime());
    struct Respawn { SSPrecipTier mTier; U32 mSeed; LLVector3 mPos; };
    std::vector<Respawn> respawns;

    // Distance past which a particle is nobody's business any more. The spawner re-centres its annulus on the camera every tick, so it does aim ahead - but nothing ever retired what it left behind.
    // Those keep their full lifetime, keep counting toward the tier population, and the spawner's own easing reads that count as "full" and stops emitting. Walk across a region and the budget stays
    // spent on snow behind you while the cells in front go hungry, which reads as the fall thinning out ahead and never catching up. Snow shows it worst because its travel budget lets it live for
    // the better part of a minute.
    const LLVector3 cull_cam = LLViewerCamera::getInstance()->getOrigin();
    F32 cull_r2[TIER_COUNT];
    {
        const SSPrecipPreset& cull_preset = SSAtmoMagic::getInstance()->preset();
        for (S32 t = 0; t < TIER_COUNT; ++t)
        {
            F32 in_lo, in_hi, out_lo, out_hi;
            tierBands((SSPrecipTier)t, cull_preset, in_lo, in_hi, out_lo, out_hi);

            // Beyond the spawn annulus by a couple of cells. The tier has already faded to nothing out there, so this frees budget without taking anything off the screen.
            const F32 r = out_hi + TIER_SPEC[t].mCell * 2.f;
            cull_r2[t] = r * r;
        }
    }

    // Integrate; dead particles swap-pop out (draw order is rebuilt per frame by the renderer, so order here doesn't matter)
    for (size_t i = 0; i < mParticles.size();)
    {
        SSPrecipParticle& p = mParticles[i];
        p.mAge += dt;

        {
            const F32 dx = p.mPos.mV[VX] - cull_cam.mV[VX];
            const F32 dy = p.mPos.mV[VY] - cull_cam.mV[VY];
            if (dx * dx + dy * dy > cull_r2[p.mTier])
            {
                // Left behind rather than landed. It never finished its fall, so it must not feed the mean life the population target is derived from, and it must not trigger a respawn either: the
                // spawner will fill the cells that are still in range, and respawning here would put the particle back out of sight.
                --mTierCount[p.mTier];
                p = mParticles.back();
                mParticles.pop_back();
                continue;
            }
        }

        if (p.mAge >= p.mMaxAge)
        {
            // What the particle actually managed to stay up for, which the population target is derived from. A non-impacting type is retired against whatever ground it has drifted over rather than
            // the column it grew from, so over a city it stays up well past the fall the preset describes.
            mMeanLife[p.mTier] = (mMeanLife[p.mTier] <= 0.f)
                               ? p.mAge : lerp(mMeanLife[p.mTier], p.mAge, LIFE_EMA);

            // Reaching full age is the landing. Top the tier back up to what the current rate sustains, and no further: replacing one for one regardless would freeze the population at whatever it
            // happened to reach and leave the density controls doing nothing.
            if (respawn_setting && respawn_env > 0.f &&
                (F32)mTierCount[p.mTier] <= mTierTarget[p.mTier])
            {
                respawns.push_back({ (SSPrecipTier)p.mTier, p.mSeed, p.mPos });
            }

            --mTierCount[p.mTier];
            p = mParticles.back();
            mParticles.pop_back();
            continue;
        }
        // Non-impacting types carry a travel budget rather than a landing time, so wind and sway can take them over a roof and straight down through it. Re-resolve the column for a rotating slice of
        // the population each frame and settle anything that has reached the surface it is now over.
        if ((slice_check && (i % GROUND_CHECK_SLICES) == slice_phase) &&
            !(p.mFlags & PART_LANDED) && p.mVel.mV[VZ] < 0.f)
        {
            LLVector3 hit;
            bool on_water = false;
            const bool found = SSRainShadowMap::getInstance()->resolveColumn(p.mPos, hit, on_water);

            if (sky_track && !found)
            {
                // Open air under a sky band: nothing to land on, so it runs its budget out and fades where it is
                p.mFloorZ = -FLT_MAX;
            }
            else if (hit.mV[VZ] - p.mPos.mV[VZ] > COVER_TOLERANCE)
            {
                // Well under the surface over it: it has drifted in through a doorway, under a bridge, or the map has only just learned about the building it is standing in. Nothing it could be
                // landing on, so drop it rather than leave snow hanging in a room.
                --mTierCount[p.mTier];
                p = mParticles.back();
                mParticles.pop_back();
                continue;
            }

            else
            {
                // This is the surface it is going to land on, tested every frame below. Re-resolving is what lets a drifting particle follow the ground down a cliff or up onto a roof.
                p.mFloorZ = hit.mV[VZ];
            }
        }

        if (!(p.mFlags & PART_LANDED) && p.mPos.mV[VZ] <= p.mFloorZ)
        {
            // Landed. Leave it exactly its fade-out window rather than dropping it on the spot, so it settles onto the surface instead of blinking off, and it goes through the normal end-of-life
            // path that tops the tier back up. It stops where it stopped: carrying on down would take it through the roof it just met and into the room underneath.
            p.mMaxAge = llmin(p.mMaxAge, p.mAge + ssPrecipFadeOut(p.mTier));
            p.mFlags |= PART_LANDED;
        }

        // A settled particle keeps its velocity, which is what orients a streak, but neither drifts nor sways while it fades
        if (!(p.mFlags & PART_LANDED))
        {
            if (p.mFlags & (PART_SWAY | PART_GUSTY))
            {
                const F32 amp = (p.mFlags & PART_GUSTY) ? 2.2f : 0.6f;
                p.mVel.mV[VX] += cosf(p.mAge * 1.4f + p.mPhase) * amp * dt;
                p.mVel.mV[VY] += sinf(p.mAge * 1.1f + p.mPhase * 1.7f) * amp * dt;
                if (p.mFlags & PART_GUSTY)
                {
                    p.mVel.mV[VZ] += sinf(p.mAge * 2.7f + p.mPhase) * 0.8f * dt;
                }
            }
            p.mPos += p.mVel * dt;
        }
        ++i;
    }

    for (const Respawn& r : respawns)
    {
        respawnParticle(r.mTier, r.mSeed, r.mPos, respawn_env);
    }

    for (size_t i = 0; i < mRipples.size();)
    {
        SSPrecipParticle& p = mRipples[i];
        p.mAge += dt;
        if (p.mAge >= p.mMaxAge)
        {
            if (p.mFlags & PART_DRIP) --mDripCount;
            if (mRippleCursor == mRipples.size() - 1) mRippleCursor = i;
            p = mRipples.back();
            mRipples.pop_back();
            continue;
        }
        if (p.mKind == KIND_ROUND || p.mKind == KIND_STREAK)
        {
            // Splash crowns and mana shards are ballistic: thrown off the surface, then pulled back down by gravity
            p.mVel.mV[VZ] -= 9.81f * dt;
        }
        p.mPos += p.mVel * dt;

        // Shards were flying straight through the surface they broke on once gravity pulled them back. Retire them at the impact plane instead.
        if (p.mPlaneD > -FLT_MAX && (p.mPos * p.mNormal) < p.mPlaneD)
        {
            if (p.mFlags & PART_DRIP) --mDripCount;
            p = mRipples.back();
            mRipples.pop_back();
            continue;
        }
        ++i;
    }
    if (mRippleCursor >= mRipples.size()) mRippleCursor = 0;
    }

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->hasWeather())
    {
        for (S32 i = 0; i < TIER_COUNT; ++i) mLastTick[i] = 0;
        return;
    }

    // Each tier advances on its own shared-clock cadence
    {
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM_SPAWN);
    for (S32 tier = 0; tier < TIER_COUNT; ++tier)
    {
        const U64 tick = (U64)(atmo->sharedTime() * TIER_SPEC[tier].mHz);
        U64& last = mLastTick[tier];
        if (last == 0 || tick < last || tick - last > MAX_CATCHUP_TICKS)
        {
            last = tick > 0 ? tick - 1 : 0;
        }
        while (last < tick)
        {
            ++last;
            spawnTier((SSPrecipTier)tier, last, (F64)last / TIER_SPEC[tier].mHz);
        }
    }
    }
}

void SSPrecipSim::spawnTier(SSPrecipTier tier, U64 tick, F64 tick_time)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();

    // A disabled tier or a gust lull has no target, which is what stops respawn-on-impact from keeping a stopped shower alive
    if (!preset.mTiers[tier].mEnabled) { mTierTarget[tier] = 0.f; return; }

    const F32 env = atmo->gustEnvelopeAt(tick_time);
    if (env <= 0.f) { mTierTarget[tier] = 0.f; return; }

    mTierSpawnAccum[tier] = 0.f;

    // Spawn annulus: the tier's fade band plus one cell of margin
    F32 in_lo, in_hi, out_lo, out_hi;
    tierBands(tier, preset, in_lo, in_hi, out_lo, out_hi);
    const F32 cell = TIER_SPEC[tier].mCell;
    const F32 r_min = llmax(0.f, in_lo - cell);
    const F32 r_max = out_hi + cell;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3d cam_global = gAgent.getPosGlobalFromAgent(cam);
    const LLVector3d agent_origin_global = cam_global - LLVector3d(cam);

    const S32 c0x = (S32)floor((cam_global.mdV[VX] - r_max) / cell);
    const S32 c1x = (S32)floor((cam_global.mdV[VX] + r_max) / cell);
    const S32 c0y = (S32)floor((cam_global.mdV[VY] - r_max) / cell);
    const S32 c1y = (S32)floor((cam_global.mdV[VY] + r_max) / cell);

    const F32 half_diag = cell * 0.7072f;
    const F64 lo = llmax(0.f, r_min - half_diag);
    const F64 hi = r_max + half_diag;

    // Rows are walked from a tick-dependent offset. Cells are visited in a fixed spatial order, so anything that runs out of budget part way through starves whichever cells come last - which showed
    // up as snow only falling on one side of the camera. Rotating the start spreads any residual shortfall around instead of always hitting the same side.
    const S32 span_y = c1y - c0y + 1;
    const S32 row_offset = (span_y > 0) ? (S32)(tick % (U64)span_y) : 0;

    for (S32 j = 0; j < span_y; ++j)
    {
        const S32 cy = c0y + (j + row_offset) % span_y;
        for (S32 cx = c0x; cx <= c1x; ++cx)
        {
            const F64 center_x = (cx + 0.5) * cell;
            const F64 center_y = (cy + 0.5) * cell;
            const F64 dx = center_x - cam_global.mdV[VX];
            const F64 dy = center_y - cam_global.mdV[VY];
            const F64 d2 = dx * dx + dy * dy;
            if (d2 > hi * hi || d2 < lo * lo) continue;

            spawnTierCell(tier, tick, tick_time, cx, cy, env, cam, agent_origin_global);
        }
    }

    // Population this rate sustains: spawns per second times how long one particle lives. Both the spawn easing and respawn-on-impact aim at it, so they hold the same level between them instead of
    // stacking.
    F32 fall_lo, fall_hi;
    fallLength(preset, tier, fall_lo, fall_hi);
    const F32 nominal_life = preset.risesFromGround()
        ? 2.25f      // risers get a flat lifetime, not a fall distance
        : ((fall_lo + fall_hi) * 0.5f) / llmax(0.1f, preset.mFallSpeed);

    // The fall the preset describes is only the nominal case: over rooftops a drifting flake keeps falling to the street below, and a target derived from the nominal fall alone would throttle the
    // spawner exactly where the snow has furthest to come. Use what the population is actually achieving, bounded so an odd run cannot run the target away.
    const F32 mean_life = (mMeanLife[tier] > 0.f)
        ? llclamp(mMeanLife[tier], nominal_life * 0.5f, nominal_life * 8.f)
        : nominal_life;

    mTierTarget[tier] = llmin(mTierSpawnAccum[tier] * (F32)TIER_SPEC[tier].mHz * mean_life,
                              (F32)tierCap(tier));
}

void SSPrecipSim::spawnTierCell(SSPrecipTier tier, U64 tick, F64 tick_time, S32 cx, S32 cy, F32 env,
                                const LLVector3& cam_agent, const LLVector3d& agent_origin_global)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    const SSTierSpec& spec = TIER_SPEC[tier];

    // One deterministic stream per (seed, tier, tick, cell); identical wherever this cell is in view
    SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
        SSAtmoNoise::combine(0x71E5A17Cu ^ (U32)(tier + 1) * 0x9E3779B9u,
        SSAtmoNoise::combine((U32)(tick & 0xffffffffu),
        SSAtmoNoise::combine((U32)cx, (U32)cy * 0x27d4eb2fu)))));
    rng.next();

    static LLCachedControl<F32> density(gSavedSettings, "SSAtmoDensity", 1.f);

    // Ease the spawn rate off as the tier fills so the budget is shared evenly by every cell. Hitting the cap instead would simply cut off whichever cells are visited last. The target is what the
    // current rate sustains, and the cap is only a ceiling on it.
    const S32 cap = tierCap(tier);
    const F32 target = llmin(mTierTarget[tier] > 1.f ? mTierTarget[tier] : (F32)cap, (F32)cap);
    const F32 fill = (F32)mTierCount[tier] / llmax(1.f, target);
    const F32 headroom = (fill < 0.7f) ? 1.f : llmax(0.f, (1.f - fill) / 0.3f);

    const F32 area_factor = atmo->areaFactorAt((cx + 0.5) * spec.mCell, (cy + 0.5) * spec.mCell);
    const F32 p = powf(atmo->precipitation(), 1.4f);

    // Unthrottled rate first: the target has to be derived from what the weather is asking for, not from what the easing let through, or the two chase each other down to nothing
    const F32 mean_full = preset.mRate * spec.mRateScale * p * area_factor * env
                          * llclamp((F32)density, 0.1f, 3.f)
                          * spec.mCell * spec.mCell / (F32)spec.mHz;
    mTierSpawnAccum[tier] += mean_full;

    // Ray anchor height must be shared between clients: the region's water height is, camera height is not. Cells over the void beyond region borders anchor to the void water surface so rain doesn't
    // cut off at the sim edge.
    const F32 cell_agent_x = (F32)((F64)cx * spec.mCell - agent_origin_global.mdV[VX]);
    const F32 cell_agent_y = (F32)((F64)cy * spec.mCell - agent_origin_global.mdV[VY]);

    // Whether landings from this cell could be seen or heard at all. The impact schedule is the only source of ripples and impact sounds, and it has to run at the rate the weather is actually asking
    // for - not at whatever rate the local particle population happens to leave room for. Those are not the same thing, and the difference is not small. Respawn on impact replaces each drop as it
    // lands and stops exactly at the population target, which parks the easing at zero headroom more or less permanently. The tier stays full of drops, the spawner stops being asked for new ones,
    // and the impacts stop with it: rain everywhere and nothing landing. So the eased rate governs how many particles are emitted, and the full rate governs how many landings are scheduled.
    const F32 impact_reach = IMPACT_QUEUE_RADIUS + spec.mCell * 1.5f;
    const F32 cell_dx = cell_agent_x + spec.mCell * 0.5f - cam_agent.mV[VX];
    const F32 cell_dy = cell_agent_y + spec.mCell * 0.5f - cam_agent.mV[VY];
    const bool impacts_here = (tier == TIER_DROPS) && preset.makesImpacts()
                            && (cell_dx * cell_dx + cell_dy * cell_dy) < impact_reach * impact_reach;

    if (headroom <= 0.f && !impacts_here) return;

    const F32 mean = impacts_here ? mean_full : (mean_full * headroom);

    S32 count = (S32)mean;
    if (rng.frand() < mean - (F32)count) ++count;
    if (count <= 0) return;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(
        LLVector3(cell_agent_x, cell_agent_y, cam_agent.mV[VZ]));

    // In a sky band the track's own ground zero is the anchor: it is derived from the region altitudes and the shared config, so it stays identical across clients the way the water height does at
    // ground level.
    const bool sky = atmo->isSkyTrack();
    const F32 fall_through = atmo->fallThrough();
    const F32 anchor_z = sky ? atmo->groundZero()
                             : (regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight());

    F32 fall_lo, fall_hi;
    fallLength(preset, tier, fall_lo, fall_hi);
    const F32 v_fall = preset.mFallSpeed;
    const bool rises = preset.risesFromGround();

    for (S32 i = 0; i < count; ++i)
    {
        // Fixed draw order, before any camera- or load-dependent skip; every client consumes the same values whether or not it ends up spawning the particle locally
        const F32 ox = rng.frand(0.f, spec.mCell);
        const F32 oy = rng.frand(0.f, spec.mCell);
        const F32 fall_len = rng.frand(fall_lo, fall_hi); // vertical meters
        const F32 strength_jitter = rng.frand(0.7f, 1.f);
        const F32 size_jitter = rng.frand(0.75f, 1.25f);
        const F32 phase = rng.frand(0.f, F_TWO_PI);
        const F32 riser_age = rng.frand(1.5f, 3.f);
        const F32 gust_jitter = rng.frand(0.f, 1.f);
        const F32 platform_roll = rng.frand(0.f, 1.f);
        const U32 vis_seed = rng.next();

        const LLVector3 anchor(cell_agent_x + ox, cell_agent_y + oy, anchor_z);
        LLVector3 hit;
        LLVector3 normal;
        bool on_water = false;
        const bool found_surface =
            SSRainShadowMap::getInstance()->resolveColumn(anchor, hit, on_water, &normal);

        // A sky band is mostly empty air. Where a column found no real surface there is nothing to catch the drop, so it falls through the track floor and fades instead of landing on an imaginary
        // plane. Only a fraction are drawn at all, otherwise open sky reads as solid rain going nowhere.
        const bool no_platform = sky && !found_surface;
        if (no_platform && platform_roll > fall_through) continue;

        // Shared impact schedule, from the individual-drop tier only;
        // ripples/sounds fire on time even when the drop itself is culled
        if (tier == TIER_DROPS && !no_platform)
        {
            const F32 strength = preset.mImpactStrength;
            if (strength > 0.f && (hit - cam_agent).magVec() < IMPACT_QUEUE_RADIUS)
            {
                // Landing velocity, reconstructed the same way emitParticle builds it, wind response included, so shards fly off at the speed that actually hit
                const LLVector3 wind_h = windAt(hit) * (0.55f + 0.45f * llclamp(env, 0.f, 2.5f))
                                       * llmax(0.f, preset.mWindResponse);
                const LLVector3 impact_vel(wind_h.mV[VX], wind_h.mV[VY], -v_fall);
                atmo->queueImpact(tick_time + fall_len / v_fall, hit, strength * strength_jitter,
                                  on_water, normal, impact_vel, preset.mShatter);
            }
        }

        // Emission is throttled where the schedule is not. Rolled locally and deliberately not from the shared stream: how full this client's tier happens to be is a local matter, and drawing it
        // from the shared stream would push every other client's draws out of step.
        if (headroom <= 0.f) continue;
        if (headroom < 1.f && ll_frand() > headroom) continue;

        // Locally invisible: landing far overhead or materializing far below
        const F32 spawn_z = rises ? hit.mV[VZ] : hit.mV[VZ] + fall_len;
        const F32 band = VIS_BAND + (tier == TIER_SHEETS ? fall_hi : 0.f);
        if (hit.mV[VZ] - cam_agent.mV[VZ] > band) continue;
        if (spawn_z - cam_agent.mV[VZ] < -band) continue;

        if (mTierCount[tier] >= cap) continue; // hard backstop; the easing above normally keeps us clear

        emitParticle(tier, hit, fall_len, env, size_jitter, phase, riser_age, gust_jitter, vis_seed,
                     found_surface || !sky);
    }
}

void SSPrecipSim::emitParticle(SSPrecipTier tier, const LLVector3& hit_pos, F32 fall_len, F32 gust,
                               F32 size_jitter, F32 phase, F32 riser_age, F32 gust_jitter, U32 vis_seed,
                               bool has_floor)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    if (!preset.mTiers[tier].mEnabled) return;
    const SSPrecipTierParams& visual = preset.mTiers[tier];

    // Forked stream: differing local asset lists draw from here without touching the shared cell stream
    SSRandStream vis(vis_seed);

    LLColor4 tint;
    F32 pbr_glow = 0.f;
    LLViewerTexture* custom = atmo->pickParticleTexture(vis, tint, pbr_glow);
    const U32 variant = (U32)vis.rand((S32)SSPrecipVariants::VARIANT_COUNT);

    // Individual drops draw the configured art directly; cluster and sheet tiers always go through the splatter variants, baked from the custom texture when one is configured, procedural otherwise
    LLViewerTexture* texturep;
    if (tier == TIER_DROPS && custom)
    {
        texturep = custom;
    }
    else
    {
        texturep = SSPrecipVariants::getInstance()->get(preset, tier, variant, custom);
    }

    const F32 v_fall = preset.mFallSpeed;
    const bool rises = preset.risesFromGround();

    SSPrecipParticle part;
    part.mSeed = vis_seed;
    part.mTier = (U8)tier;
    part.mKind = visual.mKind;
    part.mFlags = (preset.mSway >= 1.5f) ? PART_GUSTY : (preset.mSway > 0.f ? PART_SWAY : 0);
    part.mPhase = phase;
    const F32 intensity = intensitySizeScale(preset, atmo->precipitation());
    part.mSizeX = visual.mSizeX * size_jitter * intensity;
    part.mSizeY = visual.mSizeY * size_jitter * intensity;
    part.mAlpha = visual.mAlpha;
    part.mGlow = llmax(presetGlow(preset), pbr_glow);

    // The preset tint modulates whatever the asset supplied, rather than replacing it, so PBR factors still show through
    tint.mV[0] *= preset.mTint.mV[0];
    tint.mV[1] *= preset.mTint.mV[1];
    tint.mV[2] *= preset.mTint.mV[2];
    part.mTex = textureIndex(texturep);
    part.mMaterial = preset.material();

    // Vertical speed is the type's terminal velocity; horizontal speed rides the wind, surging with the gust envelope so bursts also lean. The preset's wind response scales how much of that wind
    // this type actually takes, so a heavy drop can shrug off the same gale that carries a flake away without touching the wind itself. It is the same factor the fall direction is tilted by, so the
    // column the map resolves and the path the particle takes stay the same path. Sampled at the MIDDLE of the column this particle falls down, not at its landing point. The flow solve shelters the
    // air near the ground - correctly, that is what buildings and terrain do to wind - so a cell at the foot of the column can be nearly still while the air the particle spends most of its fall in
    // is at full ambient speed. Drops fall a few metres and barely notice; a sheet spans a hundred metres of air (fallLength multiplies the column by 2.4 for that tier) and took its entire lean from
    // the one sheltered cell it happened to land in - which is why distant sheets fell vertically through a gale while the drops overhead leaned properly. Risers keep sampling at ground level: an
    // ember leaving a fire really is in the sheltered air at the bottom of its own column.
    const LLVector3 wind_pos = rises
        ? hit_pos
        : hit_pos + LLVector3(0.f, 0.f, fall_len * 0.5f);

    // The far tiers take the wind EXAGGERATED. The lean angle is atan(wind / fall speed), and against rain's 9.5 m/s fall an SL-scale gale of six or seven only buys ~30 degrees - which on an
    // 18x36m curtain seen at two hundred metres reads as rain falling dead straight through a storm. A sheet is an abstraction of a thousand drops, not a drop, so it is allowed to lean the way
    // the weather FEELS rather than the way one drop's vector sum says; the near drops keep the honest physics because they are close enough for it to read.
    const F32 tier_lean = (tier == TIER_SHEETS) ? 1.9f
                        : (tier == TIER_CLUSTERS) ? 1.35f : 1.f;

    // Sheets take the flow solve DILUTED toward ambient: the solve shelters building wakes - correctly, for drops falling into a courtyard - but a curtain spanning a hundred metres of air is
    // mostly ABOVE that shelter, and taking its whole lean from the one sheltered cell it lands in is why sheets near a big building fell dead vertical through a gale (the marker debug showed
    // exactly this: angled markers everywhere, vertical ones in the cathedral's wake).
    LLVector3 wind_sample = windAt(wind_pos);
    if (tier == TIER_SHEETS)
    {
        const LLVector3 ambient = SSAtmoMagic::getInstance()->windXY();
        wind_sample = wind_sample * 0.35f + ambient * 0.65f;
    }

    const LLVector3 wind_h = wind_sample
        * (0.55f + 0.45f * llclamp(gust, 0.f, 2.5f))
        * (0.8f + 0.4f * gust_jitter)
        * llmax(0.f, preset.mWindResponse)
        * tier_lean;

    if (rises)
    {
        part.mPos = hit_pos;
        // Risers used to take a flat 15% of the wind. That fraction is what the response slider is for now, and the ember preset carries 0.15, so it drifts exactly as it did while any other riser
        // gets a dial.
        part.mVel = LLVector3(wind_h.mV[VX], wind_h.mV[VY], v_fall);
        part.mMaxAge = riser_age;
    }
    else
    {
        F32 fall_time = fall_len / llmax(0.1f, v_fall);
        part.mVel = LLVector3(wind_h.mV[VX], wind_h.mV[VY], -v_fall);

        if (preset.makesImpacts())
        {
            // Impacting types are back-projected up their slanted path so the drop still lands exactly on the resolved hit. Bound how far upwind that can put them: a long path in strong wind would
            // push the spawn outside the tier's own radius. Bounded per TIER, because the clamp works by truncating the whole path: a flat 12m cap fit the drop tier's radius and quietly amputated
            // every windy sheet - a curtain's six-second, sixty-metre column drifts far more than 12m in any real wind, so the gale that should slant the sheets was instead cutting them down to
            // second-long stubs spawning barely off the ground.
            const F32 max_drift = (tier == TIER_SHEETS) ? 120.f
                                : (tier == TIER_CLUSTERS) ? 36.f : MAX_SPAWN_DRIFT;
            const F32 drift = wind_h.magVec() * fall_time;
            if (drift > max_drift)
            {
                fall_time *= max_drift / drift;
            }
            part.mPos = hit_pos - part.mVel * fall_time;

            // The surface it was aimed at, so the landing test can retire it there. Without this an impacting type has no floor at all and simply expires in mid-air at the end of its predicted fall.
            part.mFloorZ = hit_pos.mV[VZ];

            // Back-projection makes it arrive exactly as its age runs out, and the age is tested before the step that would have landed it - so the frame that should put it on the ground is the
            // frame that removes it, a frame's travel short of the surface. At hail speeds that gap is most of a metre. Carry it far enough past the arrival to actually cross the floor; the landing
            // test then retires it on contact, so this is a margin rather than a longer life. Added after the back-projection on purpose: the spawn point still has to be the one the shared impact
            // schedule predicts, or the sound and the splash part company with the drop.
            fall_time += IMPACT_OVERSHOOT / llmax(0.1f, v_fall);
        }
        else
        {
            // Non-impacting types don't need to land anywhere exact, so instead of back-projecting the whole path they start half a drift upwind: the midpoint of the trajectory sits over the cell
            // and the field stays centred on the camera. Full back-projection threw these tens of meters upwind - at snow's 1.4 m/s over a 6 s fall that offset outruns the 24 m spawn radius, so
            // every flake ended up on one side of the camera.
            LLVector3 lead = wind_h * (fall_time * 0.5f);
            const F32 lead_len = lead.magVec();
            if (lead_len > MAX_SPAWN_DRIFT * 0.5f)
            {
                lead *= (MAX_SPAWN_DRIFT * 0.5f) / lead_len;
            }
            part.mPos = hit_pos - lead + LLVector3(0.f, 0.f, fall_len);

            // Sized to the spawn height alone, a flake expired at the altitude of the column it grew from. Over a rooftop that is a storey above the ground the camera is standing on, so snow
            // crossing a roof faded out in mid-air on the way down. Give it enough age to carry on well past its own column, scaled by how hard the wind is pushing it, and let the floor it is
            // actually over retire it.
            const F32 slack = DRIFT_FALL_SLACK + wind_h.magVec() * DRIFT_SLACK_PER_WIND;
            fall_time = (fall_len + slack) / llmax(0.1f, v_fall);

            // The floor it is heading for, re-resolved as it drifts. A sky band column with nothing under it has no floor to land on.
            if (has_floor) part.mFloorZ = hit_pos.mV[VZ];
        }
        part.mMaxAge = llclamp(fall_time, 0.2f, preset.makesImpacts() ? 25.f : DRIFT_MAX_AGE);
    }

    if (preset.risesFromGround() && tier != TIER_SHEETS &&
        (preset.mDarkMix > 0.f || preset.mPuffMix > 0.f))
    {
        applyEmberFlavor(part, tint, vis, preset);
    }

    part.mTint.setVec((U8)llclamp((S32)(tint.mV[0] * 255.f), 0, 255),
                      (U8)llclamp((S32)(tint.mV[1] * 255.f), 0, 255),
                      (U8)llclamp((S32)(tint.mV[2] * 255.f), 0, 255), 255);

    mParticles.push_back(part);
    ++mTierCount[tier];
}

void SSPrecipSim::respawnParticle(SSPrecipTier tier, U32 seed, const LLVector3& impact_pos, F32 env)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    const SSTierSpec& spec = TIER_SPEC[tier];

    if (mTierCount[tier] >= tierCap(tier)) return;

    // Chained off the dying particle's own stream, so this is reproducible: any client holding that particle grows the same replacement from it.
    SSRandStream rng(SSAtmoNoise::combine(seed, 0x5F3A21C7u));
    rng.next();

    // A fresh column within a cell of where the last one landed, so successive drops do not retrace the same line down
    const F32 ox = rng.frand(-spec.mCell, spec.mCell);
    const F32 oy = rng.frand(-spec.mCell, spec.mCell);

    F32 fall_lo, fall_hi;
    fallLength(preset, tier, fall_lo, fall_hi);
    const F32 fall_len = rng.frand(fall_lo, fall_hi);
    const F32 strength_jitter = rng.frand(0.7f, 1.f);
    const F32 size_jitter = rng.frand(0.75f, 1.25f);
    const F32 phase = rng.frand(0.f, F_TWO_PI);
    const F32 riser_age = rng.frand(1.5f, 3.f);
    const F32 gust_jitter = rng.frand(0.f, 1.f);
    const U32 vis_seed = rng.next();

    const LLVector3 anchor(impact_pos.mV[VX] + ox, impact_pos.mV[VY] + oy, impact_pos.mV[VZ]);
    LLVector3 hit;
    LLVector3 normal;
    bool on_water = false;
    const bool found_surface =
        SSRainShadowMap::getInstance()->resolveColumn(anchor, hit, on_water, &normal);

    // Same rule as the tick spawner: in a sky band a column with nothing under it has no drop to draw
    if (atmo->isSkyTrack() && !found_surface) return;

    // Schedule this one's landing too. This used to be left out on the grounds that the impact schedule is built in the shared cell stream, and a locally grown replacement adding to it would put a
    // splash on one screen and not the next. That reasoning does not survive contact with the numbers: respawn supplies most of the drop population, so leaving it out meant most of the rain you can
    // see landed silently and without a ripple, which is a far larger discrepancy than the one it was avoiding. It is also less inconsistent than it looks. The replacement is grown from the dying
    // particle's own stream, so any client holding that particle grows the same one and now schedules the same landing. A client that never had the parent already had neither the drop nor its
    // splash. Tying the splash to the drop is what makes those two agree.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (tier == TIER_DROPS && preset.makesImpacts()
        && (hit - cam).magVec() < IMPACT_QUEUE_RADIUS)
    {
        // Landing velocity reconstructed exactly as the tick spawner does it, wind response included, so shards fly off at the speed that hit
        const LLVector3 wind_h = windAt(hit) * (0.55f + 0.45f * llclamp(env, 0.f, 2.5f))
                               * llmax(0.f, preset.mWindResponse);
        const LLVector3 impact_vel(wind_h.mV[VX], wind_h.mV[VY], -preset.mFallSpeed);

        atmo->queueImpact(atmo->sharedTime() + fall_len / llmax(0.1f, preset.mFallSpeed),
                          hit, preset.mImpactStrength * strength_jitter,
                          on_water, normal, impact_vel, preset.mShatter);
    }

    emitParticle(tier, hit, fall_len, env, size_jitter, phase, riser_age, gust_jitter, vis_seed,
                 found_surface || !atmo->isSkyTrack());
}

void SSPrecipSim::pushRipple(const SSPrecipParticle& part)
{
    if (mRipples.size() < RIPPLE_CAP)
    {
        mRipples.push_back(part);
    }
    else
    {
        // The slot being recycled may be holding a drip, which is counted
        if (mRipples[mRippleCursor].mFlags & PART_DRIP) --mDripCount;
        mRipples[mRippleCursor] = part;
        mRippleCursor = (mRippleCursor + 1) % RIPPLE_CAP;
    }
    if (part.mFlags & PART_DRIP) ++mDripCount;
}

void SSPrecipSim::spawnRipple(const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, SSRandStream& rng)
{
    LLVector3 n = on_water ? LLVector3(0.f, 0.f, 1.f) : normal;
    if (n.normVec() < 0.5f)
    {
        n.set(0.f, 0.f, 1.f);
    }

    // Spreading rings only read as rings on ground the water could actually pool on: full strength up to a 20 degree tilt, faded out by 30, and gone on anything steeper. Crowns below are left
    // ungated - a splash throws itself off a slope or a wall just as happily.
    static const F32 RING_TILT_FULL = 0.9397f;   // cos(20 degrees)
    static const F32 RING_TILT_NONE = 0.8660f;   // cos(30 degrees)
    const F32 horiz = llclamp((n.mV[VZ] - RING_TILT_NONE) / (RING_TILT_FULL - RING_TILT_NONE), 0.f, 1.f);
    const F32 ring_gate = horiz * horiz * (3.f - 2.f * horiz);

    LLViewerTexture* ripple_tex = SSAtmoMagic::getInstance()->rippleTexture();
    if (!ripple_tex)
    {
        ripple_tex = SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_RING);
    }

    static LLCachedControl<F32> ripple_scale_setting(gSavedSettings, "SSAtmoRippleScale", 1.f);
    static LLCachedControl<F32> ripple_speed_setting(gSavedSettings, "SSAtmoRippleSpeed", 2.f);
    const F32 ripple_scale = llclamp((F32)ripple_scale_setting, 0.25f, 3.f);
    const F32 ripple_speed = llclamp((F32)ripple_speed_setting, 0.5f, 5.f);

    // The preset gives the splash its shape; the two settings above stay a taste knob on top of whatever the preset asked for
    const SSPrecipPreset& preset = SSAtmoMagic::getInstance()->preset();

    // Water takes the ring wider and holds it longer than a hard surface
    const F32 water_spread = on_water ? 1.7f : 1.f;
    const F32 water_linger = on_water ? 1.55f : 1.f;

    if (ring_gate > 0.05f && preset.makesRipples())
    {
        // Surface-aligned expanding ring, lifted a hair along the normal. It opens from a point, so the start half-size is a fixed fraction of where it ends up rather than its own dial.
        const F32 ring_end = preset.mRippleSize * water_spread * strength * ripple_scale;
        SSPrecipParticle ring;
        ring.mKind = KIND_FLAT;
        ring.mMaterial = MAT_DECAL;  // lies on a surface, so it takes that
                                     // surface's light as well as the sky's
        ring.mPos = pos_agent + n * 0.02f;
        ring.mNormal = n;
        ring.mSizeX = ring_end * 0.15f;    // start half-size
        ring.mSizeY = ring_end;            // end half-size
        ring.mMaxAge = preset.mRippleLife * water_linger / ripple_speed;
        ring.mAlpha = preset.mRippleAlpha * strength * ring_gate;
        ring.mPhase = rng.frand(0.f, F_TWO_PI);
        ring.mTex = textureIndex(ripple_tex);
        pushRipple(ring);
    }

    if (!preset.makesCrowns()) return;

    // Small splash crown thrown along the surface normal: up off floors, outward off slopes and walls; ballistic under gravity from update()
    SSPrecipParticle crown;
    crown.mKind = KIND_ROUND;
    crown.mPos = pos_agent + n * 0.03f;
    crown.mVel = n * preset.mCrownSpeed * strength * ripple_scale * sqrtf(ripple_speed);
    crown.mSizeX = crown.mSizeY = preset.mCrownSize * strength * ripple_scale;
    crown.mFlags |= PART_CROWN;  // the renderer opens it from a point; the
                                 // size here is the one it is measured against
    crown.mMaxAge = preset.mCrownLife / ripple_speed;
    crown.mAlpha = preset.mCrownAlpha * strength;
    crown.mPhase = rng.frand(0.f, F_TWO_PI);
    crown.mTex = textureIndex(SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_DOT));
    pushRipple(crown);
}

// static
F32 SSPrecipSim::dropRateAt(const LLVector3& pos_agent)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->hasWeather()) return 0.f;

    const SSPrecipPreset& preset = atmo->preset();
    if (!preset.mTiers[TIER_DROPS].mEnabled) return 0.f;

    static LLCachedControl<F32> density(gSavedSettings, "SSAtmoDensity", 1.f);

    // Same expression spawnTierCell builds its per-cell mean from, minus the cell area and the tick rate: what is left is a plain rate per square metre per second, which is the only part a catchment
    // cares about.
    const LLVector3d global = gAgent.getPosGlobalFromAgent(pos_agent);
    const F32 area_factor = atmo->areaFactorAt(global.mdV[VX], global.mdV[VY]);
    const F32 p = powf(atmo->precipitation(), 1.4f);

    return preset.mRate * TIER_SPEC[TIER_DROPS].mRateScale * p * area_factor
         * atmo->gustEnvelopeAt(atmo->sharedTime())
         * llclamp((F32)density, 0.1f, 3.f);
}

// Seconds a stream keeps running after its eave stops asking for it. Long enough that a gust lull does not blink it out, short enough that walking out of a shower does not leave water hanging off
// the roof.
static const F32 STREAM_LINGER = 1.2f;

// Streams alive at once, across every eave in sight. Hundreds is the right number in a dense build - every gutter on every roof around you is running in a downpour, and that is what it should look
// like - so this only has to stop a whole region of bridge decks from running away with the frame. Eight quads each, and the list is kept in key order so finding one is a binary search rather than a
// walk. Runs are offered in descending catchment, so a cap that simply refuses new ones spends the budget on the biggest gutters.
static const size_t MAX_LIVE_STREAMS = 512;

// Which tier's art suits a stream this size. "Sheets or clusters" is the right instinct and neither answer holds on its own: rain's sheets are curtains 18 m by 36 m, drawn for a shower seen across a
// field, and its clusters are 0.76 by 3.8 - so a two metre fall off a cottage eave wants clusters and the twenty metre one off a bridge deck wants sheets. Rather than pick a tier and live with it,
// take whichever quad is closest in size to the water actually being drawn, measured as a ratio so being half the size counts the same as being twice it.
static SSPrecipTier streamTier(const SSPrecipPreset& preset, F32 span, F32 fall)
{
    SSPrecipTier best = TIER_DROPS;
    F32 best_miss = FLT_MAX;

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        const SSPrecipTierParams& tier = preset.mTiers[t];
        if (!tier.mEnabled) continue;

        const F32 quad_x = tier.mSizeX * 2.f;
        const F32 quad_y = tier.mSizeY * 2.f;
        if (quad_x < 0.001f || quad_y < 0.001f) continue;

        const F32 miss = fabsf(logf(llmax(span, 0.01f) / quad_x))
                       + fabsf(logf(llmax(fall, 0.01f) / quad_y));
        if (miss < best_miss)
        {
            best_miss = miss;
            best = (SSPrecipTier)t;
        }
    }

    return best;
}

// Gravity a stream falls under, and the speed it parts from the lip at. Water does not leave an edge at rest, and the renderer walks the same path from the same two numbers, so they live here rather
// than at each use.
static const F32 SS_STREAM_G  = 9.81f;
static const F32 SS_STREAM_V0 = 1.f;

// Seconds to fall a given height from the lip, ballistically. The inverse of the drawn length, which is what lets a length in metres be turned back into the time the path is walked over.
static F32 ssStreamFallTime(F32 height)
{
    return (sqrtf(SS_STREAM_V0 * SS_STREAM_V0 + 2.f * SS_STREAM_G * llmax(0.f, height))
            - SS_STREAM_V0) / SS_STREAM_G;
}

// How long a wind-bent stream may fall before it runs into something. A stream leaves the lip going outward and is then bent back by the wind, and on a building with any overhang at all that curve
// takes it back over the wall and draws a curtain of water down the inside of the front room. Rather than flatten the path - which is the one thing water in a gale does not do - the fall is ended
// before it arrives, and the tail fade the renderer already applies over the last third turns it into spray against the wall. The test is the same one the drips land by: the surface over a column is
// whatever the shadow map's depth capture saw first from above, so a sample that sits below it is inside the building rather than in the air beside it.
static F32 ssStreamClearTime(const LLVector3& start, const LLVector3& vel,
                             const LLVector3& drift, F32 fall_time)
{
    // Roof texels are decimetres across and the lip sits a few centimetres clear of one, so the surface at the start column is the roof itself and reads as marginally above the water leaving it.
    // Anything the stream genuinely runs into is a wall or a lower roof, metres out of that noise.
    const F32 MARGIN = 0.5f;
    const S32 SAMPLES = 8;

    SSRainShadowMap* shadow = SSRainShadowMap::getInstance();

    for (S32 k = 1; k <= SAMPLES; ++k)
    {
        const F32 t = fall_time * (F32)k / (F32)SAMPLES;
        const LLVector3 pos = start + vel * t + drift * (0.5f * t * t)
                            - LLVector3(0.f, 0.f, 0.5f * SS_STREAM_G * t * t);

        LLVector3 hit;
        bool on_water = false;
        if (!shadow->resolveColumn(pos, hit, on_water)) continue;   // no map here to be blocked by

        if (hit.mV[VZ] > pos.mV[VZ] + MARGIN)
        {
            // Ends at the last sample still in open air. Half a step of slack either way is nothing against a fade that runs over a third of the fall.
            return fall_time * (F32)(k - 1) / (F32)SAMPLES;
        }
    }

    return fall_time;
}

void SSPrecipSim::refreshStream(U32 key, const LLVector3& lip_agent, const LLVector3& out_dir,
                                const LLVector3& land_agent, F32 strength, F32 width,
                                F32 run_slope, SSRandStream& rng)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();

    const LLVector3 start = lip_agent + out_dir * 0.06f - LLVector3(0.f, 0.f, 0.04f);
    const F32 drop = start.mV[VZ] - land_agent.mV[VZ];
    if (drop < 0.5f) return;    // too short to read as a fall

    // How far the water is drawn falling, in metres, and never further than the ground it is falling towards: a long fall breaks into spray well before it arrives, so the preset says where to stop
    // and the tail fade carries it out, but a cottage eave two metres up still meets the street.
    const F32 length = llmin(drop, llclamp(preset.mStreamLength, 0.5f, SS_STREAM_LENGTH_MAX));

    // Ballistic, same as a drip: it leaves with a little downward speed and the flow still carrying it out over the edge. The drawn drop follows from the time rather than the other way about,
    // because the fall is not linear in either and the renderer walks it in time.
    const F32 g = SS_STREAM_G;
    const F32 v0 = SS_STREAM_V0;
    F32 fall_time = ssStreamFallTime(length);

    // Bent by the wind over its length, as an acceleration so the ribbon curves into it rather than leaving the lip already sideways. The LOCAL wind, from the flowmap, not the global vector: a
    // stream off a sheltered lee eave should fall near-straight while one on the windward face is torn sideways, and the flowmap is the thing that knows which is which.
    const LLVector3 drift = windAt(lip_agent) * llmax(0.f, preset.mStreamWind);
    const LLVector3 exit_vel = out_dir * llclamp(0.3f + strength * 0.5f, 0.25f, 1.2f)
                             - LLVector3(0.f, 0.f, v0);

    // Only worth asking where the water goes when it goes somewhere other than straight down. Within half a metre of the lip's own column the fall is the one the eave's landing point was already
    // resolved down, and there is nothing new to find for the cost of the lookups.
    const LLVector3 sideways = LLVector3(exit_vel.mV[VX], exit_vel.mV[VY], 0.f) * fall_time
                             + drift * (0.5f * fall_time * fall_time);
    if (sideways.magVecSquared() > 0.5f * 0.5f)
    {
        fall_time = ssStreamClearTime(start, exit_vel, drift, fall_time);
    }

    const F32 drawn = v0 * fall_time + 0.5f * g * fall_time * fall_time;
    if (drawn < 0.5f) return;   // whatever is in the way starts at the lip

    // Kept in key order, so this is a binary search rather than a walk over every running gutter in sight for every one of them
    auto slot = std::lower_bound(mStreams.begin(), mStreams.end(), key,
                                 [](const SSPrecipParticle& s, U32 k) { return s.mSeed < k; });
    SSPrecipParticle* existing = (slot != mStreams.end() && slot->mSeed == key)
                               ? &(*slot) : nullptr;

    if (!existing)
    {
        if (mStreams.size() >= MAX_LIVE_STREAMS) return;

        SSPrecipParticle s;
        s.mSeed = key;
        s.mKind = KIND_STREAM;
        s.mFlags = PART_STREAM;

        // Sheets fade in and out over the best part of a second, which is what a body of water this size wants; a drop's quarter second reads as a flicker on something several metres long.
        s.mTier = TIER_SHEETS;
        s.mAge = 0.f;

        // Where in the texture's cycle this one starts. Streams down one gutter are the same length and so run at the same speed, and a row of them started at zero scrolls in lockstep like a row of
        // copies of one animation - which is what they would be. The phase enters the texture coordinate as an offset and the art tiles, so a whole repeat's worth of offset covers every distinct
        // starting point there is. Taken from the key rather than off the shared draw sequence: it is a property of this stream, and hanging it off the order the draws happen to come in ties it to
        // code above it that has nothing to do with it.
        s.mPhase = SSAtmoNoise::hash01(key ^ 0x7A3C15E9u);

        // Same asset path the drops take, so the water off an eave is visibly the same water that was falling on the roof. A configured drop texture goes through the same bake rather than being used
        // whole: laid on directly it was one drop the size of the whole fall.
        LLColor4 tint;
        F32 pbr_glow = 0.f;
        LLViewerTexture* custom = atmo->pickParticleTexture(rng, tint, pbr_glow);
        SSPrecipVariants* variants = SSPrecipVariants::getInstance();

        // Which bake of the art this one wears, from the key for the same reason as the phase. Every stream in sight drawing the one variant is the difference between a wet roof and a row of
        // stencils, and the whole set is already baked for the tier the drops are using.
        const U32 variant = SSAtmoNoise::hashU32(key ^ 0x1F83D9ABu)
                          % SSPrecipVariants::VARIANT_COUNT;

        const SSPrecipTier art = streamTier(preset, width, drawn);
        LLViewerTexture* texturep = variants->get(preset, art, variant, custom);
        if (!texturep) texturep = variants->get(preset, TIER_DROPS, variant, custom);
        if (!texturep) texturep = custom;
        s.mArt = (U8)art;
        tint.mV[0] *= preset.mTint.mV[0];
        tint.mV[1] *= preset.mTint.mV[1];
        tint.mV[2] *= preset.mTint.mV[2];
        s.mTex = textureIndex(texturep);
        s.mTint.setVec((U8)llclamp((S32)(tint.mV[0] * 255.f), 0, 255),
                       (U8)llclamp((S32)(tint.mV[1] * 255.f), 0, 255),
                       (U8)llclamp((S32)(tint.mV[2] * 255.f), 0, 255),
                       255);
        s.mGlow = llmax(presetGlow(preset), pbr_glow);
        s.mMaterial = preset.material();

        existing = &(*mStreams.insert(slot, s));
    }

    SSPrecipParticle& s = *existing;
    const SSPrecipTier art = (SSPrecipTier)llclamp((S32)s.mArt, 0, (S32)TIER_COUNT - 1);

    // Which way the lip runs, so the curtain hangs along it. Level is the common case and steeper than a roof pitch is a run the trace has no business calling one edge, so it is held to something a
    // roof could be.
    s.mRunSlope = llclamp(run_slope, -4.f, 4.f);

    s.mPos = start;
    s.mVel = exit_vel;
    s.mPlaneD = fall_time;
    s.mNormal = drift;

    // The stream spans its slot of the lip. Widths vary a fraction per stream, keyed off the seed so one keeps its own gauge frame to frame: slots cut to exactly the same width butt together into
    // one flat wall of water, and a hand's breadth of daylight between them is what makes them read as separate falls off one gutter. A fraction is all it can be - a slot is a whole quad wide, and a
    // tenth off that is a metre of dry eave.
    const F32 gauge = 0.97f + 0.03f * SSAtmoNoise::hash01(s.mSeed ^ 0x2C1B3A5Du);
    const F32 span = llmax(0.1f, width * gauge);
    s.mSizeX = span * 0.5f;

    // Repeats across the stream and down the fall, so the art tiles at the size it was drawn for. Its tier's own quad is that size - the variants are baked with the drops in them scaled against it -
    // so one repeat covers one quad's worth of world either way, and the drops in a stream come out the size of the drops falling past it. Sizes are half extents, hence the doubling. Fractions below
    // one are the point, not an accident to be clamped away. A stream is usually smaller than the quad its art was drawn for, so it shows part of that art at true scale; forcing at least a whole
    // repeat squeezed the entire texture into the stream instead and shrank every drop in it by whatever the ratio happened to be - on rain, whose sheets are curtains 18 m by 36 m, by about ten.
    F32 tex_x = preset.mTiers[art].mSizeX * 2.f;
    F32 tex_y = preset.mTiers[art].mSizeY * 2.f;
    if (tex_x < 0.05f) tex_x = SS_STREAM_TEX_METRES;
    if (tex_y < 0.05f) tex_y = SS_STREAM_TEX_METRES;

    // The drops in that bake are not at life size: the far tiers floor their splats so a curtain seen across a field is not sub-texel, and on rain that makes them four and a half times too wide. A
    // stream is the one thing drawn from arm's length, so it tiles that much finer and puts them back where they belong. mStreamScale is the preset's own say over it on top.
    F32 fat_x = 1.f, fat_y = 1.f;
    SSPrecipVariants::getInstance()->splatInflation(preset, art, fat_x, fat_y);

    const F32 art_scale = llclamp(preset.mStreamScale, 0.1f, 4.f);
    tex_x = tex_x * art_scale / fat_x;
    tex_y = tex_y * art_scale / fat_y;

    s.mSizeY = llclamp(span / tex_x, 0.02f, 16.f);

    // Vertical repeats ride on the particle rather than being worked out on each side, because the scroll rate is derived from them and the two have to agree. mFloorZ is dead weight on a stream -
    // nothing here drifts down onto a surface - so it carries them.
    s.mFloorZ = llclamp(drawn / tex_y, 0.02f, 16.f);

    // Thinner than a strand of water would be, because there is a lot more of it: this covers metres of lip, and the density that read as a thread at that size is a wall of paint at this one. The
    // preset scales it, since how solid a fall of water looks is a property of the weather rather than of the roof.
    s.mAlpha = llclamp((0.2f + strength * 0.4f) * llmax(0.f, preset.mStreamAlpha), 0.f, 1.f);

    // Kept alive by being asked for. Stop asking and it fades.
    s.mMaxAge = s.mAge + STREAM_LINGER;
}

void SSPrecipSim::spawnDrip(const LLVector3& lip_agent, const LLVector3& out_dir,
                            const LLVector3& land_agent, F32 volume, SSRandStream& rng)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();

    // Where it actually comes down. The network's own answer is a cell centre resolved at capture time; re-resolving the column gives the exact point and the surface normal the splash needs, and it
    // is only a handful of lookups a second at drip rates.
    LLVector3 land = land_agent;
    LLVector3 normal(0.f, 0.f, 1.f);
    bool on_water = false;
    SSRainShadowMap::getInstance()->resolveColumn(land_agent + LLVector3(0.f, 0.f, 0.5f),
                                                  land, on_water, &normal);

    // Released just clear of the lip so it does not start inside the roof
    const LLVector3 start = lip_agent + out_dir * 0.06f - LLVector3(0.f, 0.f, 0.04f);

    const F32 drop = start.mV[VZ] - land.mV[VZ];
    if (drop < 0.35f) return;   // nothing worth watching fall

    // Ballistic from the lip: the flow only pushes it sideways, gravity does the rest. It leaves with a little downward speed already - water does not part from an edge at rest, and a streak is
    // oriented by its velocity, so starting purely sideways would draw the first frames lying flat.
    const F32 g = 9.81f;
    const F32 v0 = rng.frand(0.8f, 1.6f);
    const F32 fall_time = (sqrtf(v0 * v0 + 2.f * g * drop) - v0) / g;

    SSPrecipParticle p;
    p.mKind = KIND_STREAK;
    p.mFlags = PART_DRIP;
    p.mMaterial = preset.material();
    p.mPos = start;
    p.mVel = out_dir * rng.frand(0.25f, 0.7f) - LLVector3(0.f, 0.f, v0);

    // A drip is several drops' worth of collected water arriving as one, so it is fatter than a raindrop - by volume, not by count, or a big roof would shed boulders
    const F32 fat = llclamp(powf(llmax(1.f, volume), 1.f / 3.f), 1.f, 2.6f) * rng.frand(0.85f, 1.15f)
                  * llclamp(preset.mDripScale, 0.1f, 4.f);
    const SSPrecipTierParams& tier = preset.mTiers[TIER_DROPS];
    p.mSizeX = tier.mSizeX * fat;
    p.mSizeY = tier.mSizeY * fat;
    p.mAlpha = llmin(1.f, tier.mAlpha * 1.3f);
    p.mMaxAge = fall_time;
    p.mPhase = rng.frand(0.f, F_TWO_PI);
    p.mSeed = rng.next();

    // Same asset path the individual drops take, so a drip off the eave is visibly the same water that was falling on the roof
    LLColor4 tint;
    F32 pbr_glow = 0.f;
    LLViewerTexture* custom = atmo->pickParticleTexture(rng, tint, pbr_glow);
    LLViewerTexture* texturep = custom ? custom
        : SSPrecipVariants::getInstance()->get(preset, TIER_DROPS,
                                               (U32)rng.rand((S32)SSPrecipVariants::VARIANT_COUNT));
    tint.mV[0] *= preset.mTint.mV[0];
    tint.mV[1] *= preset.mTint.mV[1];
    tint.mV[2] *= preset.mTint.mV[2];
    p.mTex = textureIndex(texturep);
    p.mTint.setVec((U8)llclamp((S32)(tint.mV[0] * 255.f), 0, 255),
                   (U8)llclamp((S32)(tint.mV[1] * 255.f), 0, 255),
                   (U8)llclamp((S32)(tint.mV[2] * 255.f), 0, 255),
                   255);
    p.mGlow = llmax(presetGlow(preset), pbr_glow);

    pushRipple(p);

    // The street answers: same ripple, splash and ambient contribution any other landing gets, so a busy eave sounds like one
    if (preset.makesImpacts())
    {
        atmo->queueImpact(atmo->sharedTime() + fall_time, land,
                          preset.mImpactStrength * llclamp(volume * 0.25f, 0.6f, 1.4f),
                          on_water, normal, LLVector3(0.f, 0.f, -g * fall_time), preset.mShatter,
                          true);
    }
}

void SSPrecipSim::applyEmberFlavor(SSPrecipParticle& part, LLColor4& tint, SSRandStream& vis,
                                   const SSPrecipPreset& preset)
{
    const F32 dark = llclamp(preset.mDarkMix, 0.f, 1.f);
    const F32 puff = llclamp(preset.mPuffMix, 0.f, 1.f - dark);

    const F32 roll = vis.frand();
    if (roll < dark)
    {
        // Small dark sharp ember: burnt-out fleck, so it is alpha blended rather than additive - an additive dark sprite would be invisible
        part.mKind = KIND_STREAK;
        part.mMaterial = MAT_LIT;
        part.mGlow = 0.f;
        part.mSizeX *= vis.frand(0.35f, 0.55f);
        part.mSizeY *= vis.frand(1.1f, 1.9f);
        part.mAlpha = llmin(1.f, part.mAlpha * vis.frand(0.75f, 1.f));
        part.mVel *= vis.frand(1.05f, 1.35f);
        tint *= vis.frand(0.10f, 0.22f);   // near-black, keeps a hint of mana hue
        LLViewerTexture* dark_tex = SSAtmoMagic::textureFromList(preset.mDarkTexture);
        part.mTex = textureIndex(dark_tex ? dark_tex
            : SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_SHARD));
    }
    else if (roll < dark + puff)
    {
        // Large vague cloud: soft, dim and slow, drifting more than it rises
        part.mKind = KIND_ROUND;
        part.mMaterial = MAT_EMISSIVE;
        part.mGlow *= 0.35f;
        const F32 size = vis.frand(3.5f, 6.5f);
        part.mSizeX *= size;
        part.mSizeY *= size;
        part.mAlpha *= vis.frand(0.12f, 0.22f);
        part.mVel *= vis.frand(0.3f, 0.55f);
        part.mMaxAge *= vis.frand(1.3f, 1.9f);
        tint *= vis.frand(0.7f, 1.f);
        LLViewerTexture* puff_tex = SSAtmoMagic::textureFromList(preset.mPuffTexture);
        part.mTex = textureIndex(puff_tex ? puff_tex
            : SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_PUFF));
    }
    // otherwise the default glowing mote, unchanged
}


void SSPrecipSim::spawnShatter(const LLVector3& pos_agent, const LLVector3& normal,
                               const LLVector3& velocity, F32 strength, SSRandStream& rng)
{
    LLVector3 n = normal;
    if (n.normVec() < 0.5f)
    {
        n.set(0.f, 0.f, 1.f);
    }

    // Shards leave at the speed the hailstone arrived with
    const F32 speed = llmax(1.f, velocity.magVec());
    LLVector3 vel_dir = velocity;
    if (vel_dir.normVec() < 0.01f)
    {
        vel_dir.set(0.f, 0.f, -1.f);
    }

    // How square-on the hit was: 1 straight into the surface, 0 a graze
    const F32 incidence = llclamp(-(vel_dir * n), 0.f, 1.f);
    // How flat the surface is: 1 level ground, 0 a wall
    const F32 flatness = llclamp(n.mV[VZ], 0.f, 1.f);

    // Tangential direction the stone was already travelling, along the face
    LLVector3 slide = vel_dir - n * (vel_dir * n);
    if (slide.normVec() < 0.01f)
    {
        slide = n % LLVector3::z_axis;
        if (slide.normVec() < 0.01f) slide = LLVector3::x_axis;
    }

    // Downhill along the face, so a wall hit sprays down it rather than sideways
    LLVector3 down_face = LLVector3(0.f, 0.f, -1.f);
    down_face -= n * (down_face * n);
    if (down_face.normVec() < 0.01f)
    {
        down_face = slide;
    }

    // On a wall the burst runs down the face: square-on hits fan out widely around the downhill direction, while a graze keeps its momentum and stays a thin sliver along the travel direction.
    const LLVector3 wall_axis = lerp(slide, down_face, incidence);
    const F32 wall_inner = lerp(0.02f, 0.20f, incidence);
    const F32 wall_outer = lerp(0.10f, 0.85f, incidence);

    // On level ground the burst opens into a spherical cone running from a narrow spray near the normal all the way out to grazing the surface, like an angle-begin/angle-end cone pattern
    const F32 floor_inner = 0.30f;
    const F32 floor_outer = 1.48f;   // ~85 degrees: almost along the ground

    LLVector3 axis = lerp(wall_axis, n, flatness);
    if (axis.normVec() < 0.01f) axis = n;
    const F32 angle_begin = lerp(wall_inner, floor_inner, flatness);
    const F32 angle_end   = lerp(wall_outer, floor_outer, flatness);

    // Basis across the cone axis
    LLVector3 u = axis % LLVector3::z_axis;
    if (u.normVec() < 0.01f) u = axis % LLVector3::x_axis;
    u.normVec();
    LLVector3 v = axis % u;
    v.normVec();

    static LLCachedControl<F32> ripple_scale_setting(gSavedSettings, "SSAtmoRippleScale", 1.f);
    const F32 scale = llclamp((F32)ripple_scale_setting, 0.25f, 3.f);

    const SSPrecipPreset& preset = SSAtmoMagic::getInstance()->preset();
    const LLColor4 mana = preset.mTint;
    const U8 tex = textureIndex(SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_SHARD));

    const S32 count = 2 + rng.rand(6); // 2-7 shards
    for (S32 i = 0; i < count; ++i)
    {
        // Azimuth is fully random rather than evenly fanned, so successive impacts do not repeat the same star pattern
        const F32 phi = rng.frand(0.f, F_TWO_PI);
        // Uniform over the spherical band between the two angles: lerping the cosine keeps the density even instead of crowding the axis
        const F32 cos_theta = lerp(cosf(angle_begin), cosf(angle_end), rng.frand());
        const F32 theta = acosf(llclamp(cos_theta, -1.f, 1.f));

        LLVector3 dir = axis * cosf(theta) + (u * cosf(phi) + v * sinf(phi)) * sinf(theta);
        // Bias off the surface so shards clear the face they broke on
        dir += n * 0.10f;
        if (dir.normVec() < 0.01f) dir = axis;

        SSPrecipParticle shard;
        shard.mKind = KIND_STREAK;
        shard.mMaterial = MAT_EMISSIVE;
        shard.mPos = pos_agent + n * 0.03f;
        shard.mNormal = n;
        shard.mPlaneD = (shard.mPos * n) - 0.05f;
        // Ejection speed spreads widely: some shards barely tumble clear, others keep most of the impact energy
        shard.mVel = dir * speed * rng.frand(0.25f, 1.05f);
        shard.mSizeX = 0.018f * scale;
        shard.mSizeY = rng.frand(0.05f, 0.11f) * scale;
        shard.mMaxAge = rng.frand(0.35f, 0.7f);
        shard.mAlpha = 0.9f * llmin(1.f, strength);
        shard.mGlow = presetGlow(preset);
        shard.mPhase = rng.frand(0.f, F_TWO_PI);
        shard.mTint.setVec((U8)llclamp((S32)(mana.mV[0] * 255.f), 0, 255),
                           (U8)llclamp((S32)(mana.mV[1] * 255.f), 0, 255),
                           (U8)llclamp((S32)(mana.mV[2] * 255.f), 0, 255), 255);
        shard.mTex = tex;
        pushRipple(shard);
    }
}

// static
bool SSPrecipSim::tierSprite(const SSPrecipPreset& preset, SSPrecipTier tier,
                             F32& quad_x, F32& quad_y, F32& drop_x, F32& drop_y, S32& splats)
{
    if (!preset.mTiers[tier].mEnabled) return false;

    quad_x = preset.mTiers[tier].mSizeX;
    quad_y = preset.mTiers[tier].mSizeY;
    drop_x = preset.mTiers[TIER_DROPS].mSizeX;
    drop_y = preset.mTiers[TIER_DROPS].mSizeY;

    // A cluster or sheet paints the drops it stands in for into its own quad, so the drop scale reads there. The near tier is one drop filling its quad, and shrinking that inside its own sprite
    // would just blur it.
    if (tier != TIER_DROPS)
    {
        const F32 drop_scale = llmax(0.f, preset.mDropScale);
        drop_x *= drop_scale;
        drop_y *= drop_scale;
    }

    // How many individual drops one particle of this tier stands in for. Returned uncapped: the texture builder converts it into a splat count and per-splat size that preserve coverage.
    splats = (tier == TIER_DROPS) ? 1
           : llmax(2, (S32)(1.f / TIER_SPEC[tier].mRateScale + 0.5f));
    return true;
}

// </SS:Nexii>
