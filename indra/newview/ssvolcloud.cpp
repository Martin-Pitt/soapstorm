/**
 * @file ssvolcloud.cpp
 * @brief See ssvolcloud.h.
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

#include "ssvolcloud.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvcloudfieldstate.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvtrackstate.h"
#include "ssatmomagic.h"
#include "sslightning.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

extern bool gCubeSnapshot;

namespace
{
    const F32 CELL_M = 260.f;

    const F32 FIELD_RADIUS_M = 6000.f;

    const F32 FIELD_DRAW_M = 5000.f;
    const F32 FIELD_FADE_START_M = 4000.f;

    const S32 MAX_PUFFS = 1260;

    const F32 PUFF_CELL_FRACTION = 0.85f;

    const F32 PUFF_WIDE = 1.7f;
    const F32 PUFF_TALL = 0.62f;

    const F32 PUFF_ROUND_LO = 0.15f;
    const F32 PUFF_ROUND_HI = 0.70f;

    const S32 PUFFS_PER_CELL = 3;

    const F32 PUFF_THICKNESS_GAIN = 0.35f;

    const F32 COVERAGE_FLOOR = 0.04f;

    // Deterministic cell hash - the whole field derives from position, so every client sees the same clouds.
    U32 hashCell(S32 x, S32 y, U32 salt)
    {
        U32 h = (U32)(x * 374761393) ^ (U32)(y * 668265263) ^ (salt * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    // Smoothstep.
    F32 ss_smoothstep(F32 lo, F32 hi, F32 v)
    {
        const F32 t = llclamp((v - lo) / llmax(hi - lo, 1.0e-5f), 0.f, 1.f);
        return cubic_step(t);
    }

    // Cell hash to [0,1).
    F32 hashUnit(S32 x, S32 y, U32 salt)
    {
        return (F32)(hashCell(x, y, salt) & 0x00ffffffu) / (F32)0x01000000;
    }

    const S32 CLUSTER_CELLS_BIG = 9;
    const S32 CLUSTER_CELLS_SMALL = 3;
    const F32 CLUSTER_OCTAVE_MIX = 0.4f;

    const F32 CLUSTER_WEIGHT = 0.85f;

    const F32 CLUSTER_EDGE_HEIGHT = 0.3f;

    // <SS:Nexii> The convection noise map. One authored tileable greyscale map per deck, read
    // back to the CPU and sampled per cell to give the deck's response to convection a
    // geography. The map's values run through two ramps:
    //
    //   the HOLE window - below its low edge the cell is cut away entirely, so where the map
    //   runs low the sky opens; this is what breaks a dry stable deck into cloud and holes,
    //
    //   the TOWER window - the gradient ramp overlaid on the same values, deciding which
    //   columns are rising thermals. As the convection dial climbs, tower-weighted cells keep
    //   the full climb to the lid while the pockets between them are held low, and the tower
    //   columns take the anvil's flat-and-flare spread before the dial alone would allow it -
    //   which is how the anvil forms early, on the strong towers first.
    //
    // Moisture then lifts the whole map: the same values that broke a dry stable sky leave a
    // moist one unbroken, the overcast nimbostratus sheet. The tile is the field-scale metre
    // count at Noise Scale 1, and the grid is the cached readback's fixed resolution - the
    // structure it carries is kilometres wide, so 64 across carries it with texels to spare.
    const F32 SS_NOISE_TILE_M = 2048.f;
    const S32 SS_NOISE_GRID = 64;

    const F32 SS_HOLE_LO = 0.16f;
    const F32 SS_HOLE_HI = 0.52f;
    const F32 SS_TOWER_LO = 0.42f;
    const F32 SS_TOWER_HI = 0.78f;

    // How low the pockets between towers are held at full convection, as a fraction of the
    // height they would otherwise reach.
    const F32 SS_POCKET_H = 0.45f;

    // The moisture band that lifts the map's floor over its holes - the dry end keeps them
    // open, the mid-high end closes every one and the deck reads as unbroken nimbostratus.
    const F32 SS_NIMBUS_LO = 0.30f;
    const F32 SS_NIMBUS_HI = 0.72f;

    // And how much of the hole-cutting convection keeps alive in the dry case: a storm sky
    // still wants its gaps between towers, a stable sky its outright holes.
    const F32 SS_STORM_GAP = 0.45f;

    // The under deck's hash salt: the same cell field, offset so its cloud masses land where the
    // main deck's do not. Zero keeps the primary deck's pattern byte-identical to the single-deck
    // field it was before two decks existed.
    constexpr U32 SS_UNDER_DECK_SALT = 61u;

    // One value-noise octave over cell space, for cloud clustering.
    F32 clusterOctave(S32 cx, S32 cy, S32 cells, U32 salt, F32 shift)
    {
        const F32 fx = (F32)cx / (F32)cells + shift;
        const F32 fy = (F32)cy / (F32)cells + shift;

        const S32 x0 = llfloor(fx);
        const S32 y0 = llfloor(fy);

        F32 tx = fx - (F32)x0;
        F32 ty = fy - (F32)y0;
        tx = cubic_step(tx);
        ty = cubic_step(ty);

        const F32 c00 = hashUnit(x0,     y0,     salt);
        const F32 c10 = hashUnit(x0 + 1, y0,     salt);
        const F32 c01 = hashUnit(x0,     y0 + 1, salt);
        const F32 c11 = hashUnit(x0 + 1, y0 + 1, salt);

        const F32 top = c00 + (c10 - c00) * tx;
        const F32 bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    // Two-octave cluster noise: big masses with small-scale raggedness.
    F32 clusterUnit(S32 cx, S32 cy, U32 salt)
    {
        const F32 big = clusterOctave(cx, cy, CLUSTER_CELLS_BIG, 101u + salt, 0.f);
        const F32 small = clusterOctave(cx, cy, CLUSTER_CELLS_SMALL, 137u + salt, 0.37f);
        return big * (1.f - CLUSTER_OCTAVE_MIX) + small * CLUSTER_OCTAVE_MIX;
    }
}

// Drops the field - rebuilt from scratch next update.
void SSVolCloud::clear()
{
    mPrimary.mPuffs.clear();
    mUnder.mPuffs.clear();
    mLastBuildMS = 0.f;
}

// Rebuilds the puff field for this frame from the resolved cloud state: deterministic placement, lighting, squash band, strike lights, depth sort.
void SSVolCloud::update(F32 dt)
{
    mPrimary.mPuffs.clear();
    mUnder.mPuffs.clear();
    mLastCoverage = 0.f;
    mLastBuildMS = 0.f;

    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoVolumetricClouds", true);
    if (!enabled) return;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr || !mgr->hasAsset()) return;

    LLTimer timer;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (asset.mTracks.empty()) return;

    const F32 world_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(asset, world_z, world_z, true);
    S32 track_index = blend.mPrimaryTrack;
    if (track_index < 0 || track_index >= (S32)asset.mTracks.size()) track_index = 0;

    const SSAtmoEnvTrack& track = asset.mTracks[(size_t)track_index];
    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();

    const F32 moisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    const F32 convection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);

    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 sun_alt = sky ? sky->getSunDirection().mV[VZ] : 0.f;
    const F32 twilight = llclamp((sun_alt + 0.1f) / 0.25f, 0.f, 1.f);
    const F32 daylight = cubic_step(twilight);

    LLColor3 sunlit(1.f, 1.f, 1.f);
    LLColor3 ambient(0.4f, 0.4f, 0.5f);
    if (sky)
    {
        const LLColor3 sun_col(sky->getSunlightColor());
        const LLColor3 moon_col = LLColor3(sky->getMoonlightColor()) * 0.16f;
        sunlit = sun_col * daylight + moon_col * (1.f - daylight);
        ambient = LLColor3(sky->getAmbientColor()) * (0.05f + 0.95f * daylight);

        {
            const LLColor3 cc(sky->getCloudColor());
            for (S32 c = 0; c < 3; ++c)
            {
                const F32 tint = llclamp(cc.mV[c] / 0.41f, 0.f, 2.5f);
                sunlit.mV[c] *= tint;
                ambient.mV[c] *= tint;
            }
        }

    }
    const LLVector3 light_dir = LLEnvironment::instance().getLightDirection();

    {
        const F32 sun_lum = (sunlit.mV[0] + sunlit.mV[1] + sunlit.mV[2]) / 3.f;
        const F32 t = llclamp((sun_lum - 0.08f) / 0.5f, 0.f, 1.f);
        mBeam = cubic_step(t);
    }

    mAmbient = ambient;
    mLightDir = light_dir;
    mSunColor = sunlit;

    mEffRadius = FIELD_RADIUS_M;
    // Cap and knee: see SS_SQUASH_CAP_FRAC in ssvolcloud.h. The knee is a plain 0.8 of the cap, so the field from there outward folds into the last fifth of drawn depth.
    mSquashCap = MAX_FAR_CLIP * SS_SQUASH_CAP_FRAC;
    mSquashKnee = mSquashCap * 0.8f;

    const SSAtmoEnvCloudFieldState field =
        SSAtmoEnvCloudFieldResolver::resolve(track.mCloudField, moisture, convection, phase);

    // <SS:Nexii> Which deck the weather's noise gate reads: the authored source when it names
    // the under deck and that deck is on, the main field otherwise - which is every sky build's
    // answer, since its under deck hangs below the platform and is nobody's weather. The same
    // rule the environment editor's derivation follows, kept where the deck lives so
    // precipitation and deck can never disagree about who is making weather.
    // [interaction: precipitation]
    mWeatherDeck = (track.mWeatherSourceDeck == SS_ATMOENV_DECK_UNDER
                    && track.mUnderField.mEnabled) ? 1 : 0;

    if (field.mCoverage >= COVERAGE_FLOOR && field.mThicknessM > 1.f)
    {
        buildDeck(mPrimary, field, convection, moisture, 0u);
        mLastCoverage = field.mCoverage;
    }

    // <SS:Nexii> The under deck: the same resolver and the same builder against the track's second
    // field, hashed with its own salt so the two decks' cloud patterns are independent - a mirror
    // copy of the main deck at a different altitude would read as exactly the artifact it is.
    // </SS:Nexii>
    if (track.mUnderField.mEnabled)
    {
        const SSAtmoEnvCloudFieldState under =
            SSAtmoEnvCloudFieldResolver::resolve(track.mUnderField, moisture, convection, phase);
        if (under.mCoverage >= COVERAGE_FLOOR && under.mThicknessM > 1.f)
        {
            buildDeck(mUnder, under, convection, moisture, SS_UNDER_DECK_SALT);
        }
    }

    mStrikeLights.clear();
    for (const SSStrike& strike : SSLightning::getInstance()->strikes())
    {
        const F32 b = strike.mChannelBrightness * strike.mIntensity;
        if (b <= 0.004f) continue;

        mStrikeLights.push_back(LLVector4(strike.mOrigin.mV[VX],
                                          strike.mOrigin.mV[VY],
                                          strike.mOrigin.mV[VZ], b));
        if ((S32)mStrikeLights.size() >= SS_MAX_STRIKE_LIGHTS) break;
    }

    mOccGridDirty = true;

    mLastBuildMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
}

// Builds one deck's puffs from a resolved field state: same deterministic placement and shading for both decks, hashed with the deck's salt so their patterns differ. Moisture rides along because the
// noise map's hole-cutting is what moisture moderates.
void SSVolCloud::buildDeck(Deck& deck, const SSAtmoEnvCloudFieldState& field, F32 convection, F32 moisture, U32 salt)
{
    deck.mTexture = field.mBaseTexture.notNull()
        ? field.mBaseTexture
        : LLUUID(field.mHasAnvil || convection > 0.6f
                 ? SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS
                 : SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS);

    deck.mDetail = field.mDetailTexture;
    deck.mNoise = field.mNoiseTexture;

    deck.mBaseZ = field.mBaseHeightM;
    deck.mThicknessM = llmax(1.f, field.mThicknessM);
    deck.mAnvil = field.mAnvil;
    deck.mTextureMix = field.mTextureMix;
    deck.mPuffDensity = field.mPuffDensity;
    deck.mDetailScale = field.mDetailScale;
    deck.mDriftRate = field.mDriftRate;
    deck.mChurn = llclamp(field.mChurn, 0.f, 1.f);
    deck.mCoverage = field.mCoverage;

    // <SS:Nexii> The noise map's resolved shaping, baked once per build so every consumer of the
    // field - this builder, and the precipitation gate reading the deck from outside - runs the
    // same numbers. The tile scales off the authored Noise Scale slider; the hole weight is what
    // survives of the map's low end once moisture has lifted the floor over it and convection has
    // kept the storm gaps open in what is left.
    deck.mNoiseTileM = field.mNoiseTexture.notNull()
        ? SS_NOISE_TILE_M * llmax(0.05f, field.mNoiseScale)
        : 0.f;
    const F32 nimbus = ss_smoothstep(SS_NIMBUS_LO, SS_NIMBUS_HI, moisture);
    deck.mNoiseHole = (1.f - nimbus) * (1.f - SS_STORM_GAP * llclamp(convection, 0.f, 1.f));

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    const F32 air_x = cam.mV[VX] - drift.mV[0];
    const F32 air_y = cam.mV[VY] - drift.mV[1];

    const S32 cell_radius = llceil(FIELD_DRAW_M / CELL_M);
    const S32 cx0 = llfloor(air_x / CELL_M);
    const S32 cy0 = llfloor(air_y / CELL_M);

    const F32 base_radius = CELL_M * PUFF_CELL_FRACTION * 0.5f;
    const F32 size_gain = 1.f + PUFF_THICKNESS_GAIN * (field.mThicknessM / 500.f);

    const F32 gloom = field.mGloom;
    const LLVector3 light_dir = mLightDir;
    const F32 beam = mBeam;

    F64 dist_sum = 0.0;

    for (S32 dy = -cell_radius; dy <= cell_radius; ++dy)
    {
        for (S32 dx = -cell_radius; dx <= cell_radius; ++dx)
        {
            const S32 cx = cx0 + dx;
            const S32 cy = cy0 + dy;

            const F32 gate = clusterUnit(cx, cy, salt) * CLUSTER_WEIGHT
                           + hashUnit(cx, cy, 1u + salt) * (1.f - CLUSTER_WEIGHT);
            if (gate > field.mCoverage) continue;

            const F32 coreness = llclamp(
                (field.mCoverage - gate) / llmax(field.mCoverage, 0.01f), 0.f, 1.f);

            const F32 cell_height = CLUSTER_EDGE_HEIGHT
                + (1.f - CLUSTER_EDGE_HEIGHT) * coreness;

            for (S32 sub = 0; sub < PUFFS_PER_CELL; ++sub)
            {
                const U32 sub_salt = salt + (U32)sub * 8u;

                const F32 jx = (hashUnit(cx, cy, 2u + sub_salt) - 0.5f) * CELL_M * 0.8f;
                const F32 jy = (hashUnit(cx, cy, 3u + sub_salt) - 0.5f) * CELL_M * 0.8f;

                LLVector3 pos;
                pos.mV[VX] = (F32)cx * CELL_M + CELL_M * 0.5f + jx + drift.mV[0];
                pos.mV[VY] = (F32)cy * CELL_M + CELL_M * 0.5f + jy + drift.mV[1];

                const F32 up_cell = hashUnit(cx, cy, 4u + sub_salt);
                const F32 up = up_cell * cell_height;
                pos.mV[VZ] = field.mBaseHeightM + up * field.mThicknessM;

                const F32 waist = 1.f - 0.35f * ss_smoothstep(0.2f, 0.65f, up_cell);
                const F32 flare = 1.1f * ss_smoothstep(0.74f, 1.f, up_cell);
                const F32 flat = 1.f + field.mAnvil * coreness * (waist + flare - 1.f);

                const LLVector3 to_cam = pos - cam;
                const F32 dist_sq = to_cam.magVecSquared();
                if (dist_sq > FIELD_DRAW_M * FIELD_DRAW_M) continue;

                const bool squashed = dist_sq > mSquashKnee * mSquashKnee;
                if (squashed && sub > 0) continue;

                Puff puff;
                puff.mPosAgent = pos;
                puff.mRadius = base_radius * size_gain * flat
                    * (0.7f + 0.6f * hashUnit(cx, cy, 5u + sub_salt))
                    * (squashed ? 1.6f : 1.f);
                puff.mCamDistSq = dist_sq;

                const F32 dist = sqrtf(dist_sq);
                const F32 edge_t = llclamp((dist - FIELD_FADE_START_M)
                                           / (FIELD_DRAW_M - FIELD_FADE_START_M), 0.f, 1.f);
                const F32 edge = 1.f - edge_t * edge_t;
                puff.mAlpha = edge * llclamp(0.35f + 0.65f * field.mCoverage, 0.f, 1.f);

                const F32 facing = llclamp(
                    0.5f + 0.2f * (light_dir.mV[VZ] * (up - 0.5f) * 2.f), 0.f, 1.f);

                const F32 sun_z = light_dir.mV[VZ];
                const F32 th = (0.5f + llclamp(field.mThicknessM / 500.f, 0.f, 1.f))
                             * (0.35f + 0.65f * field.mCoverage);
                F32 shade;
                if (sun_z >= 0.f)
                {
                    const F32 above = llmax(cell_height - up, 0.f);
                    shade = expf(-(above / llmax(sun_z, 0.35f) * 0.9f
                                   + coreness * (1.f - sun_z) * 1.5f) * th);
                }
                else
                {
                    shade = expf(-(up / llmax(-sun_z, 0.35f) * 0.9f
                                   + coreness * 1.5f) * th);
                }

                const F32 rim = cubic_step(edge_t);

                const F32 form = lerp(0.65f, lerp(facing, 0.65f, rim), beam)
                               * lerp(1.f, shade, beam);
                puff.mColor = (mAmbient + mSunColor * form) * gloom;

                dist_sum += dist_sq;
                deck.mPuffs.push_back(puff);
            }
        }
    }

    if (!deck.mPuffs.empty())
    {
        deck.mMeanDistSq = (F32)(dist_sum / (F64)deck.mPuffs.size());

        std::sort(deck.mPuffs.begin(), deck.mPuffs.end(),
                  [](const Puff& a, const Puff& b) { return a.mCamDistSq > b.mCamDistSq; });

        if ((S32)deck.mPuffs.size() > MAX_PUFFS)
        {
            deck.mPuffs.erase(deck.mPuffs.begin(), deck.mPuffs.end() - MAX_PUFFS);
        }
    }
}

// Drawn/true distance ratio of the shared squash band, for anything that must land at the field's drawn depth.
F32 SSVolCloud::squashScale(F32 true_dist) const
{
    if (true_dist <= mSquashKnee || true_dist <= 0.f) return 1.f;
    const F32 span = llmax(mEffRadius - mSquashKnee, 1.f);
    const F32 drawn = llmin(mSquashKnee + (true_dist - mSquashKnee) * (mSquashCap - mSquashKnee) / span,
                            mSquashCap * 0.999f);
    return drawn / true_dist;
}

// How much light survives from A to B through the primary deck's puffs - grid-accelerated, drives lightning occlusion. The under deck sits below the weather and is nobody's occluder.
F32 SSVolCloud::transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength)
{
    if (mPrimary.mPuffs.empty() || strength <= 0.f) return 1.f;

    if (mOccGridDirty)
    {
        mOccGrid.clear();
        mMaxPuffR = 0.f;
        for (S32 i = 0; i < (S32)mPrimary.mPuffs.size(); ++i)
        {
            const Puff& p = mPrimary.mPuffs[(size_t)i];
            const F32 r = p.mRadius * PUFF_WIDE;
            mMaxPuffR = llmax(mMaxPuffR, r);
            const S32 x0 = llfloor((p.mPosAgent.mV[VX] - r) / CELL_M);
            const S32 x1 = llfloor((p.mPosAgent.mV[VX] + r) / CELL_M);
            const S32 y0 = llfloor((p.mPosAgent.mV[VY] - r) / CELL_M);
            const S32 y1 = llfloor((p.mPosAgent.mV[VY] + r) / CELL_M);
            for (S32 gy = y0; gy <= y1; ++gy)
            {
                for (S32 gx = x0; gx <= x1; ++gx)
                {
                    mOccGrid[((U64)(U32)gx << 32) | (U64)(U32)gy].push_back(i);
                }
            }
        }
        mOccStamp.assign(mPrimary.mPuffs.size(), 0u);
        mOccQuery = 0;
        mOccGridDirty = false;
    }

    const LLVector3 d = to_agent - from_agent;
    const F32 z_lo = mPrimary.mBaseZ - mMaxPuffR;
    const F32 z_hi = mPrimary.mBaseZ + mPrimary.mThicknessM + mMaxPuffR;
    F32 t0 = 0.f, t1 = 1.f;
    if (llabs(d.mV[VZ]) > 0.001f)
    {
        F32 ta = (z_lo - from_agent.mV[VZ]) / d.mV[VZ];
        F32 tb = (z_hi - from_agent.mV[VZ]) / d.mV[VZ];
        if (ta > tb) { const F32 tmp = ta; ta = tb; tb = tmp; }
        t0 = llmax(0.f, ta);
        t1 = llmin(1.f, tb);
        if (t0 >= t1) return 1.f;
    }
    else if (from_agent.mV[VZ] < z_lo || from_agent.mV[VZ] > z_hi)
    {
        return 1.f;
    }

    const LLVector3 a = from_agent + d * t0;
    const LLVector3 b = from_agent + d * t1;
    const F32 d_sq = llmax(d.magVecSquared(), 0.0001f);

    ++mOccQuery;
    F32 trans = 1.f;

    const F32 len_xy = sqrtf((b.mV[VX] - a.mV[VX]) * (b.mV[VX] - a.mV[VX])
                             + (b.mV[VY] - a.mV[VY]) * (b.mV[VY] - a.mV[VY]));
    const S32 steps = llmin((S32)(len_xy / CELL_M) + 1, 64);
    for (S32 s = 0; s <= steps; ++s)
    {
        const LLVector3 px = a + (b - a) * ((F32)s / (F32)steps);
        const S32 gx = llfloor(px.mV[VX] / CELL_M);
        const S32 gy = llfloor(px.mV[VY] / CELL_M);
        auto it = mOccGrid.find(((U64)(U32)gx << 32) | (U64)(U32)gy);
        if (it == mOccGrid.end()) continue;

        for (S32 idx : it->second)
        {
            if (mOccStamp[(size_t)idx] == mOccQuery) continue;
            mOccStamp[(size_t)idx] = mOccQuery;

            const Puff& p = mPrimary.mPuffs[(size_t)idx];

            const F32 t = llclamp(((p.mPosAgent - from_agent) * d) / d_sq, 0.f, 1.f);
            const LLVector3 closest = from_agent + d * t;
            const F32 r_eff = p.mRadius * 1.15f;
            const F32 off_sq = (closest - p.mPosAgent).magVecSquared();
            if (off_sq >= r_eff * r_eff) continue;

            const F32 prof = 1.f - off_sq / (r_eff * r_eff);
            trans *= 1.f - llclamp(p.mAlpha * prof * strength, 0.f, 1.f);
            if (trans < 0.004f) return 0.f;
        }
    }
    return trans;
}

// Draws the sorted puffs as camera-faced billboards with soft depth, storm lighting and strike flashes.
void SSVolCloud::render()
{
    if ((mPrimary.mPuffs.empty() && mUnder.mPuffs.empty())) return;
    if (!gSSVolCloudProgram.isComplete()) return;

    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }

    // <SS:Nexii> Depth copy: taken once before either deck draws - the primary deck is the occluder
    // the soft edges belong to, and the under deck at the bottom of a build blends against world
    // geometry plus the primary deck above it in the one copy. </SS:Nexii>
    LL_PROFILE_GPU_ZONE("atmo volumetric clouds");

    bool have_depth_copy = false;

    {
        const S32 view_w = (S32)gGLViewport[2];
        const S32 view_h = (S32)gGLViewport[3];

        if (view_w > 0 && view_h > 0 && gCopyDepthProgram.isComplete())
        {
            if ((S32)mDepthCopy.getWidth() != view_w || (S32)mDepthCopy.getHeight() != view_h)
            {
                mDepthCopy.release();
                have_depth_copy = mDepthCopy.allocate(view_w, view_h, GL_RGBA, true);
            }
            else
            {
                have_depth_copy = true;
            }

            if (have_depth_copy)
            {
                LL_PROFILE_GPU_ZONE("atmo cloud depth copy");

                LLGLDepthTest copy_depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

                gPipeline.mRT->screen.flush();
                mDepthCopy.bindTarget();

                gCopyDepthProgram.bind();

                S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
                S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
                gGL.getTexUnit(diff_map)->bind(&gPipeline.mRT->screen);
                gGL.getTexUnit(depth_map)->bind(&gPipeline.mRT->deferredScreen, true);

                gGL.setColorMask(false, false);
                gPipeline.mScreenTriangleVB->setBuffer();
                gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                gGL.setColorMask(true, true);

                gCopyDepthProgram.unbind();

                mDepthCopy.flush();
                gPipeline.mRT->screen.bindTarget();
            }
        }
    }

    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    gGL.setColorMask(true, false);

    gSSVolCloudProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    static LLStaticHashedString s_drift("ss_drift");
    static LLStaticHashedString s_time("ss_time");
    static LLStaticHashedString s_churn("ss_churn");
    static LLStaticHashedString s_anvil("ss_anvil");
    static LLStaticHashedString s_base_z("ss_base_z");
    static LLStaticHashedString s_thick("ss_layer_thick");
    static LLStaticHashedString s_tex_mix("ss_tex_mix");
    static LLStaticHashedString s_puff_density("ss_puff_density");
    static LLStaticHashedString s_detail_scale("ss_detail_scale");
    static LLStaticHashedString s_drift_rate("ss_drift_rate");
    static LLStaticHashedString s_wind("ss_wind");
    static LLStaticHashedString s_strike("ss_strike");
    static LLStaticHashedString s_strike_count("ss_strike_count");
    static LLStaticHashedString s_strike_color("ss_strike_color");
    static LLStaticHashedString s_strike_occ("ss_strike_occ");
    static LLStaticHashedString s_light_dir("ss_light_dir");
    static LLStaticHashedString s_sun_color("ss_sun_color");
    static LLStaticHashedString s_cam_pos("ss_cam_pos");
    static LLStaticHashedString s_beam("ss_beam");
    static LLStaticHashedString s_rim("ss_rim");
    static LLStaticHashedString s_squash("ss_squash");
    static LLStaticHashedString s_clip("ss_clip");
    static LLStaticHashedString s_soft("ss_soft_m");

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    LLVector2 wind = drift;
    if (wind.length() < 0.001f)
    {
        wind.setVec(1.f, 0.f);
    }
    else
    {
        wind.normalize();
    }

    {
        const LLColor3 lit = SSAtmoMagic::getInstance()->lightningColor();
        gSSVolCloudProgram.uniform3fv(s_strike_color, 1, lit.mV);

        const S32 count = llmin((S32)mStrikeLights.size(), SS_MAX_STRIKE_LIGHTS);
        gSSVolCloudProgram.uniform1i(s_strike_count, count);
        if (count > 0)
        {
            gSSVolCloudProgram.uniform4fv(s_strike, count,
                                          (F32*)mStrikeLights.data());
        }

        static LLCachedControl<F32> occl_setting(gSavedSettings, "SSAtmoLightningOcclusion", 0.85f);
        gSSVolCloudProgram.uniform1f(s_strike_occ, llclamp((F32)occl_setting, 0.f, 1.f));
    }

    LLVector3 light = mLightDir;
    if (light.normalize() < 0.001f) light = LLVector3::z_axis;

    gSSVolCloudProgram.uniform3fv(s_light_dir, 1, light.mV);
    gSSVolCloudProgram.uniform3fv(s_sun_color, 1, mSunColor.mV);
    gSSVolCloudProgram.uniform3fv(s_cam_pos, 1, camera->getOrigin().mV);

    gSSVolCloudProgram.uniform1f(s_beam, mBeam);

    gSSVolCloudProgram.uniform2f(s_rim, 4000.f, 4900.f);

    gSSVolCloudProgram.uniform3f(s_squash, mSquashKnee, mSquashCap, mEffRadius);

    static const F32 SOFT_M = 112.5f;

    bool soft = have_depth_copy &&
        gSSVolCloudProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mDepthCopy, true) >= 0;

    gSSVolCloudProgram.uniform1f(s_soft, soft ? SOFT_M : 0.f);
    if (soft)
    {
        gSSVolCloudProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                     (F32)gGLViewport[2], (F32)gGLViewport[3]);
        gSSVolCloudProgram.uniform2f(s_clip, camera->getNear(), camera->getFar());
    }

    gSSVolCloudProgram.uniform2f(s_wind, wind.mV[0], wind.mV[1]);

    // <SS:Nexii> Far deck first: the primary deck lives at storm altitude and the under deck at the
    // build's floor, so the deck whose mean puff is farther from the eye draws first and the nearer
    // one blends over it. Each deck sets its own per-deck uniforms and textures; blending state and
    // the shared uniforms above survive across both. </SS:Nexii>
    const bool under_on_top = mUnder.mMeanDistSq < mPrimary.mMeanDistSq;
    Deck* order[2] = { under_on_top ? &mPrimary : &mUnder,
                       under_on_top ? &mUnder    : &mPrimary };

    const LLVector3 cam_pos = camera->getOrigin();
    const LLVector3 cam_right_fallback = camera->getLeftAxis() * -1.f;

    for (Deck* deckp : order)
    {
        Deck& deck = *deckp;
        if (deck.mPuffs.empty() || deck.mTexture.isNull()) continue;

        if (!fetchDeckTextures(deck)) continue;

        gSSVolCloudProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, deck.mTextureRef, LLTexUnit::TT_TEXTURE);
        gSSVolCloudProgram.bindTexture(LLShaderMgr::CLOUD_NOISE_MAP,
                                       deck.mDetailRef.notNull() ? deck.mDetailRef.get() : deck.mTextureRef.get(),
                                       LLTexUnit::TT_TEXTURE);

        gSSVolCloudProgram.uniform2f(s_drift, drift.mV[0], drift.mV[1]);
        gSSVolCloudProgram.uniform1f(s_time, (F32)LLFrameTimer::getElapsedSeconds());
        gSSVolCloudProgram.uniform1f(s_churn, deck.mChurn);

        gSSVolCloudProgram.uniform1f(s_anvil, deck.mAnvil);

        gSSVolCloudProgram.uniform1f(s_base_z, deck.mBaseZ);
        gSSVolCloudProgram.uniform1f(s_thick, deck.mThicknessM);
        gSSVolCloudProgram.uniform1f(s_tex_mix, deck.mTextureMix);
        gSSVolCloudProgram.uniform1f(s_puff_density, deck.mPuffDensity);
        gSSVolCloudProgram.uniform1f(s_detail_scale, deck.mDetailScale);
        gSSVolCloudProgram.uniform1f(s_drift_rate, deck.mDriftRate);

        gGL.begin(LLRender::TRIANGLES);
        for (const Puff& puff : deck.mPuffs)
        {
            LLVector3 normal = cam_pos - puff.mPosAgent;
            if (normal.normalize() < 0.001f)
            {
                normal = LLVector3::z_axis;
            }

            const F32 flatten = llclamp((llabs(normal.mV[VZ]) - 0.6f) / 0.35f, 0.f, 1.f);
            if (flatten > 0.f)
            {
                const F32 sgn = (normal.mV[VZ] >= 0.f) ? 1.f : -1.f;
                normal = normal * (1.f - flatten) + LLVector3(0.f, 0.f, sgn) * flatten;
                if (normal.normalize() < 0.001f)
                {
                    normal.setVec(0.f, 0.f, sgn);
                }
            }

            LLVector3 ref = LLVector3::z_axis * (1.f - flatten)
                          + LLVector3::x_axis * flatten;
            ref.normalize();

            LLVector3 base_right = ref % normal;
            if (base_right.normalize() < 0.001f)
            {
                base_right = cam_right_fallback;
            }
            const LLVector3 base_up = normal % base_right;

            const F32 layer_h = llclamp(
                (puff.mPosAgent.mV[VZ] - deck.mBaseZ) / deck.mThicknessM, 0.f, 1.f);
            const F32 round = llclamp(
                (layer_h - PUFF_ROUND_LO) / (PUFF_ROUND_HI - PUFF_ROUND_LO), 0.f, 1.f);

            const F32 wide = PUFF_WIDE + (1.f - PUFF_WIDE) * round;
            const F32 tall = PUFF_TALL + (1.f - PUFF_TALL) * round;

            const LLVector3 right = base_right * (puff.mRadius * wide);
            const LLVector3 up = base_up * (puff.mRadius * tall);

            gGL.color4f(puff.mColor.mV[0], puff.mColor.mV[1], puff.mColor.mV[2], puff.mAlpha);

            const LLVector3 tl = puff.mPosAgent - right + up;
            const LLVector3 bl = puff.mPosAgent - right - up;
            const LLVector3 tr = puff.mPosAgent + right + up;
            const LLVector3 br = puff.mPosAgent + right - up;

            gGL.texCoord2f(0.f, 1.f); gGL.vertex3fv(tl.mV);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bl.mV);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(tr.mV);

            gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(tr.mV);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bl.mV);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex3fv(br.mV);
        }
        gGL.end();
    }

    gGL.flush();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSVolCloudProgram.unbind();

    gGL.setColorMask(true, true);
}

// Binds a deck's authored textures, falling back to the sky dome's noise for an empty detail slot - per deck, since the two may carry different maps.
bool SSVolCloud::fetchDeckTextures(Deck& deck)
{
    if (deck.mTextureRef.isNull() || deck.mTextureRef->getID() != deck.mTexture)
    {
        deck.mTextureRef = LLViewerTextureManager::getFetchedTexture(
            deck.mTexture, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (deck.mTextureRef.notNull())
        {
            deck.mTextureRef->setNoDelete();
        }
    }
    if (deck.mTextureRef.isNull()) return false;
    deck.mTextureRef->addTextureStats((F32)MAX_IMAGE_AREA);

    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const LLUUID detail_id = deck.mDetail.notNull()
        ? deck.mDetail
        : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
    if (detail_id.notNull() && (deck.mDetailRef.isNull() || deck.mDetailRef->getID() != detail_id))
    {
        deck.mDetailRef = LLViewerTextureManager::getFetchedTexture(
            detail_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (deck.mDetailRef.notNull())
        {
            deck.mDetailRef->setNoDelete();
        }
    }
    if (deck.mDetailRef.notNull())
    {
        deck.mDetailRef->addTextureStats((F32)MAX_IMAGE_AREA);
    }

    return true;
}
