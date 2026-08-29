/**
 * @file ssprecipitation.cpp
 * @brief See ssprecipitation.h.
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
#include "ssvolcloud.h"
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

static const U64 MAX_CATCHUP_TICKS = 4;
static const F32 VIS_BAND = 75.f;
static const F32 IMPACT_QUEUE_RADIUS = 32.f;
static const F32 MAX_FRAME_DT = 0.2f;
static const size_t RIPPLE_CAP = 4096;
static const F32 MAX_SPAWN_DRIFT = 12.f;
static const U32 GROUND_CHECK_SLICES = 8;

static const F32 IMPACT_OVERSHOOT = 2.f;
static const F32 DRIFT_FALL_SLACK = 40.f;
static const F32 DRIFT_SLACK_PER_WIND = 6.f;
static const F32 DRIFT_MAX_AGE = 90.f;

static const F32 COVER_TOLERANCE = 2.f;

static const F32 LIFE_EMA = 0.02f;

static LLTrace::BlockTimerStatHandle FTM_SS_SIM("Atmo Magic Sim");
static LLTrace::BlockTimerStatHandle FTM_SS_SIM_INTEGRATE("Integrate");
static LLTrace::BlockTimerStatHandle FTM_SS_SIM_SPAWN("Spawn");

struct SSTierSpec
{
    F32 mCell;
    F64 mHz;
    F32 mRateScale;
    F32 mCapShare;
};
static const SSTierSpec TIER_SPEC[TIER_COUNT] = {
    {  8.f, 8.0, 1.f,    0.61f },
    { 16.f, 4.0, 0.14f,  0.31f },
    { 32.f, 2.0, 0.004f, 0.08f },
};

// Particle budget per tier.
static S32 tierCap(SSPrecipTier tier)
{
    static LLCachedControl<U32> budget(gSavedSettings, "SSAtmoParticleBudget", 40000);
    const S32 total = (S32)llclamp((U32)budget, 500u, 200000u);
    return llmax(16, (S32)((F32)total * TIER_SPEC[tier].mCapShare));
}

// Wind at a position: flowmap when available, uniform otherwise.
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

// The three tier band radii from the preset.
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

// How far above and below the camera a tier's drops live.
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

// Emissive glow for fantasy weather.
static F32 presetGlow(const SSPrecipPreset& preset)
{
    static LLCachedControl<F32> glow_scale(gSavedSettings, "SSAtmoGlowScale", 1.f);
    return llclamp(preset.mGlow, 0.f, 1.f) * llclamp((F32)glow_scale, 0.f, 3.f);
}

// Drops grow a little with intensity.
static F32 intensitySizeScale(const SSPrecipPreset& preset, F32 precipitation)
{
    if (preset.mIntensitySize <= 0.f) return 1.f;
    const F32 scale = 0.55f + 0.9f * llclamp(precipitation, 0.f, 1.f);
    return lerp(1.f, scale, llclamp(preset.mIntensitySize, 0.f, 1.f));
}

// The distance band a tier owns, with its cross-fade overlaps.
void SSPrecipSim::tierBands(SSPrecipTier tier, const SSPrecipPreset& preset,
                            F32& in_lo, F32& in_hi, F32& out_lo, F32& out_hi)
{
    F32 r0, r1, r2;
    tierRadii(preset, r0, r1, r2);
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

// Empty pools; textures resolve lazily.
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

// Table lookup for a particle's texture index.
LLViewerTexture* SSPrecipSim::texture(U8 index) const
{
    if (index < mTextures.size() && mTextures[index].notNull())
    {
        return mTextures[index];
    }
    return LLViewerFetchedTexture::sDefaultParticleImagep;
}

// Interns a texture into the small per-sim table.
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
        resetTextureTable();
    }
    mTextures.push_back(texturep);
    return (U8)(mTextures.size() - 1);
}

// Clears interned textures (preset switch).
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

// Drops every particle.
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

// Region origin shift for all pools.
void SSPrecipSim::shift(const LLVector3& offset)
{
    for (SSPrecipParticle& p : mParticles) p.mPos += offset;
    for (SSPrecipParticle& p : mRipples) p.mPos += offset;
    for (SSPrecipParticle& p : mStreams) p.mPos += offset;
}

// Ages the persistent gutter streams and retires dead ones.
void SSPrecipSim::updateStreams(F32 dt)
{
    for (size_t i = 0; i < mStreams.size(); )
    {
        SSPrecipParticle& s = mStreams[i];
        s.mAge += dt;

        if (s.mAge >= s.mMaxAge)
        {
            mStreams.erase(mStreams.begin() + (S32)i);
            continue;
        }

        s.mPhase += dt * ssStreamScroll(llmax(0.05f, s.mFloorZ), s.mPlaneD);

        if (s.mPhase > 1.f) s.mPhase -= 1.f;

        ++i;
    }
}

// The sim step: spawn per tier, integrate motion and landings, cull - deterministic against shared time.
void SSPrecipSim::update(F32 dt)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM);

    dt = llclamp(dt, 0.f, MAX_FRAME_DT);

    updateStreams(dt);

    {
    LL_RECORD_BLOCK_TIME(FTM_SS_SIM_INTEGRATE);

    static U32 sSlicePhase = 0;
    const U32 slice_phase = sSlicePhase++ % GROUND_CHECK_SLICES;
    const bool slice_check = !SSAtmoMagic::getInstance()->preset().makesImpacts();
    const bool sky_track = SSAtmoMagic::getInstance()->isSkyTrack();

    static LLCachedControl<bool> respawn_setting(gSavedSettings, "SSAtmoRespawnOnImpact", true);
    const F32 respawn_env = SSAtmoMagic::getInstance()->gustEnvelopeAt(
        SSAtmoMagic::getInstance()->sharedTime());
    struct Respawn { SSPrecipTier mTier; U32 mSeed; LLVector3 mPos; };
    std::vector<Respawn> respawns;

    const LLVector3 cull_cam = LLViewerCamera::getInstance()->getOrigin();
    F32 cull_r2[TIER_COUNT];
    {
        const SSPrecipPreset& cull_preset = SSAtmoMagic::getInstance()->preset();
        for (S32 t = 0; t < TIER_COUNT; ++t)
        {
            F32 in_lo, in_hi, out_lo, out_hi;
            tierBands((SSPrecipTier)t, cull_preset, in_lo, in_hi, out_lo, out_hi);

            const F32 r = out_hi + TIER_SPEC[t].mCell * 2.f;
            cull_r2[t] = r * r;
        }
    }

    for (size_t i = 0; i < mParticles.size();)
    {
        SSPrecipParticle& p = mParticles[i];
        p.mAge += dt;

        {
            const F32 dx = p.mPos.mV[VX] - cull_cam.mV[VX];
            const F32 dy = p.mPos.mV[VY] - cull_cam.mV[VY];
            if (dx * dx + dy * dy > cull_r2[p.mTier])
            {
                --mTierCount[p.mTier];
                p = mParticles.back();
                mParticles.pop_back();
                continue;
            }
        }

        if (p.mAge >= p.mMaxAge)
        {
            mMeanLife[p.mTier] = (mMeanLife[p.mTier] <= 0.f)
                               ? p.mAge : lerp(mMeanLife[p.mTier], p.mAge, LIFE_EMA);

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
        if ((slice_check && (i % GROUND_CHECK_SLICES) == slice_phase) &&
            !(p.mFlags & PART_LANDED) && p.mVel.mV[VZ] < 0.f)
        {
            LLVector3 hit;
            bool on_water = false;
            const bool found = SSRainShadowMap::getInstance()->resolveColumn(p.mPos, hit, on_water);

            if (sky_track && !found)
            {
                p.mFloorZ = -FLT_MAX;
            }
            else if (hit.mV[VZ] - p.mPos.mV[VZ] > COVER_TOLERANCE)
            {
                --mTierCount[p.mTier];
                p = mParticles.back();
                mParticles.pop_back();
                continue;
            }

            else
            {
                p.mFloorZ = hit.mV[VZ];
            }
        }

        if (!(p.mFlags & PART_LANDED) && p.mPos.mV[VZ] <= p.mFloorZ)
        {
            p.mMaxAge = llmin(p.mMaxAge, p.mAge + ssPrecipFadeOut(p.mTier));
            p.mFlags |= PART_LANDED;
        }

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
            p.mVel.mV[VZ] -= 9.81f * dt;
        }
        p.mPos += p.mVel * dt;

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

// Spawns one tier's due particles for this tick, deterministically from cell hashes.
void SSPrecipSim::spawnTier(SSPrecipTier tier, U64 tick, F64 tick_time)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();

    if (!preset.mTiers[tier].mEnabled) { mTierTarget[tier] = 0.f; return; }

    const F32 env = atmo->gustEnvelopeAt(tick_time);
    if (env <= 0.f) { mTierTarget[tier] = 0.f; return; }

    mTierSpawnAccum[tier] = 0.f;

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

    F32 fall_lo, fall_hi;
    fallLength(preset, tier, fall_lo, fall_hi);
    const F32 nominal_life = preset.risesFromGround()
        ? 2.25f
        : ((fall_lo + fall_hi) * 0.5f) / llmax(0.1f, preset.mFallSpeed);

    const F32 mean_life = (mMeanLife[tier] > 0.f)
        ? llclamp(mMeanLife[tier], nominal_life * 0.5f, nominal_life * 8.f)
        : nominal_life;

    mTierTarget[tier] = llmin(mTierSpawnAccum[tier] * (F32)TIER_SPEC[tier].mHz * mean_life,
                              (F32)tierCap(tier));
}

// Spawns one cell's particles: placement, landing resolve, kind selection.
void SSPrecipSim::spawnTierCell(SSPrecipTier tier, U64 tick, F64 tick_time, S32 cx, S32 cy, F32 env,
                                const LLVector3& cam_agent, const LLVector3d& agent_origin_global)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    const SSTierSpec& spec = TIER_SPEC[tier];

    SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
        SSAtmoNoise::combine(0x71E5A17Cu ^ (U32)(tier + 1) * 0x9E3779B9u,
        SSAtmoNoise::combine((U32)(tick & 0xffffffffu),
        SSAtmoNoise::combine((U32)cx, (U32)cy * 0x27d4eb2fu)))));
    rng.next();

    static LLCachedControl<F32> density(gSavedSettings, "SSAtmoDensity", 1.f);

    const S32 cap = tierCap(tier);
    const F32 target = llmin(mTierTarget[tier] > 1.f ? mTierTarget[tier] : (F32)cap, (F32)cap);
    const F32 fill = (F32)mTierCount[tier] / llmax(1.f, target);
    const F32 headroom = (fill < 0.7f) ? 1.f : llmax(0.f, (1.f - fill) / 0.3f);

    const F32 area_factor = atmo->areaFactorAt((cx + 0.5) * spec.mCell, (cy + 0.5) * spec.mCell);

    const F32 cell_agent_x = (F32)((F64)cx * spec.mCell - agent_origin_global.mdV[VX]);
    const F32 cell_agent_y = (F32)((F64)cy * spec.mCell - agent_origin_global.mdV[VY]);

    // <SS:Nexii> The deck's convection noise map, asked about the column this cell's weather
    // falls out of - and asked where the rain COMES FROM, not where it lands. Wind tips the
    // fall, so a drop that lands here entered the deck's base a wind-drift upwind of here;
    // sampling the tilted point is what keeps it raining on a spot right under a gap when the
    // wind carries the weather across it, and what dries the spot the wind has carried AWAY
    // from. Presence gates the rate - a hole in the map takes its rain with it - and the tower
    // weight tweaks intensity slightly toward the high, dense parts. Weather that rises from
    // the ground never passes through the deck and is left alone.
    // [interaction: SSVolCloud]
    F32 noise_presence = 1.f;
    F32 noise_tower = 0.f;
    if (!preset.risesFromGround())
    {
        SSVolCloud* vol = SSVolCloud::getInstance();
        if (vol && vol->precipNoiseReady())
        {
            const LLVector3 cell_mid(cell_agent_x + spec.mCell * 0.5f,
                                     cell_agent_y + spec.mCell * 0.5f,
                                     cam_agent.mV[VZ]);
            LLVector3 hit;
            bool on_water = false;
            if (SSRainShadowMap::getInstance()->resolveColumn(cell_mid, hit, on_water))
            {
                const LLVector3 wind_h = windAt(hit);
                const F32 fall_t = llmax(0.f, vol->precipBaseZ() - hit.mV[VZ])
                                 / llmax(0.1f, preset.mFallSpeed);
                const LLVector2 gate = vol->precipNoiseAt(
                    hit - LLVector3(wind_h.mV[VX], wind_h.mV[VY], 0.f) * fall_t);
                noise_presence = gate.mV[VX];
                noise_tower = gate.mV[VY];
            }
        }
    }

    const F32 p = powf(atmo->precipitation(), 1.4f)
                * noise_presence * (0.85f + 0.30f * noise_tower);

    const F32 mean_full = preset.mRate * spec.mRateScale * p * area_factor * env
                          * llclamp((F32)density, 0.1f, 3.f)
                          * spec.mCell * spec.mCell / (F32)spec.mHz;
    mTierSpawnAccum[tier] += mean_full;

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
        const F32 ox = rng.frand(0.f, spec.mCell);
        const F32 oy = rng.frand(0.f, spec.mCell);
        const F32 fall_len = rng.frand(fall_lo, fall_hi);
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

        const bool no_platform = sky && !found_surface;
        if (no_platform && platform_roll > fall_through) continue;

        if (tier == TIER_DROPS && !no_platform)
        {
            const F32 strength = preset.mImpactStrength;
            if (strength > 0.f && (hit - cam_agent).magVec() < IMPACT_QUEUE_RADIUS)
            {
                const LLVector3 wind_h = windAt(hit) * (0.55f + 0.45f * llclamp(env, 0.f, 2.5f))
                                       * llmax(0.f, preset.mWindResponse);
                const LLVector3 impact_vel(wind_h.mV[VX], wind_h.mV[VY], -v_fall);
                atmo->queueImpact(tick_time + fall_len / v_fall, hit, strength * strength_jitter,
                                  on_water, normal, impact_vel, preset.mShatter);
            }
        }

        if (headroom <= 0.f) continue;
        if (headroom < 1.f && ll_frand() > headroom) continue;

        const F32 spawn_z = rises ? hit.mV[VZ] : hit.mV[VZ] + fall_len;
        const F32 band = VIS_BAND + (tier == TIER_SHEETS ? fall_hi : 0.f);
        if (hit.mV[VZ] - cam_agent.mV[VZ] > band) continue;
        if (spawn_z - cam_agent.mV[VZ] < -band) continue;

        if (mTierCount[tier] >= cap) continue;

        emitParticle(tier, hit, fall_len, env, size_jitter, phase, riser_age, gust_jitter, vis_seed,
                     found_surface || !sky);
    }
}

// Fills in one particle from its seed: motion, size, texture, material.
void SSPrecipSim::emitParticle(SSPrecipTier tier, const LLVector3& hit_pos, F32 fall_len, F32 gust,
                               F32 size_jitter, F32 phase, F32 riser_age, F32 gust_jitter, U32 vis_seed,
                               bool has_floor)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    if (!preset.mTiers[tier].mEnabled) return;
    const SSPrecipTierParams& visual = preset.mTiers[tier];

    SSRandStream vis(vis_seed);

    LLColor4 tint;
    F32 pbr_glow = 0.f;
    LLViewerTexture* custom = atmo->pickParticleTexture(vis, tint, pbr_glow);
    const U32 variant = (U32)vis.rand((S32)SSPrecipVariants::VARIANT_COUNT);

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

    tint.mV[0] *= preset.mTint.mV[0];
    tint.mV[1] *= preset.mTint.mV[1];
    tint.mV[2] *= preset.mTint.mV[2];
    part.mTex = textureIndex(texturep);
    part.mMaterial = preset.material();

    const LLVector3 wind_pos = rises
        ? hit_pos
        : hit_pos + LLVector3(0.f, 0.f, fall_len * 0.5f);

    const F32 tier_lean = (tier == TIER_SHEETS) ? 1.9f
                        : (tier == TIER_CLUSTERS) ? 1.35f : 1.f;

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
        part.mVel = LLVector3(wind_h.mV[VX], wind_h.mV[VY], v_fall);
        part.mMaxAge = riser_age;
    }
    else
    {
        F32 fall_time = fall_len / llmax(0.1f, v_fall);
        part.mVel = LLVector3(wind_h.mV[VX], wind_h.mV[VY], -v_fall);

        if (preset.makesImpacts())
        {
            const F32 max_drift = (tier == TIER_SHEETS) ? 120.f
                                : (tier == TIER_CLUSTERS) ? 36.f : MAX_SPAWN_DRIFT;
            const F32 drift = wind_h.magVec() * fall_time;
            if (drift > max_drift)
            {
                fall_time *= max_drift / drift;
            }
            part.mPos = hit_pos - part.mVel * fall_time;

            part.mFloorZ = hit_pos.mV[VZ];

            fall_time += IMPACT_OVERSHOOT / llmax(0.1f, v_fall);
        }
        else
        {
            LLVector3 lead = wind_h * (fall_time * 0.5f);
            const F32 lead_len = lead.magVec();
            if (lead_len > MAX_SPAWN_DRIFT * 0.5f)
            {
                lead *= (MAX_SPAWN_DRIFT * 0.5f) / lead_len;
            }
            part.mPos = hit_pos - lead + LLVector3(0.f, 0.f, fall_len);

            const F32 slack = DRIFT_FALL_SLACK + wind_h.magVec() * DRIFT_SLACK_PER_WIND;
            fall_time = (fall_len + slack) / llmax(0.1f, v_fall);

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

// Recycles a dead particle at a fresh deterministic position.
void SSPrecipSim::respawnParticle(SSPrecipTier tier, U32 seed, const LLVector3& impact_pos, F32 env)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();
    const SSTierSpec& spec = TIER_SPEC[tier];

    if (mTierCount[tier] >= tierCap(tier)) return;

    SSRandStream rng(SSAtmoNoise::combine(seed, 0x5F3A21C7u));
    rng.next();

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

    if (atmo->isSkyTrack() && !found_surface) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (tier == TIER_DROPS && preset.makesImpacts()
        && (hit - cam).magVec() < IMPACT_QUEUE_RADIUS)
    {
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

// Converts a landed particle into its ripple or splash.
void SSPrecipSim::pushRipple(const SSPrecipParticle& part)
{
    if (mRipples.size() < RIPPLE_CAP)
    {
        mRipples.push_back(part);
    }
    else
    {
        if (mRipples[mRippleCursor].mFlags & PART_DRIP) --mDripCount;
        mRipples[mRippleCursor] = part;
        mRippleCursor = (mRippleCursor + 1) % RIPPLE_CAP;
    }
    if (part.mFlags & PART_DRIP) ++mDripCount;
}

// Spawns a ripple ring (or splash crown) at a landing.
void SSPrecipSim::spawnRipple(const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, SSRandStream& rng)
{
    LLVector3 n = on_water ? LLVector3(0.f, 0.f, 1.f) : normal;
    if (n.normVec() < 0.5f)
    {
        n.set(0.f, 0.f, 1.f);
    }

    static const F32 RING_TILT_FULL = 0.9397f;
    static const F32 RING_TILT_NONE = 0.8660f;
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

    const SSPrecipPreset& preset = SSAtmoMagic::getInstance()->preset();

    const F32 water_spread = on_water ? 1.7f : 1.f;
    const F32 water_linger = on_water ? 1.55f : 1.f;

    if (ring_gate > 0.05f && preset.makesRipples())
    {
        const F32 ring_end = preset.mRippleSize * water_spread * strength * ripple_scale;
        SSPrecipParticle ring;
        ring.mKind = KIND_FLAT;
        ring.mMaterial = MAT_DECAL;
        ring.mPos = pos_agent + n * 0.02f;
        ring.mNormal = n;
        ring.mSizeX = ring_end * 0.15f;
        ring.mSizeY = ring_end;
        ring.mMaxAge = preset.mRippleLife * water_linger / ripple_speed;
        ring.mAlpha = preset.mRippleAlpha * strength * ring_gate;
        ring.mPhase = rng.frand(0.f, F_TWO_PI);
        ring.mTex = textureIndex(ripple_tex);
        pushRipple(ring);
    }

    if (!preset.makesCrowns()) return;

    SSPrecipParticle crown;
    crown.mKind = KIND_ROUND;
    crown.mPos = pos_agent + n * 0.03f;
    crown.mVel = n * preset.mCrownSpeed * strength * ripple_scale * sqrtf(ripple_speed);
    crown.mSizeX = crown.mSizeY = preset.mCrownSize * strength * ripple_scale;
    crown.mFlags |= PART_CROWN;
    crown.mMaxAge = preset.mCrownLife / ripple_speed;
    crown.mAlpha = preset.mCrownAlpha * strength;
    crown.mPhase = rng.frand(0.f, F_TWO_PI);
    crown.mTex = textureIndex(SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_DOT));
    pushRipple(crown);
}

// Drops per square metre per second at a position - the shared arrival-rate figure.
F32 SSPrecipSim::dropRateAt(const LLVector3& pos_agent)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->hasWeather()) return 0.f;

    const SSPrecipPreset& preset = atmo->preset();
    if (!preset.mTiers[TIER_DROPS].mEnabled) return 0.f;

    static LLCachedControl<F32> density(gSavedSettings, "SSAtmoDensity", 1.f);

    const LLVector3d global = gAgent.getPosGlobalFromAgent(pos_agent);
    const F32 area_factor = atmo->areaFactorAt(global.mdV[VX], global.mdV[VY]);
    F32 p = powf(atmo->precipitation(), 1.4f);

    // <SS:Nexii> The same noise gate the spawner runs, wind tilt included: whatever consumes
    // the arrival rate - impacts, wetness, the sound of it - follows the holes and the dense
    // parts of the deck rather than averaging over them. [interaction: SSVolCloud]
    if (!preset.risesFromGround())
    {
        SSVolCloud* vol = SSVolCloud::getInstance();
        if (vol && vol->precipNoiseReady())
        {
            LLVector3 hit;
            bool on_water = false;
            if (SSRainShadowMap::getInstance()->resolveColumn(pos_agent, hit, on_water))
            {
                const LLVector3 wind_h = windAt(hit);
                const F32 fall_t = llmax(0.f, vol->precipBaseZ() - hit.mV[VZ])
                                 / llmax(0.1f, preset.mFallSpeed);
                const LLVector2 gate = vol->precipNoiseAt(
                    hit - LLVector3(wind_h.mV[VX], wind_h.mV[VY], 0.f) * fall_t);
                p *= gate.mV[VX] * (0.85f + 0.30f * gate.mV[VY]);
            }
        }
    }

    return preset.mRate * TIER_SPEC[TIER_DROPS].mRateScale * p * area_factor
         * atmo->gustEnvelopeAt(atmo->sharedTime())
         * llclamp((F32)density, 0.1f, 3.f);
}

static const F32 STREAM_LINGER = 1.2f;

static const size_t MAX_LIVE_STREAMS = 512;

// Which tier a stream's span and fall claim.
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

static const F32 SS_STREAM_G  = 9.81f;
static const F32 SS_STREAM_V0 = 1.f;

// Fall time for a stream height.
static F32 ssStreamFallTime(F32 height)
{
    return (sqrtf(SS_STREAM_V0 * SS_STREAM_V0 + 2.f * SS_STREAM_G * llmax(0.f, height))
            - SS_STREAM_V0) / SS_STREAM_G;
}

// How long until a moved stream's old drops have cleared the air.
static F32 ssStreamClearTime(const LLVector3& start, const LLVector3& vel,
                             const LLVector3& drift, F32 fall_time)
{
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
        if (!shadow->resolveColumn(pos, hit, on_water)) continue;

        if (hit.mV[VZ] > pos.mV[VZ] + MARGIN)
        {
            return fall_time * (F32)(k - 1) / (F32)SAMPLES;
        }
    }

    return fall_time;
}

// Creates or updates a persistent roof-edge stream, keyed so it survives frames.
void SSPrecipSim::refreshStream(U32 key, const LLVector3& lip_agent, const LLVector3& out_dir,
                                const LLVector3& land_agent, F32 strength, F32 width,
                                F32 run_slope, SSRandStream& rng)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();

    const LLVector3 start = lip_agent + out_dir * 0.06f - LLVector3(0.f, 0.f, 0.04f);
    const F32 drop = start.mV[VZ] - land_agent.mV[VZ];
    if (drop < 0.5f) return;

    const F32 length = llmin(drop, llclamp(preset.mStreamLength, 0.5f, SS_STREAM_LENGTH_MAX));

    const F32 g = SS_STREAM_G;
    const F32 v0 = SS_STREAM_V0;
    F32 fall_time = ssStreamFallTime(length);

    const LLVector3 drift = windAt(lip_agent) * llmax(0.f, preset.mStreamWind);
    const LLVector3 exit_vel = out_dir * llclamp(0.3f + strength * 0.5f, 0.25f, 1.2f)
                             - LLVector3(0.f, 0.f, v0);

    const LLVector3 sideways = LLVector3(exit_vel.mV[VX], exit_vel.mV[VY], 0.f) * fall_time
                             + drift * (0.5f * fall_time * fall_time);
    if (sideways.magVecSquared() > 0.5f * 0.5f)
    {
        fall_time = ssStreamClearTime(start, exit_vel, drift, fall_time);
    }

    const F32 drawn = v0 * fall_time + 0.5f * g * fall_time * fall_time;
    if (drawn < 0.5f) return;

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

        s.mTier = TIER_SHEETS;
        s.mAge = 0.f;

        s.mPhase = SSAtmoNoise::hash01(key ^ 0x7A3C15E9u);

        LLColor4 tint;
        F32 pbr_glow = 0.f;
        LLViewerTexture* custom = atmo->pickParticleTexture(rng, tint, pbr_glow);
        SSPrecipVariants* variants = SSPrecipVariants::getInstance();

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

    s.mRunSlope = llclamp(run_slope, -4.f, 4.f);

    s.mPos = start;
    s.mVel = exit_vel;
    s.mPlaneD = fall_time;
    s.mNormal = drift;

    const F32 gauge = 0.97f + 0.03f * SSAtmoNoise::hash01(s.mSeed ^ 0x2C1B3A5Du);
    const F32 span = llmax(0.1f, width * gauge);
    s.mSizeX = span * 0.5f;

    F32 tex_x = preset.mTiers[art].mSizeX * 2.f;
    F32 tex_y = preset.mTiers[art].mSizeY * 2.f;
    if (tex_x < 0.05f) tex_x = SS_STREAM_TEX_METRES;
    if (tex_y < 0.05f) tex_y = SS_STREAM_TEX_METRES;

    F32 fat_x = 1.f, fat_y = 1.f;
    SSPrecipVariants::getInstance()->splatInflation(preset, art, fat_x, fat_y);

    const F32 art_scale = llclamp(preset.mStreamScale, 0.1f, 4.f);
    tex_x = tex_x * art_scale / fat_x;
    tex_y = tex_y * art_scale / fat_y;

    s.mSizeY = llclamp(span / tex_x, 0.02f, 16.f);

    s.mFloorZ = llclamp(drawn / tex_y, 0.02f, 16.f);

    s.mAlpha = llclamp((0.2f + strength * 0.4f) * llmax(0.f, preset.mStreamAlpha), 0.f, 1.f);

    s.mMaxAge = s.mAge + STREAM_LINGER;
}

// One runoff drip off a shelter lip.
void SSPrecipSim::spawnDrip(const LLVector3& lip_agent, const LLVector3& out_dir,
                            const LLVector3& land_agent, F32 volume, SSRandStream& rng)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const SSPrecipPreset& preset = atmo->preset();

    LLVector3 land = land_agent;
    LLVector3 normal(0.f, 0.f, 1.f);
    bool on_water = false;
    SSRainShadowMap::getInstance()->resolveColumn(land_agent + LLVector3(0.f, 0.f, 0.5f),
                                                  land, on_water, &normal);

    const LLVector3 start = lip_agent + out_dir * 0.06f - LLVector3(0.f, 0.f, 0.04f);

    const F32 drop = start.mV[VZ] - land.mV[VZ];
    if (drop < 0.35f) return;

    const F32 g = 9.81f;
    const F32 v0 = rng.frand(0.8f, 1.6f);
    const F32 fall_time = (sqrtf(v0 * v0 + 2.f * g * drop) - v0) / g;

    SSPrecipParticle p;
    p.mKind = KIND_STREAK;
    p.mFlags = PART_DRIP;
    p.mMaterial = preset.material();
    p.mPos = start;
    p.mVel = out_dir * rng.frand(0.25f, 0.7f) - LLVector3(0.f, 0.f, v0);

    const F32 fat = llclamp(powf(llmax(1.f, volume), 1.f / 3.f), 1.f, 2.6f) * rng.frand(0.85f, 1.15f)
                  * llclamp(preset.mDripScale, 0.1f, 4.f);
    const SSPrecipTierParams& tier = preset.mTiers[TIER_DROPS];
    p.mSizeX = tier.mSizeX * fat;
    p.mSizeY = tier.mSizeY * fat;
    p.mAlpha = llmin(1.f, tier.mAlpha * 1.3f);
    p.mMaxAge = fall_time;
    p.mPhase = rng.frand(0.f, F_TWO_PI);
    p.mSeed = rng.next();

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

    if (preset.makesImpacts())
    {
        atmo->queueImpact(atmo->sharedTime() + fall_time, land,
                          preset.mImpactStrength * llclamp(volume * 0.25f, 0.6f, 1.4f),
                          on_water, normal, LLVector3(0.f, 0.f, -g * fall_time), preset.mShatter,
                          true);
    }
}

// Fantasy ember look: tint, glow, flicker.
void SSPrecipSim::applyEmberFlavor(SSPrecipParticle& part, LLColor4& tint, SSRandStream& vis,
                                   const SSPrecipPreset& preset)
{
    const F32 dark = llclamp(preset.mDarkMix, 0.f, 1.f);
    const F32 puff = llclamp(preset.mPuffMix, 0.f, 1.f - dark);

    const F32 roll = vis.frand();
    if (roll < dark)
    {
        part.mKind = KIND_STREAK;
        part.mMaterial = MAT_LIT;
        part.mGlow = 0.f;
        part.mSizeX *= vis.frand(0.35f, 0.55f);
        part.mSizeY *= vis.frand(1.1f, 1.9f);
        part.mAlpha = llmin(1.f, part.mAlpha * vis.frand(0.75f, 1.f));
        part.mVel *= vis.frand(1.05f, 1.35f);
        tint *= vis.frand(0.10f, 0.22f);
        LLViewerTexture* dark_tex = SSAtmoMagic::textureFromList(preset.mDarkTexture);
        part.mTex = textureIndex(dark_tex ? dark_tex
            : SSPrecipVariants::getInstance()->utility(SSPrecipVariants::UTIL_SHARD));
    }
    else if (roll < dark + puff)
    {
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
}

// Hail shatter burst at an impact.
void SSPrecipSim::spawnShatter(const LLVector3& pos_agent, const LLVector3& normal,
                               const LLVector3& velocity, F32 strength, SSRandStream& rng)
{
    LLVector3 n = normal;
    if (n.normVec() < 0.5f)
    {
        n.set(0.f, 0.f, 1.f);
    }

    const F32 speed = llmax(1.f, velocity.magVec());
    LLVector3 vel_dir = velocity;
    if (vel_dir.normVec() < 0.01f)
    {
        vel_dir.set(0.f, 0.f, -1.f);
    }

    const F32 incidence = llclamp(-(vel_dir * n), 0.f, 1.f);
    const F32 flatness = llclamp(n.mV[VZ], 0.f, 1.f);

    LLVector3 slide = vel_dir - n * (vel_dir * n);
    if (slide.normVec() < 0.01f)
    {
        slide = n % LLVector3::z_axis;
        if (slide.normVec() < 0.01f) slide = LLVector3::x_axis;
    }

    LLVector3 down_face = LLVector3(0.f, 0.f, -1.f);
    down_face -= n * (down_face * n);
    if (down_face.normVec() < 0.01f)
    {
        down_face = slide;
    }

    const LLVector3 wall_axis = lerp(slide, down_face, incidence);
    const F32 wall_inner = lerp(0.02f, 0.20f, incidence);
    const F32 wall_outer = lerp(0.10f, 0.85f, incidence);

    const F32 floor_inner = 0.30f;
    const F32 floor_outer = 1.48f;

    LLVector3 axis = lerp(wall_axis, n, flatness);
    if (axis.normVec() < 0.01f) axis = n;
    const F32 angle_begin = lerp(wall_inner, floor_inner, flatness);
    const F32 angle_end   = lerp(wall_outer, floor_outer, flatness);

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

    const S32 count = 2 + rng.rand(6);
    for (S32 i = 0; i < count; ++i)
    {
        const F32 phi = rng.frand(0.f, F_TWO_PI);
        const F32 cos_theta = lerp(cosf(angle_begin), cosf(angle_end), rng.frand());
        const F32 theta = acosf(llclamp(cos_theta, -1.f, 1.f));

        LLVector3 dir = axis * cosf(theta) + (u * cosf(phi) + v * sinf(phi)) * sinf(theta);
        dir += n * 0.10f;
        if (dir.normVec() < 0.01f) dir = axis;

        SSPrecipParticle shard;
        shard.mKind = KIND_STREAK;
        shard.mMaterial = MAT_EMISSIVE;
        shard.mPos = pos_agent + n * 0.03f;
        shard.mNormal = n;
        shard.mPlaneD = (shard.mPos * n) - 0.05f;
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

// The sprite geometry a tier's texture bake assumes: quad size, drop size, splat count.
bool SSPrecipSim::tierSprite(const SSPrecipPreset& preset, SSPrecipTier tier,
                             F32& quad_x, F32& quad_y, F32& drop_x, F32& drop_y, S32& splats)
{
    if (!preset.mTiers[tier].mEnabled) return false;

    quad_x = preset.mTiers[tier].mSizeX;
    quad_y = preset.mTiers[tier].mSizeY;
    drop_x = preset.mTiers[TIER_DROPS].mSizeX;
    drop_y = preset.mTiers[TIER_DROPS].mSizeY;

    if (tier != TIER_DROPS)
    {
        const F32 drop_scale = llmax(0.f, preset.mDropScale);
        drop_x *= drop_scale;
        drop_y *= drop_scale;
    }

    splats = (tier == TIER_DROPS) ? 1
           : llmax(2, (S32)(1.f / TIER_SPEC[tier].mRateScale + 0.5f));
    return true;
}
