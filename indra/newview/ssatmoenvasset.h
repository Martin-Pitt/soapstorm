/**
 * @file ssatmoenvasset.h
 * @brief Atmo Magic: the unified environment asset schema.
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

#ifndef SS_ATMOENVASSET_H
#define SS_ATMOENVASSET_H

#include <cmath>

#include "llsd.h"
#include "lluuid.h"

#include <cfloat>
#include <map>
#include <string>
#include <vector>

#include "ssatmoenvkeyframe.h"

class LLSettingsSky;

const S32 SS_ATMOENV_VERSION = 1;

const S32 SS_ATMOENV_MIN_TRACKS = 1;
const S32 SS_ATMOENV_MAX_TRACKS = 8;

const F32 SS_ATMOENV_REGION_CEILING = 4096.f;

const F32 SS_ATMOENV_MIN_TRACK_FLOOR = 256.f;

const F32 SS_ATMOENV_WATER_CEILING = 100.f;

// <SS:Nexii> The water height the spinner will take by hand: sky-themed builds put the ocean
// way below the platform, so the typed range runs -10 km to +10 km while the slider keeps its
// honest near-surface dial. Values past the slider's ends show there pinned at the rail.
const F32 SS_ATMOENV_WATER_MIN = -10000.f;
const F32 SS_ATMOENV_WATER_MAX = 10000.f;

const S32 SS_ATMOENV_PREVIEW_STEPS = 100;

inline F64 ss_atmoenv_snap_phase(F64 phase)
{
    const F64 steps = (F64)SS_ATMOENV_PREVIEW_STEPS;
    F64 snapped = std::floor(phase * steps + 0.5) / steps;
    snapped = std::fmod(snapped, 1.0);
    if (snapped < 0.0) snapped += 1.0;
    return snapped;
}

struct SSAtmoEnvWeather
{
    SSAtmoEnvKeyframed<F32> mMoisture{0.f};
    SSAtmoEnvKeyframed<F32> mConvection{0.f};
    SSAtmoEnvKeyframed<F32> mTemperatureC{15.f};

    SSAtmoEnvKeyframed<F32> mWindHeading{0.f};
    SSAtmoEnvKeyframed<F32> mWindSpeed{0.f};

    bool mGustAuto = true;
    SSAtmoEnvKeyframed<F32> mGustDepth{0.f};
    SSAtmoEnvKeyframed<F32> mGustLength{140.f};
    SSAtmoEnvKeyframed<F32> mGustVeer{0.f};

    bool mLightningEnabled = true;
    bool mLightningCharge = true;
    bool mLightningSparks = true;

    bool mLightningAuto = true;
    SSAtmoEnvKeyframed<F32> mLightningIntensity{0.f};

    SSAtmoEnvKeyframed<LLColor3> mLightningColor{LLColor3(0.62f, 0.55f, 1.f)};

    SSAtmoEnvKeyframed<F32> mLightningCoreWhite{0.85f};

    SSAtmoEnvKeyframed<std::string> mPrecipitationOverride{std::string()};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvWater
{
    bool mEnabled = false;

    SSAtmoEnvKeyframed<F32> mHeight{0.f};

    SSAtmoEnvKeyframed<LLColor3> mFogColor{LLColor3(0.f, 0.24f, 0.34f)};
    SSAtmoEnvKeyframed<F32> mFogDensity{16.f};
    SSAtmoEnvKeyframed<F32> mUnderwaterModifier{0.25f};

    SSAtmoEnvKeyframed<F32> mFresnelScale{0.4f};
    SSAtmoEnvKeyframed<F32> mFresnelOffset{0.5f};

    SSAtmoEnvKeyframed<LLUUID> mNormalMap{LLUUID::null};
    SSAtmoEnvKeyframed<LLVector2> mLargeWaveSpeed{LLVector2(0.f, -0.2f)};
    SSAtmoEnvKeyframed<LLVector2> mSmallWaveSpeed{LLVector2(0.f, -0.3f)};

    SSAtmoEnvKeyframed<F32> mNormalScaleX{2.f};
    SSAtmoEnvKeyframed<F32> mNormalScaleY{2.f};
    SSAtmoEnvKeyframed<F32> mNormalScaleZ{2.f};

    SSAtmoEnvKeyframed<F32> mRefractionScaleAbove{0.03f};
    SSAtmoEnvKeyframed<F32> mRefractionScaleBelow{0.2f};
    SSAtmoEnvKeyframed<F32> mBlurMultiplier{0.04f};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

const S32 SS_ATMOENV_MAX_SUNS = 4;

struct SSAtmoEnvCelestialBody
{
    enum EKind { SUN = 0, PLANET = 1, MOON = 2 };

    EKind mKind = PLANET;
    std::string mName = "Body";

    bool mNameCustom = false;

    S32 mParentIndex = -1;

    F32 mDiameterM = 1.0e7f;
    F32 mMassRelative = 1.f;

    F32 mOrbitalRadius = 1.0e8f;
    F32 mOrbitalInclinationDeg = 0.f;
    F32 mOrbitalPhaseDeg = 0.f;

    F64 mOrbitalPeriodSeconds = 0.0;
    F64 mRotationPeriodSeconds = 0.0;

    F32 mAxialTiltDeg = 0.f;

    F32 mLatitudeDeg = 50.f;

    bool mEmissive = false;

    bool mPhaseShaded = true;

    F64 mSpinPeriodSeconds = 0.0;

    bool mIsHome = false;

    bool mIsLightEmitter = false;

    S32 mBoundPartnerIndex = -1;

    LLUUID mCustomTexture;

    bool mHasRing = false;
    F32 mRingInnerRadius = 1.5f;
    F32 mRingOuterRadius = 2.2f;
    LLUUID mRingTexture;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvPlanetary
{
    std::vector<SSAtmoEnvCelestialBody> mBodies;

    F32 mSunPlanetScale = 1.f;
    F32 mPlanetMoonScale = 1.f;

    S32 homeBodyIndex() const;
    bool setHomeBody(S32 index);

    std::vector<S32> lightEmitterIndices() const;
    bool canSetLightEmitter(S32 index) const;

    S32 addBody(SSAtmoEnvCelestialBody::EKind kind, S32 preferred_parent_index = -1);

    bool removeBody(S32 index);

    S32 effectiveParent(S32 index) const;

    void normalizeSunTopology();

    void autoNameBodies();

    bool setBoundPartner(S32 a, S32 b);
    bool clearBoundPartner(S32 index);

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvCloudField
{
    SSAtmoEnvCloudField();

    // <SS:Nexii> The secondary field a sky-themed build hangs below its platform: disabled by
    // default (no undercloud unless asked for), authored rather than auto-derived, and seeded to
    // a low flat deck the author dials down to wherever the build's floor wants its cloud base.
    static SSAtmoEnvCloudField under();

    // Whether this field renders at all. The primary deck is always on - its empty state is
    // coverage-driven - so only the under deck's flag is ever false.
    bool mEnabled = true;

    bool mAuto = true;

    SSAtmoEnvKeyframed<F32> mBaseHeightM{800.f};
    SSAtmoEnvKeyframed<F32> mBaseThicknessM{300.f};

    SSAtmoEnvKeyframed<F32> mCoverageScale{1.f};

    SSAtmoEnvKeyframed<LLUUID> mBaseTexture;
    SSAtmoEnvKeyframed<LLUUID> mDetailTexture;

    SSAtmoEnvKeyframed<F32> mTextureMix{0.4f};

    SSAtmoEnvKeyframed<F32> mPuffDensity{0.8f};

    SSAtmoEnvKeyframed<F32> mDetailScale{3.f};

    SSAtmoEnvKeyframed<F32> mDriftRate{1.f};

    SSAtmoEnvKeyframed<F32> mStormDarkening{0.85f};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvCloudDome
{
    // <SS:Nexii> The dome layer's own ALTITUDE, metres - what a metre of camera travel is worth to the parallax, and the one authority the disc occlusion shares (doc/atmo_magic_cloud_parallax.md).
    // Authored here rather than borrowed from max altitude, which is an atmosphere ceiling dialled for haze and has no business setting where a cloud sits. mAuto hands the number back to the
    // volumetric field's derivation - cirrus-high while the field is empty, merging down onto the deck's mid-height as coverage builds - for anyone who wants dome and deck to agree at the rim
    // without dialling it themselves.
    bool mAuto = false;
    SSAtmoEnvKeyframed<F32> mHeightM{6000.f};

    SSAtmoEnvKeyframed<LLColor3> mColor{LLColor3(0.4099f, 0.4099f, 0.4099f)};

    SSAtmoEnvKeyframed<F32> mCoverage{0.2699f};
    SSAtmoEnvKeyframed<F32> mScale{0.4199f};
    SSAtmoEnvKeyframed<F32> mVariance{0.f};

    SSAtmoEnvKeyframed<LLVector2> mScrollRate{LLVector2(0.2f, 0.01f)};

    SSAtmoEnvKeyframed<F32> mDensityX{1.f};
    SSAtmoEnvKeyframed<F32> mDensityY{0.526f};
    SSAtmoEnvKeyframed<F32> mDensityD{1.f};
    SSAtmoEnvKeyframed<F32> mDetailX{1.f};
    SSAtmoEnvKeyframed<F32> mDetailY{0.526f};
    SSAtmoEnvKeyframed<F32> mDetailD{1.f};

    SSAtmoEnvKeyframed<LLUUID> mNoiseTexture{LLUUID::null};

    static const char* const CLOUD_TEXTURE_LAYERED;
    static const char* const CLOUD_TEXTURE_CUMULONIMBUS;
    static const char* const CLOUD_TEXTURE_ALTOCUMULUS;

    static const char* const BODY_TEXTURE_SUN;
    static const char* const BODY_TEXTURE_MOON;

    void fromSettingsSky(const LLSettingsSky& sky);

    void addKeyframesFromSky(const LLSettingsSky& sky, F64 phase);

    void collapseConstantKeyframes();

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvAtmosphere
{
    SSAtmoEnvKeyframed<LLColor3> mAmbientColor{LLColor3(0.25f, 0.25f, 0.25f)};
    SSAtmoEnvKeyframed<LLColor3> mBlueHorizon{LLColor3(0.4954f, 0.4954f, 0.6399f)};
    SSAtmoEnvKeyframed<LLColor3> mBlueDensity{LLColor3(0.2447f, 0.4487f, 0.7599f)};
    SSAtmoEnvKeyframed<LLColor3> mSunlightColor{LLColor3(0.7342f, 0.7815f, 0.8999f)};

    SSAtmoEnvKeyframed<F32> mHazeHorizon{0.19f};
    SSAtmoEnvKeyframed<F32> mHazeDensity{0.7f};

    SSAtmoEnvKeyframed<F32> mSkyMoistureLevel{0.f};
    SSAtmoEnvKeyframed<F32> mSkyDropletRadius{800.f};
    SSAtmoEnvKeyframed<F32> mSkyIceLevel{0.f};

    SSAtmoEnvKeyframed<F32> mDensityMultiplier{0.0001f};
    SSAtmoEnvKeyframed<F32> mDistanceMultiplier{0.8f};
    SSAtmoEnvKeyframed<F32> mMaxAltitude{1605.f};

    SSAtmoEnvKeyframed<F32> mReflectionProbeAmbiance{0.f};
    SSAtmoEnvKeyframed<F32> mSceneGamma{1.f};

    SSAtmoEnvKeyframed<F32> mStarBrightness{250.f};
    SSAtmoEnvKeyframed<F32> mGlowFocus{0.096f};
    SSAtmoEnvKeyframed<F32> mGlowSize{1.75f};

    SSAtmoEnvKeyframed<F32> mMoonBrightness{0.5f};

    // <SS:Nexii> Atmo Magic: the horizon clip. When on, the sky dome is split at the horizon plane
    // and its lower half takes a depth slot one step nearer than the cloud layer (the
    // LL_SHADER_CONST_HORIZON_DEPTH shader const against the clouds' 0.99998 and the celestial
    // discs' 0.99999 - see skyF.glsl), so the sun, moon, stars, planetary bodies and the dome's own
    // cloud layer are hidden by the dome the moment they set instead of glowing through the world
    // below it. A look, not a dial: there is no meaningful "partially clipped", and nothing here
    // follows the day, so this is a plain authored flag rather than a keyframed value - the same
    // call mGustAuto makes.
    bool mHorizonClip = true;

    void fromSettingsSky(const LLSettingsSky& sky);

    void addKeyframesFromSky(const LLSettingsSky& sky, F64 phase);

    void collapseConstantKeyframes();

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvWeatherInfluence
{
    bool mEnabled = true;

    bool mCloudCoverEnabled = true;
    F32  mCloudCoverStrength = 1.f;

    bool mWindScrollEnabled = true;
    F32  mWindScrollStrength = 1.f;

    bool mHazeEnabled = true;
    F32  mHazeStrength = 1.f;

    bool mStormDarkeningEnabled = true;
    F32  mStormDarkeningStrength = 1.f;

    bool mColdSkyEnabled = true;
    F32  mColdSkyStrength = 1.f;

    bool mRainbowEnabled = true;
    F32  mRainbowStrength = 1.f;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// Values for SSAtmoEnvTrack::mWeatherSourceDeck.
const S32 SS_ATMOENV_DECK_DERIVED = -1;
const S32 SS_ATMOENV_DECK_MAIN    = 0;
const S32 SS_ATMOENV_DECK_UNDER   = 1;

struct SSAtmoEnvTrack
{
    std::string mName = "Ground";

    F32 mFloorZ = 0.f;

    F32 mTransitionBuffer = 15.f;

    F64 mDayLengthSeconds = 4.0 * 60.0 * 60.0;
    F64 mDayOffsetSeconds = 0.0;

    SSAtmoEnvWater    mWater;
    SSAtmoEnvWeather  mWeather;
    SSAtmoEnvPlanetary mPlanetary;

    SSAtmoEnvWeatherInfluence mWeatherInfluence;

    SSAtmoEnvAtmosphere mAtmosphere;
    SSAtmoEnvCloudField mCloudField;
    SSAtmoEnvCloudDome  mCloudDome;

    // <SS:Nexii> The optional under deck: a second volumetric field at the bottom of a
    // sky-themed build - cloud layer below the platform, ocean way below that - whose base
    // height is authored straight to wherever the build's floor sits. Seeded off, manual,
    // and low: see SSAtmoEnvCloudField::under().
    SSAtmoEnvCloudField mUnderField = SSAtmoEnvCloudField::under();

    // <SS:Nexii> Which deck precipitation falls from. Derived by default - the lowest enabled deck
    // above the reference surface, which resolves to the main deck for a sky build because the
    // under deck hangs below the platform floor. Authored only for the case of wanting weather from
    // the upper deck while a lower one is enabled for looks. Not keyframed: it is a property of the
    // track, not of a moment. See doc/atmo_magic_env_ui.md.
    S32 mWeatherSourceDeck = SS_ATMOENV_DECK_DERIVED;

    F64 currentDayCyclePhase() const;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// <SS:Nexii> World templates. An author arrives with a theme - a coastline, a sky archipelago, a
// permanent barrage - and a theme is never one setting: a sky world is water kilometres down AND a
// deck under the landmass AND another above it. Seeding those together is what makes the first
// screen useful to someone who came to build a place rather than to tune haze.
//
// Seeding is one-shot: the template is copied into the track and forgotten, with no live link back.
// A link would mean drift and reconciliation for no gain, since the whole point is that the author
// dials the result afterwards. Numbers here are starting points, not authored presets - tune them
// in place. See doc/atmo_magic_env_ui.md.
struct SSAtmoEnvTemplate
{
    const char* mKey;
    const char* mLabel;

    F64 mDayLengthHours;

    bool mWaterEnabled;
    F32  mWaterHeightM;
    LLColor3 mWaterFogColor;
    F32  mWaterFogDensity;

    F32  mDeckBaseM;
    F32  mDeckThicknessM;
    F32  mDeckCoverage;
    F32  mDeckStormDarkening;

    bool mUnderEnabled;
    F32  mUnderBaseM;
    F32  mUnderThicknessM;

    bool mDomeAuto;
    F32  mDomeHeightM;
    F32  mDomeCoverage;

    F32  mTemperatureC;
    F32  mMoisture;
    F32  mConvection;
    F32  mWindSpeed;

    LLColor3 mBlueHorizon;
    LLColor3 mBlueDensity;
    F32  mHazeDensity;
    F32  mMaxAltitudeM;
};

const std::vector<SSAtmoEnvTemplate>& ssAtmoEnvTemplates();

// Returns false only for an unknown key. Everything the template names is overwritten on the track,
// keyframes included - a seed is destructive by design, which is why the UI confirms first.
bool ssAtmoEnvApplyTemplate(SSAtmoEnvTrack& track, const std::string& key);

struct SSAtmoEnvAsset
{
    std::string mName = "New Atmo Environment";

    std::vector<SSAtmoEnvTrack> mTracks;

    // <SS:Nexii> The environment's own precipitation types, name to serialised SSPrecipPreset.
    // Two tiers exist: the ones shipped with the viewer, which we author, and these, which the
    // region's author derives from them. Held as LLSD rather than SSPrecipPreset so the asset
    // schema does not have to depend on the particle system's headers, and so a type authored by
    // a newer build survives a round trip through an older one instead of being silently dropped.
    //
    // Derived types carry a full copy of their parent, never a reference: a viewer update that
    // retunes stock rain must not silently change a shipped region. Referenced shipped types are
    // copied in here on save for the same reason, which is what makes an environment
    // self-contained and an unresolvable type reference impossible.
    // See doc/atmo_magic_env_ui.md.
    std::map<std::string, LLSD> mPrecipitationTypes;

    static SSAtmoEnvAsset makeDefault();

    bool addTrack();
    bool removeTrack(S32 index);

    F32 trackCeilingZ(S32 index) const;

    S32 sortTracksByAltitude(S32 follow_index = -1);

    std::string nextDefaultTrackName() const;

    bool visibleWaterHeight(F32& out_height) const;

    LLSD asLLSD() const;

    bool fromLLSD(const LLSD& sd, std::string& out_error);
};

// <SS:Nexii> Stages the environment's own precipitation types into the live preset list, so every
// consumer resolves a type by name without needing to know which tier it came from. Called when an
// environment is adopted; the manager drops them again on unload.
void ssAtmoEnvStagePrecipTypes(const SSAtmoEnvAsset& asset);

// Copies the definition of every shipped type the asset's keyframes name into the asset itself.
// Run before saving: it is what makes an environment self-contained, so a region opened on a build
// whose shipped set differs still renders the precipitation its author chose.
void ssAtmoEnvEmbedReferencedPrecipTypes(SSAtmoEnvAsset& asset);

#endif
