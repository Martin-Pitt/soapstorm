/**
 * @file ssatmoenvapplier.h
 * @brief Atmo Magic: applies the environment asset to the live sky and water.
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

#ifndef SS_ATMOENVAPPLIER_H
#define SS_ATMOENVAPPLIER_H

#include "llsingleton.h"
#include "ssatmoenvskymodulator.h"
#include "llsettingssky.h"
#include "llsettingswater.h"
#include "llpointer.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3math.h"
#include "v3color.h"

#include <vector>

struct SSAtmoEnvTrack;

struct SSAtmoEnvBillboard
{
    S32 mBodyIndex = -1;

    LLVector3 mDirection;
    F32 mAngularDiameterDeg = 0.f;
    LLUUID mTexture;
    bool mIsSun = false;

    LLVector3 mSunDirection;

    F32 mSunlight = 1.f;

    bool mEmissive = false;
    bool mPhaseShaded = true;
};

class SSAtmoEnvApplier : public LLSingleton<SSAtmoEnvApplier>
{
    LLSINGLETON(SSAtmoEnvApplier);
    ~SSAtmoEnvApplier() = default;

public:
    void apply();

    const SSAtmoEnvSkyModulation& lastModulation() const { return mLastModulation; }

    const LLVector2& cloudDriftMetres() const { return mCloudDriftM; }

    // <SS:Nexii> The dome cloud layer's altitude in metres, authored or derived - the single authority the parallax and the disc occlusion both scale by (doc/atmo_magic_cloud_parallax.md).
    // Resolved per call rather than cached with the rest of the sky walk: the auto derivation reads the volumetric field's LIVE coverage, which moves between applies.
    F32 cloudDomeAltitudeMetres() const;
    static F32 autoCloudDomeAltitudeMetres();

    // <SS:Nexii> Whether the sky dome's lower half takes the nearer depth slot that clips what it
    // draws over at the horizon (SSAtmoEnvAtmosphere::mHorizonClip). Sampled at the applied phase
    // like the dome altitude; the sky pool reads it when it binds the dome shader.
    bool horizonClip() const { return mHorizonClip; }

    const LLVector3& moonSunDirection() const { return mMoonSunDir; }

    bool sunSlotEmissive() const { return mSunSlotEmissive; }
    bool moonSlotEmissive() const { return mMoonSlotEmissive; }
    bool sunSlotPhaseShaded() const { return mSunSlotPhaseShaded; }
    bool moonSlotPhaseShaded() const { return mMoonSlotPhaseShaded; }
    const LLVector3& sunSlotSunDirection() const { return mSunSlotSunDir; }
    F32 sunSlotSunlight() const { return mSunSlotSunlight; }
    F32 moonSlotSunlight() const { return mMoonSlotSunlight; }

    F32 sunSlotAngularDeg() const { return mSunSlotAngularDeg; }
    F32 moonSlotAngularDeg() const { return mMoonSlotAngularDeg; }

    // <SS:Nexii> How much of the sun slot's disc has cleared the horizon: 0 fully set, 1 fully risen, ramping across the disc's OWN angular span with the share of the disc that sheds light on
    // the observer (the risen area). The sky dome, the dome clouds and the atmospheric module ramp their sun glow and haze on this instead of snapping the whole sunrise/sunset horizon on the
    // moment the disc's centre crosses zero (skyV.glsl, cloudsV.glsl, atmosphericsFuncs.glsl). Zero unless an ACTIVE environment is driving the sky, so a plain EEP sky keeps stock's step.
    F32 sunRiseFraction() const { return mActive ? mSunRiseFraction : 0.f; }

    const LLVector3& observerPole() const { return mObserverPole; }

    void renderCelestialDebug();

    static F32 celestialAngularFromScale(F32 scale);

    bool isActive() const { return mActive; }

    bool waterPlaneOn() const { return mWaterPlaneOn; }

    const std::vector<SSAtmoEnvBillboard>& celestialBillboards() const { return mBillboards; }

    static F32 celestialDiscScale(F32 angular_diameter_deg);

private:
    void activate();
    void deactivate();
    void install();

    void applySky(const SSAtmoEnvTrack& track, F64 phase,
                  const SSAtmoEnvSkyModulation& mod);
    void applyWater(const SSAtmoEnvTrack& track, F64 phase,
                    const SSAtmoEnvSkyModulation& mod);

    SSAtmoEnvSkyModulation computeModulation(const SSAtmoEnvTrack& track, F64 phase);

    F32 sunElevationSin(const SSAtmoEnvTrack& track, F64 phase) const;

    bool mWasPrecipitating = false;
    F32 mSecondsSinceRainStopped = -1.f;
    F64 mLastTrailUpdate = 0.0;

    SSAtmoEnvSkyModulation mLastModulation;
    LLVector2 mCloudDriftM;

    // <SS:Nexii> The dome's authored altitude and its auto flag, sampled at the applied phase - see cloudDomeAltitudeMetres.
    bool mCloudDomeAuto = false;
    F32 mCloudDomeHeightM = 6000.f;

    // <SS:Nexii> The horizon clip, sampled at the applied phase - see horizonClip.
    bool mHorizonClip = true;

    F32 mMoonSlotBrightness = 1.f;

    LLVector3 mMoonSunDir;
    bool mSunSlotEmissive = false;
    bool mMoonSlotEmissive = false;
    bool mSunSlotPhaseShaded = true;
    bool mMoonSlotPhaseShaded = true;
    LLVector3 mSunSlotSunDir;
    F32 mSunSlotSunlight = 1.f;
    F32 mMoonSlotSunlight = 1.f;
    LLVector3 mObserverPole = LLVector3::z_axis;
    F32 mSunSlotAngularDeg = 0.53f;
    F32 mMoonSlotAngularDeg = 0.53f;

    // <SS:Nexii> The sun slot's risen fraction, sampled at the applied phase - see sunRiseFraction.
    F32 mSunRiseFraction = 0.f;

    std::vector<LLPointer<class LLHUDText> > mDebugLabels;
    void releaseDebugLabels();

    struct DebugMark
    {
        std::string mName;
        LLVector3 mDirection;
        F32 mAngularDiameterDeg = 0.f;
        F32 mSunlight = 1.f;
        bool mEmissive = false;
        bool mIsSunSlot = false;
        bool mIsMoonSlot = false;
    };
    std::vector<DebugMark> mDebugMarks;
    void applyWaterDefaults();

    void setWaterRendering(bool enabled);
    bool mWaterDerendered = false;
    bool mWaterPlaneOn = false;

    void applyCelestial(const SSAtmoEnvTrack& track, F64 phase);

    bool mActive = false;

    LLSettingsSky::ptr_t   mSky;
    LLSettingsWater::ptr_t mWater;

    LLSettingsWater::ptr_t mDefaultWater;

    F32 mGlowG = 0.f;

    F32    mPrevWorldZ = 0.f;
    bool   mPrevWorldZValid = false;
    LLUUID mPrevRegionID;

    bool mSkyCacheValid = false;
    LLColor3 mLastAmbient;
    LLColor3 mLastBlueHorizon;
    LLColor3 mLastBlueDensity;
    LLColor3 mLastSunlight;
    F32 mLastHazeHorizon = 0.f;
    F32 mLastHazeDensity = 0.f;
    F32 mLastSkyMoisture = 0.f;
    F32 mLastSkyDroplet = 0.f;
    F32 mLastSkyIce = 0.f;
    F32 mLastProbeAmbiance = 0.f;
    F32 mLastDensityMult = 0.f;
    F32 mLastDistanceMult = 0.f;
    F32 mLastMaxY = 0.f;
    F32 mLastGamma = 0.f;
    F32 mLastStarBrightness = 0.f;
    F32 mLastMoonBrightness = 0.f;
    LLColor3 mLastGlow;

    LLColor3 mLastCloudColor;
    F32 mLastCloudCoverage = 0.f;
    F32 mLastCloudScale = 0.f;
    F32 mLastCloudVariance = 0.f;
    LLVector2 mLastCloudScroll;
    LLColor3 mLastCloudDensity;
    LLColor3 mLastCloudDetail;
    LLUUID mLastCloudNoise;

    bool mCelestialCacheValid = false;
    LLVector3 mLastSunDir;
    LLVector3 mLastMoonDir;
    F32 mLastSunScale = 0.f;
    F32 mLastMoonScale = 0.f;
    LLUUID mLastSunTexture;
    LLUUID mLastMoonTexture;

    std::vector<SSAtmoEnvBillboard> mBillboards;

    bool mWaterCacheValid = false;
    LLColor3 mLastFogColor;
    F32 mLastFogDensity = 0.f;
    F32 mLastFogMod = 0.f;
    F32 mLastFresnelScale = 0.f;
    F32 mLastFresnelOffset = 0.f;
    LLUUID mLastNormalMap;
    LLVector3 mLastNormalScale;
    LLVector2 mLastWave1;
    LLVector2 mLastWave2;
    F32 mLastScaleAbove = 0.f;
    F32 mLastScaleBelow = 0.f;
    F32 mLastBlur = 0.f;
};

#endif
