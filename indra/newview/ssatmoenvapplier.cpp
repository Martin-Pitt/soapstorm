/**
 * @file ssatmoenvapplier.cpp
 * @brief Atmo Magic: drives the on-screen sky and water from the loaded
 *        environment asset via EEP's ENV_LOCAL slot. See the header and
 *        doc/atmo_magic_environment.md, "Rendering integration (the
 *        applier)".
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

#include <algorithm>
#include <cmath>

// <SS:Nexii> Atmo Magic: rendering applier

namespace
{
    // The schema stores glow in the same UI-space EEP's own sliders use (see SSAtmoEnvAtmosphere); the renderer wants the packed glow colour. These are llpaneleditsky.cpp's SLIDER_SCALE_GLOW_R/B.
    // Outside EEP's own panels this forward conversion lives only here, and its exact inverse only in SSAtmoEnvAtmosphere::fromSettingsSky (seeding a new asset from a fetched sky) - packed glow must
    // never leak into the schema, and UI-space must never leak past this boundary.
    const F32 SLIDER_SCALE_GLOW_R(20.0f);
    const F32 SLIDER_SCALE_GLOW_B(-5.0f);

    // A single-frame altitude jump larger than this counts as a teleport for the resolver's instant-cut rule - same figure SSAtmoMagic uses for the precipitation bridge, so sky and rain always agree
    // on whether a given jump was "movement" or "arrival".
    const F32 TELEPORT_JUMP_M(60.f);

    // Emitter angular size -> EEP disc scale: scale 1.0 is treated as the real Sun's apparent ~0.53 degrees, so a body authored with Earth-Sun geometry renders at the familiar size. Clamped because
    // a body parked on its home's doorstep would otherwise ask for a screen-filling disc, and a distant speck for an invisible one.
    const F32 REFERENCE_SUN_DIAMETER_DEG(0.53f);
    const F32 CELESTIAL_SCALE_MIN(0.1f);
    const F32 CELESTIAL_SCALE_MAX(20.f);

    // Billboard bodies apparently smaller than this are dropped outright rather than published - a subpixel quad just shimmers. Emitters are exempt: their scale clamp already floors them at a
    // visible size.
    const F32 BILLBOARD_MIN_DIAMETER_DEG(0.05f);

    // Inverse of the engine's own convention: LLSettingsSky derives the world-space sun/moon direction as x_axis * rotation (see calculateHeavenlyBodyPositions), so the quaternion for a desired
    // direction is the shortest arc taking +X onto it - the same construction its convert_azimuth_and_altitude_to_quat performs from angles.
    LLQuaternion quat_from_direction(const LLVector3& dir)
    {
        LLQuaternion quat; // identity
        LLVector3 axis = LLVector3::x_axis % dir;
        if (axis.normalize() < 0.0001f)
        {
            // Parallel or antiparallel to +X: the cross product vanishes. Antiparallel needs a half-turn about any perpendicular (+Z serves); parallel is the identity already.
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

SSAtmoEnvApplier::SSAtmoEnvApplier()
{
}

// static
F32 SSAtmoEnvApplier::celestialDiscScale(F32 angular_diameter_deg)
{
    return llclamp(angular_diameter_deg / REFERENCE_SUN_DIAMETER_DEG,
                   CELESTIAL_SCALE_MIN, CELESTIAL_SCALE_MAX);
}

void SSAtmoEnvApplier::apply()
{
    // The shared master switch - same control SSAtmoMagic gates on.
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    // Re-read the asset through the manager every frame - it can be replaced wholesale between frames (a notecard load landing), so no pointers into its mTracks ever survive past one apply() call.
    // An asset with no tracks at all should be impossible (fromLLSD rejects it), but treat it as "nothing to show" rather than trusting that.
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
        // Something else cleared ENV_LOCAL out from under us (Personal Lighting's reset button does exactly that) - reclaim it. While the master switch is on and an asset is loaded, Atmo Magic owns
        // the local slot.
        install();
    }

    const SSAtmoEnvAsset& asset = mgr->asset();

    // Same resolve call the precipitation bridge makes, with the same prev-z/teleport bookkeeping (region change or an implausibly large single-frame jump both count as "teleported" - there is no
    // dedicated completion event to hook instead). Tracked in our own members rather than shared with SSAtmoMagic's copy: that state is the bridge's private bookkeeping, not a published service.
    const F32 world_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    LLViewerRegion* region = gAgent.getRegion();
    const LLUUID region_id = region ? region->getRegionID() : LLUUID::null;

    const bool teleported = !mPrevWorldZValid
        || region_id != mPrevRegionID
        || fabsf(world_z - mPrevWorldZ) > TELEPORT_JUMP_M;

    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(
        asset, world_z, mPrevWorldZValid ? mPrevWorldZ : world_z, teleported);

    mPrevWorldZ = world_z;
    mPrevWorldZValid = true;
    mPrevRegionID = region_id;

    // Only the primary track is rendered: cross-track sky/water blending on a soft crossing is deferred per the design doc (precipitation already fades; sky cuts), so mNeighborTrack/mNeighborWeight
    // are deliberately ignored here. When the primary flips, values simply change this frame - the change-detection caches make that a one-off burst of setter calls, not a stall.
    S32 track_index = blend.mPrimaryTrack;
    if (track_index < 0 || track_index >= static_cast<S32>(asset.mTracks.size()))
    {
        track_index = 0;
    }
    const SSAtmoEnvTrack& track = asset.mTracks[static_cast<size_t>(track_index)];

    // The floater's preview scrubber overrides the wall clock while it is looking - scrubbing the editor scrubs the actual sky.
    const F64 phase = mgr->hasPreviewPhaseOverride()
        ? mgr->previewPhaseOverride()
        : track.currentDayCyclePhase();

    const SSAtmoEnvSkyModulation mod = computeModulation(track, phase);

    // The overlay's labels are HUD objects with a life of their own, so switching the overlay off has to take them with it - otherwise they hang in the world at their last position, which is what
    // "the text is still visible but just not updating" was.
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
        // Stock hole and edge water render as normal - the far sea excludes itself from their footprint instead (the ss_sea_hole rect in waterV), water around the water rather than water under
        // it. The standdown is only ever an undo now, in case an earlier state left it latched.
        setVoidWaterRendering(true);
    }
    else
    {
        // Water disabled on this track: apply sky only and park the installed water instance on EEP's stock defaults. Uninstalling the water half instead would mean install/uninstall churn on every
        // track cross between water and no-water tracks - default water is the lesser evil. Void restored FIRST: setWaterRendering(false)'s toggle flips VOIDWATER in lockstep with WATER, and it must
        // flip from the stock-true state, not from our standdown.
        setVoidWaterRendering(true);
        applyWaterDefaults();
        setWaterRendering(false);
    }
}

// The inverse of celestialDiscScale, so the overlay can report the angular size that produced a slot's scale without the caller having to keep it.
F32 SSAtmoEnvApplier::celestialAngularFromScale(F32 scale)
{
    return scale * 0.53f;
}

void SSAtmoEnvApplier::releaseDebugLabels()
{
    for (LLPointer<LLHUDText>& label : mDebugLabels)
    {
        if (label.notNull()) label->markDead();
    }
    mDebugLabels.clear();
}

void SSAtmoEnvApplier::renderCelestialDebug()
{
    if (mDebugMarks.empty())
    {
        releaseDebugLabels();
        return;
    }

    // Labels live at a fixed distance rather than out at the real shell: HUD text scales with distance, and a name a kilometre away is a pixel. The ray is what says which way the body actually is;
    // the label only has to sit on that ray.
    static const F32 LABEL_DIST_M = 24.f;
    static const F32 RAY_DIST_M = 40.f;

    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();

    // One label per mark, reused frame to frame - creating and killing HUD objects every frame would churn the HUD list for no reason.
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
    LLGLDepthTest depth(GL_FALSE);   // rays read as guides, not as geometry

    gGL.begin(LLRender::LINES);
    for (size_t i = 0; i < mDebugMarks.size(); ++i)
    {
        const DebugMark& mark = mDebugMarks[i];

        // Warm for the sun slot, cool for the moon slot, green for everything else - the same reading the designer's own list gives.
        LLColor4 colour(0.4f, 1.f, 0.5f, 0.9f);
        if (mark.mIsSunSlot) colour = LLColor4(1.f, 0.85f, 0.3f, 0.9f);
        else if (mark.mIsMoonSlot) colour = LLColor4(0.6f, 0.75f, 1.f, 0.9f);

        gGL.color4fv(colour.mV);
        gGL.vertex3fv(origin.mV);
        gGL.vertex3fv((origin + mark.mDirection * RAY_DIST_M).mV);

        // A short cross at the end, so the ray reads as pointing AT something rather than merely away from here.
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
            // Elevation and azimuth as an author reads a sky: up from the horizon, and round from north. Both come from the direction the renderer is actually using, so a body drawn in the wrong
            // place reads wrong here too - which is the point.
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
    // The sun and moon quads AS LLVOSKY ACTUALLY BUILT THEM, so a disagreement between where a body is supposed to be and where its disc is drawn can be read off the screen instead of reasoned
    // about. LLHeavenBody::corner() holds the quad's camera-relative corners, which is what went into the vertex buffer (before the pass's own translate), so the mean of them is the disc's centre
    // and its direction is where the disc really points. If this ray sits on the authored one, the geometry is right and any visible offset is inside the disc TEXTURE - art whose bright part is not
    // centred in its image will look displaced however correctly the quad is placed. If the two rays diverge, the geometry is stale or wrong, and the printed angle says by how much.
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

            // White, and shorter than the authored rays, so the pair reads as "authored" and "as drawn" rather than as two more bodies.
            const LLColor4 colour(1.f, 1.f, 1.f, 0.7f);
            gGL.color4fv(colour.mV);
            gGL.vertex3fv(origin.mV);
            gGL.vertex3fv((origin + centre * (RAY_DIST_M * 0.75f)).mV);

            // Reported against whichever authored mark holds that slot.
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

void SSAtmoEnvApplier::setWaterRendering(bool enabled)
{
    // Only ever undo our own change, and only ever make one: the control is a global the user (and the EEP derender path) can also touch, so "toggle it to what I want" would fight them.
    // mWaterDerendered is the record of what we did, not of what the pipeline currently says.
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

void SSAtmoEnvApplier::setVoidWaterRendering(bool enabled)
{
    // Same shape as setWaterRendering. toggleRenderType's pair-flip special case only fires for the WATER type, so toggling VOIDWATER through the same public control moves that one flag alone -
    // which is exactly the divergence this needs (region water on, filler off). Bookkeeping keeps us from undoing a state the user set themselves.
    if (!enabled)
    {
        if (!mVoidDerendered && LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_VOIDWATER))
        {
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_VOIDWATER);
            mVoidDerendered = true;
        }
        return;
    }

    if (mVoidDerendered)
    {
        if (!LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_VOIDWATER))
        {
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_VOIDWATER);
        }
        mVoidDerendered = false;
    }
}

void SSAtmoEnvApplier::activate()
{
    // One settings pair for the whole active span, mutated in place each frame - the DayInstance holds these same shared_ptrs, so setters plus update() propagate without reinstalling.
    mSky = LLSettingsVOSky::buildDefaultSky();
    mWater = LLSettingsVOWater::buildDefaultWater();

    // Pristine copy: the value source for applyWaterDefaults() and the null-normal-map fallback, instead of hardcoding EEP's numbers here.
    mDefaultWater = LLSettingsVOWater::buildDefaultWater();

    // The schema stores glow size/focus only (packed r/b); the packed colour's g comes from the default sky and is preserved as-is.
    mGlowG = mSky->getGlow().mV[1];

    install();
    mActive = true;
}

void SSAtmoEnvApplier::install()
{
    // Fresh (or re-claimed) instances: nothing about their current field values is known any more - Personal Lighting may even have live- edited them while sharing the slot - so every field
    // re-applies once.
    mSkyCacheValid = false;
    mCelestialCacheValid = false;
    mWaterCacheValid = false;

    // The same take-over Personal Lighting's captureCurrentEnvironment() performs: install into ENV_LOCAL and select it instantly. If a Personal Lighting session currently owns ENV_LOCAL this stomps
    // it - accepted and documented in the design doc; the local slot has no arbitration and both features are explicit user opt-ins.
    LLEnvironment& env = LLEnvironment::instance();
    env.setEnvironment(LLEnvironment::ENV_LOCAL, mSky, mWater);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
}

void SSAtmoEnvApplier::deactivate()
{
    // The exact release pair Personal Lighting's reset path uses (LLFloaterEnvironmentAdjust::onButtonReset): clearing ENV_LOCAL and re-selecting it makes the selection fall through to whatever the
    // parcel/region would otherwise show, with the default transition.
    LLEnvironment::instance().clearEnvironment(LLEnvironment::ENV_LOCAL);
    LLEnvironment::instance().setSelectedEnvironment(LLEnvironment::ENV_LOCAL);

    // Before dropping the settings: a track with water disabled may have switched the water render type off, and leaving Atmo Magic must not leave the world's water switched off behind it. Void
    // first, for the same lockstep reason as in apply().
    setVoidWaterRendering(true);
    setWaterRendering(true);

    mSky.reset();
    mWater.reset();
    mDefaultWater.reset();
    // The draw pool gates on this vector's emptiness, so clearing it here is what actually switches the billboard pass off.
    mBillboards.clear();
    mDebugMarks.clear();
    releaseDebugLabels();
    mSkyCacheValid = false;
    mCelestialCacheValid = false;
    mWaterCacheValid = false;
    mPrevWorldZValid = false;
    mActive = false;
}

SSAtmoEnvSkyModulation SSAtmoEnvApplier::computeModulation(const SSAtmoEnvTrack& track, F64 phase)
{
    const SSAtmoEnvWeatherInfluence& influence = track.mWeatherInfluence;

    // Off means off: no weather resolution, no trail bookkeeping, and an identity modulation - so a world with influence disabled renders precisely its authored sky and pays nothing for the feature.
    if (!influence.mEnabled)
    {
        mWasPrecipitating = false;
        mSecondsSinceRainStopped = -1.f;
        mLastModulation = SSAtmoEnvSkyModulation();
        // mCloudDriftM deliberately keeps its value: it is a position, not a rate, and zeroing it here would snap the whole cloud layer back to where it started the moment influence was switched
        // off.
        return mLastModulation;
    }

    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);

    //---------------------------------------------------------------
    // Rain-stop trail. Wall clock rather than frame count: the window is stated in seconds because it is a weather phenomenon, not a rendering one, and it should decay at the same rate on a machine
    // running at 20fps as at 200.
    //---------------------------------------------------------------
    const F64 now = LLTimer::getElapsedSeconds();
    const F64 elapsed = (mLastTrailUpdate > 0.0) ? (now - mLastTrailUpdate) : 0.0;
    mLastTrailUpdate = now;

    const bool precipitating = !state.mPrecipitationType.empty()
        && state.mPrecipitationIntensity > 0.f;
    if (precipitating)
    {
        // Raining now: no window open, and none pending. A shower that restarts resets the clock rather than resuming it - the bow belongs to the END of a shower.
        mWasPrecipitating = true;
        mSecondsSinceRainStopped = -1.f;
    }
    else if (mWasPrecipitating)
    {
        mWasPrecipitating = false;
        mSecondsSinceRainStopped = 0.f; // it stopped just now - window opens
    }
    else if (mSecondsSinceRainStopped >= 0.f)
    {
        mSecondsSinceRainStopped += static_cast<F32>(elapsed);
    }

    SSAtmoEnvSkyWeatherInput in;
    // The raw cube alongside the resolved state: the resolver's bands are right for particles and forecast text, but a sky that steps between four convection phases reads as a bug (see the input
    // struct).
    in.mMoisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    in.mConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    in.mTemperatureC = track.mWeather.mTemperatureC.valueAt(phase);
    in.mWindHeadingDeg = state.mWindHeading;
    in.mWindSpeedMS = state.mWindSpeed;
    in.mOktaCloudCover = state.mOktaCloudCover;
    // The cloud layer's geometry, so the wind mapping can work out what a given wind speed should LOOK like on this particular sky - see SSAtmoEnvSkyModulation::cloudScrollRate.
    in.mMaxAltitudeM = track.mAtmosphere.mMaxAltitude.valueAt(phase);
    in.mCloudScale = track.mCloudDome.mScale.valueAt(phase);
    in.mPrecipitationIntensity = state.mPrecipitationIntensity;
    in.mSecondsSinceRainStopped = mSecondsSinceRainStopped;

    // Only resolved when a bow is actually possible - see sunElevationSin.
    const bool rainbow_possible = influence.mRainbowEnabled
        && influence.mRainbowStrength > 0.f
        && mSecondsSinceRainStopped >= 0.f;
    in.mSunElevationSin = rainbow_possible ? sunElevationSin(track, phase) : 0.f;

    mLastModulation = SSAtmoEnvSkyWeatherModulator::compute(in, influence);

    // Integrate the drift. Wall clock rather than the day-cycle phase: this is the sky physically moving over the world, so it advances at the rate the world does even while an editor's scrubber
    // holds the phase still.
    const F32 drift_dt = static_cast<F32>(llclamp(elapsed, 0.0, 0.25));
    mCloudDriftM += mLastModulation.mDriftVelocity * drift_dt;

    static const F32 DRIFT_WRAP_M = 1.0e6f;
    mCloudDriftM.mV[0] = fmodf(mCloudDriftM.mV[0], DRIFT_WRAP_M);
    mCloudDriftM.mV[1] = fmodf(mCloudDriftM.mV[1], DRIFT_WRAP_M);

    return mLastModulation;
}

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

void SSAtmoEnvApplier::applySky(const SSAtmoEnvTrack& track, F64 phase,
                                const SSAtmoEnvSkyModulation& mod)
{
    if (!mSky)
    {
        return;
    }

    const SSAtmoEnvAtmosphere& atm = track.mAtmosphere;

    // Write-if-changed: the settings setters mark the whole object dirty, which re-uploads shader uniforms downstream, so identical values are compared away. Exact compares suffice - the same
    // keyframe evaluation at the same phase is bit-identical frame to frame.
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

    // Colours are stored in LLSettingsSky's own space (the schema's defaults are the settings' own), so no slider scaling applies here - only glow converts, below. Modulated fields evaluate their
    // keyframes exactly as authored and are then bent by this frame's weather - see SSAtmoEnvSkyModulation. The cache still holds the FINAL value, so the write-if-changed test stays honest: weather
    // changing a value counts as a change.
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
    put(mLastHazeDensity, mod.hazeDensity(atm.mHazeDensity.valueAt(phase)),
        [this](F32 v) { mSky->setHazeDensity(v); });
    put(mLastSkyMoisture, mod.skyMoistureLevel(atm.mSkyMoistureLevel.valueAt(phase)),
        [this](F32 v) { mSky->setSkyMoistureLevel(v); });
    put(mLastSkyDroplet, atm.mSkyDropletRadius.valueAt(phase),
        [this](F32 v) { mSky->setSkyDropletRadius(v); });
    put(mLastSkyIce, mod.skyIceLevel(atm.mSkyIceLevel.valueAt(phase)),
        [this](F32 v) { mSky->setSkyIceLevel(v); });
    put(mLastDensityMult, atm.mDensityMultiplier.valueAt(phase),
        [this](F32 v) { mSky->setDensityMultiplier(v); });
    put(mLastDistanceMult, mod.distanceMultiplier(atm.mDistanceMultiplier.valueAt(phase)),
        [this](F32 v) { mSky->setDistanceMultiplier(v); });
    put(mLastMaxY, atm.mMaxAltitude.valueAt(phase),
        [this](F32 v) { mSky->setMaxY(v); });
    // Set unconditionally, exactly like EEP's own commit handler - see SSAtmoEnvAtmosphere::mReflectionProbeAmbiance on why there is no PBR-mode gate here.
    put(mLastProbeAmbiance, atm.mReflectionProbeAmbiance.valueAt(phase),
        [this](F32 v) { mSky->setReflectionProbeAmbiance(v); });
    put(mLastGamma, mod.sceneGamma(atm.mSceneGamma.valueAt(phase)),
        [this](F32 v) { mSky->setGamma(v); });
    put(mLastStarBrightness, atm.mStarBrightness.valueAt(phase),
        [this](F32 v) { mSky->setStarBrightness(v); });
    // The moon disc's own luminance. This is the only writer: applyCelestial drives the moon's rotation/scale/texture but deliberately never its brightness - that is an appearance dial, not
    // planetary structure. Scaled by how lit the body in the moon slot actually is - its phase times whatever the home body's shadow leaves of it - so the Atmosphere tab's dial stays the master
    // while a new moon lights the world less than a full one. That factor is resolved in applyCelestial, which runs after this, so it is last frame's; the phase moves over minutes, so a frame of lag
    // is not observable.
    put(mLastMoonBrightness, atm.mMoonBrightness.valueAt(phase) * mMoonSlotBrightness,
        [this](F32 v) { mSky->setMoonBrightness(v); });

    // UI-space -> packed glow, exactly inverse to llpaneleditsky's
    // refresh: r = (2 - size) * 20, b = focus * -5, g preserved from the
    // default sky's own packed glow.
    const F32 glow_size = atm.mGlowSize.valueAt(phase);
    const F32 glow_focus = atm.mGlowFocus.valueAt(phase);
    const LLColor3 glow((2.0f - glow_size) * SLIDER_SCALE_GLOW_R,
                        mGlowG,
                        glow_focus * SLIDER_SCALE_GLOW_B);
    put(mLastGlow, glow, [this](const LLColor3& v) { mSky->setGlow(v); });

    // The Sky Dome - the legacy cirrus layer's full parameter set (see SSAtmoEnvCloudDome). Note "coverage" lands on setCloudShadow: that is genuinely the field EEP's own Cloud Coverage slider
    // drives (see llpaneleditsky.cpp's onCloudCoverageChanged).
    const SSAtmoEnvCloudDome& dome = track.mCloudDome;

    put(mLastCloudColor, dome.mColor.valueAt(phase),
        [this](const LLColor3& v) { mSky->setCloudColor(v); });
    put(mLastCloudCoverage, mod.cloudCoverage(dome.mCoverage.valueAt(phase)),
        [this](F32 v) { mSky->setCloudShadow(v); });
    put(mLastCloudScale, dome.mScale.valueAt(phase),
        [this](F32 v) { mSky->setCloudScale(v); });
    put(mLastCloudVariance, mod.cloudVariance(dome.mVariance.valueAt(phase)),
        [this](F32 v) { mSky->setCloudVariance(v); });
    put(mLastCloudScroll, mod.cloudScrollRate(dome.mScrollRate.valueAt(phase)),
        [this](const LLVector2& v) { mSky->setCloudScrollRate(v); });

    // Three separately keyframable scalars fold into each packed setter, same as the water wavelet triple.
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

    // A null noise texture in the schema means "the default cloud noise" - same guard idiom as the water normal map: actually setting null would leave the layer with no noise texture at all, and
    // mapping null to the default makes a keyframe stepping back to null restore the stock look rather than keeping the last custom map.
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

    // A null normal map in the schema means "the default map" - actually setting null would leave the surface with no normal texture at all, so it maps to EEP's stock water normal instead. That also
    // makes a keyframe stepping back to null behave: it restores the default rather than silently keeping the last custom map.
    LLUUID normal_map = water.mNormalMap.valueAt(phase);
    if (normal_map.isNull())
    {
        normal_map = mDefaultWater->getNormalMapID();
    }
    put(mLastNormalMap, normal_map,
        [this](const LLUUID& v) { mWater->setNormalMapID(v); });

    // Three separately keyframable wavelet scalars fold into the one vector setter.
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

    // Water HEIGHT (the tide) is deliberately not applied: the plane's height is region state, not a settings field, and overriding it client-side fights the sim - see the design doc.

    mWaterCacheValid = true;

    if (dirty)
    {
        mWater->update();
    }
}

void SSAtmoEnvApplier::applyWaterDefaults()
{
    if (!mWater || !mDefaultWater)
    {
        return;
    }

    // Same write-if-changed walk as applyWater, sourcing every value from the pristine default instance: after the first frame on a no-water track this is pure compares, no setter traffic.
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

void SSAtmoEnvApplier::applyCelestial(const SSAtmoEnvTrack& track, F64 phase)
{
    if (!mSky)
    {
        return;
    }

    const SSAtmoEnvPlanetary& planetary = track.mPlanetary;

    // A track with no bodies, or no home body to observe the sky from, behaves exactly like zero emitters - per the design doc, "No emitters = a dim, sun-below-horizon sky rather than a fallback
    // sun". (resolveSky() has no vantage point without a home, so emitters without one could not be placed anyway.)
    const S32 home_index = planetary.homeBodyIndex();
    std::vector<S32> emitters;
    if (home_index >= 0)
    {
        emitters = planetary.lightEmitterIndices();
    }

    // Obliquity gives the world its seasons; latitude says where on it the observer is standing. They were one field once - see SSAtmoEnvCelestialBody::mLatitudeDeg.
    const F32 tilt_deg = (home_index >= 0)
        ? planetary.mBodies[static_cast<size_t>(home_index)].mAxialTiltDeg
        : 0.f;
    const F32 lat_deg = (home_index >= 0)
        ? planetary.mBodies[static_cast<size_t>(home_index)].mLatitudeDeg
        : 0.f;

    // The celestial pole, in the (east, north, up) frame resolveObserverDirection hands back. It is (0,0,1) in the equatorial frame, so its components here are its dots with those three axes - which
    // come out as due north at an elevation of the latitude, as they should.
    {
        const F32 lat = lat_deg * DEG_TO_RAD;
        mObserverPole.setVec(0.f, cosf(lat), sinf(lat));
    }

    // Re-resolved from the asset every frame like everything else - no cached positions survive an asset replacement. Without a home there is no vantage point, so the sky resolves empty (and with it
    // the billboard list below - a homeless track shows no bodies at all).
    const std::vector<SSAtmoEnvResolvedBody> sky_bodies = (home_index >= 0)
        ? SSAtmoEnvPlanetaryResolver::resolveSky(planetary)
        : std::vector<SSAtmoEnvResolvedBody>();

    // World positions too: directions alone cannot say which side of a body the sun is on, and that is the whole of a phase. Same resolve the sky directions came from, so the two describe one
    // arrangement of bodies.
    const std::vector<LLVector3> world_pos = (home_index >= 0)
        ? SSAtmoEnvPlanetaryResolver::resolveWorldPositions(planetary)
        : std::vector<LLVector3>();

    // Emitterless defaults, overwritten below when emitters exist. The moon points straight DOWN whenever no secondary emitter drives it (single-emitter and zero-emitter worlds alike): EEP casts
    // moonlight whenever the moon's z >= 0 (getIsMoonUp), so leaving the default rotation would smuggle a phantom light source into a world whose author placed no such body. Reset each frame: an
    // emitterless track must not keep dimming (or brightening) the moon slot with whatever body used to hold it.
    mMoonSlotBrightness = 1.f;

    // Which body ended up in the moon slot, hoisted out of the emitter block below so the illumination pass after it can ask.
    S32 moon_slot_body = -1;

    // Which bodies took the two light slots, for the debug overlay's benefit - it labels them, so it has to be told rather than guess.
    S32 debug_slot_sun = -1;
    S32 debug_slot_moon = -1;

    // ...and for the disc shader, which needs to know where each slot's own star is in order to draw a terminator across it.
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

    LLVector3 sun_dir = -LLVector3::z_axis;
    LLVector3 moon_dir = -LLVector3::z_axis;
    F32 sun_scale = 1.f;
    F32 moon_scale = 1.f;
    // A null custom texture means "the stock disc" - same guard idiom as the water normal map: actually setting null on the moon would drop its texture entirely, and mapping null to the default also
    // makes an emitter stepping back to null restore the stock look rather than keeping the last custom map. Which stock disc stands in follows the BODY's kind, not the slot it landed in - see the
    // fallback below.
    LLUUID sun_texture = LLSettingsSky::GetDefaultSunTextureId();
    LLUUID moon_texture = LLSettingsSky::GetDefaultMoonTextureId();

    if (!emitters.empty())
    {
        // Which emitter takes which of EEP's two light slots is the resolver's call, not this file's - see resolveLightRoles(), whose comment carries the physical-diameter rule and the sky-entry
        // indexing invariant that go with it. The editor's rise/set markers ask the same function, so what those markers annotate is by construction the body this code lights the world with.
        SSAtmoEnvResolvedBody sun_resolved;
        SSAtmoEnvResolvedBody moon_resolved;
        SSAtmoEnvPlanetaryResolver::resolveLightRoles(planetary, sky_bodies,
                                                      sun_resolved, moon_resolved);
        const S32 sun_body = sun_resolved.mBodyIndex;
        const S32 moon_body = moon_resolved.mBodyIndex;
        moon_slot_body = moon_body;

        // The null-texture fallback follows the BODY's kind, not the slot it landed in: a textureless SUN-kind body shows a sun disc in either slot, anything else the stock moon disc. Nuance for a
        // sun-kind body in the MOON slot: the "default sun" id is null (EEP's built-in sun rendering), and setting null on the moon would drop its texture entirely - so the blank-sun disc ASSET
        // stands in there instead.
        auto fallbackFor = [&planetary](S32 body_index, bool sun_slot) -> LLUUID
        {
            const bool is_sun_kind = planetary.mBodies[static_cast<size_t>(body_index)].mKind
                == SSAtmoEnvCelestialBody::SUN;
            if (is_sun_kind)
            {
                return sun_slot ? LLSettingsSky::GetDefaultSunTextureId()
                                : LLSettingsSky::GetBlankSunTextureId();
            }
            return LLSettingsSky::GetDefaultMoonTextureId();
        };

        // Both slots get IDENTICAL treatment: the emitter's authored home-relative direction swept through the home body's diurnal rotation - so orbital radius, phase and inclination all visibly
        // place the rendered sun exactly as the designer canvas shows, and dragging an emitter's phase moves the light in the sky.
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
                ? body.mCustomTexture : fallbackFor(sun_body, true);
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
                ? body.mCustomTexture : fallbackFor(moon_body, false);
        }
    }

    // Illumination geometry, shared by the billboards and by the moon slot. The lit body is whichever SUN-kind body is largest - not the body in EEP's sun slot, which is a rendering role and could
    // be a moon on a world with no star at all. What lights a moon is a star.
    S32 lamp = -1;
    for (size_t i = 0; i < planetary.mBodies.size(); ++i)
    {
        if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN) continue;
        if (lamp < 0 || planetary.mBodies[i].mDiameterM > planetary.mBodies[(size_t)lamp].mDiameterM)
        {
            lamp = (S32)i;
        }
    }

    // Sun direction from a body, and how much of that sunlight reaches it. Both fall back to "lit from the observer's side, fully" when there is no star to be lit by: a world lit by nothing at all
    // should show its bodies rather than a sky of black discs.
    auto illuminate = [&](S32 body_index, LLVector3& out_dir, F32& out_light)
    {
        out_dir = LLVector3::z_axis;
        out_light = 1.f;
        if (lamp < 0 || home_index < 0 || body_index < 0) return;
        if (world_pos.size() <= (size_t)llmax(lamp, llmax(body_index, home_index))) return;

        const LLVector3 body = world_pos[(size_t)body_index];
        const LLVector3 to_sun_world = world_pos[(size_t)lamp] - body;

        // The inertial sky frame is shared: both this and resolveSky's directions are differences of the same world positions, so no rotation is needed to express one in the other. But every body
        // direction is then swept through the home body's diurnal rotation before it is drawn, and this has to be swept with them. It is a direction in the sky, and the whole sky turns. Leaving it
        // in the inertial frame while the quad's own axes are built from a rotated direction is comparing two different frames: the terminator ends up at the wrong angle across the disc and the
        // phase intensity - a dot product between the two - comes out wrong with it.
        out_dir = to_sun_world;
        if (out_dir.normalize() < 0.0001f) { out_dir = LLVector3::z_axis; return; }
        out_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
            out_dir, tilt_deg, lat_deg, phase);

        // Eclipse: is the home body between this one and the star?
        const LLVector3 home = world_pos[(size_t)home_index];
        const LLVector3 home_to_body = body - home;
        const F32 behind = home_to_body * (-out_dir);   // along the shadow
        if (behind <= 0.f) return;                      // sunward side; nothing to shadow it

        const LLVector3 perp = home_to_body - (-out_dir) * behind;
        const F32 miss = perp.magVec();
        const F32 home_r = planetary.mBodies[(size_t)home_index].mDiameterM * 0.5f;
        if (home_r <= 0.f) return;

        // A real shadow is a CONE, not a cylinder, and the cone matters: the star has an angular size, so the umbra narrows with distance while the penumbra widens. An earlier version used the
        // planet's radius for both edges and faded over another radius, which put the total shadow far too wide and made anything near the anti-sun line almost black. The half-angle is the star's
        // angular radius as seen from the home body, which is data we already have.
        const F32 star_r = planetary.mBodies[(size_t)lamp].mDiameterM * 0.5f;
        const F32 star_dist = (world_pos[(size_t)lamp] - home).magVec();
        const F32 spread = (star_dist > 1.f) ? (star_r / star_dist) * behind : 0.f;

        const F32 umbra = llmax(home_r - spread, 0.f);   // total shadow, narrowing
        const F32 penumbra = home_r + spread;            // partial, widening
        if (miss >= penumbra) return;                    // clean miss

        // Inside the umbra it is not black: a planet with an atmosphere refracts light around its limb, which is what makes a real totally-eclipsed moon a dim red rather than invisible. Modelled as
        // a floor for now - the red is not, and would want its own tint.
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

        // Phase as a scalar for EEP's own moonlight: a new moon lights nothing, a full moon lights the most. The terminator itself is drawn by the shader, which gets the direction rather than this.
        // Both are skipped for a body the author has taken out of phase shading, or one lighting itself: neither has a phase to have.
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

    // Every remaining body - not an emitter (those became the sun/moon above), not home (resolveSky() already excludes it) - is published for LLDrawPoolWLSky to draw as a camera-facing quad, per the
    // design doc's "quad/billboard only for v1". Swept through the same diurnal rotation as the emitters so the whole sky rises and sets as one. The texture is published raw - a null means "no
    // custom texture" and the draw pool substitutes a stock disc chosen by mIsSun, so a textureless body still reads as its kind rather than nothing.
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
        // Same invariant as resolvedFor() above: mBodies is addressed by the entry's own mBodyIndex, never by this loop's position - sky_bodies skips the home body, so positions and body indices
        // diverge.
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

    // The debug overlay's list, gathered from what was just applied rather than resolved a second time - an overlay that disagrees with the sky it is meant to be checking is worse than no overlay.
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
            // Straight off the billboard's own body index. This used to re-sweep every resolved direction and match by distance, which failed intermittently on floating-point noise - so a body's
            // marker flickered in and out frame to frame.
            add_mark(bb.mBodyIndex, bb.mDirection, bb.mAngularDiameterDeg,
                     bb.mSunlight, false, false);
        }
    }

    // Same write-if-changed walk as applySky, on the celestial cache's own validity flag (see the header). While time advances the sun and moon directions change every frame - correct and
    // unavoidable for a keyframed phase - but scales and textures compare away.
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

    // Changing these has to poke the sky into rebuilding its own geometry. LLVOSky bakes the sun and moon quads from hb.getRotation() inside updateGeometry, which only runs when the sky drawable is
    // marked for rebuild - and that is throttled (UPDATE_EXPRY, a quarter second) and gated on the sky deciding it needs one at all. An authored sun that barely moves frame to frame never trips that
    // test, so the disc sat at whatever direction was current the last time the sky felt like rebuilding, while a billboard is placed from the live direction every frame. Toggling a body between the
    // two paths therefore moved it - which is the "the sun quad drifts from its original position" this fixes.
    bool celestial_moved = false;
    put(mLastSunDir, sun_dir,
        [this, &celestial_moved](const LLVector3& v)
        { mSky->setSunRotation(quat_from_direction(v)); celestial_moved = true; });
    put(mLastMoonDir, moon_dir,
        [this, &celestial_moved](const LLVector3& v)
        { mSky->setMoonRotation(quat_from_direction(v)); celestial_moved = true; });
    // Scale is baked into the same vertices, so it wants the same rebuild.
    put(mLastSunScale, sun_scale,
        [this, &celestial_moved](F32 v) { mSky->setSunScale(v); celestial_moved = true; });
    put(mLastMoonScale, moon_scale,
        [this, &celestial_moved](F32 v) { mSky->setMoonScale(v); celestial_moved = true; });
    put(mLastSunTexture, sun_texture,
        [this](const LLUUID& v) { mSky->setSunTextureId(v); });
    put(mLastMoonTexture, moon_texture,
        [this](const LLUUID& v) { mSky->setMoonTextureId(v); });

    mCelestialCacheValid = true;

    if (dirty)
    {
        mSky->update();
    }

    // The sun and moon discs are geometry, and geometry the sky only rebuilds when asked. Asked, then - otherwise the disc lags the direction that was just written by up to a rebuild interval.
    if (celestial_moved && gSky.mVOSkyp.notNull())
    {
        gSky.mVOSkyp->forceSkyUpdate();
    }
}

// </SS:Nexii>
