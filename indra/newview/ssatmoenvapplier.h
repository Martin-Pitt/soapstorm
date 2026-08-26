/**
 * @file ssatmoenvapplier.h
 * @brief Atmo Magic: drives the on-screen sky and water from the loaded
 *        environment asset. The sink is EEP's ENV_LOCAL slot - the same
 *        public LLEnvironment mechanism the Personal Lighting floater
 *        uses - and this class is deliberately the ONLY code that knows
 *        that, so a future parallel renderer is a swap of this one class.
 *        See doc/atmo_magic_environment.md, "Rendering integration (the
 *        applier)". No EEP-proper source file is modified; injection is
 *        exclusively through public LLEnvironment API.
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

// <SS:Nexii> Atmo Magic: rendering applier

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

// One non-emitter, non-home celestial body resolved for this frame's sky: everything the WL sky draw pool needs to draw the camera-facing quad the design doc's Planetary section calls for
// ("quad/billboard only for v1"). The direction has already been swept through the home body's diurnal rotation, so consumers just draw where it points.
struct SSAtmoEnvBillboard
{
    // Which body in the track's planetary this is. Carried rather than re-derived: the debug overlay used to match billboards back to bodies by comparing directions, which failed on floating-point
    // noise and made markers flicker.
    S32 mBodyIndex = -1;

    LLVector3 mDirection;           // unit vector, observer -> body
    F32 mAngularDiameterDeg = 0.f;
    // May be null: the draw pool substitutes a stock disc, chosen by the BODY's kind (mIsSun) - a textureless sun reads as a sun, not a moon.
    LLUUID mTexture;
    bool mIsSun = false;            // SUN-kind body - picks the fallback disc

    // Where the sun is FROM THIS BODY, as a unit vector in the observer's sky frame. The draw pool shades the quad as the sphere it stands in for, so this is what puts a terminator across it: a moon
    // a quarter of the way round its orbit is drawn as a quarter moon, without anybody authoring a phase.
    LLVector3 mSunDirection;

    // 0 fully eclipsed by the home body, 1 in open sunlight. A moon passing behind its planet stops being lit, which is a thing worth seeing on a world whose moon is close in.
    F32 mSunlight = 1.f;

    // Straight from the body - see SSAtmoEnvCelestialBody::mEmissive. An emissive body is drawn at full brightness with no terminator, and a body with phase shading off is drawn as a flat disc the
    // way the stock sky draws one.
    bool mEmissive = false;
    bool mPhaseShaded = true;
};

class SSAtmoEnvApplier : public LLSingleton<SSAtmoEnvApplier>
{
    LLSINGLETON(SSAtmoEnvApplier);
    ~SSAtmoEnvApplier() = default;

public:
    // Per-frame entry point, called from llviewerdisplay right after the SSAtmoMagic idle touchpoint. Handles the whole lifecycle itself: becoming active (SSAtmoEnabled on and an asset loaded)
    // installs the settings pair into ENV_LOCAL, becoming inactive clears it and lets the selection fall back, and while active every frame re-resolves the track/phase and writes changed values
    // through the settings setters. Safe to call in any state - no region, no asset, asset replaced since last frame all just resolve to the right thing.
    void apply();

    // What the weather cube did to the authored sky on the most recent apply() - identity when influence is off, or before the first frame. Published so the Weather Influence editor can show each
    // mapping's live effect instead of the author having to infer it from the sky, the same way the Auto weather fields show their computed values.
    const SSAtmoEnvSkyModulation& lastModulation() const { return mLastModulation; }

    // How far the cloud layer has travelled on the wind, in metres, east and north. Integrated here rather than in the draw pool because this is where the weather is evaluated and where the
    // influence dial that gates it lives; the sky pass just reads it and shifts the layer's UVs by it. Wrapped at a round million metres. The shader divides it by the layer's metres-per-UV, so what
    // matters is that the number stays small enough to keep single-precision meaningful at that scale, not that it means anything on its own.
    const LLVector2& cloudDriftMetres() const { return mCloudDriftM; }

    // Sun direction from the body currently in EEP's moon slot, in the observer's sky frame. The sky pass shades that quad as a sphere with it - see the phase block in moonF.glsl.
    const LLVector3& moonSunDirection() const { return mMoonSunDir; }

    // Whether the bodies in EEP's two light slots are emissive - see SSAtmoEnvCelestialBody::mEmissive. The sky pass asks so it can draw those discs at full luminance instead of at whatever the
    // sky's own interpolated body colour has faded them to. What the bodies in EEP's two light slots are, as the disc shader needs them: whether they light themselves, whether they take a phase,
    // where their own star is, and how much of its light reaches them.
    bool sunSlotEmissive() const { return mSunSlotEmissive; }
    bool moonSlotEmissive() const { return mMoonSlotEmissive; }
    bool sunSlotPhaseShaded() const { return mSunSlotPhaseShaded; }
    bool moonSlotPhaseShaded() const { return mMoonSlotPhaseShaded; }
    const LLVector3& sunSlotSunDirection() const { return mSunSlotSunDir; }
    F32 sunSlotSunlight() const { return mSunSlotSunlight; }
    F32 moonSlotSunlight() const { return mMoonSlotSunlight; }

    // Angular DIAMETER of the body in each light slot, in degrees, as actually drawn. The water pass wants it: a reflection's spread is the surface's roughness convolved with the light's angular
    // size, and without this it can only ever use the roughness - so every body, however large its disc, laid down the same glitter path.
    F32 sunSlotAngularDeg() const { return mSunSlotAngularDeg; }
    F32 moonSlotAngularDeg() const { return mMoonSlotAngularDeg; }

    // Where the home body's rotation axis points, in the same observer frame the disc directions are given in: due north, elevation equal to the observer's latitude. The sky turns about this, so it
    // is what a body's own face is fixed relative to - the sky pass rotates the disc art by the angle between it and the zenith. See ss_bind_disc.
    const LLVector3& observerPole() const { return mObserverPole; }

    // Debug overlay, toggled from the System Designer: a ray toward every body in the sky with its name and reading beside it, so where a body actually IS can be checked against where the designer
    // says it is. Gated by the caller on the SSAtmoPlanetaryDebugOverlay setting.
    void renderCelestialDebug();

    // Angular diameter that produced a slot's disc scale - the inverse of celestialDiscScale, for the overlay's readout.
    static F32 celestialAngularFromScale(F32 scale);

    // Whether Atmo Magic currently owns the sky. The sky pass asks before applying anything that would change how a stock environment renders.
    bool isActive() const { return mActive; }

    // Whether the frame's resolved track has its water plane enabled - the gate for everything that only exists when Atmo owns the WATER, not just the sky (currently the far sea disc). Only
    // meaningful while isActive(); callers pair the two. [interaction: SSFarSea via LLDrawPoolWater]
    bool waterPlaneOn() const { return mWaterPlaneOn; }

    // Whether the void-water standdown is currently latched - overlay ground truth for "whose water is that grid".
    bool voidWaterDerendered() const { return mVoidDerendered; }

    // This frame's resolved billboard bodies - every body that is neither a light emitter nor home - rebuilt by apply() and consumed by LLDrawPoolWLSky::renderHeavenlyBodies(). Empty whenever the
    // applier is inactive (or the track has nothing beyond its emitters), so the draw pool's whole gate is one emptiness check.
    const std::vector<SSAtmoEnvBillboard>& celestialBillboards() const { return mBillboards; }

    // Angular diameter -> EEP disc scale, shared between the emitter mapping in applyCelestial() and the draw pool's billboard sizing so a billboard body and the moon at equal angular size render at
    // equal size. See REFERENCE_SUN_DIAMETER_DEG in the .cpp.
    static F32 celestialDiscScale(F32 angular_diameter_deg);

private:
    void activate();
    void deactivate();
    void install();

    void applySky(const SSAtmoEnvTrack& track, F64 phase,
                  const SSAtmoEnvSkyModulation& mod);
    void applyWater(const SSAtmoEnvTrack& track, F64 phase,
                    const SSAtmoEnvSkyModulation& mod);

    // The weather cube's push on this frame's authored values - computed once per frame in apply() and handed down, so the two setters below share one derivation rather than each asking for their
    // own.
    SSAtmoEnvSkyModulation computeModulation(const SSAtmoEnvTrack& track, F64 phase);

    // sin(elevation) of whichever body currently holds the sun slot. Resolved on demand rather than cached from applyCelestial(): only the rainbow gate wants it, and only in the minutes after rain
    // stops, so paying for it every frame to serve that would be the wrong trade.
    F32 sunElevationSin(const SSAtmoEnvTrack& track, F64 phase) const;

    //-------------------------------------------------------------------
    // Rain-stop trail - the one piece of state the modulation model needs beyond the current instant (rainbows happen AFTER rain, which no evaluation of "right now" can know). Kept here rather than
    // in the modulator so that stays a pure function.
    //-------------------------------------------------------------------

    bool mWasPrecipitating = false;
    // Seconds since precipitation last stopped; negative means "not applicable" - it is raining now, or it has not rained since this applier started caring. Reset by install()/release() along with
    // the caches: a fresh environment has no rain history to remember.
    F32 mSecondsSinceRainStopped = -1.f;
    F64 mLastTrailUpdate = 0.0;

    // Last computeModulation() result - see lastModulation().
    SSAtmoEnvSkyModulation mLastModulation;
    LLVector2 mCloudDriftM;

    // How lit the body in the EEP moon slot currently is: its phase times whatever the home body's shadow leaves of it. Scales the authored Moon Brightness, which stays the master dial - see
    // applySky.
    F32 mMoonSlotBrightness = 1.f;

    // Where the sun is from the body in the moon slot, for the terminator the moon shader draws across it.
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

    //-------------------------------------------------------------------
    // Debug overlay bookkeeping. The rays are drawn from the render path, but the names are LLHUDText - world-anchored labels the HUD pass already knows how to draw and fade - so they are objects
    // with a life of their own rather than something painted per frame.
    //-------------------------------------------------------------------
    std::vector<LLPointer<class LLHUDText> > mDebugLabels;
    void releaseDebugLabels();

    // What the overlay draws, gathered by the celestial pass so the overlay reports what was actually applied rather than resolving its own second opinion.
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
    // The active track has water disabled: rather than uninstalling the water half (install/uninstall churn on every track cross), the installed instance is walked back to EEP's stock defaults,
    // sourced from mDefaultWater's getters.
    void applyWaterDefaults();

    // Water off on a track means no water plane at all, not "water with default settings": the render type itself is switched off, which is the only way to actually remove SL's water surface (EEP
    // water settings describe how water looks, never whether there is any). Restored on deactivate() and whenever a track with water becomes active. mWaterDerendered records that WE were the ones
    // who turned it off, so a user who had water off in their own view keeps it off and we never turn something back on that we did not turn off - the same bookkeeping FSAllowEEPWaterDerender does
    // in llenvironment.cpp.
    void setWaterRendering(bool enabled);
    bool mWaterDerendered = false;
    bool mWaterPlaneOn = false;

    // Void water alone - the ocean filler tiles, not region water - stands down while the far sea disc is up: the disc's squashed drawn depth compresses metres of true separation into
    // sub-millimetres, so beyond the squash knee the two can only z-fight, and the disc IS the void water's replacement out there anyway. Same only-undo-our-own-change bookkeeping as
    // mWaterDerendered, and always restored BEFORE any setWaterRendering(false) so LLPipeline::toggleRenderType's WATER/VOIDWATER lockstep flip never desynchronises the pair.
    // [interaction: SSFarSea, LLPipeline::toggleRenderType]
    void setVoidWaterRendering(bool enabled);
    bool mVoidDerendered = false;

    // Planetary lighting: the active track's light emitters drive the rendered sun and moon. Both slots get identical treatment - the emitter's authored home-relative direction from resolveSky(),
    // swept by the home body's diurnal rotation - plus per-emitter disc scale and custom texture, per the design doc's Planetary section. The SUN slot goes to whichever emitter subtends the larger
    // apparent angular diameter (not list order), the other takes the moon slot. Zero emitters (or no home body to observe from) points both straight down: a dim, sun-below-horizon sky rather than a
    // fallback sun.
    void applyCelestial(const SSAtmoEnvTrack& track, F64 phase);

    bool mActive = false;

    // The one settings pair, kept across frames and mutated in place - the DayInstance holds the same shared_ptrs, so setters + update() propagate without a reinstall. Only identity changes
    // reinstall.
    LLSettingsSky::ptr_t   mSky;
    LLSettingsWater::ptr_t mWater;

    // Pristine EEP defaults, built alongside mWater on activation: the value source for applyWaterDefaults() and for a null-normal-map fallback, without hardcoding EEP's numbers here.
    LLSettingsWater::ptr_t mDefaultWater;

    // The default glow's green component, captured from the freshly built
    // sky - the UI-space schema only stores size (-> packed r) and focus
    // (-> packed b), so g is preserved rather than invented.
    F32 mGlowG = 0.f;

    // SSAtmoEnvTrackResolver wants this evaluation's altitude and the previous one, plus whether a jump happened - tracked here exactly the way SSAtmoMagic::refreshParams() tracks its own copy for
    // the precipitation bridge (region change or an implausibly large single-frame jump both count as "teleported"). Deliberately a separate set of members rather than reaching into SSAtmoMagic's:
    // that state is private bookkeeping of the bridge's own resolve call, not a published "where was the camera" service.
    F32    mPrevWorldZ = 0.f;
    bool   mPrevWorldZValid = false;
    LLUUID mPrevRegionID;

    // Last-applied values, one per setter actually driven. The LLSettings setters mark the whole settings object dirty (re-uploading shader uniforms downstream), so identical values are compared
    // away here rather than re-set every frame. Exact compares are enough: the same keyframe evaluation at the same phase is bit-identical.
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

    // The Sky Dome (legacy cirrus layer - see SSAtmoEnvCloudDome). The density/detail caches hold the PACKED triples, matching the one setter each triple folds into, so a change to any of the three
    // scalars re-applies exactly that one setter.
    LLColor3 mLastCloudColor;
    F32 mLastCloudCoverage = 0.f;
    F32 mLastCloudScale = 0.f;
    F32 mLastCloudVariance = 0.f;
    LLVector2 mLastCloudScroll;
    LLColor3 mLastCloudDensity;
    LLColor3 mLastCloudDetail;
    LLUUID mLastCloudNoise;

    // Celestial fields get their own validity flag: applySky() runs first each frame and flips mSkyCacheValid before applyCelestial() would read it, so sharing the sky flag would skip the mandatory
    // first re-apply after (re)install. The rotation caches key on the resolved DIRECTION rather than the quaternion - same identity, cheaper compare, and the quat conversion only runs when the sun
    // actually moved (it does move every frame while time advances; scales and textures stay compared away).
    bool mCelestialCacheValid = false;
    LLVector3 mLastSunDir;
    LLVector3 mLastMoonDir;
    F32 mLastSunScale = 0.f;
    F32 mLastMoonScale = 0.f;
    LLUUID mLastSunTexture;
    LLUUID mLastMoonTexture;

    // Backing store for celestialBillboards(). Rebuilt from scratch every applyCelestial() - a handful of entries, and rebuilding is what guarantees no stale directions survive an asset replacement.
    // Not change-detected like the setter caches: nothing downstream is dirtied by writing it, the draw pool just reads it.
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

// </SS:Nexii>

#endif // SS_ATMOENVAPPLIER_H
