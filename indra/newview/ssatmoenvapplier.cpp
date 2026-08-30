/**
 * @file ssatmoenvapplier.cpp
 * @brief See ssatmoenvapplier.h.
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

#include "ssatmoenvapplier.h"

#include "ssatmoenvweatherstate.h"

#include "llhudobject.h"
#include "llfontgl.h"
#include "llhudtext.h"

#include "pipeline.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llsky.h"
#include "llvosky.h"
#include "llsettingsvo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvplanetarystate.h"
#include "ssatmoenvtrackstate.h"
#include "ssvolcloud.h" // <SS:Nexii> the auto dome altitude reads the volumetric deck

#include <algorithm>
#include <cmath>

namespace
{
    const F32 SLIDER_SCALE_GLOW_R(20.0f);
    const F32 SLIDER_SCALE_GLOW_B(-5.0f);

    const F32 TELEPORT_JUMP_M(60.f);

    const F32 CELESTIAL_SCALE_MIN(0.1f);
    const F32 CELESTIAL_SCALE_MAX(20.f);

    const F32 BILLBOARD_MIN_DIAMETER_DEG(0.05f);

    // Shortest arc taking +X onto a direction - the engine's own sun/moon rotation convention, inverted.
    LLQuaternion quat_from_direction(const LLVector3& dir)
    {
        LLQuaternion quat;
        LLVector3 axis = LLVector3::x_axis % dir;
        if (axis.normalize() < 0.0001f)
        {
            if (dir.mV[VX] < 0.f)
            {
                quat.setAngleAxis(F_PI, LLVector3::z_axis);
            }
            return quat;
        }
        quat.setAngleAxis(acosf(llclamp(LLVector3::x_axis * dir, -1.f, 1.f)), axis);
        return quat;
    }
}

// Singleton shell.
SSAtmoEnvApplier::SSAtmoEnvApplier()
{
}

// Angular diameter to EEP's disc scale.
F32 SSAtmoEnvApplier::celestialDiscScale(F32 angular_diameter_deg)
{
    return llclamp(angular_diameter_deg / SS_ATMOENV_REFERENCE_DISC_DEG,
                   CELESTIAL_SCALE_MIN, CELESTIAL_SCALE_MAX);
}

// <SS:Nexii> The dome band's altitude derivation. The band sits at its authored dome height while
// the air is calm and merges down onto the deck's mid-altitude as the deck's coverage builds, so
// band and deck agree about where the cloud IS exactly as they merge at the rim. As convection
// anvils the deck, the merge source descends onto the deck's lid - the same ramp that flattens
// the deck's own tops (SSAtmoEnvCloudFieldResolver::mAnvil) - so by full anvil the band hangs
// just over the deck's max height and the two read as one integrated structure. What the dome's
// Auto flag hands the dry altitude back to, and what the floater shows in the greyed-out row.
static const F32 SS_CIRRUS_M         = 6000.f;
static const F32 SS_CIRRUS_LID_GAP_M = 300.f;
static const F32 SS_ANVIL_ONSET      = 0.6f;
static const F32 SS_ANVIL_FULL       = 0.9f;

F32 SSAtmoEnvApplier::cirrusAltitudeMetres() const
{
    const F32 dry = mCloudDomeAuto ? SS_CIRRUS_M : llmax(mCloudDomeHeightM, 1.f);

    SSVolCloud* vol = SSVolCloud::getInstance();
    if (!vol || vol->empty()) return dry;

    const F32 anvil = llclamp((mLastConvection - SS_ANVIL_ONSET)
                              / (SS_ANVIL_FULL - SS_ANVIL_ONSET), 0.f, 1.f);
    const F32 lid = llmax(vol->cloudTopZ() + SS_CIRRUS_LID_GAP_M, 300.f);
    return lerp(dry, lid, anvil);
}

F32 SSAtmoEnvApplier::autoCloudDomeAltitudeMetres()
{
    SSVolCloud* vol = SSVolCloud::getInstance();
    const F32 source = instance().cirrusAltitudeMetres();
    if (!vol || vol->empty()) return source;

    const F32 merge = cubic_step((vol->lastCoverage() - 0.05f) / 0.25f);
    const F32 deck_mid = (vol->cloudBaseZ() + vol->cloudTopZ()) * 0.5f;
    return lerp(source, llmax(deck_mid, 300.f), merge);
}

// The altitude the shaders actually get, WORLD height: the dome band tracks the deck through the
// merge derivation - the authored height governs the calm-air source, the deck pulls it down.
F32 SSAtmoEnvApplier::cloudDomeAltitudeMetres() const
{
    return autoCloudDomeAltitudeMetres();
}

// Per-frame: resolve the primary track, evaluate its keyframes at the phase, and push sky/water/celestial through EEP's ENV_LOCAL slot.
void SSAtmoEnvApplier::apply()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    const bool want_active = enabled && mgr->hasAsset() && !mgr->asset().mTracks.empty();

    if (!want_active)
    {
        if (mActive)
        {
            deactivate();
        }
        return;
    }

    if (!mActive)
    {
        activate();
    }
    else if (!LLEnvironment::instance().hasEnvironment(LLEnvironment::ENV_LOCAL))
    {
        install();
    }

    const SSAtmoEnvAsset& asset = mgr->asset();

    const F32 world_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    LLViewerRegion* region = gAgent.getRegion();
    const LLUUID region_id = region ? region->getRegionID() : LLUUID::null;

    const bool teleported = !mPrevWorldZValid
        || region_id != mPrevRegionID
        || llabs(world_z - mPrevWorldZ) > TELEPORT_JUMP_M;

    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(
        asset, world_z, mPrevWorldZValid ? mPrevWorldZ : world_z, teleported);

    mPrevWorldZ = world_z;
    mPrevWorldZValid = true;
    mPrevRegionID = region_id;

    S32 track_index = blend.mPrimaryTrack;
    if (track_index < 0 || track_index >= static_cast<S32>(asset.mTracks.size()))
    {
        track_index = 0;
    }
    const SSAtmoEnvTrack& track = asset.mTracks[static_cast<size_t>(track_index)];

    // <SS:Nexii> The home body's radius - the curvature authority the dome cloud's deck mapping
    // curves around (cloudsF.glsl, fed by lldrawpoolwlsky). Zero when the track carries no home
    // body, which leaves the shader on its flat-deck fallback.
    mHomePlanetRadiusM = 0.f;
    const S32 home_index = track.mPlanetary.homeBodyIndex();
    if (home_index >= 0 && home_index < static_cast<S32>(track.mPlanetary.mBodies.size()))
    {
        mHomePlanetRadiusM =
            0.5f * track.mPlanetary.mBodies[static_cast<size_t>(home_index)].mDiameterM;
    }

    const F64 phase = mgr->hasPreviewPhaseOverride()
        ? mgr->previewPhaseOverride()
        : track.currentDayCyclePhase();

    const SSAtmoEnvSkyModulation mod = computeModulation(track, phase);

    {
        static LLCachedControl<bool> overlay(gSavedSettings, "SSAtmoPlanetaryDebugOverlay", false);
        if (!overlay && !mDebugLabels.empty())
        {
            releaseDebugLabels();
        }
    }

    applySky(track, phase, mod);
    applyCelestial(track, phase);

    mWaterPlaneOn = track.mWater.mEnabled;
    if (track.mWater.mEnabled)
    {
        applyWater(track, phase, mod);
        setWaterRendering(true);
    }
    else
    {
        applyWaterDefaults();
        setWaterRendering(false);
    }
}

// Inverse of celestialDiscScale, for the overlay.
F32 SSAtmoEnvApplier::celestialAngularFromScale(F32 scale)
{
    return scale * SS_ATMOENV_REFERENCE_DISC_DEG;
}

// Kills the celestial debug HUD texts.
void SSAtmoEnvApplier::releaseDebugLabels()
{
    for (LLPointer<LLHUDText>& label : mDebugLabels)
    {
        if (label.notNull()) label->markDead();
    }
    mDebugLabels.clear();
}

// Labels every resolved body on screen - warm for the sun slot, cool for the moon slot, green for the rest.
void SSAtmoEnvApplier::renderCelestialDebug()
{
    if (mDebugMarks.empty())
    {
        releaseDebugLabels();
        return;
    }

    static const F32 LABEL_DIST_M = 24.f;
    static const F32 RAY_DIST_M = 40.f;

    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();

    while ((S32)mDebugLabels.size() < (S32)mDebugMarks.size())
    {
        LLHUDText* text = static_cast<LLHUDText*>(
            LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT));
        if (!text) break;
        text->setFont(LLFontGL::getFontSansSerifSmall());
        text->setZCompare(false);
        text->setDoFade(false);
        text->setVertAlignment(LLHUDText::ALIGN_VERT_CENTER);
        mDebugLabels.push_back(LLPointer<LLHUDText>(text));
    }
    while ((S32)mDebugLabels.size() > (S32)mDebugMarks.size())
    {
        if (mDebugLabels.back().notNull()) mDebugLabels.back()->markDead();
        mDebugLabels.pop_back();
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE);

    gGL.begin(LLRender::LINES);
    for (size_t i = 0; i < mDebugMarks.size(); ++i)
    {
        const DebugMark& mark = mDebugMarks[i];

        LLColor4 colour(0.4f, 1.f, 0.5f, 0.9f);
        if (mark.mIsSunSlot) colour = LLColor4(1.f, 0.85f, 0.3f, 0.9f);
        else if (mark.mIsMoonSlot) colour = LLColor4(0.6f, 0.75f, 1.f, 0.9f);

        gGL.color4fv(colour.mV);
        gGL.vertex3fv(origin.mV);
        gGL.vertex3fv((origin + mark.mDirection * RAY_DIST_M).mV);

        const LLVector3 tip = origin + mark.mDirection * RAY_DIST_M;
        LLVector3 side = mark.mDirection % LLVector3::z_axis;
        if (side.normalize() < 0.001f) side = LLVector3::x_axis;
        const LLVector3 up = side % mark.mDirection;

        gGL.vertex3fv((tip - side * 1.5f).mV);
        gGL.vertex3fv((tip + side * 1.5f).mV);
        gGL.vertex3fv((tip - up * 1.5f).mV);
        gGL.vertex3fv((tip + up * 1.5f).mV);

        if (i < mDebugLabels.size() && mDebugLabels[i].notNull())
        {
            const F32 elev = RAD_TO_DEG * asinf(llclamp(mark.mDirection.mV[VZ], -1.f, 1.f));
            F32 azim = RAD_TO_DEG * atan2f(mark.mDirection.mV[VX], mark.mDirection.mV[VY]);
            if (azim < 0.f) azim += 360.f;

            std::string line = mark.mName;
            if (mark.mIsSunSlot) line += " [sun slot]";
            else if (mark.mIsMoonSlot) line += " [moon slot]";
            line += llformat("\nalt %.1f deg  az %.1f deg", elev, azim);
            line += llformat("\nsize %.2f deg", mark.mAngularDiameterDeg);
            line += mark.mEmissive ? "\nemissive"
                                   : llformat("\nlit %.0f%%", mark.mSunlight * 100.f);

            mDebugLabels[i]->setString(line);
            mDebugLabels[i]->setColor(colour);
            mDebugLabels[i]->setPositionAgent(origin + mark.mDirection * LABEL_DIST_M);
        }
    }
    if (gSky.mVOSkyp.notNull())
    {
        struct { const LLHeavenBody* mBody; bool mIsSun; } quads[2] = {
            { &gSky.mVOSkyp->getSun(),  true  },
            { &gSky.mVOSkyp->getMoon(), false }
        };

        gGL.begin(LLRender::LINES);
        for (const auto& quad : quads)
        {
            LLVector3 centre;
            for (S32 c = 0; c < 4; ++c) centre += quad.mBody->corner(c);
            centre *= 0.25f;
            if (centre.normalize() < 0.0001f) continue;

            const LLColor4 colour(1.f, 1.f, 1.f, 0.7f);
            gGL.color4fv(colour.mV);
            gGL.vertex3fv(origin.mV);
            gGL.vertex3fv((origin + centre * (RAY_DIST_M * 0.75f)).mV);

            for (const DebugMark& mark : mDebugMarks)
            {
                if (mark.mIsSunSlot != quad.mIsSun || mark.mIsMoonSlot == quad.mIsSun) continue;

                const F32 quad_elev = RAD_TO_DEG * asinf(llclamp(centre.mV[VZ], -1.f, 1.f));
                const F32 want_elev = RAD_TO_DEG * asinf(llclamp(mark.mDirection.mV[VZ], -1.f, 1.f));
                const F32 apart = RAD_TO_DEG * acosf(llclamp(centre * mark.mDirection, -1.f, 1.f));

                if (apart > 0.25f)
                {
                    LL_INFOS_ONCE("AtmoMagicEnv") << "Celestial debug: "
                        << (quad.mIsSun ? "sun" : "moon") << " quad is drawn "
                        << apart << " deg from where it was placed (quad alt "
                        << quad_elev << " deg, authored alt " << want_elev
                        << " deg) - geometry, not texture" << LL_ENDL;
                }
                break;
            }
        }
        gGL.end();
        gGL.flush();
    }

    if (shader) shader->bind();
}

// Toggles the stock water render types - the own-water-plane rule's bookkeeping.
void SSAtmoEnvApplier::setWaterRendering(bool enabled)
{
    if (!enabled)
    {
        if (!mWaterDerendered && LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_WATER))
        {
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_WATER);
            mWaterDerendered = true;
        }
        return;
    }

    if (mWaterDerendered)
    {
        if (!LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_WATER))
        {
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_WATER);
        }
        mWaterDerendered = false;
    }
}

// Starts owning the environment when an asset is present.
void SSAtmoEnvApplier::activate()
{
    mSky = LLSettingsVOSky::buildDefaultSky();
    mWater = LLSettingsVOWater::buildDefaultWater();

    mDefaultWater = LLSettingsVOWater::buildDefaultWater();

    mGlowG = mSky->getGlow().mV[1];

    install();
    mActive = true;
}

// Pushes our settings objects into EEP's local slot.
void SSAtmoEnvApplier::install()
{
    mSkyCacheValid = false;
    mCelestialCacheValid = false;
    mWaterCacheValid = false;

    LLEnvironment& env = LLEnvironment::instance();
    env.setEnvironment(LLEnvironment::ENV_LOCAL, mSky, mWater);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
}

// Releases ENV_LOCAL and restores whatever EEP had.
void SSAtmoEnvApplier::deactivate()
{
    LLEnvironment::instance().clearEnvironment(LLEnvironment::ENV_LOCAL);
    LLEnvironment::instance().setSelectedEnvironment(LLEnvironment::ENV_LOCAL);

    setWaterRendering(true);

    mSky.reset();
    mWater.reset();
    mDefaultWater.reset();
    mBillboards.clear();
    mDebugMarks.clear();
    releaseDebugLabels();
    mSkyCacheValid = false;
    mCelestialCacheValid = false;
    mWaterCacheValid = false;
    mPrevWorldZValid = false;
    mActive = false;
}

// Weather cube plus the author's influence settings into this frame's sky transforms, cached for the readouts.
SSAtmoEnvSkyModulation SSAtmoEnvApplier::computeModulation(const SSAtmoEnvTrack& track, F64 phase)
{
    // Sampled before the influence gate, not behind it: the volumetric deck ignores that gate
    // (SSVolCloud::update resolves straight off the weather cube), and the dome band integrates
    // with the DECK - with the deck, not with the influence switch.
    mLastConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);

    const SSAtmoEnvWeatherInfluence& influence = track.mWeatherInfluence;

    if (!influence.mEnabled)
    {
        mWasPrecipitating = false;
        mSecondsSinceRainStopped = -1.f;
        mLastModulation = SSAtmoEnvSkyModulation();
        return mLastModulation;
    }

    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);

    const F64 now = LLTimer::getElapsedSeconds();
    const F64 elapsed = (mLastTrailUpdate > 0.0) ? (now - mLastTrailUpdate) : 0.0;
    mLastTrailUpdate = now;

    const bool precipitating = !state.mPrecipitationType.empty()
        && state.mPrecipitationIntensity > 0.f;
    if (precipitating)
    {
        mWasPrecipitating = true;
        mSecondsSinceRainStopped = -1.f;
    }
    else if (mWasPrecipitating)
    {
        mWasPrecipitating = false;
        mSecondsSinceRainStopped = 0.f;
    }
    else if (mSecondsSinceRainStopped >= 0.f)
    {
        mSecondsSinceRainStopped += static_cast<F32>(elapsed);
    }

    SSAtmoEnvSkyWeatherInput in;
    in.mMoisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    in.mConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    in.mTemperatureC = track.mWeather.mTemperatureC.valueAt(phase);
    in.mWindHeadingDeg = state.mWindHeading;
    in.mWindSpeedMS = state.mWindSpeed;
    // The overcast band's target: the deck's own coverage, so dome and deck move as one. Last
    // frame's build at worst - the same staleness the dome altitude derivation accepts.
    SSVolCloud* vol = SSVolCloud::getInstance();
    in.mDeckCoverage = (vol && !vol->empty()) ? vol->lastCoverage() : 0.f;
    in.mMaxAltitudeM = track.mAtmosphere.mMaxAltitude.valueAt(phase);
    in.mCloudScale = track.mCloudDome.mScale.valueAt(phase);
    in.mPrecipitationIntensity = state.mPrecipitationIntensity;
    in.mSecondsSinceRainStopped = mSecondsSinceRainStopped;

    const bool rainbow_possible = influence.mRainbowEnabled
        && influence.mRainbowStrength > 0.f
        && mSecondsSinceRainStopped >= 0.f;
    in.mSunElevationSin = rainbow_possible ? sunElevationSin(track, phase) : 0.f;

    mLastModulation = SSAtmoEnvSkyWeatherModulator::compute(in, influence);

    const F32 drift_dt = static_cast<F32>(llclamp(elapsed, 0.0, 0.25));
    mCloudDriftM += mLastModulation.mDriftVelocity * drift_dt;

    static const F32 DRIFT_WRAP_M = 1.0e6f;
    mCloudDriftM.mV[0] = fmodf(mCloudDriftM.mV[0], DRIFT_WRAP_M);
    mCloudDriftM.mV[1] = fmodf(mCloudDriftM.mV[1], DRIFT_WRAP_M);

    return mLastModulation;
}

// The lit sun's elevation sine at a phase, for twilight and rainbow gating.
F32 SSAtmoEnvApplier::sunElevationSin(const SSAtmoEnvTrack& track, F64 phase) const
{
    const SSAtmoEnvPlanetary& planetary = track.mPlanetary;
    const S32 home = planetary.homeBodyIndex();
    if (home < 0) return 0.f;

    SSAtmoEnvResolvedBody sun;
    SSAtmoEnvResolvedBody moon;
    SSAtmoEnvPlanetaryResolver::resolveLightRoles(planetary, sun, moon);
    if (sun.mBodyIndex < 0) return 0.f;

    const SSAtmoEnvCelestialBody& home_body = planetary.mBodies[static_cast<size_t>(home)];
    const LLVector3 dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
        sun.mDirection, home_body.mAxialTiltDeg, home_body.mLatitudeDeg, phase);
    return dir.mV[VZ];
}

// Write-if-changed walk of every sky parameter: keyframes, then modulation, then setters only where values differ.
void SSAtmoEnvApplier::applySky(const SSAtmoEnvTrack& track, F64 phase,
                                const SSAtmoEnvSkyModulation& mod)
{
    if (!mSky)
    {
        return;
    }

    const SSAtmoEnvAtmosphere& atm = track.mAtmosphere;

    bool dirty = false;
    const bool valid = mSkyCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    put(mLastAmbient, mod.ambientColor(atm.mAmbientColor.valueAt(phase)),
        [this](const LLColor3& v) { mSky->setAmbientColor(v); });
    put(mLastBlueHorizon, atm.mBlueHorizon.valueAt(phase),
        [this](const LLColor3& v) { mSky->setBlueHorizon(v); });
    put(mLastBlueDensity, mod.blueDensity(atm.mBlueDensity.valueAt(phase)),
        [this](const LLColor3& v) { mSky->setBlueDensity(v); });
    put(mLastSunlight, atm.mSunlightColor.valueAt(phase),
        [this](const LLColor3& v) { mSky->setSunlightColor(v); });

    put(mLastHazeHorizon, atm.mHazeHorizon.valueAt(phase),
        [this](F32 v) { mSky->setHazeHorizon(v); });
    put(mLastHazeDensity, atm.mHazeDensity.valueAt(phase),
        [this](F32 v) { mSky->setHazeDensity(v); });
    put(mLastSkyMoisture, mod.skyMoistureLevel(atm.mSkyMoistureLevel.valueAt(phase)),
        [this](F32 v) { mSky->setSkyMoistureLevel(v); });
    put(mLastSkyDroplet, atm.mSkyDropletRadius.valueAt(phase),
        [this](F32 v) { mSky->setSkyDropletRadius(v); });
    put(mLastSkyIce, mod.skyIceLevel(atm.mSkyIceLevel.valueAt(phase)),
        [this](F32 v) { mSky->setSkyIceLevel(v); });
    put(mLastDensityMult, atm.mDensityMultiplier.valueAt(phase),
        [this](F32 v) { mSky->setDensityMultiplier(v); });
    put(mLastDistanceMult, atm.mDistanceMultiplier.valueAt(phase),
        [this](F32 v) { mSky->setDistanceMultiplier(v); });
    put(mLastMaxY, atm.mMaxAltitude.valueAt(phase),
        [this](F32 v) { mSky->setMaxY(v); });
    put(mLastProbeAmbiance, atm.mReflectionProbeAmbiance.valueAt(phase),
        [this](F32 v) { mSky->setReflectionProbeAmbiance(v); });
    put(mLastGamma, mod.sceneGamma(atm.mSceneGamma.valueAt(phase)),
        [this](F32 v) { mSky->setGamma(v); });
    put(mLastStarBrightness, atm.mStarBrightness.valueAt(phase),
        [this](F32 v) { mSky->setStarBrightness(v); });
    put(mLastMoonBrightness, atm.mMoonBrightness.valueAt(phase) * mMoonSlotBrightness,
        [this](F32 v) { mSky->setMoonBrightness(v); });

    const F32 glow_size = atm.mGlowSize.valueAt(phase);
    const F32 glow_focus = atm.mGlowFocus.valueAt(phase);
    const LLColor3 glow((2.0f - glow_size) * SLIDER_SCALE_GLOW_R,
                        mGlowG,
                        glow_focus * SLIDER_SCALE_GLOW_B);
    put(mLastGlow, glow, [this](const LLColor3& v) { mSky->setGlow(v); });

    const SSAtmoEnvCloudDome& dome = track.mCloudDome;

    // <SS:Nexii> Not put()s - the dome altitude pair has no LLSettingsSky home to write into. It
    // goes to the cloud and disc shaders straight off this applier, so all that is kept here is
    // the sample. The live sky's cloud shadow below is the tracked blend (authored floor lifted
    // toward the deck's coverage), lights the world, and is the ONE density the dome band draws
    // with - band, deck and world light overcast in lockstep.
    mCloudDomeAuto = dome.mAuto;
    mCloudDomeHeightM = dome.mHeightM.valueAt(phase);

    // Same for the horizon clip: no LLSettingsSky home either - the sky pool reads it straight off this applier when it binds the dome shader, and turns it into the lower dome's depth gate (LL_SHADER_CONST_HORIZON_DEPTH in skyF.glsl).
    mHorizonClip = atm.mHorizonClip;
    // </SS:Nexii>

    put(mLastCloudColor, dome.mColor.valueAt(phase),
        [this](const LLColor3& v) { mSky->setCloudColor(v); });
    put(mLastCloudCoverage, mod.cloudCoverage(dome.mCoverage.valueAt(phase)),
        [this](F32 v) { mSky->setCloudShadow(v); });
    put(mLastCloudScale, dome.mScale.valueAt(phase),
        [this](F32 v) { mSky->setCloudScale(v); });
    // <SS:Nexii> Deliberately unmodulated - the one weather mapping that was removed rather than
    // retuned. Storm darkening used to add the dome a little variance for texture, but EEP's
    // variance erodes (cloudsF.glsl: cloudDensity *= 1 - density_variance^2, saturated wherever
    // four noise samples agree), and on the cirrus layer that erosion has nothing to eat into but
    // the overcast sheet max moisture builds: past convection ~0.8 the saturation spreads and the
    // sheet tears open - gaps, and the disturbed lookup peeling at their edges. The bump predates
    // the volumetric split, when this layer WAS the storm deck; the deck now carries the storm's
    // texture (its churn and flow) and its weight (coverage), so convection leaves the dome alone.
    // </SS:Nexii>
    put(mLastCloudVariance, dome.mVariance.valueAt(phase),
        [this](F32 v) { mSky->setCloudVariance(v); });
    put(mLastCloudScroll, mod.cloudScrollRate(dome.mScrollRate.valueAt(phase)),
        [this](const LLVector2& v) { mSky->setCloudScrollRate(v); });

    const LLColor3 cloud_density(dome.mDensityX.valueAt(phase),
                                 dome.mDensityY.valueAt(phase),
                                 dome.mDensityD.valueAt(phase));
    put(mLastCloudDensity, cloud_density,
        [this](const LLColor3& v) { mSky->setCloudPosDensity1(v); });

    const LLColor3 cloud_detail(dome.mDetailX.valueAt(phase),
                                dome.mDetailY.valueAt(phase),
                                dome.mDetailD.valueAt(phase));
    put(mLastCloudDetail, cloud_detail,
        [this](const LLColor3& v) { mSky->setCloudPosDensity2(v); });

    LLUUID cloud_noise = dome.mNoiseTexture.valueAt(phase);
    if (cloud_noise.isNull())
    {
        cloud_noise = LLSettingsSky::GetDefaultCloudNoiseTextureId();
    }
    put(mLastCloudNoise, cloud_noise,
        [this](const LLUUID& v) { mSky->setCloudNoiseTextureId(v); });

    mSkyCacheValid = true;

    if (dirty)
    {
        mSky->update();
    }
}

// Write-if-changed walk of the water parameters.
void SSAtmoEnvApplier::applyWater(const SSAtmoEnvTrack& track, F64 phase,
                                  const SSAtmoEnvSkyModulation& mod)
{
    if (!mWater || !mDefaultWater)
    {
        return;
    }

    const SSAtmoEnvWater& water = track.mWater;

    bool dirty = false;
    const bool valid = mWaterCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    put(mLastFogColor, water.mFogColor.valueAt(phase),
        [this](const LLColor3& v) { mWater->setWaterFogColor(v); });
    put(mLastFogDensity, water.mFogDensity.valueAt(phase),
        [this](F32 v) { mWater->setWaterFogDensity(v); });
    put(mLastFogMod, mod.waterFogModifier(water.mUnderwaterModifier.valueAt(phase)),
        [this](F32 v) { mWater->setFogMod(v); });
    put(mLastFresnelScale, water.mFresnelScale.valueAt(phase),
        [this](F32 v) { mWater->setFresnelScale(v); });
    put(mLastFresnelOffset, water.mFresnelOffset.valueAt(phase),
        [this](F32 v) { mWater->setFresnelOffset(v); });

    LLUUID normal_map = water.mNormalMap.valueAt(phase);
    if (normal_map.isNull())
    {
        normal_map = mDefaultWater->getNormalMapID();
    }
    put(mLastNormalMap, normal_map,
        [this](const LLUUID& v) { mWater->setNormalMapID(v); });

    const LLVector3 normal_scale(water.mNormalScaleX.valueAt(phase),
                                 water.mNormalScaleY.valueAt(phase),
                                 water.mNormalScaleZ.valueAt(phase));
    put(mLastNormalScale, normal_scale,
        [this](const LLVector3& v) { mWater->setNormalScale(v); });

    put(mLastWave1, water.mLargeWaveSpeed.valueAt(phase),
        [this](const LLVector2& v) { mWater->setWave1Dir(v); });
    put(mLastWave2, water.mSmallWaveSpeed.valueAt(phase),
        [this](const LLVector2& v) { mWater->setWave2Dir(v); });

    put(mLastScaleAbove, water.mRefractionScaleAbove.valueAt(phase),
        [this](F32 v) { mWater->setScaleAbove(v); });
    put(mLastScaleBelow, water.mRefractionScaleBelow.valueAt(phase),
        [this](F32 v) { mWater->setScaleBelow(v); });
    put(mLastBlur, water.mBlurMultiplier.valueAt(phase),
        [this](F32 v) { mWater->setBlurMultiplier(v); });

    mWaterCacheValid = true;

    if (dirty)
    {
        mWater->update();
    }
}

// Write-if-changed walk against the pristine default water, for tracks with no water plane.
void SSAtmoEnvApplier::applyWaterDefaults()
{
    if (!mWater || !mDefaultWater)
    {
        return;
    }

    bool dirty = false;
    const bool valid = mWaterCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    put(mLastFogColor, mDefaultWater->getWaterFogColor(),
        [this](const LLColor3& v) { mWater->setWaterFogColor(v); });
    put(mLastFogDensity, mDefaultWater->getWaterFogDensity(),
        [this](F32 v) { mWater->setWaterFogDensity(v); });
    put(mLastFogMod, mDefaultWater->getFogMod(),
        [this](F32 v) { mWater->setFogMod(v); });
    put(mLastFresnelScale, mDefaultWater->getFresnelScale(),
        [this](F32 v) { mWater->setFresnelScale(v); });
    put(mLastFresnelOffset, mDefaultWater->getFresnelOffset(),
        [this](F32 v) { mWater->setFresnelOffset(v); });
    put(mLastNormalMap, mDefaultWater->getNormalMapID(),
        [this](const LLUUID& v) { mWater->setNormalMapID(v); });
    put(mLastNormalScale, mDefaultWater->getNormalScale(),
        [this](const LLVector3& v) { mWater->setNormalScale(v); });
    put(mLastWave1, mDefaultWater->getWave1Dir(),
        [this](const LLVector2& v) { mWater->setWave1Dir(v); });
    put(mLastWave2, mDefaultWater->getWave2Dir(),
        [this](const LLVector2& v) { mWater->setWave2Dir(v); });
    put(mLastScaleAbove, mDefaultWater->getScaleAbove(),
        [this](F32 v) { mWater->setScaleAbove(v); });
    put(mLastScaleBelow, mDefaultWater->getScaleBelow(),
        [this](F32 v) { mWater->setScaleBelow(v); });
    put(mLastBlur, mDefaultWater->getBlurMultiplier(),
        [this](F32 v) { mWater->setBlurMultiplier(v); });

    mWaterCacheValid = true;

    if (dirty)
    {
        mWater->update();
    }
}

// Places every body: the two EEP light slots by the resolver's rules, billboards for the rest, and the sky poked to rebuild when discs move.
void SSAtmoEnvApplier::applyCelestial(const SSAtmoEnvTrack& track, F64 phase)
{
    if (!mSky)
    {
        return;
    }

    const SSAtmoEnvPlanetary& planetary = track.mPlanetary;

    const S32 home_index = planetary.homeBodyIndex();
    std::vector<S32> emitters;
    if (home_index >= 0)
    {
        emitters = planetary.lightEmitterIndices();
    }

    const F32 tilt_deg = (home_index >= 0)
        ? planetary.mBodies[static_cast<size_t>(home_index)].mAxialTiltDeg
        : 0.f;
    const F32 lat_deg = (home_index >= 0)
        ? planetary.mBodies[static_cast<size_t>(home_index)].mLatitudeDeg
        : 0.f;

    {
        const F32 lat = lat_deg * DEG_TO_RAD;
        mObserverPole.setVec(0.f, cosf(lat), sinf(lat));
    }

    const std::vector<SSAtmoEnvResolvedBody> sky_bodies = (home_index >= 0)
        ? SSAtmoEnvPlanetaryResolver::resolveSky(planetary)
        : std::vector<SSAtmoEnvResolvedBody>();

    const std::vector<LLVector3> world_pos = (home_index >= 0)
        ? SSAtmoEnvPlanetaryResolver::resolveWorldPositions(planetary)
        : std::vector<LLVector3>();

    mMoonSlotBrightness = 1.f;

    S32 moon_slot_body = -1;

    S32 debug_slot_sun = -1;
    S32 debug_slot_moon = -1;

    S32 sun_slot_body = -1;

    mSunSlotEmissive = false;
    mMoonSlotEmissive = false;
    mSunSlotPhaseShaded = true;
    mMoonSlotPhaseShaded = true;
    mSunSlotSunDir = LLVector3::z_axis;
    mSunSlotSunlight = 1.f;
    mMoonSlotSunlight = 1.f;
    mSunSlotAngularDeg = 0.53f;
    mMoonSlotAngularDeg = 0.53f;
    mSunRiseFraction = 0.f;
    mSunSlotDir = LLVector3::z_axis;

    LLVector3 sun_dir = -LLVector3::z_axis;
    LLVector3 moon_dir = -LLVector3::z_axis;
    F32 sun_scale = 1.f;
    F32 moon_scale = 1.f;
    // <SS:Nexii> EEP's default sun id is null, which means "no disc" - the stock pool only
    // draws a sun face that has a texture (see lldrawpoolwlsky's tex_a/tex_b gate). So the
    // stand-in here is the blank-sun disc ASSET, the same drawable default the billboards use.
    LLUUID sun_texture = LLSettingsSky::GetBlankSunTextureId();
    LLUUID moon_texture = LLSettingsSky::GetDefaultMoonTextureId();

    if (!emitters.empty())
    {
        SSAtmoEnvResolvedBody sun_resolved;
        SSAtmoEnvResolvedBody moon_resolved;
        SSAtmoEnvPlanetaryResolver::resolveLightRoles(planetary, sky_bodies,
                                                      sun_resolved, moon_resolved);
        const S32 sun_body = sun_resolved.mBodyIndex;
        const S32 moon_body = moon_resolved.mBodyIndex;
        moon_slot_body = moon_body;

        // <SS:Nexii> The null-texture fallback follows the BODY's kind, not the slot it landed
        // in: a textureless SUN-kind body shows a sun disc in either slot, anything else the
        // stock moon disc. Both stand-ins are real assets - EEP's own default sun id is null
        // and a null id would drop the disc entirely (a null custom texture means "the stock
        // disc", not "no disc").
        auto fallbackFor = [&planetary](S32 body_index) -> LLUUID
        {
            const bool is_sun_kind = planetary.mBodies[static_cast<size_t>(body_index)].mKind
                == SSAtmoEnvCelestialBody::SUN;
            return is_sun_kind ? LLSettingsSky::GetBlankSunTextureId()
                               : LLSettingsSky::GetDefaultMoonTextureId();
        };

        if (sun_body >= 0)
        {
            const SSAtmoEnvCelestialBody& body =
                planetary.mBodies[static_cast<size_t>(sun_body)];
            debug_slot_sun = sun_body;
            mSunSlotEmissive = body.mEmissive;
            mSunSlotPhaseShaded = body.mPhaseShaded;
            sun_slot_body = sun_body;
            sun_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
                sun_resolved.mDirection, tilt_deg, lat_deg, phase);
            sun_scale = celestialDiscScale(sun_resolved.mAngularDiameterDeg);
            mSunSlotAngularDeg = sun_resolved.mAngularDiameterDeg;
            sun_texture = body.mCustomTexture.notNull()
                ? body.mCustomTexture : fallbackFor(sun_body);
        }
        if (moon_body >= 0)
        {
            const SSAtmoEnvCelestialBody& body =
                planetary.mBodies[static_cast<size_t>(moon_body)];
            debug_slot_moon = moon_body;
            mMoonSlotEmissive = body.mEmissive;
            mMoonSlotPhaseShaded = body.mPhaseShaded;
            moon_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
                moon_resolved.mDirection, tilt_deg, lat_deg, phase);
            moon_scale = celestialDiscScale(moon_resolved.mAngularDiameterDeg);
            mMoonSlotAngularDeg = moon_resolved.mAngularDiameterDeg;
            moon_texture = body.mCustomTexture.notNull()
                ? body.mCustomTexture : fallbackFor(moon_body);
        }
    }

    S32 lamp = -1;
    for (size_t i = 0; i < planetary.mBodies.size(); ++i)
    {
        if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN) continue;
        if (lamp < 0 || planetary.mBodies[i].mDiameterM > planetary.mBodies[(size_t)lamp].mDiameterM)
        {
            lamp = (S32)i;
        }
    }

    auto illuminate = [&](S32 body_index, LLVector3& out_dir, F32& out_light)
    {
        out_dir = LLVector3::z_axis;
        out_light = 1.f;
        if (lamp < 0 || home_index < 0 || body_index < 0) return;
        if (world_pos.size() <= (size_t)llmax(lamp, llmax(body_index, home_index))) return;

        const LLVector3 body = world_pos[(size_t)body_index];
        const LLVector3 to_sun_world = world_pos[(size_t)lamp] - body;

        out_dir = to_sun_world;
        if (out_dir.normalize() < 0.0001f) { out_dir = LLVector3::z_axis; return; }
        out_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
            out_dir, tilt_deg, lat_deg, phase);

        const LLVector3 home = world_pos[(size_t)home_index];
        const LLVector3 home_to_body = body - home;
        const F32 behind = home_to_body * (-out_dir);
        if (behind <= 0.f) return;

        const LLVector3 perp = home_to_body - (-out_dir) * behind;
        const F32 miss = perp.magVec();
        const F32 home_r = planetary.mBodies[(size_t)home_index].mDiameterM * 0.5f;
        if (home_r <= 0.f) return;

        const F32 star_r = planetary.mBodies[(size_t)lamp].mDiameterM * 0.5f;
        const F32 star_dist = (world_pos[(size_t)lamp] - home).magVec();
        const F32 spread = (star_dist > 1.f) ? (star_r / star_dist) * behind : 0.f;

        const F32 umbra = llmax(home_r - spread, 0.f);
        const F32 penumbra = home_r + spread;
        if (miss >= penumbra) return;

        static const F32 ECLIPSE_FLOOR = 0.05f;

        const F32 across = (penumbra > umbra) ? ((miss - umbra) / (penumbra - umbra)) : 1.f;
        out_light = ECLIPSE_FLOOR + (1.f - ECLIPSE_FLOOR) * llclamp(across, 0.f, 1.f);
    };

    if (sun_slot_body >= 0)
    {
        LLVector3 slot_sun_dir;
        F32 slot_light = 1.f;
        illuminate(sun_slot_body, slot_sun_dir, slot_light);
        mSunSlotSunDir = slot_sun_dir;
        mSunSlotSunlight = slot_light;
    }

    mMoonSunDir.setZero();
    if (moon_slot_body >= 0)
    {
        LLVector3 moon_sun_dir;
        F32 moon_light = 1.f;
        illuminate(moon_slot_body, moon_sun_dir, moon_light);

        const SSAtmoEnvCelestialBody& slot_body =
            planetary.mBodies[static_cast<size_t>(moon_slot_body)];
        if (slot_body.mPhaseShaded && !slot_body.mEmissive)
        {
            const F32 lit = 0.5f + 0.5f * (moon_sun_dir * -moon_dir);
            mMoonSlotBrightness = llclamp(lit, 0.f, 1.f) * moon_light;
            mMoonSunDir = moon_sun_dir;
            mMoonSlotSunlight = moon_light;
        }
    }

    mBillboards.clear();
    for (const SSAtmoEnvResolvedBody& body : sky_bodies)
    {
        if (std::find(emitters.begin(), emitters.end(), body.mBodyIndex) != emitters.end())
        {
            continue;
        }
        if (body.mAngularDiameterDeg < BILLBOARD_MIN_DIAMETER_DEG)
        {
            continue;
        }
        SSAtmoEnvBillboard billboard;
        billboard.mDirection = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
            body.mDirection, tilt_deg, lat_deg, phase);
        billboard.mAngularDiameterDeg = body.mAngularDiameterDeg;
        const SSAtmoEnvCelestialBody& authored =
            planetary.mBodies[static_cast<size_t>(body.mBodyIndex)];
        billboard.mTexture = authored.mCustomTexture;
        billboard.mIsSun = (authored.mKind == SSAtmoEnvCelestialBody::SUN);
        billboard.mBodyIndex = body.mBodyIndex;
        billboard.mEmissive = authored.mEmissive;
        billboard.mPhaseShaded = authored.mPhaseShaded;
        illuminate(body.mBodyIndex, billboard.mSunDirection, billboard.mSunlight);
        mBillboards.push_back(billboard);
    }

    mDebugMarks.clear();
    if (home_index >= 0)
    {
        auto add_mark = [&](S32 body_index, const LLVector3& dir, F32 diameter,
                            F32 sunlight, bool is_sun_slot, bool is_moon_slot)
        {
            if (body_index < 0) return;
            const SSAtmoEnvCelestialBody& b = planetary.mBodies[static_cast<size_t>(body_index)];

            DebugMark mark;
            mark.mName = b.mName.empty() ? llformat("body %d", body_index) : b.mName;
            mark.mDirection = dir;
            mark.mAngularDiameterDeg = diameter;
            mark.mSunlight = sunlight;
            mark.mEmissive = b.mEmissive;
            mark.mIsSunSlot = is_sun_slot;
            mark.mIsMoonSlot = is_moon_slot;
            mDebugMarks.push_back(mark);
        };

        if (debug_slot_sun >= 0)
        {
            add_mark(debug_slot_sun, sun_dir, celestialAngularFromScale(sun_scale), 1.f, true, false);
        }
        if (debug_slot_moon >= 0)
        {
            add_mark(debug_slot_moon, moon_dir, celestialAngularFromScale(moon_scale),
                     mMoonSlotBrightness, false, true);
        }
        for (const SSAtmoEnvBillboard& bb : mBillboards)
        {
            add_mark(bb.mBodyIndex, bb.mDirection, bb.mAngularDiameterDeg,
                     bb.mSunlight, false, false);
        }
    }

    bool dirty = false;
    const bool valid = mCelestialCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    bool celestial_moved = false;
    put(mLastSunDir, sun_dir,
        [this, &celestial_moved](const LLVector3& v)
        { mSky->setSunRotation(quat_from_direction(v)); celestial_moved = true; });
    put(mLastMoonDir, moon_dir,
        [this, &celestial_moved](const LLVector3& v)
        { mSky->setMoonRotation(quat_from_direction(v)); celestial_moved = true; });
    put(mLastSunScale, sun_scale,
        [this, &celestial_moved](F32 v) { mSky->setSunScale(v); celestial_moved = true; });
    put(mLastMoonScale, moon_scale,
        [this, &celestial_moved](F32 v) { mSky->setMoonScale(v); celestial_moved = true; });
    put(mLastSunTexture, sun_texture,
        [this](const LLUUID& v) { mSky->setSunTextureId(v); });
    put(mLastMoonTexture, moon_texture,
        [this](const LLUUID& v) { mSky->setMoonTextureId(v); });

    // <SS:Nexii> The sun slot's risen fraction, from the RESOLVED direction and disc - see
    // sunRiseFraction. The band spans the slot quad's OWN half-angle - the same sizing chain
    // updateHeavenlyBodyGeometry lays the disc out with (scale * HEAVENLY_BODY_FACTOR * the
    // sun's disk radius, over the HEAVENLY_BODY_DIST shell) - so the ramp tracks what the disc
    // actually draws, through its whole rise, however large it is authored. And the fraction is
    // the share of the disc's area above the horizon - the share of it that sheds light on the
    // observer. That is what makes the ramp start as the top edge breaks, run through half light
    // at centre-rise where stock flips its switch, and complete when the full disc stands clear,
    // gently at both ends.
    F32 half_tan = sun_scale * HEAVENLY_BODY_FACTOR * 0.5f; // llvosky.cpp's SUN_DISK_RADIUS
    if (gSky.mVOSkyp.notNull())
    {
        half_tan = sun_scale * HEAVENLY_BODY_FACTOR * gSky.mVOSkyp->getSun().getDiskRadius();
    }
    const F32 half_sin = half_tan / sqrtf(1.f + half_tan * half_tan);
    if (half_sin > 1e-6f)
    {
        const F32 u = llclamp(sun_dir.mV[VZ] / half_sin, -1.f, 1.f);
        mSunRiseFraction = (u * sqrtf(1.f - u * u) + asinf(u)) / F_PI + 0.5f;
    }
    else
    {
        mSunRiseFraction = (sun_dir.mV[VZ] > 0.f) ? 1.f : 0.f;
    }

    // ...and the direction itself, for everything that must keep aiming at the SUN through the
    // rise band - see sunSlotDirection.
    mSunSlotDir = sun_dir;

    mCelestialCacheValid = true;

    if (dirty)
    {
        mSky->update();
    }

    if (celestial_moved && gSky.mVOSkyp.notNull())
    {
        gSky.mVOSkyp->forceSkyUpdate();
    }
}
