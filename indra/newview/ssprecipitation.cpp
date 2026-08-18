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

#include "llagent.h"
#include "llfasttimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"

// <SS:Nexii> Atmo Magic precipitation simulation

static const U64 MAX_CATCHUP_TICKS = 4;
static const F32 VIS_BAND = 75.f;           // skip drops entirely above/below the camera by this much
static const F32 IMPACT_QUEUE_RADIUS = 32.f;
static const F32 MAX_FRAME_DT = 0.2f;
static const size_t RIPPLE_CAP = 1024;
static const F32 MAX_SPAWN_DRIFT = 12.f;    // cap on upwind spawn offset for impacting types
static const U32 GROUND_CHECK_SLICES = 8;   // frames a full ground re-check is spread over

static LLTrace::BlockTimerStatHandle FTM_SS_SIM("Atmo Magic Sim");
static LLTrace::BlockTimerStatHandle FTM_SS_SIM_INTEGRATE("Integrate");
static LLTrace::BlockTimerStatHandle FTM_SS_SIM_SPAWN("Spawn");

// Spawn grid per tier: cell size, shared ticks per second, and the fraction
// of the type's base drop rate each spawned particle stands in for
struct SSTierSpec
{
    F32 mCell;
    F64 mHz;
    F32 mRateScale;
    S32 mCap;
};
static const SSTierSpec TIER_SPEC[TIER_COUNT] = {
    {  8.f, 8.0, 1.f,   24000 }, // TIER_DROPS
    { 16.f, 4.0, 0.14f, 12000 }, // TIER_CLUSTERS
    { 32.f, 2.0, 0.004f, 3200 }, // TIER_SHEETS
};

// LOD handoff radii come from the preset, scaled by the global LOD sliders
// relative to the rain-family reference distances.
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

// How far above its landing point a particle materializes. Distant sheets
// span longer columns so the horizon stays busy.
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

// The precipitation slider grows liquid drops as well as multiplying them,
// which is what carries one rain preset from a drizzle to a downpour.
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
        // Returning slot 0 here silently aliased every new texture onto the
        // first one ever registered, which is why clusters and sheets drew a
        // single drop stretched across their quad and a preset switch kept
        // the previous preset's art. Start the table over instead; live
        // particles have to go with it because their indices point into it.
        resetTextureTable();
    }
    mTextures.push_back(texturep);
    return (U8)(mTextures.size() - 1);
}

void SSPrecipSim::resetTextureTable()
{
    mParticles.clear();
    mRipples.clear();
    mTextures.clear();
    mRippleCursor = 0;
    for (S32 i = 0; i < TIER_COUNT; ++i)
    {
        mTierCount[i] = 0;
    }
}

void SSPrecipSim::clear()
{
    mParticles.clear();
    mRipples.clear();
    mTextures.clear();
    mRippleCursor = 0;
    for (S32 i = 0; i < TIER_COUNT; ++i)
    {
        mTierCount[i] = 0;
        mLastTick[i] = 0;
    }
}

void SSPrecipSim::shift(const LLVector3& offset)
{
    for (SSPrecipParticle& p : mParticles) p.mPos += offset;
    for (SSPrecipParticle& p : mRipples) p.mPos += offset;
}

void SSPrecipSim::update(F32 dt)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM);

    dt = llclamp(dt, 0.f, MAX_FRAME_DT);

    {
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM_INTEGRATE);

    // Ground re-checks are spread over several frames: one slice per frame
    // keeps the shadow map lookups bounded no matter how many are in the air
    static U32 sSlicePhase = 0;
    const U32 slice_phase = sSlicePhase++ % GROUND_CHECK_SLICES;
    const bool slice_check = !SSAtmoMagic::getInstance()->preset().makesImpacts();

    // Integrate; dead particles swap-pop out (draw order is rebuilt per
    // frame by the renderer, so order here doesn't matter)
    for (size_t i = 0; i < mParticles.size();)
    {
        SSPrecipParticle& p = mParticles[i];
        p.mAge += dt;
        if (p.mAge >= p.mMaxAge)
        {
            --mTierCount[p.mTier];
            p = mParticles.back();
            mParticles.pop_back();
            continue;
        }
        // Non-impacting types are only given a lifetime, so wind and sway can
        // carry them over a roof and straight down through it. Re-resolve the
        // column for a rotating slice of the population each frame and retire
        // anything that has sunk below the surface it is now over.
        if ((slice_check && (i % GROUND_CHECK_SLICES) == slice_phase) && p.mVel.mV[VZ] < 0.f)
        {
            LLVector3 hit;
            bool on_water = false;
            SSRainShadowMap::getInstance()->resolveColumn(p.mPos, hit, on_water);
            if (p.mPos.mV[VZ] < hit.mV[VZ])
            {
                --mTierCount[p.mTier];
                p = mParticles.back();
                mParticles.pop_back();
                continue;
            }
        }

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
        ++i;
    }

    for (size_t i = 0; i < mRipples.size();)
    {
        SSPrecipParticle& p = mRipples[i];
        p.mAge += dt;
        if (p.mAge >= p.mMaxAge)
        {
            if (mRippleCursor == mRipples.size() - 1) mRippleCursor = i;
            p = mRipples.back();
            mRipples.pop_back();
            continue;
        }
        if (p.mKind == KIND_ROUND || p.mKind == KIND_STREAK)
        {
            // Splash crowns and mana shards are ballistic: thrown off the
            // surface, then pulled back down by gravity
            p.mVel.mV[VZ] -= 9.81f * dt;
        }
        p.mPos += p.mVel * dt;

        // Shards were flying straight through the surface they broke on once
        // gravity pulled them back. Retire them at the impact plane instead.
        if (p.mPlaneD > -FLT_MAX && (p.mPos * p.mNormal) < p.mPlaneD)
        {
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
    if (!preset.mTiers[tier].mEnabled) return;

    const F32 env = atmo->gustEnvelopeAt(tick_time);
    if (env <= 0.f) return;

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

    // Rows are walked from a tick-dependent offset. Cells are visited in a
    // fixed spatial order, so anything that runs out of budget part way
    // through starves whichever cells come last - which showed up as snow
    // only falling on one side of the camera. Rotating the start spreads
    // any residual shortfall around instead of always hitting the same side.
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
}

void SSPrecipSim::spawnTierCell(SSPrecipTier tier, U64 tick, F64 tick_time, S32 cx, S32 cy, F32 env,
                                const LLVector3& cam_agent, const LLVector3d& agent_origin_global)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    const SSTierSpec& spec = TIER_SPEC[tier];

    // One deterministic stream per (seed, tier, tick, cell); identical
    // wherever this cell is in view
    SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
        SSAtmoNoise::combine(0x71E5A17Cu ^ (U32)(tier + 1) * 0x9E3779B9u,
        SSAtmoNoise::combine((U32)(tick & 0xffffffffu),
        SSAtmoNoise::combine((U32)cx, (U32)cy * 0x27d4eb2fu)))));
    rng.next();

    static LLCachedControl<F32> density(gSavedSettings, "SSAtmoDensity", 1.f);
    static LLCachedControl<F32> budget_setting(gSavedSettings, "SSAtmoParticleBudget", 1.f);

    // Ease the spawn rate off as the tier fills so the budget is shared
    // evenly by every cell. Hitting the hard cap instead would simply cut
    // off whichever cells are visited last.
    const S32 cap = (S32)(spec.mCap * llclamp((F32)budget_setting, 0.25f, 2.f));
    const F32 fill = (F32)mTierCount[tier] / (F32)llmax(1, cap);
    const F32 headroom = (fill < 0.7f) ? 1.f : llmax(0.f, (1.f - fill) / 0.3f);
    if (headroom <= 0.f) return;

    const F32 area_factor = atmo->areaFactorAt((cx + 0.5) * spec.mCell, (cy + 0.5) * spec.mCell);
    const F32 p = powf(atmo->precipitation(), 1.4f);
    const F32 mean = preset.mRate * spec.mRateScale * p * area_factor * env * headroom
                     * llclamp((F32)density, 0.1f, 3.f)
                     * spec.mCell * spec.mCell / (F32)spec.mHz;

    S32 count = (S32)mean;
    if (rng.frand() < mean - (F32)count) ++count;
    if (count <= 0) return;

    // Ray anchor height must be shared between clients: the region's water
    // height is, camera height is not. Cells over the void beyond region
    // borders anchor to the void water surface so rain doesn't cut off at
    // the sim edge.
    const F32 cell_agent_x = (F32)((F64)cx * spec.mCell - agent_origin_global.mdV[VX]);
    const F32 cell_agent_y = (F32)((F64)cy * spec.mCell - agent_origin_global.mdV[VY]);
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(
        LLVector3(cell_agent_x, cell_agent_y, cam_agent.mV[VZ]));
    const F32 anchor_z = regionp ? regionp->getWaterHeight() : SSAtmoMagic::voidWaterHeight();

    F32 fall_lo, fall_hi;
    fallLength(preset, tier, fall_lo, fall_hi);
    const F32 v_fall = preset.mFallSpeed;
    const bool rises = preset.risesFromGround();

    for (S32 i = 0; i < count; ++i)
    {
        // Fixed draw order, before any camera- or load-dependent skip; every
        // client consumes the same values whether or not it ends up spawning
        // the particle locally
        const F32 ox = rng.frand(0.f, spec.mCell);
        const F32 oy = rng.frand(0.f, spec.mCell);
        const F32 fall_len = rng.frand(fall_lo, fall_hi); // vertical meters
        const F32 strength_jitter = rng.frand(0.7f, 1.f);
        const F32 size_jitter = rng.frand(0.75f, 1.25f);
        const F32 phase = rng.frand(0.f, F_TWO_PI);
        const F32 riser_age = rng.frand(1.5f, 3.f);
        const F32 gust_jitter = rng.frand(0.f, 1.f);
        const U32 vis_seed = rng.next();

        const LLVector3 anchor(cell_agent_x + ox, cell_agent_y + oy, anchor_z);
        LLVector3 hit;
        LLVector3 normal;
        bool on_water = false;
        SSRainShadowMap::getInstance()->resolveColumn(anchor, hit, on_water, &normal);

        // Shared impact schedule, from the individual-drop tier only;
        // ripples/sounds fire on time even when the drop itself is culled
        if (tier == TIER_DROPS)
        {
            const F32 strength = preset.mImpactStrength;
            if (strength > 0.f && (hit - cam_agent).magVec() < IMPACT_QUEUE_RADIUS)
            {
                // Landing velocity, reconstructed the same way emitParticle
                // builds it, so shards fly off at the speed that actually hit
                const LLVector3 wind_h = atmo->windXY() * (0.55f + 0.45f * llclamp(env, 0.f, 2.5f));
                const LLVector3 impact_vel(wind_h.mV[VX], wind_h.mV[VY], -v_fall);
                atmo->queueImpact(tick_time + fall_len / v_fall, hit, strength * strength_jitter,
                                  on_water, normal, impact_vel, preset.mShatter);
            }
        }

        // Locally invisible: landing far overhead or materializing far below
        const F32 spawn_z = rises ? hit.mV[VZ] : hit.mV[VZ] + fall_len;
        const F32 band = VIS_BAND + (tier == TIER_SHEETS ? fall_hi : 0.f);
        if (hit.mV[VZ] - cam_agent.mV[VZ] > band) continue;
        if (spawn_z - cam_agent.mV[VZ] < -band) continue;

        if (mTierCount[tier] >= cap) continue; // hard backstop; the easing above normally keeps us clear

        emitParticle(tier, hit, fall_len, env, size_jitter, phase, riser_age, gust_jitter, vis_seed);
    }
}

void SSPrecipSim::emitParticle(SSPrecipTier tier, const LLVector3& hit_pos, F32 fall_len, F32 gust,
                               F32 size_jitter, F32 phase, F32 riser_age, F32 gust_jitter, U32 vis_seed)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    if (!preset.mTiers[tier].mEnabled) return;
    const SSPrecipTierParams& visual = preset.mTiers[tier];

    // Forked stream: differing local asset lists draw from here without
    // touching the shared cell stream
    SSRandStream vis(vis_seed);

    LLColor4 tint;
    F32 pbr_glow = 0.f;
    LLViewerTexture* custom = atmo->pickParticleTexture(vis, tint, pbr_glow);
    const U32 variant = (U32)vis.rand((S32)SSPrecipVariants::VARIANT_COUNT);

    // Individual drops draw the configured art directly; cluster and sheet
    // tiers always go through the splatter variants, baked from the custom
    // texture when one is configured, procedural otherwise
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
    part.mTier = (U8)tier;
    part.mKind = visual.mKind;
    part.mFlags = (preset.mSway >= 1.5f) ? PART_GUSTY : (preset.mSway > 0.f ? PART_SWAY : 0);
    part.mPhase = phase;
    const F32 intensity = intensitySizeScale(preset, atmo->precipitation());
    part.mSizeX = visual.mSizeX * size_jitter * intensity;
    part.mSizeY = visual.mSizeY * size_jitter * intensity;
    part.mAlpha = visual.mAlpha;
    part.mGlow = llmax(presetGlow(preset), pbr_glow);

    // The preset tint modulates whatever the asset supplied, rather than
    // replacing it, so PBR factors still show through
    tint.mV[0] *= preset.mTint.mV[0];
    tint.mV[1] *= preset.mTint.mV[1];
    tint.mV[2] *= preset.mTint.mV[2];
    part.mTex = textureIndex(texturep);
    part.mMaterial = preset.material();

    // Vertical speed is the type's terminal velocity; horizontal speed
    // rides the wind, surging with the gust envelope so bursts also lean
    const LLVector3 wind_h = atmo->windXY()
        * (0.55f + 0.45f * llclamp(gust, 0.f, 2.5f))
        * (0.8f + 0.4f * gust_jitter);

    if (rises)
    {
        part.mPos = hit_pos;
        part.mVel = LLVector3(wind_h.mV[VX] * 0.15f, wind_h.mV[VY] * 0.15f, v_fall);
        part.mMaxAge = riser_age;
    }
    else
    {
        F32 fall_time = fall_len / llmax(0.1f, v_fall);
        part.mVel = LLVector3(wind_h.mV[VX], wind_h.mV[VY], -v_fall);

        if (preset.makesImpacts())
        {
            // Impacting types are back-projected up their slanted path so the
            // drop still lands exactly on the resolved hit. Bound how far
            // upwind that can put them: a long path in strong wind would push
            // the spawn outside the tier's own radius.
            const F32 drift = wind_h.magVec() * fall_time;
            if (drift > MAX_SPAWN_DRIFT)
            {
                fall_time *= MAX_SPAWN_DRIFT / drift;
            }
            part.mPos = hit_pos - part.mVel * fall_time;
        }
        else
        {
            // Non-impacting types don't need to land anywhere exact, so
            // instead of back-projecting the whole path they start half a
            // drift upwind: the midpoint of the trajectory sits over the cell
            // and the field stays centred on the camera. Full back-projection
            // threw these tens of meters upwind - at snow's 1.4 m/s over a 6 s
            // fall that offset outruns the 24 m spawn radius, so every flake
            // ended up on one side of the camera.
            LLVector3 lead = wind_h * (fall_time * 0.5f);
            const F32 lead_len = lead.magVec();
            if (lead_len > MAX_SPAWN_DRIFT * 0.5f)
            {
                lead *= (MAX_SPAWN_DRIFT * 0.5f) / lead_len;
            }
            part.mPos = hit_pos - lead + LLVector3(0.f, 0.f, fall_len);
        }
        part.mMaxAge = llclamp(fall_time, 0.2f, 25.f);
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

void SSPrecipSim::pushRipple(const SSPrecipParticle& part)
{
    if (mRipples.size() < RIPPLE_CAP)
    {
        mRipples.push_back(part);
    }
    else
    {
        mRipples[mRippleCursor] = part;
        mRippleCursor = (mRippleCursor + 1) % RIPPLE_CAP;
    }
}

void SSPrecipSim::spawnRipple(const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, SSRandStream& rng)
{
    LLVector3 n = on_water ? LLVector3(0.f, 0.f, 1.f) : normal;
    if (n.normVec() < 0.5f)
    {
        n.set(0.f, 0.f, 1.f);
    }

    // Spreading rings belong to flat-ish surfaces; fade the ring out as the
    // surface tips toward vertical, and drop it entirely on walls
    const F32 horiz = llclamp((n.mV[VZ] - 0.35f) / 0.4f, 0.f, 1.f);
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

    if (ring_gate > 0.05f)
    {
        // Surface-aligned expanding ring, lifted a hair along the normal
        SSPrecipParticle ring;
        ring.mKind = KIND_FLAT;
        ring.mPos = pos_agent + n * 0.02f;
        ring.mNormal = n;
        ring.mSizeX = 0.05f * strength * ripple_scale;                     // start half-size
        ring.mSizeY = (on_water ? 0.6f : 0.35f) * strength * ripple_scale; // end half-size
        ring.mMaxAge = (on_water ? 0.7f : 0.45f) / ripple_speed;
        ring.mAlpha = 0.4f * strength * ring_gate;
        ring.mPhase = rng.frand(0.f, F_TWO_PI);
        ring.mTex = textureIndex(ripple_tex);
        pushRipple(ring);
    }

    // Small splash crown thrown along the surface normal: up off floors,
    // outward off slopes and walls; ballistic under gravity from update()
    SSPrecipParticle crown;
    crown.mKind = KIND_ROUND;
    crown.mPos = pos_agent + n * 0.03f;
    crown.mVel = n * 0.6f * strength * ripple_scale * sqrtf(ripple_speed);
    crown.mSizeX = crown.mSizeY = 0.05f * strength * ripple_scale;
    crown.mMaxAge = 0.3f / ripple_speed;
    crown.mAlpha = 0.35f * strength;
    crown.mPhase = rng.frand(0.f, F_TWO_PI);
    crown.mTex = textureIndex(SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_DOT));
    pushRipple(crown);
}

void SSPrecipSim::applyEmberFlavor(SSPrecipParticle& part, LLColor4& tint, SSRandStream& vis,
                                   const SSPrecipPreset& preset)
{
    const F32 dark = llclamp(preset.mDarkMix, 0.f, 1.f);
    const F32 puff = llclamp(preset.mPuffMix, 0.f, 1.f - dark);

    const F32 roll = vis.frand();
    if (roll < dark)
    {
        // Small dark sharp ember: burnt-out fleck, so it is alpha blended
        // rather than additive - an additive dark sprite would be invisible
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

    // On a wall the burst runs down the face: square-on hits fan out widely
    // around the downhill direction, while a graze keeps its momentum and
    // stays a thin sliver along the travel direction.
    const LLVector3 wall_axis = lerp(slide, down_face, incidence);
    const F32 wall_inner = lerp(0.02f, 0.20f, incidence);
    const F32 wall_outer = lerp(0.10f, 0.85f, incidence);

    // On level ground the burst opens into a spherical cone running from a
    // narrow spray near the normal all the way out to grazing the surface,
    // like an angle-begin/angle-end cone pattern
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
        // Azimuth is fully random rather than evenly fanned, so successive
        // impacts do not repeat the same star pattern
        const F32 phi = rng.frand(0.f, F_TWO_PI);
        // Uniform over the spherical band between the two angles: lerping the
        // cosine keeps the density even instead of crowding the axis
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
        // Ejection speed spreads widely: some shards barely tumble clear,
        // others keep most of the impact energy
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

    // How many individual drops one particle of this tier stands in for.
    // Returned uncapped: the texture builder converts it into a splat count
    // and per-splat size that preserve coverage.
    splats = (tier == TIER_DROPS) ? 1
           : llmax(2, (S32)(1.f / TIER_SPEC[tier].mRateScale + 0.5f));
    return true;
}

// </SS:Nexii>
