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
#include "llimage.h"
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

    // The profile curve's row count - a vertical curve needs no more resolution than this.
    const S32 SS_PROFILE_N = 64;

    // The built-in profile strip's paint size, for the picker's preview only.
    const S32 SS_PROFILE_STRIP_W = 256;
    const S32 SS_PROFILE_STRIP_H = 8;

    const F32 SS_HOLE_LO = 0.16f;
    const F32 SS_HOLE_HI = 0.52f;
    const F32 SS_TOWER_LO = 0.42f;
    const F32 SS_TOWER_HI = 0.78f;

    // How low the pockets between towers are held at full convection, as a fraction of the
    // height they would otherwise reach. Softened from where it started: at 0.45 the pockets
    // read as stubs hanging under the deck and the map's carving dominated the silhouette from
    // below; at 0.55 the field keeps a body under its own structure.
    const F32 SS_POCKET_H = 0.55f;

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

    // <SS:Nexii> The procedural fallback for the convection noise map, for decks with no authored
    // texture. Square by construction (the field map is a tiling square, and a square tile is what
    // every consumer of it assumes), tileable by wrapping lattice, and seeded off the weather - so
    // every client sharing an environment grows the same geography without anyone uploading a map.
    const S32 SS_NOISE_PROC_SIZE = 256;

    // One octave of tileable value noise: a period-cell lattice over the unit square, every
    // corner hash wrapping at the period so the tile seamless.
    F32 tileLattice(F32 x, F32 y, S32 period, U32 salt)
    {
        const F32 fx = x * (F32)period;
        const F32 fy = y * (F32)period;

        const S32 x0 = llfloor(fx);
        const S32 y0 = llfloor(fy);

        F32 tx = fx - (F32)x0;
        F32 ty = fy - (F32)y0;
        tx = cubic_step(tx);
        ty = cubic_step(ty);

        const S32 wx = ((x0 % period) + period) % period;
        const S32 wy = ((y0 % period) + period) % period;
        const S32 wx1 = (wx + 1) % period;
        const S32 wy1 = (wy + 1) % period;

        const F32 c00 = hashUnit(wx,  wy,  salt);
        const F32 c10 = hashUnit(wx1, wy,  salt);
        const F32 c01 = hashUnit(wx,  wy1, salt);
        const F32 c11 = hashUnit(wx1, wy1, salt);

        const F32 top = c00 + (c10 - c00) * tx;
        const F32 bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    // Five octaves, periods 3..48, summed and normalised, then spread - plain FBM of value noise
    // bunches around the middle, and a map the ramps can actually split into towers and pockets
    // needs its tails.
    F32 tileFbm(F32 x, F32 y, U32 salt)
    {
        F32 v = 0.f;
        v += tileLattice(x, y,  3, salt +  1u) * 0.500f;
        v += tileLattice(x, y,  6, salt +  7u) * 0.250f;
        v += tileLattice(x, y, 12, salt + 13u) * 0.125f;
        v += tileLattice(x, y, 24, salt + 29u) * 0.0625f;
        v += tileLattice(x, y, 48, salt + 53u) * 0.0625f;
        v = llclamp(0.5f + (v - 0.5f) * 1.9f, 0.f, 1.f);
        return v;
    }

    // The square map itself: luminance in an RGB raw image, one FBM read per texel.
    LLPointer<LLImageRaw> makeProceduralNoise(U32 seed)
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw(SS_NOISE_PROC_SIZE, SS_NOISE_PROC_SIZE, 3);
        U8* data = raw->getData();
        if (!data) return nullptr;

        for (S32 y = 0; y < SS_NOISE_PROC_SIZE; ++y)
        {
            U8* row = data + (size_t)y * SS_NOISE_PROC_SIZE * 3;
            const F32 fy = (F32)y / (F32)SS_NOISE_PROC_SIZE;
            for (S32 x = 0; x < SS_NOISE_PROC_SIZE; ++x)
            {
                const F32 v = tileFbm((F32)x / (F32)SS_NOISE_PROC_SIZE, fy, seed);
                const U8 b = (U8)llclamp((S32)(v * 255.f), 0, 255);
                row[x * 3 + 0] = b;
                row[x * 3 + 1] = b;
                row[x * 3 + 2] = b;
            }
        }
        return raw;
    }

    // The built-in vertical curves painted as a strip - the picker's preview for a None profile.
    // Row 0 is v 0, the deck's base, exactly the orientation an authored strip displays in. RGB
    // only: the built-ins carry no base fill, and an RGB strip previews without its alpha
    // channel masquerading as transparency.
    LLPointer<LLImageRaw> makeProfilePreview()
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw(SS_PROFILE_STRIP_W, SS_PROFILE_STRIP_H, 3);
        U8* data = raw->getData();
        if (!data) return nullptr;

        for (S32 y = 0; y < SS_PROFILE_STRIP_H; ++y)
        {
            // Row 0 at v 0: the readback and the shader both treat the first row as the base.
            const F32 v = (F32)y / (F32)(SS_PROFILE_STRIP_H - 1);
            const F32 r = ss_smoothstep(0.70f, 1.25f, v) * 0.7f;
            const F32 g = ss_smoothstep(0.20f, 0.45f, v);
            const F32 b = ss_smoothstep(0.74f, 0.97f, v);

            U8* row = data + (size_t)y * SS_PROFILE_STRIP_W * 3;
            for (S32 x = 0; x < SS_PROFILE_STRIP_W; ++x)
            {
                row[x * 3 + 0] = (U8)llclamp((S32)(r * 255.f), 0, 255);
                row[x * 3 + 1] = (U8)llclamp((S32)(g * 255.f), 0, 255);
                row[x * 3 + 2] = (U8)llclamp((S32)(b * 255.f), 0, 255);
            }
        }
        return raw;
    }

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
        SSAtmoEnvCloudFieldResolver::resolve(track.mCloudField, moisture, convection, phase, track.mFloorZ);

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
            SSAtmoEnvCloudFieldResolver::resolve(track.mUnderField, moisture, convection, phase, track.mFloorZ);
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
    // <SS:Nexii> The base map and its crossfade partner both fall back to the same built-in art
    // when their keyframe is empty, so a fade between an authored texture and None - either
    // direction - fades between real maps instead of cutting through the fallback logic.
    const bool stormy = field.mHasAnvil || convection > 0.6f;
    const LLUUID base_fallback(stormy ? SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS
                                      : SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS);
    deck.mTexture = field.mBaseTexture.notNull() ? field.mBaseTexture : base_fallback;
    deck.mTextureNext = field.mBaseTextureNext.notNull() ? field.mBaseTextureNext : base_fallback;
    deck.mTextureBlend = field.mBaseTextureBlend;

    deck.mDetail = field.mDetailTexture;
    deck.mDetailNext = field.mDetailTextureNext;
    deck.mDetailBlend = field.mDetailTextureBlend;
    deck.mNoise = field.mNoiseTexture;
    deck.mProfile = field.mProfileTexture;

    // <SS:Nexii> No authored map, the deck still gets the feature: a square procedural tile
    // grown from the weather seed. Generated once and folded into the same grid cache an
    // authored map would fill, so the builder, the precipitation gate and the shader's anvil
    // carving all read one field whether it came from a texture or from the seed. Toggleable
    // (SSAtmoCloudProceduralNoise) so a plain sky is always one switch away.
    static LLCachedControl<bool> proc_noise_setting(gSavedSettings, "SSAtmoCloudProceduralNoise", true);
    if (deck.mNoise.isNull())
    {
        if (proc_noise_setting)
        {
            ensureProceduralNoise(deck, salt);
        }
        else if (deck.mNoiseProcRaw.notNull())
        {
            deck.mNoiseProcRaw = nullptr;
            deck.mNoiseProcRef = nullptr;
            deck.mNoiseLuma.clear();
            deck.mNoiseW = 0;
            deck.mNoiseH = 0;
            deck.mNoiseSrcW = 0;
            deck.mNoiseSrcH = 0;
        }
    }

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
    // kept the storm gaps open in what is left. The procedural fallback counts as a map here the
    // same as an authored one.
    deck.mNoiseTileM = (field.mNoiseTexture.notNull() || deck.mNoiseProcRaw.notNull())
        ? SS_NOISE_TILE_M * llmax(0.05f, field.mNoiseScale)
        : 0.f;
    const F32 nimbus = ss_smoothstep(SS_NIMBUS_LO, SS_NIMBUS_HI, moisture);
    deck.mNoiseHole = (1.f - nimbus) * (1.f - SS_STORM_GAP * llclamp(convection, 0.f, 1.f));

    // <SS:Nexii> The storm consolidation: high moisture DRIVING high convection is not the regime
    // the map's carving is for - a rain cloud busy making weather is a large solid mass, not a
    // shredded one. As the two climb together the tower ramp's window widens until most of the
    // map passes it, so the deck's convection variety calms from pockets-and-spikes into the
    // 1-3km connected cells of a thunderstorm, and the pocket suppression eases off with it.
    // The window is baked onto the deck so the shader's carving and the precipitation gate run
    // the same numbers as this builder.
    const F32 storm = ss_smoothstep(0.55f, 0.85f, moisture)
                    * ss_smoothstep(0.45f, 0.75f, convection);
    deck.mNoiseTowerLo = lerp(SS_TOWER_LO, 0.12f, storm);
    deck.mNoiseTowerHi = lerp(SS_TOWER_HI, 0.60f, storm);

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

    // <SS:Nexii> The base veil's lighting, resolved here rather than per fragment: the shade a
    // puff at the deck's floor would wear, run through the same formulas the puff loop below
    // uses - the facing term at a representative low up, the exponential shade through the layer
    // from that height, the beam gate, the gloom. The sheet is a fragment of the same body of
    // cloud as the puffs, so it wears the same colour a puff in its place would, and the blend
    // at the boundary is a lighting match rather than a hope.
    {
        const F32 sun_z = llclamp(light_dir.mV[VZ], -1.f, 1.f);
        const F32 th = (0.5f + llclamp(field.mThicknessM / 500.f, 0.f, 1.f))
                     * (0.35f + 0.65f * field.mCoverage);
        F32 shade;
        if (sun_z >= 0.f)
        {
            shade = expf(-(0.65f / llmax(sun_z, 0.35f) * 0.9f
                           + (1.f - sun_z) * 1.5f) * th);
        }
        else
        {
            shade = expf(-(0.65f / llmax(-sun_z, 0.35f) * 0.9f + 1.5f) * th);
        }
        const F32 facing = 0.5f + 0.2f * light_dir.mV[VZ] * (0.15f - 0.5f) * 2.f;
        const F32 form = lerp(0.65f, facing, beam) * lerp(1.f, shade, beam);

        deck.mSheetColor = (mAmbient + mSunColor * form) * gloom;
        // The inset: just off the deck's floor, deep enough to sit inside the puffs' base fade,
        // shallow enough that the sheet reads as the deck's underside and not as a second layer.
        deck.mSheetZ = field.mBaseHeightM + llclamp(field.mThicknessM * 0.08f, 30.f, 90.f);
        deck.mSheetAlpha = llclamp(0.30f + 0.45f * field.mCoverage, 0.f, 0.7f);
    }

    F64 dist_sum = 0.0;

    for (S32 dy = -cell_radius; dy <= cell_radius; ++dy)
    {
        for (S32 dx = -cell_radius; dx <= cell_radius; ++dx)
        {
            const S32 cx = cx0 + dx;
            const S32 cy = cy0 + dy;

            const F32 gate_raw = clusterUnit(cx, cy, salt) * CLUSTER_WEIGHT
                               + hashUnit(cx, cy, 1u + salt) * (1.f - CLUSTER_WEIGHT);

            // <SS:Nexii> The noise map's say over this cell - two ramps over one sample, taken in
            // the air frame so the pattern drifts with the deck exactly as the cells do. Presence
            // runs the hole window: where the map runs low, the cell's gate is pushed toward a
            // certain skip, and the sky opens. Tower runs the gradient ramp window overlaid on
            // the same values, and decides what this column does with whatever height it keeps.
            F32 presence = 1.f;
            F32 tower = 0.f;
            noiseFieldAt(deck, (F32)(cx + 0.5) * CELL_M, (F32)(cy + 0.5) * CELL_M, presence, tower);

            const F32 gate = gate_raw + (1.f - gate_raw) * (1.f - presence);
            if (gate > field.mCoverage) continue;

            const F32 coreness = llclamp(
                (field.mCoverage - gate) / llmax(field.mCoverage, 0.01f), 0.f, 1.f);

            // <SS:Nexii> The tower shaping. Convection decides how much say the map gets over
            // heights at all - a stable sky keeps every column at the cluster's own height and
            // only the holes differ - and past that the map decides which columns RISE:
            // tower-weighted cells keep the full climb to the lid while the pockets between
            // them are held low, which is what stands a cumulonimbus tower up in the gaps of
            // its own field. The stretch to the towers comes free with the same stroke: a
            // column the map marks high spans the layer's whole convective thickness, base to
            // lid, because nothing pulls it back down.
            const F32 conv_gain = ss_smoothstep(0.12f, 0.55f, convection) * (1.f - storm);
            const F32 height_shape = lerp(1.f, SS_POCKET_H + (1.f - SS_POCKET_H) * tower, conv_gain);

            const F32 cell_height =
                (CLUSTER_EDGE_HEIGHT + (1.f - CLUSTER_EDGE_HEIGHT) * coreness) * height_shape;

            // ...and the anvil: the ramp's tower columns take the lid's spread EARLY - flattened
            // and flared while the convection dial alone still calls for rounded tops - so the
            // anvil forms on the strong towers first and fills in deck-wide as convection rises.
            // The deck's own anvil figure stays the ceiling; the ramp can only bring it forward.
            const F32 cell_anvil = llmax(field.mAnvil,
                                         ss_smoothstep(0.40f, 0.70f, convection) * tower);

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

                // <SS:Nexii> The anvil is a TOP feature and must behave like one. The height
                // weight comes from the authored profile ramp's RED channel when there is one -
                // the same curve the shader's carving samples - and from the built-in window
                // otherwise. Either way it is gated by convection: a stable sky keeps rounded
                // tops whatever the profile says, and the ramp can only bring the anvil
                // forward, never hold it back once the deck-wide figure takes over.
                const F32 ramp_v = (deck.mProfileN > 0)
                    ? profileSample(deck, up, 0)
                    : ss_smoothstep(0.55f, 0.85f, up);
                const F32 anvil_h = ramp_v * ss_smoothstep(0.40f, 0.60f, convection);
                const F32 puff_anvil = llmax(cell_anvil, anvil_h);

                const F32 flat = 1.f + puff_anvil * coreness * (waist + flare - 1.f);

                const LLVector3 to_cam = pos - cam;
                const F32 dist_sq = to_cam.magVecSquared();
                if (dist_sq > FIELD_DRAW_M * FIELD_DRAW_M) continue;

                const bool squashed = dist_sq > mSquashKnee * mSquashKnee;
                if (squashed && sub > 0) continue;

                Puff puff;
                puff.mPosAgent = pos;
                puff.mAnvil = puff_anvil;
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
    static LLStaticHashedString s_base_blend("ss_base_blend");
    static LLStaticHashedString s_detail_blend("ss_detail_blend");
    static LLStaticHashedString s_puff_density("ss_puff_density");
    static LLStaticHashedString s_detail_scale("ss_detail_scale");
    static LLStaticHashedString s_drift_rate("ss_drift_rate");
    static LLStaticHashedString s_noise_tile("ss_noise_tile");
    static LLStaticHashedString s_noise_hole("ss_noise_hole");
    static LLStaticHashedString s_tower_ramp("ss_tower_ramp");
    static LLStaticHashedString s_profile("ss_profile");
    static LLStaticHashedString s_sheet("ss_sheet");
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

    // <SS:Nexii> The puff tessellation toggle. Off (the default), every puff is the single
    // camera-facing quad it has always been. On, each becomes a 4x4 grid of sub-quads the
    // renderer can shear, curl and dissolve per row - the anvil skirt. It exists as a toggle
    // because it is an experiment in shaping: the fragment carving works either way, and the
    // grid is there to see how much of the anvil the GEOMETRY carrying it adds over the
    // fragment work alone.
    static LLCachedControl<bool> tessellate_setting(gSavedSettings, "SSAtmoCloudTessellation", false);
    const bool tessellate = tessellate_setting;

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

        // <SS:Nexii> The crossfade partners on the spare reserved channels (bumpMap, specularMap -
        // same reserved-name rule as altDiffuseMap above), pinned on the current maps when no fade
        // runs so the shader's partner samples never read an unbound unit. The weights mix the
        // pairs per sample in the fragment stage.
        LLViewerFetchedTexture* tex_next = deck.mTextureNextRef.notNull()
            ? deck.mTextureNextRef.get()
            : deck.mTextureRef.get();
        gSSVolCloudProgram.bindTexture(LLShaderMgr::BUMP_MAP, tex_next, LLTexUnit::TT_TEXTURE);
        gSSVolCloudProgram.uniform1f(s_base_blend, deck.mTextureBlend);

        LLViewerFetchedTexture* det_cur = deck.mDetailRef.notNull() ? deck.mDetailRef.get() : deck.mTextureRef.get();
        LLViewerFetchedTexture* det_next = deck.mDetailNextRef.notNull() ? deck.mDetailNextRef.get() : det_cur;
        gSSVolCloudProgram.bindTexture(LLShaderMgr::SPECULAR_MAP, det_next, LLTexUnit::TT_TEXTURE);
        gSSVolCloudProgram.uniform1f(s_detail_blend, deck.mDetailBlend);

        // <SS:Nexii> The convection noise map, bound for the fragment stage's anvil carving -
        // the same map the field was shaped with, authored or procedural, so the shader cuts
        // the puffs by the very geography the towers were grown from. A reserved channel
        // (altDiffuseMap): only reserved names can be bound as textures, see the depthMap note
        // in ssVolCloudF.glsl. Tile metres of zero tells the shader there is nothing to read.
        LLTexture* noise_map = deck.mNoiseRef.notNull()
            ? (LLTexture*)deck.mNoiseRef.get()
            : (LLTexture*)deck.mNoiseProcRef.get();
        if (noise_map)
        {
            gSSVolCloudProgram.bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, noise_map);
        }
        gSSVolCloudProgram.uniform1f(s_noise_tile, noise_map ? deck.mNoiseTileM : 0.f);
        gSSVolCloudProgram.uniform1f(s_noise_hole, deck.mNoiseHole);
        gSSVolCloudProgram.uniform2f(s_tower_ramp, deck.mNoiseTowerLo, deck.mNoiseTowerHi);

        // <SS:Nexii> The vertical profile ramp, bound for the fragment stage's four vertical
        // curves (tower weight, carve guard, cap band, base fill) on the bumpMap2 reserved
        // channel - same rule as altDiffuseMap above: only reserved names can be bound as
        // textures. Sampled clamped at the deck's base and lid, so the strip must address
        // CLAMP, not the fetched default's wrap - or v 0 would blend with v 1 at both rails.
        LLTexture* profile_map = deck.mProfileRef.notNull() ? (LLTexture*)deck.mProfileRef.get() : nullptr;
        if (profile_map)
        {
            gSSVolCloudProgram.bindTexture(LLShaderMgr::BUMP_MAP2, profile_map);
        }
        gSSVolCloudProgram.uniform1f(s_profile, profile_map ? 1.f : 0.f);

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

        // <SS:Nexii> The base veil: one horizontal sheet inset into the deck's floor, camera-
        // centred and drawn BEFORE the deck's puffs, so the field's gaps read filled - the puffs
        // pile up over their own floor and the spaces between them show it. The sheet is the
        // deck's underside, so it is wound to face DOWN (the camera sees its front from below);
        // from above the deck it is a backface and rightly culls - the gaps over a deck open on
        // what is behind the deck, not on its floor. It rides the same shader as the puffs with
        // ss_sheet switched on: same texture, aperiodically read, same lighting vocabulary, the
        // same fog and dome handoff - which is the whole point of it blending rather than
        // sitting under the deck as a second material.
        gSSVolCloudProgram.uniform1f(s_sheet, 1.f);
        {
            // <SS:Nexii> Culling off for the sheet: it is wound to face down - the deck's
            // underside - but the pass's cull state is not this function's to reason about, and
            // a wrongly-fallen winding would silent-drop the whole layer. A two-triangle quad
            // drawn double-sided costs nothing; the veil is soft enough that its back face
            // reading through the deck's gaps from above reads as the floor it is.
            LLGLDisable no_cull(GL_CULL_FACE);

            const F32 z = deck.mSheetZ;

            // <SS:Nexii> The sheet is TILED, on the same air-frame cell grid the puffs are placed
            // on (CELL_M steps about the camera's cell, corners slid back by the drift into world
            // space), not drawn as the one camera-centred rect it used to be. The far-field squash
            // is exact per VERTEX, and the fragment stage un-squashes per fragment along the view
            // ray - but a fragment inside a triangle gets its drawn position by interpolation, and
            // the squash bends the sheet's plane, so a triangle as wide as the old 10 km rect
            // reconstructed a world position tens to hundreds of metres off its true plane point,
            // by an amount that changes with the camera's relation to the sheet: the veil swam
            // across the field with every camera move and its texture would not sit under the
            // puffs. At puff-quad scale the interpolation error collapses to nothing, sheet and
            // field read from one anchored frame, and the per-tile cull keeps the pass inside the
            // same draw radius the puffs run.
            const F32 draw_sq = FIELD_DRAW_M * FIELD_DRAW_M;
            const S32 sheet_radius = llceil(FIELD_DRAW_M / CELL_M);
            const S32 scx0 = llfloor((cam_pos.mV[VX] - drift.mV[0]) / CELL_M);
            const S32 scy0 = llfloor((cam_pos.mV[VY] - drift.mV[1]) / CELL_M);

            gGL.begin(LLRender::TRIANGLES);
            gGL.color4f(deck.mSheetColor.mV[0], deck.mSheetColor.mV[1],
                        deck.mSheetColor.mV[2], deck.mSheetAlpha);
            for (S32 ty = -sheet_radius; ty <= sheet_radius; ++ty)
            {
                for (S32 tx = -sheet_radius; tx <= sheet_radius; ++tx)
                {
                    const F32 x0 = (F32)(scx0 + tx) * CELL_M + drift.mV[0];
                    const F32 y0 = (F32)(scy0 + ty) * CELL_M + drift.mV[1];
                    const F32 x1 = x0 + CELL_M;
                    const F32 y1 = y0 + CELL_M;

                    const F32 mx = 0.5f * (x0 + x1) - cam_pos.mV[VX];
                    const F32 my = 0.5f * (y0 + y1) - cam_pos.mV[VY];
                    if (mx * mx + my * my > draw_sq) continue;

                    // A=(x0,y0) B=(x1,y0) C=(x1,y1) D=(x0,y1); (A,D,C) and (A,C,B) run
                    // front-facing seen from underneath - the old sheet's winding, per tile.
                    // Texcoords are unused on this path.
                    gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x0, y0, z).mV);
                    gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x0, y1, z).mV);
                    gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x1, y1, z).mV);

                    gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x0, y0, z).mV);
                    gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x1, y1, z).mV);
                    gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(LLVector3(x1, y0, z).mV);
                }
            }
            gGL.end();
        }
        gSSVolCloudProgram.uniform1f(s_sheet, 0.f);

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

            // <SS:Nexii> One corner of the puff's quad, parameterised over the billboard: (0,0)
            // is the bottom left, (1,1) the top right, exactly the corners the single quad used.
            // With tessellation on (SSAtmoCloudTessellation) the quad becomes a 4x4 grid of
            // sub-quads, and the top rows shear out along the wind, widen, curl down and
            // dissolve - the anvil skirt. A shear is linear and one quad could carry it; the
            // CURL is not, and the alpha dissolve wants rows to pull apart. That is the whole
            // reason the toggle exists: puffs are a single flat quad each, and no amount of
            // fragment noise can bend what the geometry does not have. Off, the corner maths
            // collapses to exactly the quad that was always drawn.
            const LLVector3 wind3(wind.mV[VX], wind.mV[VY], 0.f);
            auto emit_corner = [&](F32 u, F32 v)
            {
                const F32 skirt = tessellate ? (puff.mAnvil * v * v) : 0.f;

                LLVector3 pos = puff.mPosAgent
                    + right * ((u * 2.f - 1.f) * (1.f + 0.30f * skirt))
                    + up * (v * 2.f - 1.f)
                    + wind3 * (puff.mRadius * 0.55f * skirt);
                pos.mV[VZ] -= puff.mRadius * 0.22f * skirt
                            * (0.5f + 0.5f * fabsf(u * 2.f - 1.f));

                const F32 alpha = puff.mAlpha * (1.f - 0.35f * skirt);

                gGL.color4f(puff.mColor.mV[0], puff.mColor.mV[1], puff.mColor.mV[2], alpha);
                gGL.texCoord2f(u, v);
                gGL.vertex3fv(pos.mV);
            };

            const S32 segs = tessellate ? 4 : 1;
            for (S32 iy = 0; iy < segs; ++iy)
            {
                for (S32 ix = 0; ix < segs; ++ix)
                {
                    const F32 u0 = (F32)ix / segs;
                    const F32 v0 = (F32)iy / segs;
                    const F32 u1 = (F32)(ix + 1) / segs;
                    const F32 v1 = (F32)(iy + 1) / segs;

                    emit_corner(u0, v1); emit_corner(u0, v0); emit_corner(u1, v1);
                    emit_corner(u1, v1); emit_corner(u0, v0); emit_corner(u1, v0);
                }
            }
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
    // <SS:Nexii> The convection noise map: fetched like the other maps, then read back out of
    // VRAM once it has one - the way sculpties read theirs - into the small wrapped grid the
    // builder and the precipitation gate sample on the CPU. The GPU reads the same map through
    // its own binding in render(), for the anvil's carving; the CPU grid is the same geography
    // at field scale. readbackRawImage keeps its raw copy current as better mips stream in, so
    // re-caching whenever that copy's size changes keeps both sides honest through the load.
    if (deck.mNoise.notNull())
    {
        if (deck.mNoiseRef.isNull() || deck.mNoiseRef->getID() != deck.mNoise)
        {
            deck.mNoiseRef = LLViewerTextureManager::getFetchedTexture(
                deck.mNoise, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (deck.mNoiseRef.notNull())
            {
                deck.mNoiseRef->setNoDelete();
            }
            deck.mNoiseW = 0;
            deck.mNoiseH = 0;
            deck.mNoiseSrcW = 0;
            deck.mNoiseSrcH = 0;
            deck.mNoiseLuma.clear();
        }

        if (deck.mNoiseRef.notNull())
        {
            deck.mNoiseRef->addTextureStats((F32)MAX_IMAGE_AREA);
            deck.mNoiseProcRaw = nullptr;
            deck.mNoiseProcRef = nullptr;

            LLImageRaw* raw = deck.mNoiseRef->getRawImage();
            if (!raw || raw->getWidth() < deck.mNoiseRef->getWidth()
                     || raw->getHeight() < deck.mNoiseRef->getHeight())
            {
                deck.mNoiseRef->readbackRawImage();
                raw = deck.mNoiseRef->getRawImage();
            }

            if (raw && (raw->getWidth() != deck.mNoiseSrcW
                     || raw->getHeight() != deck.mNoiseSrcH))
            {
                cacheNoiseGrid(deck, raw);
            }
        }
    }
    else
    {
        // <SS:Nexii> Nothing authored: the procedural map is the noise map. Its CPU grid was
        // folded by the builder; this is where its GPU copy uploads - once per generation -
        // wrapped, mipmapped, and ready for the fragment stage's carving.
        deck.mNoiseRef = nullptr;
        if (deck.mNoiseProcRaw.notNull())
        {
            if (deck.mNoiseProcRef.isNull())
            {
                deck.mNoiseProcRef = LLViewerTextureManager::getLocalTexture(deck.mNoiseProcRaw.get(), true);
                if (deck.mNoiseProcRef.notNull() && deck.mNoiseProcRef->getGLTexture())
                {
                    // The map tiles, so the GL copy has to as well.
                    deck.mNoiseProcRef->getGLTexture()->setAddressMode(LLTexUnit::TAM_WRAP);
                }
            }
        }
        else if (deck.mNoiseW > 0)
        {
            deck.mNoiseLuma.clear();
            deck.mNoiseW = 0;
            deck.mNoiseH = 0;
            deck.mNoiseSrcW = 0;
            deck.mNoiseSrcH = 0;
        }
    }

    // <SS:Nexii> The vertical profile ramp: authored only (none runs the built-in curves), read
    // back through the same ladder as the noise map and folded into one averaged curve per
    // channel. The readback's rows arrive in GL order - row 0 is v 0, the deck's base - which is
    // exactly the orientation the shader's own texture read samples, so CPU and GPU run one
    // profile however the author painted it.
    if (deck.mProfile.notNull())
    {
        if (deck.mProfileRef.isNull() || deck.mProfileRef->getID() != deck.mProfile)
        {
            deck.mProfileRef = LLViewerTextureManager::getFetchedTexture(
                deck.mProfile, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (deck.mProfileRef.notNull())
            {
                deck.mProfileRef->setNoDelete();
                if (deck.mProfileRef->getGLTexture())
                {
                    // The ramp runs base to lid and clamps at both rails; a wrapped strip
                    // would blend its ends together at v 0 and v 1.
                    deck.mProfileRef->getGLTexture()->setAddressMode(LLTexUnit::TAM_CLAMP);
                }
            }
            deck.mProfileN = 0;
            deck.mProfileCurve.clear();
        }

        if (deck.mProfileRef.notNull())
        {
            deck.mProfileRef->addTextureStats((F32)MAX_IMAGE_AREA);

            LLImageRaw* raw = deck.mProfileRef->getRawImage();
            if (!raw || raw->getWidth() < deck.mProfileRef->getWidth()
                     || raw->getHeight() < deck.mProfileRef->getHeight())
            {
                deck.mProfileRef->readbackRawImage();
                raw = deck.mProfileRef->getRawImage();
            }

            if (raw && raw->getHeight() != deck.mProfileN)
            {
                cacheProfileCurve(deck, raw);
            }
        }
    }
    else if (deck.mProfileRef.notNull() || deck.mProfileN > 0)
    {
        deck.mProfileRef = nullptr;
        deck.mProfileCurve.clear();
        deck.mProfileN = 0;
    }
    else if (deck.mProfileProcRef.isNull())
    {
        // <SS:Nexii> Nothing authored: paint the built-in curves once so the picker's
        // placeholder preview has something honest to show for the None state. Display only -
        // the shader runs these curves as maths, never as a texture.
        LLPointer<LLImageRaw> strip = makeProfilePreview();
        if (strip.notNull())
        {
            deck.mProfileProcRef = LLViewerTextureManager::getLocalTexture(strip.get(), true);
        }
    }

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

    // <SS:Nexii> The crossfade partners, fetched only while a fade is live - the same ladder as
    // the primaries, keyed by id so a fade holds one fetch. The detail partner falls back to the
    // dome's cloud noise exactly as the primary does; a partner that lands on the current map is
    // skipped and the renderer pins that pair on the primary, so a fade to the same texture costs
    // nothing. Dropped the moment the weight reaches the rail.
    deck.mTextureNextRef = nullptr;
    deck.mDetailNextRef = nullptr;
    if (deck.mTextureBlend > 0.f && deck.mTextureNext.notNull() && deck.mTextureNext != deck.mTexture)
    {
        if (deck.mTextureNextRef.isNull() || deck.mTextureNextRef->getID() != deck.mTextureNext)
        {
            deck.mTextureNextRef = LLViewerTextureManager::getFetchedTexture(
                deck.mTextureNext, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (deck.mTextureNextRef.notNull())
            {
                deck.mTextureNextRef->setNoDelete();
            }
        }
        if (deck.mTextureNextRef.notNull())
        {
            deck.mTextureNextRef->addTextureStats((F32)MAX_IMAGE_AREA);
        }
    }

    if (deck.mDetailBlend > 0.f)
    {
        const LLUUID detail_cur_id = deck.mDetail.notNull()
            ? deck.mDetail
            : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
        const LLUUID detail_next_id = deck.mDetailNext.notNull()
            ? deck.mDetailNext
            : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
        if (detail_next_id.notNull() && detail_next_id != detail_cur_id)
        {
            if (deck.mDetailNextRef.isNull() || deck.mDetailNextRef->getID() != detail_next_id)
            {
                deck.mDetailNextRef = LLViewerTextureManager::getFetchedTexture(
                    detail_next_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
                if (deck.mDetailNextRef.notNull())
                {
                    deck.mDetailNextRef->setNoDelete();
                }
            }
            if (deck.mDetailNextRef.notNull())
            {
                deck.mDetailNextRef->addTextureStats((F32)MAX_IMAGE_AREA);
            }
        }
    }

    return true;
}

// Box-averages the noise map's raw image into the deck's fixed wrapped grid, luminance only: each destination texel the mean of the source block it covers. The field's structure is kilometres
// wide, so a 64-across grid carries it whole; the point of shrinking it is that the builder and the precipitation gate sample it thousands of times a frame.
void SSVolCloud::cacheNoiseGrid(Deck& deck, LLImageRaw* raw)
{
    const S32 sw = raw->getWidth();
    const S32 sh = raw->getHeight();
    const S32 comps = raw->getComponents();
    if (sw <= 0 || sh <= 0 || comps < 1) return;

    const U8* data = raw->getData();
    deck.mNoiseLuma.assign((size_t)SS_NOISE_GRID * SS_NOISE_GRID, 0.f);

    for (S32 gy = 0; gy < SS_NOISE_GRID; ++gy)
    {
        const S32 sy0 = llclamp((S32)((F64)gy * sh / SS_NOISE_GRID), 0, sh - 1);
        const S32 sy1 = llclamp(llmax(sy0 + 1, (S32)((F64)(gy + 1) * sh / SS_NOISE_GRID)), 1, sh);
        for (S32 gx = 0; gx < SS_NOISE_GRID; ++gx)
        {
            const S32 sx0 = llclamp((S32)((F64)gx * sw / SS_NOISE_GRID), 0, sw - 1);
            const S32 sx1 = llclamp(llmax(sx0 + 1, (S32)((F64)(gx + 1) * sw / SS_NOISE_GRID)), 1, sw);

            F64 sum = 0.0;
            for (S32 y = sy0; y < sy1; ++y)
            {
                const U8* row = data + (size_t)y * sw * comps;
                for (S32 x = sx0; x < sx1; ++x)
                {
                    const U8* px = row + (size_t)x * comps;
                    // Luminance the same way the shaders read their maps - the mean of RGB
                    // when there are three channels to mean, the one channel otherwise.
                    const F32 lum = (comps >= 3)
                        ? (F32)(px[0] + px[1] + px[2]) / 765.f
                        : (F32)px[0] / 255.f;
                    sum += lum;
                }
            }
            deck.mNoiseLuma[(size_t)gy * SS_NOISE_GRID + gx] = (F32)(sum / (F64)((sy1 - sy0) * (sx1 - sx0)));
        }
    }

    deck.mNoiseW = SS_NOISE_GRID;
    deck.mNoiseH = SS_NOISE_GRID;
    deck.mNoiseSrcW = sw;
    deck.mNoiseSrcH = sh;
}

// The procedural fallback map: one square tileable FBM per deck salt, regrown only when the weather seed changes. Both decks of a build derive different patterns from the same seed by the
// salt, and every client sharing the environment derives the same patterns, period - the
// geography is as syncable as the weather that seeds it.
void SSVolCloud::ensureProceduralNoise(Deck& deck, U32 salt)
{
    const U32 seed = SSAtmoMagic::getInstance()->seed() ^ (salt * 0x9E3779B9u);
    if (deck.mNoiseProcRaw.notNull() && deck.mNoiseProcSeed == seed && deck.mNoiseW > 0) return;

    deck.mNoiseProcRaw = makeProceduralNoise(seed);
    deck.mNoiseProcSeed = seed;
    deck.mNoiseProcRef = nullptr;   // the GPU copy re-uploads at fetch time

    if (deck.mNoiseProcRaw.notNull())
    {
        cacheNoiseGrid(deck, deck.mNoiseProcRaw);
    }
    else if (deck.mNoiseW > 0)
    {
        deck.mNoiseLuma.clear();
        deck.mNoiseW = 0;
        deck.mNoiseH = 0;
        deck.mNoiseSrcW = 0;
        deck.mNoiseSrcH = 0;
    }
}

// Folds the profile ramp's raw readback into one averaged curve per channel: each destination row the mean of a source row band across the full width, row 0 the deck's BASE (the readback
// arrives in GL order, the same v the shader's texture read samples). Row count fixed at
// SS_PROFILE_N - a vertical curve needs no more - and the four channels ride along so the
// tower weight, carve guard, cap band and base fill stay separate curves.
void SSVolCloud::cacheProfileCurve(Deck& deck, LLImageRaw* raw)
{
    const S32 sw = raw->getWidth();
    const S32 sh = raw->getHeight();
    const S32 comps = raw->getComponents();
    if (sw <= 0 || sh <= 0 || comps < 1) return;

    const S32 ch = llmin(comps, 4);
    const U8* data = raw->getData();

    deck.mProfileCurve.assign((size_t)SS_PROFILE_N * 4, 0.f);

    for (S32 gy = 0; gy < SS_PROFILE_N; ++gy)
    {
        const S32 sy0 = llclamp((S32)((F64)gy * sh / SS_PROFILE_N), 0, sh - 1);
        const S32 sy1 = llclamp(llmax(sy0 + 1, (S32)((F64)(gy + 1) * sh / SS_PROFILE_N)), 1, sh);

        F64 sum[4] = { 0.0, 0.0, 0.0, 0.0 };
        S32 count = 0;
        for (S32 y = sy0; y < sy1; ++y)
        {
            const U8* row = data + (size_t)y * sw * comps;
            for (S32 x = 0; x < sw; ++x)
            {
                const U8* px = row + (size_t)x * comps;
                for (S32 c = 0; c < ch; ++c)
                {
                    sum[c] += (F32)px[c] / 255.f;
                }
            }
            ++count;
        }

        if (count <= 0) continue;
        for (S32 c = 0; c < 4; ++c)
        {
            // Channels the image does not carry read as white for the tower ramp and black for
            // the rest - a grey-scale strip authors the ramp and nothing else.
            deck.mProfileCurve[(size_t)gy * 4 + c] =
                (c < ch) ? (F32)(sum[c] / (F64)count) : ((c == 0 && comps < 3) ? 1.f : 0.f);
        }
    }

    deck.mProfileN = SS_PROFILE_N;
}

// One linear read of the profile curve: v in deck-height fractions (0 base, 1 lid), channel 0..3.
F32 SSVolCloud::profileSample(const Deck& deck, F32 v, S32 channel) const
{
    const S32 n = deck.mProfileN;
    if (n <= 0 || deck.mProfileCurve.empty()) return 0.f;

    const F32 fv = llclamp(v, 0.f, 1.f) * (F32)(n - 1);
    const S32 i0 = llfloor(fv);
    const S32 i1 = llmin(i0 + 1, n - 1);
    const F32 t = llclamp(v, 0.f, 1.f) - (F32)i0;

    const F32 a = deck.mProfileCurve[(size_t)i0 * 4 + channel];
    const F32 b = deck.mProfileCurve[(size_t)i1 * 4 + channel];
    return a + (b - a) * t;
}

// One wrapped bilinear read of the cached grid at an air-frame position, in map values. Anything that stops the read - no map, no readback yet - answers a negative, which every
F32 SSVolCloud::noiseSample(const Deck& deck, F32 air_x, F32 air_y) const
{
    const S32 w = deck.mNoiseW;
    const S32 h = deck.mNoiseH;
    if (w <= 0 || h <= 0 || deck.mNoiseLuma.empty() || deck.mNoiseTileM <= 0.f) return -1.f;

    const F32 fx = air_x / deck.mNoiseTileM * (F32)w - 0.5f;
    const F32 fy = air_y / deck.mNoiseTileM * (F32)h - 0.5f;

    const S32 ix = llfloor(fx);
    const S32 iy = llfloor(fy);
    const F32 tx = fx - (F32)ix;
    const F32 ty = fy - (F32)iy;

    // The map tiles, exactly as it draws: wrap both axes, so a field kilometres across never
    // falls off the edge of its own pattern.
    const S32 x0 = ((ix % w) + w) % w;
    const S32 y0 = ((iy % h) + h) % h;
    const S32 x1 = (x0 + 1) % w;
    const S32 y1 = (y0 + 1) % h;

    const F32 c00 = deck.mNoiseLuma[(size_t)y0 * w + x0];
    const F32 c10 = deck.mNoiseLuma[(size_t)y0 * w + x1];
    const F32 c01 = deck.mNoiseLuma[(size_t)y1 * w + x0];
    const F32 c11 = deck.mNoiseLuma[(size_t)y1 * w + x1];

    const F32 top = c00 + (c10 - c00) * tx;
    const F32 bot = c01 + (c11 - c01) * tx;
    return top + (bot - top) * ty;
}

// The noise map's two ramps at one point of a deck's field: presence (1 = cloud whole, 0 = a hole the map cut) and tower (0 = pocket, 1 = a rising thermal's column). The hole window's
// strength was baked at build time - moisture's floor lift and convection's storm-gap keep are
// already inside it, so every reader of the field gets the same sky.
void SSVolCloud::noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower) const
{
    presence = 1.f;
    tower = 0.f;

    const F32 n = noiseSample(deck, air_x, air_y);
    if (n < 0.f) return;

    const F32 cut = ss_smoothstep(SS_HOLE_LO, SS_HOLE_HI, n);
    presence = 1.f - (1.f - cut) * deck.mNoiseHole;
    tower = ss_smoothstep(deck.mNoiseTowerLo, deck.mNoiseTowerHi, n);
}

// The deck the weather reads, resolved at build time - see update().
const SSVolCloud::Deck* SSVolCloud::weatherDeck() const
{
    return (mWeatherDeck == 1) ? &mUnder : &mPrimary;
}

// Whether the precipitation gate has anything to say: a built weather deck with a noise map read back. Checked before any rain-shadow work so a plain sky pays nothing.
bool SSVolCloud::precipNoiseReady() const
{
    const Deck* deck = weatherDeck();
    return deck && !deck->mPuffs.empty() && deck->mNoiseW > 0 && deck->mNoiseTileM > 0.f;
}

LLVector2 SSVolCloud::precipNoiseAt(const LLVector3& pos_agent) const
{
    const Deck* deck = weatherDeck();
    if (!deck || deck->mPuffs.empty() || deck->mNoiseW <= 0 || deck->mNoiseTileM <= 0.f)
    {
        return LLVector2(1.f, 0.f);
    }

    // The air frame, the same one the cells are placed in, so the gate moves with the deck.
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    F32 presence = 1.f;
    F32 tower = 0.f;
    noiseFieldAt(*deck,
                 pos_agent.mV[VX] - drift.mV[0],
                 pos_agent.mV[VY] - drift.mV[1],
                 presence, tower);
    return LLVector2(presence, tower);
}

F32 SSVolCloud::precipBaseZ() const
{
    const Deck* deck = weatherDeck();
    return (deck && !deck->mPuffs.empty()) ? deck->mBaseZ : cloudBaseZ();
}

// How solid the under deck is over one point of the sky: the same noise-map presence gate the
// builder ran for the column, read in the air frame so it drifts with the deck. No map read
// back yet answers neutral - the deck is there until proven a hole.
F32 SSVolCloud::underPresenceAt(const LLVector3& pos_agent) const
{
    if (mUnder.mPuffs.empty() || mUnder.mNoiseW <= 0 || mUnder.mNoiseTileM <= 0.f) return 1.f;

    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    F32 presence = 1.f;
    F32 tower = 0.f;
    noiseFieldAt(mUnder,
                 pos_agent.mV[VX] - drift.mV[0],
                 pos_agent.mV[VY] - drift.mV[1],
                 presence, tower);
    return presence;
}

// The picker previews' stand-ins: what the deck is actually running while its authored field is
// None. An authored texture hands back null - the picker then previews the real asset by its
// own uuid, as texture pickers always have.
LLViewerTexture* SSVolCloud::noisePreviewTexture(bool under_deck) const
{
    const Deck& deck = under_deck ? mUnder : mPrimary;
    if (deck.mNoise.notNull()) return nullptr;
    return deck.mNoiseProcRef;
}

LLViewerTexture* SSVolCloud::profilePreviewTexture(bool under_deck) const
{
    const Deck& deck = under_deck ? mUnder : mPrimary;
    if (deck.mProfile.notNull()) return nullptr;
    return deck.mProfileProcRef;
}
