/**
 * @file ssfloateratmoenv.cpp
 * @brief Atmo Magic floater implementation - phase 1.
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

#include "ssfloateratmoenv.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvweatherstate.h"
#include "ssatmoenvcloudfieldstate.h"
#include "ssatmoenvplanetarystate.h"
#include "ssfloateratmoplanetary.h"
#include "ssfloateratmoinfluence.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llfloatercolorpicker.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llmultisliderctrl.h"
#include "llnotificationsutil.h"
#include "llpermissionsflags.h"
#include "llsettingssky.h"
#include "llsettingsvo.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltexturectrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

#include <algorithm>

// <SS:Nexii> Atmo Magic floater

static const F64 STATUS_POLL_INTERVAL = 0.5;

// U+25C6 filled / U+25C7 hollow, as escaped UTF-8 bytes rather than literal
// glyphs - that keeps this file pure ASCII, so a tool that mis-guesses the
// encoding cannot mangle them. One already did: they had become
// C3A2 C297 C286, which is E2 97 86 decoded as latin-1 and re-encoded.
static const char* const FILLED_DIAMOND = "\xE2\x97\x86";
static const char* const HOLLOW_DIAMOND = "\xE2\x97\x87";

// Rise/set markers on the preview scrubber. Same escaped-UTF-8 treatment as
// the diamonds above, and the same Geometric Shapes block they come from:
// U+25B2/U+25BC are the full-size up/down triangles, U+25B4/U+25BE their
// small siblings. Big = the sun, small = the moon, so which body a marker
// belongs to reads at a glance without a legend, in the same way the sun is
// the bigger light in the sky.
static const char* const RISE_TRIANGLE = "\xE2\x96\xB2";
static const char* const SET_TRIANGLE = "\xE2\x96\xBC";
static const char* const RISE_TRIANGLE_SMALL = "\xE2\x96\xB4";
static const char* const SET_TRIANGLE_SMALL = "\xE2\x96\xBE";

// Warm daylight amber against a pale moonlit blue-white - the two lights'
// own colours, kept dim enough that markers annotate the scrubber rather
// than competing with the keyframe ghosts drawn on the same strip.
static const LLColor4 SUN_MARKER_COLOUR(1.f, 0.8f, 0.4f, 0.8f);
static const LLColor4 MOON_MARKER_COLOUR(0.72f, 0.78f, 0.95f, 0.75f);

// How far outside the preview scrubber still counts as hovering it, for the
// whole-tab keyframe overview. The scrubber is an 18px strip in a busy
// floater; without a little slack around it the overview flickers on and off
// as the pointer drifts a pixel or two.
static const S32 HOVER_PAD_X = 6;
static const S32 HOVER_PAD_Y = 10;

SSFloaterAtmoEnv::SSFloaterAtmoEnv(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoEnv::postBuild()
{
    getChild<LLButton>("create_new_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCreateNew(); });
    getChild<LLButton>("load_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickLoad(); });
    getChild<LLButton>("save_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickSave(); });
    getChild<LLButton>("revert_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRevert(); });
    getChild<LLButton>("unload_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickUnload(); });
    getChild<LLButton>("add_track_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddTrack(); });
    getChild<LLButton>("remove_track_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveTrack(); });

    getChild<LLButton>("track_ground_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickGroundRow(); });

    // The rail's name buttons are the tab selector for the panel on the
    // right - slot N is mTracks[N], see refreshAltitudeLabel().
    for (S32 slot = 1; slot < SS_ATMOENV_MAX_TRACKS; ++slot)
    {
        getChild<LLButton>(llformat("track_name_button_%d", slot))->setClickedCallback(
            [this, slot](LLUICtrl*, const LLSD&) { selectTrack(slot); });
    }

    // Commit only - deliberately no mouse-down hook. LLMultiSlider fires
    // its mouse-down signal *before* it works out which thumb was actually
    // hit (see LLMultiSlider::handleMouseDown), so a handler there reads
    // the previously selected thumb and picks the wrong track. Selection
    // belongs to the name buttons anyway; dragging a thumb selects that
    // track too, but from the commit, where mCurSlider is correct.
    LLMultiSliderCtrl* alt_slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    alt_slider->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitAltitudeSlider(); });
    alt_slider->setSliderMouseUpCallback(
        [this](LLUICtrl*, const LLSD&) { onMouseUpAltitudeSlider(); });

    getChild<LLUICtrl>("name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitName(); });

    // The Weather Influence editor is reachable from every tab whose
    // parameters weather can modulate; all four buttons open the one
    // floater. findChild rather than getChild: a tab whose panel failed
    // to load should not take the whole floater down with it.
    static const char* const INFLUENCE_BUTTONS[] = {
        "influence_button_weather", "influence_button_water",
        "influence_button_clouds", "influence_button_atmosphere"
    };
    for (const char* button_name : INFLUENCE_BUTTONS)
    {
        LLUICtrl* button = findChild<LLUICtrl>(button_name);
        if (button)
        {
            button->setCommitCallback(
                [this](LLUICtrl*, const LLSD&) { onClickWeatherInfluence(); });
        }
    }

    {
        LLUICtrl* match_button = findChild<LLUICtrl>("match_button_atmosphere");
        if (match_button)
        {
            match_button->setCommitCallback(
                [this](LLUICtrl*, const LLSD&)
                { LLFloaterReg::showInstance("ss_atmo_match", LLSD(mSelectedTrackIndex)); });
        }
    }

    getChild<LLUICtrl>("preview_time_slider")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitPreviewTime(); });

    // Track tab - see the header comment above refreshTrackTab().
    getChild<LLUICtrl>("track_name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitTrackName(); });
    const char* day_cycle_fields[] = { "day_length_slider", "day_offset_slider",
                                      "day_length_value_spinner", "day_offset_value_spinner" };
    for (const char* name : day_cycle_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitDayCycle(); });
    }

    // Planetary tab - just the two scale dials and the designer button;
    // the bodies themselves are edited in SSFloaterAtmoPlanetary.
    const char* planetary_scale_fields[] = { "sun_planet_scale_slider", "sun_planet_scale_spinner",
                                             "planet_moon_scale_slider", "planet_moon_scale_spinner" };
    for (const char* name : planetary_scale_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitPlanetaryScales(); });
    }
    getChild<LLButton>("open_planetary_designer_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickOpenPlanetaryDesigner(); });

    // Water tab - the one non-keyframed control; everything else on that
    // tab is a keyframe row set up alongside the Weather cube's below.
    getChild<LLUICtrl>("water_enabled_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWaterEnabled(); });

    // The Auto structural toggles - see refreshAutoRows() for what
    // flipping one greys out and what fills the greyed rows in.
    getChild<LLUICtrl>("gust_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitGustAuto(); });
    getChild<LLUICtrl>("lightning_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitLightningAuto(); });
    getChild<LLUICtrl>("cloud_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitCloudAuto(); });

    // mSelectedTrackIndex's Weather cube, one AE-style row per field - see
    // the FloatRow comment in the header. mField captures `this` rather
    // than being cached, so switching the selected track (or an add/remove
    // track reallocating SSAtmoEnvAsset::mTracks) can never leave a
    // dangling reference or a stale track index behind.
    mFloatRows = {
        { "moisture",     [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mMoisture; },     false },
        { "convection",   [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mConvection; },   false },
        { "temperature",  [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mTemperatureC; }, true },
        { "wind_heading", [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mWindHeading; },  true },
        { "wind_speed",   [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mWindSpeed; },    true },
        { "gust_depth",   [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mGustDepth; },    false },
        { "gust_length",  [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mGustLength; },   true },
        { "gust_veer",    [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mGustVeer; },     true },
        { "lightning_intensity", [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mLightningIntensity; }, false },
    };

    // The Water tab's own scalar rows - same mechanics, same widget naming
    // convention, just a different struct on the far end of the accessor.
    auto water = [this]() -> SSAtmoEnvWater& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWater;
    };
    const std::vector<FloatRow> water_rows = {
        { "water_height",           [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mHeight; },               false },
        { "water_fog_density",      [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mFogDensity; },           false },
        { "water_underwater_mod",   [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mUnderwaterModifier; },   false },
        { "water_fresnel_scale",    [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mFresnelScale; },         false },
        { "water_fresnel_offset",   [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mFresnelOffset; },        false },
        { "water_normal_scale_x",   [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mNormalScaleX; },         false },
        { "water_normal_scale_y",   [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mNormalScaleY; },         false },
        { "water_normal_scale_z",   [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mNormalScaleZ; },         false },
        { "water_refraction_above", [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mRefractionScaleAbove; }, false },
        { "water_refraction_below", [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mRefractionScaleBelow; }, false },
        { "water_blur_multip",      [water]() -> SSAtmoEnvKeyframed<F32>& { return water().mBlurMultiplier; },       false },
    };
    mFloatRows.insert(mFloatRows.end(), water_rows.begin(), water_rows.end());

    // The Clouds tab's Volumetric Field sub-tab rows - the storm layer's
    // artist baseline. Live coverage/density derive from these plus the
    // weather cube (see ssatmoenvcloudfieldstate); the legacy Windlight
    // cloud plane (cirrus) is the Sky Dome sub-tab's below, a separate
    // thing these rows never touch.
    auto clouds = [this]() -> SSAtmoEnvCloudField& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mCloudField;
    };
    const std::vector<FloatRow> cloud_rows = {
        { "cloud_base_height", [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mBaseHeightM; },    true },
        { "cloud_thickness",   [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mBaseThicknessM; }, true },
        { "cloud_coverage",    [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mCoverageScale; },  false },
    };
    mFloatRows.insert(mFloatRows.end(), cloud_rows.begin(), cloud_rows.end());

    // The Sky Dome sub-tab's scalar rows - the legacy cirrus layer's EEP
    // parameter set (see SSAtmoEnvCloudDome). The non-scalar rows (colour,
    // scroll vector, noise texture) join their typed lists below.
    auto dome = [this]() -> SSAtmoEnvCloudDome& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mCloudDome;
    };
    const std::vector<FloatRow> dome_rows = {
        { "dome_coverage",  [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mCoverage; }, false },
        { "dome_scale",     [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mScale; },    false },
        { "dome_variance",  [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mVariance; }, false },
        { "dome_density_x", [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mDensityX; }, false },
        { "dome_density_y", [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mDensityY; }, false },
        { "dome_density_d", [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mDensityD; }, false },
        { "dome_detail_x",  [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mDetailX; },  false },
        { "dome_detail_y",  [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mDetailY; },  false },
        { "dome_detail_d",  [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mDetailD; },  false },
    };
    mFloatRows.insert(mFloatRows.end(), dome_rows.begin(), dome_rows.end());

    // The Atmosphere & Lighting tab's scalar rows - the EEP haze/gamma/glow
    // set (see SSAtmoEnvAtmosphere). No azimuth/elevation rows anywhere
    // here, deliberately: sun/moon position is computed from the Planetary
    // tab's home body, not keyframed on this tab. The colour half of the
    // tab joins mColorRows below.
    auto atmos = [this]() -> SSAtmoEnvAtmosphere& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mAtmosphere;
    };
    const std::vector<FloatRow> atmos_rows = {
        { "atmo_haze_horizon",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mHazeHorizon; },        false },
        { "atmo_haze_density",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mHazeDensity; },        false },
        { "atmo_moisture_level",  [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSkyMoistureLevel; },   false },
        { "atmo_droplet_radius",  [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSkyDropletRadius; },   false },
        { "atmo_ice_level",       [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSkyIceLevel; },        false },
        { "atmo_density_mult",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mDensityMultiplier; },  false },
        { "atmo_distance_mult",   [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mDistanceMultiplier; }, false },
        { "atmo_max_altitude",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mMaxAltitude; },        true },
        { "atmo_probe_ambiance",  [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mReflectionProbeAmbiance; }, false },
        { "atmo_scene_gamma",     [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSceneGamma; },         false },
        { "atmo_glow_focus",      [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mGlowFocus; },          false },
        { "atmo_glow_size",       [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mGlowSize; },           false },
        { "atmo_star_brightness", [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mStarBrightness; },     true },
        { "atmo_moon_brightness", [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mMoonBrightness; },     false },
    };
    mFloatRows.insert(mFloatRows.end(), atmos_rows.begin(), atmos_rows.end());

    for (const FloatRow& row : mFloatRows)
    {
        getChild<LLUICtrl>(row.mPrefix + "_slider")->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitFloatRow(row); refreshPreview(); refreshStatus(); });
        getChild<LLUICtrl>(row.mPrefix + "_value_spinner")->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitFloatRowSpinner(row); refreshPreview(); refreshStatus(); });
        bindKeyframeButtons<F32>(row.mPrefix, row.mField);
    }

    // The water height slider's ceiling comes from the constant rather than
    // from the XML alone, so the bound the floater enforces and the one the
    // schema documents cannot drift apart. It is only a UI bound - see
    // SS_ATMOENV_WATER_CEILING - so a value loaded from a notecard above it
    // is displayed pinned at the top of the slider rather than rewritten.
    getChild<LLSliderCtrl>("water_height_slider")->setMaxValue(SS_ATMOENV_WATER_CEILING);
    getChild<LLSpinCtrl>("water_height_value_spinner")->setMaxValue(SS_ATMOENV_WATER_CEILING);

    // The Water tab's non-scalar keyframed controls. Split by type rather
    // than crammed into one list - see the KeyRow comment in the header.
    // The HDR scales EEP's own editor uses - see KeyRow::mScale.
    static const F32 SCALE_SUN_AMBIENT = 3.f;
    static const F32 SCALE_BLUE = 2.f;

    mColorRows = {
        { "water_fog_color", [water]() -> SSAtmoEnvKeyframed<LLColor3>& { return water().mFogColor; } },
        // Atmosphere & Lighting's colour half - same swatch row the water
        // fog colour uses, one entry per EEP colour. Cloud colour is the
        // Sky Dome's row below, not this tab's - it moved out with the
        // rest of the legacy layer's parameters.
        { "atmo_ambient",        [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mAmbientColor; }, SCALE_SUN_AMBIENT },
        { "atmo_blue_horizon",   [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mBlueHorizon; }, SCALE_BLUE },
        { "atmo_blue_density",   [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mBlueDensity; }, SCALE_BLUE },
        { "atmo_sunlight_color", [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mSunlightColor; }, SCALE_SUN_AMBIENT },
        { "dome_color",          [dome]() -> SSAtmoEnvKeyframed<LLColor3>& { return dome().mColor; } },
    };
    for (const KeyRow<LLColor3>& row : mColorRows)
    {
        getChild<LLUICtrl>(row.mPrefix)->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitColorRow(row); refreshPreview(); refreshStatus(); });
        bindKeyframeButtons<LLColor3>(row.mPrefix, row.mField);
    }

    mVectorRows = {
        { "water_large_wave", [water]() -> SSAtmoEnvKeyframed<LLVector2>& { return water().mLargeWaveSpeed; } },
        { "water_small_wave", [water]() -> SSAtmoEnvKeyframed<LLVector2>& { return water().mSmallWaveSpeed; } },
        { "dome_scroll",      [dome]() -> SSAtmoEnvKeyframed<LLVector2>& { return dome().mScrollRate; } },
    };
    for (const KeyRow<LLVector2>& row : mVectorRows)
    {
        getChild<LLUICtrl>(row.mPrefix)->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitVectorRow(row); refreshPreview(); refreshStatus(); });
        for (const char* axis : { "_x_spinner", "_y_spinner" })
        {
            getChild<LLUICtrl>(row.mPrefix + axis)->setCommitCallback(
                [this, row](LLUICtrl*, const LLSD&) { commitVectorSpinners(row); refreshPreview(); refreshStatus(); });
        }
        bindKeyframeButtons<LLVector2>(row.mPrefix, row.mField);
    }

    mTextureRows = {
        { "water_normal_map", [water]() -> SSAtmoEnvKeyframed<LLUUID>& { return water().mNormalMap; } },
        { "dome_image",       [dome]() -> SSAtmoEnvKeyframed<LLUUID>& { return dome().mNoiseTexture; } },
    };
    for (const KeyRow<LLUUID>& row : mTextureRows)
    {
        getChild<LLUICtrl>(row.mPrefix)->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitTextureRow(row); refreshPreview(); refreshStatus(); });
        bindKeyframeButtons<LLUUID>(row.mPrefix, row.mField);
    }

    // The dome's noise picker previews the stock cloud noise for a null
    // value and allows None - null IS the schema's "default noise" value
    // (see SSAtmoEnvCloudDome::mNoiseTexture), so without an explicit None
    // choice an author could never step a keyframe back to the default.
    // Same pair of calls EEP's own clouds panel makes on its picker.
    LLTextureCtrl* dome_image = getChild<LLTextureCtrl>("dome_image");
    dome_image->setDefaultImageAssetID(LLSettingsSky::GetDefaultCloudNoiseTextureId());
    dome_image->setAllowNoTexture(true);

    // The Weather tab's precipitation override - a dropdown over a
    // keyframed string. Same cluster machinery as every other row; the
    // string's own lerp is what makes its keyframes hold rather than blend.
    mStringRows = {
        { "precipitation_combo", [this]() -> SSAtmoEnvKeyframed<std::string>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mPrecipitationOverride; } },
    };
    for (const KeyRow<std::string>& row : mStringRows)
    {
        getChild<LLUICtrl>(row.mPrefix)->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitStringRow(row); refreshPreview(); refreshStatus(); });
        bindKeyframeButtons<std::string>(row.mPrefix, row.mField);
    }

    refreshVisibility();
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
    return true;
}

void SSFloaterAtmoEnv::onOpen(const LLSD& key)
{
    refreshVisibility();
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

void SSFloaterAtmoEnv::onClose(bool app_quitting)
{
    // Give the real wall clock back control the moment nobody's actually
    // looking at a preview - see SSAtmoEnvManager::setPreviewPhaseOverride().
    // onVisibilityChange (below) already covers the common single-instance
    // hide-not-destroy case, but this catches actual destruction too.
    SSAtmoEnvManager::getInstance()->clearPreviewPhaseOverride();
    LLFloater::onClose(app_quitting);
}

void SSFloaterAtmoEnv::onVisibilityChange(bool new_visibility)
{
    LLFloater::onVisibilityChange(new_visibility);

    if (new_visibility)
    {
        // Re-establish the override immediately rather than waiting for
        // draw()'s next 0.5s poll tick, so reopening the floater doesn't
        // leave the live scene on the real wall clock for a moment first.
        refreshPreview();
    }
    else
    {
        SSAtmoEnvManager::getInstance()->clearPreviewPhaseOverride();
    }
}

void SSFloaterAtmoEnv::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);

    // After the base class has moved everything the layout owns, put the
    // rail's own markers back where the rail now says they belong. See the
    // header on why the periodic refresh cannot cover this.
    repositionRailMarkers();
}

void SSFloaterAtmoEnv::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;
        refreshStatus();
        // Whether hasAsset() has flipped since the last check - a fetch
        // completing while this floater sits open should reveal the
        // editing chrome without needing to be reopened. Track list and
        // keyframe proof are refreshed here too rather than only at click
        // time, since loadFromInventory()'s fetch is asynchronous and may
        // complete well after the click that started it.
        refreshVisibility();
        refreshTrackTab();
        // Skip the keyframe/slider refresh (including the altitude rail's
        // own full rebuild) while any of this floater's own widgets has
        // mouse capture (a slider mid-drag) - forcing every slider's thumb
        // back to its last-committed value out from under an active drag,
        // every 0.5s, made a real drag look like it was fighting itself or
        // "not sticking". A drag's own hover handler already keeps
        // mPreviewPhase/the field values/the dragged track's altitude live
        // via its continuous commit, so skipping this one poll tick loses
        // nothing.
        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            refreshTrackRail();
            refreshPlanetaryScales();
            refreshPreview();
        }
    }

    LLFloater::draw();

    // After the children, so the ghosts sit on top of the scrubber they
    // annotate rather than being painted over by it. Rise/set markers go
    // down first: they are permanent furniture, and a keyframe ghost
    // landing on the same pixel is the more urgent of the two to read.
    drawRiseSetMarkers();
    drawKeyframeGhosts();
    drawSliderValueGhosts();
}

//-----------------------------------------------------------------------------
// Drag and drop
//-----------------------------------------------------------------------------

bool SSFloaterAtmoEnv::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                        EDragAndDropType cargo_type, void* cargo_data,
                                        EAcceptance* accept, std::string& tooltip_msg)
{
    // Type dispatch: a notecard drop loads a whole environment (below,
    // unchanged); a settings drop imports one EEP sky into the loaded one,
    // so it needs an asset to import into - with nothing loaded there is
    // no selected track to stamp, and the drop is refused at hover time.
    if (cargo_type == DAD_SETTINGS)
    {
        if (!SSAtmoEnvManager::getInstance()->hasAsset())
        {
            *accept = ACCEPT_NO;
            return false;
        }
        *accept = ACCEPT_YES_SINGLE;
        if (drop)
        {
            handleSettingsDrop((const LLInventoryItem*)cargo_data);
        }
        return true;
    }

    if (cargo_type != DAD_NOTECARD)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    *accept = ACCEPT_YES_SINGLE;

    if (drop)
    {
        const LLInventoryItem* item = (const LLInventoryItem*)cargo_data;
        const bool started = SSAtmoEnvManager::getInstance()->loadFromInventory(item,
            [this](bool success)
            {
                // This callback only ever fires for a load the floater
                // itself asked for (a drag-and-drop drop) - parcel
                // discovery's own fetches go through a completely separate
                // path (SSAtmoEnvDiscoveryManager) that never sets one, so this
                // alert can never fire for an unattended background load.
                if (!success)
                {
                    LLNotificationsUtil::add("GenericAlert", LLSD().with(
                        "MESSAGE", "That notecard could not be loaded as an Atmo Magic environment."));
                }
                refreshVisibility();
                mSelectedTrackIndex = 0;
                refreshTrackRail();
                refreshTrackTab();
                refreshPlanetaryScales();
                refreshPreview();
            });
        if (started)
        {
            // The fetch is async; the callback above (and the regular
            // poll in draw()) pick up the eventual result either way -
            // this just reflects that a load attempt is now in flight.
            refreshStatus();
        }
        else
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with(
                "MESSAGE", "You don't have permission to read that notecard."));
        }
    }

    return true;
}

void SSFloaterAtmoEnv::handleSettingsDrop(const LLInventoryItem* drop_item)
{
    // Re-read through the model rather than trusting the drag's raw
    // pointer - same guard LLSettingsDropTarget applies to its own
    // DAD_SETTINGS drops.
    const LLViewerInventoryItem* item =
        drop_item ? gInventory.getItem(drop_item->getUUID()) : nullptr;
    if (!item || item->getAssetUUID().isNull()) return;

    // FULL-PERM GATE: import means transcribing someone's settings asset
    // into a plain notecard the author can then hand around, so only an
    // item the owner holds copy + modify + transfer on may come in - the
    // same PERM_ITEM_UNRESTRICTED check the texture-save and
    // copy-asset-id gates use (see LLViewerInventoryItem::checkPermissionsSet).
    if (!item->checkPermissionsSet(PERM_ITEM_UNRESTRICTED))
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with(
            "MESSAGE", "That setting isn't full permission - only full-perm settings can be imported."));
        return;
    }

    // Capture what the author saw at drop time: the track they had
    // selected and where the preview head sat. The fetch below is async,
    // so resolving against live members instead would stamp wherever the
    // author happened to have scrubbed to by the time it lands.
    const S32 track_index = mSelectedTrackIndex;
    const F64 phase = mPreviewPhase;
    LLHandle<LLFloater> handle = getHandle();

    LLSettingsVOBase::getSettingsAsset(item->getAssetUUID(),
        [handle, track_index, phase](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
        {
            // The floater may have closed while the fetch was in flight -
            // a dead handle means nobody is editing any more, so the drop
            // just evaporates.
            SSFloaterAtmoEnv* self = (SSFloaterAtmoEnv*)handle.get();
            if (!self) return;

            if (status || !settings)
            {
                LL_WARNS("AtmoMagicEnv") << "Dropped settings asset " << asset_id
                                         << " failed to load, status " << status << LL_ENDL;
                LLNotificationsUtil::add("GenericAlert", LLSD().with(
                    "MESSAGE", "That setting could not be loaded."));
                return;
            }

            LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(settings);
            if (!sky)
            {
                // A water or day-cycle settings item. Water would be a
                // trivial follow-up - SSAtmoEnvWater mirrors EEP's water
                // panel one-for-one, it just needs its own
                // addKeyframesFromWater transcription - while a day cycle
                // is a bigger lift (each of its frames would expand into a
                // stamp of its own), so both are refused for now.
                LLNotificationsUtil::add("GenericAlert", LLSD().with(
                    "MESSAGE", "Only sky settings can be imported for now."));
                return;
            }

            // The loaded asset may have been replaced or reshaped (tracks
            // removed) while the fetch was in flight - re-read through the
            // manager and re-check the captured index rather than assuming
            // anything about what's loaded now.
            SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
            if (!mgr->hasAsset()
                || track_index < 0 || track_index >= (S32)mgr->editable().mTracks.size())
            {
                return;
            }

            // Dropping a sky stamps its look as keyframes at the preview
            // head: the author scrubs to a time, drops a sky, and that
            // instant now looks like that sky - repeatable at other phases
            // to build a whole day out of EEP presets. Both structs stamp
            // because a sky's fields split across them, exactly as in
            // creation-time seeding.
            SSAtmoEnvTrack& track = mgr->editable().mTracks[track_index];
            track.mAtmosphere.addKeyframesFromSky(*sky, phase);
            track.mCloudDome.addKeyframesFromSky(*sky, phase);

            self->refreshPreview();
            self->refreshStatus(); // the modified asterisk lights up here
        });
}

//-----------------------------------------------------------------------------
// Population
//-----------------------------------------------------------------------------

void SSFloaterAtmoEnv::refreshVisibility()
{
    const bool has_asset = SSAtmoEnvManager::getInstance()->hasAsset();

    getChild<LLUICtrl>("create_new_button")->setVisible(!has_asset);
    getChild<LLUICtrl>("no_asset_text")->setVisible(!has_asset);

    // track_panel and atmo_tabs now own every per-track/per-field widget as
    // descendants (the vertical track list and the Weather tab's rows
    // respectively), so hiding those two containers is enough - no need to
    // walk every row individually the way this did back when they were all
    // direct floater children. load_button stays out of this list, same as
    // before - it works with nothing loaded yet, so it's always visible.
    const char* editing_widgets[] = {
        "name_editor", "save_button", "revert_button", "unload_button",
        "track_panel", "atmo_tabs",
        "preview_time_caption", "preview_time_slider", "preview_time_value_text",
        "forecast_text"
    };
    for (const char* name : editing_widgets)
    {
        getChild<LLUICtrl>(name)->setVisible(has_asset);
    }
}

void SSFloaterAtmoEnv::refreshTrackRail()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();

    // Selection can't outlive a track being removed out from under it
    // (Remove Track always drops back to the ground track), so clamp
    // before it drives anything below.
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size())
    {
        mSelectedTrackIndex = 0;
    }

    // Cleared and rebuilt from the asset every call, same as
    // LLPanelEnvironmentInfo::refresh() does for its own altitude sliders -
    // simpler than diffing which thumbs already exist, and cheap enough at
    // this many. Never called mid-drag - see the draw() and
    // onCommitAltitudeSlider() comments.
    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    slider->clear();

    const S32 optional_count = (S32)asset.mTracks.size() - 1; // excludes ground
    for (S32 slot = 1; slot <= SS_ATMOENV_MAX_TRACKS - 1; ++slot)
    {
        const bool exists = slot <= optional_count;
        getChild<LLUICtrl>(llformat("track_name_button_%d", slot))->setVisible(exists);
        getChild<LLUICtrl>(llformat("track_alt_label_%d", slot))->setVisible(exists);
        if (!exists) continue;

        const std::string slider_name = llformat("track%d", slot);
        F32 alt = llclamp(asset.mTracks[slot].mFloorZ,
                          llmax(slider->getMinValue(), SS_ATMOENV_MIN_TRACK_FLOOR),
                          slider->getMaxValue());

        // addSlider refuses a value too close to an existing marker (see
        // overlap_threshold on the rail): a notecard authored before that
        // minimum existed, or hand-edited, can ask for two tracks almost
        // on top of each other. Walk upward a step at a time until one
        // sticks rather than silently ending up with no marker for the
        // track - the same nudge LLPanelEnvironmentInfo::refresh() does for
        // out-of-range altitudes from the server.
        if (!slider->addSlider(alt, slider_name))
        {
            const F32 step = slider->getIncrement();
            for (F32 probe = alt + step; probe <= slider->getMaxValue(); probe += step)
            {
                if (slider->addSlider(probe, slider_name))
                {
                    alt = probe;
                    break;
                }
            }
        }

        // Whatever the rail actually accepted is the truth from here on,
        // so write it back rather than leaving the asset claiming an
        // altitude no marker is sitting at.
        const F32 accepted = slider->getSliderValue(slider_name);
        if (accepted != asset.mTracks[slot].mFloorZ)
        {
            asset.mTracks[slot].mFloorZ = accepted;
        }

        refreshAltitudeLabel(slot);
    }

    repositionRailMarkers();
    getChild<LLButton>("track_ground_button")->setToggleState(mSelectedTrackIndex == 0);

    getChild<LLUICtrl>("add_track_button")->setEnabled(
        (S32)asset.mTracks.size() < SS_ATMOENV_MAX_TRACKS);
    getChild<LLUICtrl>("remove_track_button")->setEnabled(mSelectedTrackIndex > 0);

    LLLineEditor* name_editor = getChild<LLLineEditor>("name_editor");
    if (!name_editor->hasFocus())
    {
        name_editor->setText(asset.mName);
    }
}

void SSFloaterAtmoEnv::repositionRailMarkers()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // Ground is pinned to the rail's own 0m position rather than being a
    // draggable thumb - it is 0m by definition. The rail runs all the way
    // down to 0 so ground sits on the same scale as every other track
    // rather than reading as a footnote below it, and it is placed by the
    // same arithmetic a real marker is, so "where 0m is" is answered once.
    //
    // Its XML follows="left|bottom" would otherwise park it a fixed
    // distance above the panel's bottom edge, which is only where 0m
    // happens to be at one particular floater height.
    const S32 centre = railCentreForValue(0.f);
    centreViewOn(getChild<LLUICtrl>("track_ground_button"), centre);
    centreViewOn(getChild<LLUICtrl>("track_ground_alt_label"), centre);

    // ...and each optional track's own labels, which are likewise placed
    // against the thumb rather than anchored.
    const SSAtmoEnvAsset& asset = mgr->asset();
    for (S32 slot = 1; slot < (S32)asset.mTracks.size() && slot < SS_ATMOENV_MAX_TRACKS; ++slot)
    {
        refreshAltitudeLabel(slot);
    }
}

S32 SSFloaterAtmoEnv::railCentreForValue(F32 value) const
{
    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    const LLRect sld_rect = slider->getRect();

    // A thumb's centre travels between thumb_width/2 and
    // height - thumb_width/2; this is LLMultiSlider::setSliderValue()'s
    // arithmetic verbatim, S32 truncation included, so a marker placed by
    // this lands on exactly the pixel the rail drew its thumb on.
    //
    // thumb_width is read back off a real thumb rather than hard-coded, so
    // nothing here has to be kept in step with the XML by hand. Any thumb
    // will do - they are all the same size - and the fallback only matters
    // before the first one exists (a lone Ground track).
    S32 thumb = 8;
    for (S32 slot = 1; slot < SS_ATMOENV_MAX_TRACKS; ++slot)
    {
        const LLRect probe = slider->getSliderThumbRect(llformat("track%d", slot));
        if (probe.getHeight() > 0)
        {
            thumb = probe.getHeight();
            break;
        }
    }

    const S32 bottom_edge = thumb / 2;
    const S32 top_edge = sld_rect.getHeight() - (thumb / 2);

    const F32 range = slider->getMaxValue() - slider->getMinValue();
    const F32 t = (range > 0.f) ? (value - slider->getMinValue()) / range : 0.f;

    return sld_rect.mBottom + bottom_edge + (S32)(t * (F32)(top_edge - bottom_edge));
}

// static
void SSFloaterAtmoEnv::centreViewOn(LLView* view, S32 centre_y)
{
    LLRect rect = view->getRect();
    const S32 height = rect.getHeight();
    rect.mBottom = centre_y - (height / 2);
    rect.mTop = rect.mBottom + height;
    view->setRect(rect);
}

void SSFloaterAtmoEnv::refreshAltitudeLabel(S32 slot)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    LLButton*  name_button = getChild<LLButton>(llformat("track_name_button_%d", slot));
    LLTextBox* alt_label   = getChild<LLTextBox>(llformat("track_alt_label_%d", slot));

    const F32 value = slider->getSliderValue(llformat("track%d", slot));

    // Same placement routine the Ground marker uses, so the two agree by
    // construction rather than by two hand-matched offsets.
    const S32 centre = railCentreForValue(value);
    centreViewOn(name_button, centre);
    centreViewOn(alt_label, centre);

    // Name on the left of the rail, altitude on the right - the same
    // arrangement the region Environment tab uses for "Sky 3 / 2000m". The
    // name is whatever the author set (see
    // SSAtmoEnvAsset::nextDefaultTrackName), so it comes from the asset
    // rather than being formatted from the index here.
    const SSAtmoEnvAsset& asset = mgr->asset();
    if (slot < (S32)asset.mTracks.size())
    {
        name_button->setLabel(asset.mTracks[slot].mName);
        name_button->setToggleState(slot == mSelectedTrackIndex);
    }
    alt_label->setText(llformat("%.0fm", value));
}

void SSFloaterAtmoEnv::onCommitAltitudeSlider()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    const std::string cur = slider->getCurSlider();
    if (cur.size() <= 5) return; // "track" + at least one digit

    const S32 slot = atoi(cur.c_str() + 5);
    SSAtmoEnvAsset& asset = mgr->editable();
    if (slot < 1 || slot >= (S32)asset.mTracks.size()) return;

    // Fires continuously while dragging, so this does the least it can get
    // away with: write the value and move that one marker's own labels.
    //
    // Emphatically no reordering or rail rebuild here - rebuilding calls
    // addSlider(), which sets LLMultiSlider's mCurSlider to whatever it
    // added last, so doing it from this callback silently transferred the
    // in-progress drag to the topmost marker. That work happens on
    // mouse-up instead, see onMouseUpAltitudeSlider().
    //
    // The rail runs down to 0 so ground reads as part of the same scale,
    // but ground owns everything below the first track - so a marker
    // dragged into that band is pushed back out here rather than the rail
    // being given a floor of its own. Writing it back to the control is
    // what makes the thumb actually stop, instead of the value being
    // silently corrected while the thumb keeps following the cursor.
    F32 value = slider->getCurSliderValue();
    if (value < SS_ATMOENV_MIN_TRACK_FLOOR)
    {
        value = SS_ATMOENV_MIN_TRACK_FLOOR;
        slider->setCurSliderValue(value);
    }
    asset.mTracks[slot].mFloorZ = value;

    // Actually moving a track is the one case where touching the rail also
    // selects: you are unambiguously working on that track by then, and
    // mCurSlider is reliable here in a way it isn't at mouse-down time
    // (see postBuild).
    if (slot != mSelectedTrackIndex)
    {
        selectTrack(slot);
    }

    refreshAltitudeLabel(slot);
    // Moving one track changes which band its neighbours cover, which the
    // Track tab spells out.
    refreshTrackTab();
    refreshStatus();
}

void SSFloaterAtmoEnv::onMouseUpAltitudeSlider()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // Safe to reorder now the drag is over - see the header comment. If the
    // marker was dragged past a neighbour the array order changed with it,
    // so follow the selection through and rebuild the rail from scratch.
    mSelectedTrackIndex = mgr->editable().sortTracksByAltitude(mSelectedTrackIndex);
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();

    // The sort can have renumbered every track; an open designer must
    // follow the same track to its new index rather than silently editing
    // whichever one now sits at the old number.
    SSFloaterAtmoPlanetary* designer =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoPlanetary>("ss_atmo_planetary");
    if (designer)
    {
        designer->setTrack(mSelectedTrackIndex);
    }

    SSFloaterAtmoInfluence* influence =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoInfluence>("ss_atmo_influence");
    if (influence)
    {
        influence->setTrack(mSelectedTrackIndex);
    }
}

void SSFloaterAtmoEnv::onClickWeatherInfluence()
{
    // One floater, opened from four places - see
    // floater_ss_atmo_weather_influence.xml on why the mappings are not
    // split across the tabs they affect. Keyed on the selected track, the
    // same way the System Designer is.
    LLFloaterReg::showInstance("ss_atmo_influence", LLSD(mSelectedTrackIndex));
}

void SSFloaterAtmoEnv::onClickGroundRow()
{
    selectTrack(0);
}

void SSFloaterAtmoEnv::selectTrack(S32 index)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (index < 0 || index >= (S32)mgr->asset().mTracks.size()) return;
    if (index == mSelectedTrackIndex) return;

    mSelectedTrackIndex = index;

    getChild<LLButton>("track_ground_button")->setToggleState(index == 0);
    for (S32 slot = 1; slot < SS_ATMOENV_MAX_TRACKS; ++slot)
    {
        getChild<LLButton>(llformat("track_name_button_%d", slot))->setToggleState(slot == index);
    }
    getChild<LLUICtrl>("remove_track_button")->setEnabled(index > 0);

    refreshTrackTab();
    refreshPlanetaryScales();
    refreshPreview();

    // An open System Designer follows the selection - each track is its
    // own planetary system, and the designer editing a track this floater
    // is no longer looking at would be two views silently disagreeing.
    SSFloaterAtmoPlanetary* designer =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoPlanetary>("ss_atmo_planetary");
    if (designer)
    {
        designer->setTrack(index);
    }

    // ...and so does the Weather Influence editor, for the same reason:
    // influence is per track, like the weather cube it reads.
    SSFloaterAtmoInfluence* influence =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoInfluence>("ss_atmo_influence");
    if (influence)
    {
        influence->setTrack(index);
    }
}

void SSFloaterAtmoEnv::refreshStatus()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const bool modified = mgr->isModified();

    setTitle(std::string("Edit Atmo Magic Environment") + (modified ? " - Unsaved changes*" : ""));

    getChild<LLUICtrl>("revert_button")->setEnabled(modified);
    getChild<LLUICtrl>("save_button")->setEnabled(mgr->hasAsset());
}

//-----------------------------------------------------------------------------
// Buttons
//-----------------------------------------------------------------------------

void SSFloaterAtmoEnv::onClickCreateNew()
{
    // Same underlying write as the inventory "New Settings > New Atmo
    // Magic" menu item (see menu_create_inventory_item's "atmoenv" branch);
    // the only difference is this also adopts the result once it lands, so
    // opening the floater with nothing active never dead-ends.
    //
    // Deliberately does not re-fetch the notecard we just wrote to load it
    // back - gInventory.getItem()/loadFromInventory() would work, but only
    // once local inventory has actually caught up with the new item, which
    // is one more race to fall into for no reason. Nor does it regenerate
    // SSAtmoEnvAsset::makeDefault() here: that used to be safe because
    // makeDefault() was deterministic, but creation now seeds the
    // atmosphere from a fetched Midday sky (see createDefaultNotecard),
    // so only the asset the callback hands back is guaranteed to be the
    // content that was actually written.
    SSAtmoEnvManager::createDefaultNotecard(LLUUID::null,
        [](const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)
        {
            if (item_id.isNull() || asset_id.isNull()) return;
            SSAtmoEnvManager::getInstance()->adoptCreated(item_id, asset_id, asset);
        });
}

void SSFloaterAtmoEnv::onClickLoad()
{
    // There is no generic inventory item picker to call, so this opens
    // inventory and reveals the "Atmo Magic" folder under Settings (see
    // SSAtmoEnvManager::atmoFolderId) rather than filtering the whole panel to
    // notecards - a type filter left the panel's own filter indicator
    // reading "All Types" while actually restricting what showed, which
    // read as broken rather than filtered. Everything Atmo Magic creates
    // lives in that one folder, so revealing it directly needs no filter
    // at all. Dropping a notecard onto this floater does the actual load;
    // this button is a shortcut to the right place, not a picker.
    LLFloaterSidePanelContainer* inv = LLFloaterReg::showTypedInstance<LLFloaterSidePanelContainer>(
        "inventory", LLSD());
    if (!inv) return;

    LLInventoryPanel* panel = inv->findChild<LLInventoryPanel>("All Items", true);
    if (!panel) return;

    SSAtmoEnvManager::atmoFolderId([panel](const LLUUID& folder_id)
    {
        panel->setSelectionByID(folder_id, false);
    });
}

void SSFloaterAtmoEnv::onClickSave()
{
    std::string name = getChild<LLLineEditor>("name_editor")->getText();
    SSAtmoEnvManager::getInstance()->saveNotecard(name);
    refreshStatus();
}

void SSFloaterAtmoEnv::onClickUnload()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // No confirmation prompt: nothing is destroyed. The notecard is still
    // in inventory and the parcel still references it - this only stops
    // this viewer showing it, and the applier hands EEP's local slot back
    // on the next frame because hasAsset() has gone false.
    mgr->unload();

    // The rail, the tabs and the preview all read the asset, so they have
    // to be told it is gone rather than discovering it a poll later.
    mSelectedTrackIndex = 0;
    refreshVisibility();
    refreshTrackRail();
    refreshTrackTab();
    refreshStatus();
}

void SSFloaterAtmoEnv::onClickRevert()
{
    SSAtmoEnvManager::getInstance()->revertToBaseline();
    mSelectedTrackIndex = 0; // may not exist in the reverted-to track set otherwise
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

void SSFloaterAtmoEnv::onClickAddTrack()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (!asset.addTrack()) return;

    // You just asked for a new track - you almost certainly want to look
    // at it, not stay on whatever was selected before.
    mSelectedTrackIndex = (S32)asset.mTracks.size() - 1;
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

void SSFloaterAtmoEnv::onClickRemoveTrack()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex <= 0) return; // ground track, or nothing selected

    mgr->editable().removeTrack(mSelectedTrackIndex);
    mSelectedTrackIndex = 0; // back to the one track that's always there
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

void SSFloaterAtmoEnv::onCommitName()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->editable().mName = getChild<LLLineEditor>("name_editor")->getText();
    refreshStatus();
}

//-----------------------------------------------------------------------------
// Track tab - floor/ceiling/day-cycle/water for mSelectedTrackIndex. See
// panel_ss_atmo_env_track.xml.
//-----------------------------------------------------------------------------

void SSFloaterAtmoEnv::refreshTrackTab()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    // Left alone while it has focus, so a refresh landing mid-typing (the
    // 0.5s poll in draw(), say) can't yank the caret or discard a partial
    // edit - same rule the asset-level name field follows.
    LLLineEditor* track_name = getChild<LLLineEditor>("track_name_editor");
    if (!track_name->hasFocus())
    {
        track_name->setText(track.mName);
    }

    const F64 day_length_hours = track.mDayLengthSeconds / 3600.0;
    const F64 day_offset_hours = track.mDayOffsetSeconds / 3600.0;
    getChild<LLUICtrl>("day_length_slider")->setValue(day_length_hours);
    getChild<LLUICtrl>("day_offset_slider")->setValue(day_offset_hours);
    if (!getChild<LLUICtrl>("day_length_value_spinner")->hasFocus())
    {
        getChild<LLUICtrl>("day_length_value_spinner")->setValue(day_length_hours);
    }
    if (!getChild<LLUICtrl>("day_offset_value_spinner")->hasFocus())
    {
        getChild<LLUICtrl>("day_offset_value_spinner")->setValue(day_offset_hours);
    }

    // Water tab: the enable checkbox is the one control there that isn't a
    // keyframe row, so it refreshes here with the rest of the per-track
    // structural state rather than in refreshPreview().
    getChild<LLUICtrl>("water_enabled_check")->setValue(track.mWater.mEnabled);

    // Weather and Clouds tabs: same rule for their Auto toggles.
    getChild<LLUICtrl>("gust_auto_check")->setValue(track.mWeather.mGustAuto);
    getChild<LLUICtrl>("lightning_auto_check")->setValue(track.mWeather.mLightningAuto);
    getChild<LLUICtrl>("cloud_auto_check")->setValue(track.mCloudField.mAuto);
    refreshAutoRows();
    refreshWaterRows();
}

void SSFloaterAtmoEnv::onCommitTrackName()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    std::string name = getChild<LLLineEditor>("track_name_editor")->getText();
    LLStringUtil::trim(name);
    // An unnamed track would leave a blank button on the rail with nothing
    // to click on; fall back rather than allow that.
    if (name.empty()) name = asset.nextDefaultTrackName();

    asset.mTracks[mSelectedTrackIndex].mName = name;

    // The rail's own button carries this name.
    refreshTrackRail();
    refreshStatus();
}

void SSFloaterAtmoEnv::onCommitDayCycle()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    // Slider and spinner are two views of one value; whichever the user
    // just touched is the authority, and the refresh below puts the other
    // in step.
    const bool from_spinner =
        getChild<LLUICtrl>("day_length_value_spinner")->hasFocus() ||
        getChild<LLUICtrl>("day_offset_value_spinner")->hasFocus();
    const char* length_src = from_spinner ? "day_length_value_spinner" : "day_length_slider";
    const char* offset_src = from_spinner ? "day_offset_value_spinner" : "day_offset_slider";

    track.mDayLengthSeconds = getChild<LLUICtrl>(length_src)->getValue().asReal() * 3600.0;
    track.mDayOffsetSeconds = getChild<LLUICtrl>(offset_src)->getValue().asReal() * 3600.0;

    // Nothing to rescale. Keyframes and the preview head are both stored as
    // a position in the cycle rather than a duration into it, so day length
    // is purely how fast the cycle plays - it cannot move a keyframe or
    // shift what the scrubber is pointing at.
    refreshTrackTab();
    refreshStatus();
}

void SSFloaterAtmoEnv::onCommitWaterEnabled()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mWater.mEnabled =
        getChild<LLUICtrl>("water_enabled_check")->getValue().asBoolean();
    refreshWaterRows();
    refreshStatus();
}

void SSFloaterAtmoEnv::onCommitGustAuto()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mWeather.mGustAuto =
        getChild<LLUICtrl>("gust_auto_check")->getValue().asBoolean();
    refreshAutoRows();
    refreshStatus();
}

void SSFloaterAtmoEnv::onCommitLightningAuto()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mWeather.mLightningAuto =
        getChild<LLUICtrl>("lightning_auto_check")->getValue().asBoolean();
    refreshAutoRows();
    refreshStatus();
}

void SSFloaterAtmoEnv::onCommitCloudAuto()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mCloudField.mAuto =
        getChild<LLUICtrl>("cloud_auto_check")->getValue().asBoolean();
    refreshAutoRows();
    refreshStatus();
}

namespace
{
    // Defined with the other display helpers further down; forward declared
    // so refreshAutoRows can share the same name mapping the combo items
    // and ghost labels use.
    std::string precipDisplayName(const std::string& value);
}

void SSFloaterAtmoEnv::refreshAutoRows()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return;

    const SSAtmoEnvTrack& track = mgr->asset().mTracks[mSelectedTrackIndex];
    const SSAtmoEnvWeather& weather = track.mWeather;

    // One evaluation feeds every computed readout below - the gust and
    // lightning figures Auto derived, and the precipitation type - so they
    // can never describe different instants.
    const SSAtmoEnvWeatherState resolved = SSAtmoEnvWeatherResolver::resolve(weather, mPreviewPhase);

    F32 auto_height = 0.f, auto_thickness = 0.f, auto_coverage = 0.f;
    SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(
        weather.mMoisture.valueAt(mPreviewPhase),
        weather.mConvection.valueAt(mPreviewPhase),
        auto_height, auto_thickness, auto_coverage);

    const struct { const char* mPrefix; bool mAuto; F32 mComputed; } rows[] = {
        { "gust_depth",          weather.mGustAuto,       resolved.mGustDepth },
        { "gust_length",         weather.mGustAuto,       resolved.mGustLength },
        { "gust_veer",           weather.mGustAuto,       resolved.mGustVeer },
        { "lightning_intensity", weather.mLightningAuto,  resolved.mLightningIntensity },
        { "cloud_base_height",   track.mCloudField.mAuto, auto_height },
        { "cloud_thickness",     track.mCloudField.mAuto, auto_thickness },
        { "cloud_coverage",      track.mCloudField.mAuto, auto_coverage },
    };
    for (const auto& row : rows)
    {
        const std::string prefix(row.mPrefix);
        getChild<LLUICtrl>(prefix + "_slider")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_value_spinner")->setEnabled(!row.mAuto);
        // The whole keyframe cluster too: a keyframe inserted into a field
        // Auto is ignoring only parks confusion for later, and chevrons
        // stepping the head between keyframes that visibly do nothing read
        // as broken rather than parked. This runs after the generic row
        // refresh re-enabled them by keyframe count, so this verdict wins.
        getChild<LLUICtrl>(prefix + "_keyframe_button")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_prev_button")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_next_button")->setEnabled(!row.mAuto);

        if (row.mAuto)
        {
            // The greyed controls become a live readout of what Auto
            // decided for this instant (refreshFloatRow already wrote the
            // parked authored value moments ago; this deliberately
            // overwrites it). A disabled spinner cannot hold focus, so no
            // focus guard is needed here.
            getChild<LLUICtrl>(prefix + "_slider")->setValue(row.mComputed);
            getChild<LLUICtrl>(prefix + "_value_spinner")->setValue(row.mComputed);
        }
    }

    // What Auto would drop at this instant, beside the combo - only while
    // the combo actually says Auto; a forced type makes it redundant.
    const std::string override_now = weather.mPrecipitationOverride.valueAt(mPreviewPhase);
    std::string auto_text;
    if (override_now.empty())
    {
        auto_text = resolved.mPrecipitationType.empty()
            ? std::string("Clear")
            : precipDisplayName(resolved.mPrecipitationType);
    }
    getChild<LLTextBox>("precipitation_auto_text")->setText(auto_text);
}

bool SSFloaterAtmoEnv::waterRowsInactive() const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return false;

    return !mgr->asset().mTracks[mSelectedTrackIndex].mWater.mEnabled;
}

void SSFloaterAtmoEnv::refreshWaterRows()
{
    const bool enabled = !waterRowsInactive();

    // Every water control is named "water_..." - the prefix IS the
    // grouping, so this walks the row lists rather than naming thirty
    // widgets and going stale the next time one is added.
    auto setRow = [this, enabled](const std::string& prefix, LLUICtrl* primary)
    {
        if (primary) primary->setEnabled(enabled);

        // The keyframe cluster too: a keyframe on a plane that does not
        // exist is confusion parked for later, and chevrons stepping
        // between keyframes that visibly do nothing read as broken.
        static const char* const CLUSTER[] = {
            "_keyframe_button", "_prev_button", "_next_button"
        };
        for (const char* suffix : CLUSTER)
        {
            LLUICtrl* part = findChild<LLUICtrl>(prefix + suffix);
            if (part) part->setEnabled(enabled);
        }
    };

    auto isWater = [](const std::string& prefix)
    {
        return prefix.compare(0, 6, "water_") == 0;
    };

    for (const FloatRow& row : mFloatRows)
    {
        if (!isWater(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix + "_slider"));
        LLUICtrl* spinner = findChild<LLUICtrl>(row.mPrefix + "_value_spinner");
        if (spinner) spinner->setEnabled(enabled);
    }
    for (const KeyRow<LLColor3>& row : mColorRows)
    {
        if (!isWater(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix));
    }
    for (const KeyRow<LLVector2>& row : mVectorRows)
    {
        if (!isWater(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix));
        for (const char* suffix : { "_x_spinner", "_y_spinner" })
        {
            LLUICtrl* part = findChild<LLUICtrl>(row.mPrefix + suffix);
            if (part) part->setEnabled(enabled);
        }
    }
    for (const KeyRow<LLUUID>& row : mTextureRows)
    {
        if (!isWater(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix));
    }
}

bool SSFloaterAtmoEnv::rowAutoOwned(const std::string& prefix) const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return false;

    const SSAtmoEnvTrack& track = mgr->asset().mTracks[mSelectedTrackIndex];

    if (prefix == "gust_depth" || prefix == "gust_length" || prefix == "gust_veer")
    {
        return track.mWeather.mGustAuto;
    }
    if (prefix == "lightning_intensity")
    {
        return track.mWeather.mLightningAuto;
    }
    if (prefix == "cloud_base_height" || prefix == "cloud_thickness" || prefix == "cloud_coverage")
    {
        return track.mCloudField.mAuto;
    }
    // Water with no plane is inactive in exactly the sense this function
    // exists to describe: the field is real and keyframed, but nothing is
    // reading it, so it neither edits nor ghosts.
    if (prefix.compare(0, 6, "water_") == 0)
    {
        return !track.mWater.mEnabled;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Planetary tab - the two per-track distance-scale dials and the button
// into the System Designer (SSFloaterAtmoPlanetary), which is where the
// celestial bodies themselves are edited now. See
// panel_ss_atmo_env_planetary.xml.
//-----------------------------------------------------------------------------

void SSFloaterAtmoEnv::refreshPlanetaryScales()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return;

    const SSAtmoEnvPlanetary& planetary = mgr->asset().mTracks[mSelectedTrackIndex].mPlanetary;

    // Slider and spinner are two views of one value, spinner left alone
    // while focused, same as every other slider/spinner pair.
    getChild<LLUICtrl>("sun_planet_scale_slider")->setValue(planetary.mSunPlanetScale);
    getChild<LLUICtrl>("planet_moon_scale_slider")->setValue(planetary.mPlanetMoonScale);
    if (!getChild<LLUICtrl>("sun_planet_scale_spinner")->hasFocus())
    {
        getChild<LLUICtrl>("sun_planet_scale_spinner")->setValue(planetary.mSunPlanetScale);
    }
    if (!getChild<LLUICtrl>("planet_moon_scale_spinner")->hasFocus())
    {
        getChild<LLUICtrl>("planet_moon_scale_spinner")->setValue(planetary.mPlanetMoonScale);
    }
}

void SSFloaterAtmoEnv::onCommitPlanetaryScales()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    SSAtmoEnvPlanetary& planetary = asset.mTracks[mSelectedTrackIndex].mPlanetary;

    // Whichever of the pair the user just touched is the authority - the
    // same rule onCommitDayCycle() applies.
    const bool from_spinner =
        getChild<LLUICtrl>("sun_planet_scale_spinner")->hasFocus() ||
        getChild<LLUICtrl>("planet_moon_scale_spinner")->hasFocus();
    const char* sun_src  = from_spinner ? "sun_planet_scale_spinner"  : "sun_planet_scale_slider";
    const char* moon_src = from_spinner ? "planet_moon_scale_spinner" : "planet_moon_scale_slider";

    planetary.mSunPlanetScale  = (F32)getChild<LLUICtrl>(sun_src)->getValue().asReal();
    planetary.mPlanetMoonScale = (F32)getChild<LLUICtrl>(moon_src)->getValue().asReal();

    refreshPlanetaryScales();
    refreshStatus();
}

void SSFloaterAtmoEnv::onClickOpenPlanetaryDesigner()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // The designer's key is the track to edit; selectTrack() keeps an
    // already-open instance following this floater's selection from
    // there on.
    LLFloaterReg::showInstance("ss_atmo_planetary", LLSD(mSelectedTrackIndex));
}

//-----------------------------------------------------------------------------
// Keyframe rows - AE-style, one per animatable field across every tab. See
// ssatmoenvkeyframe.h for the container every row exercises, and the
// FloatRow comment in the header for why mField is a fresh accessor rather
// than a cached reference. refreshPreview() is the one entry point that
// re-reads everything driven by the preview phase; its name used to be
// refreshKeyframeProof, a fossil from when this was a one-slider proof of
// concept.
//-----------------------------------------------------------------------------

namespace
{
    // "Apparent Time of Day" - same idiom and the same [HH]:[MM][AP] (PRC%)
    // template LLPanelEnvironmentInfo::udpateApparentTimeOfDay() uses for
    // the region/estate Environment tab, so a position in the cycle reads
    // as a clock time rather than a raw fraction - and the "(41%)" is that
    // same position stated plainly.
    //
    // Takes the phase directly: day length is a playback-speed control and
    // has nothing to say about what time of day a given point in the cycle
    // represents.
    std::string formatApparentTime(F64 phase)
    {
        // Deliberately not wrapped: sitting exactly at the end of the loop
        // is 24:00 (100%), the same instant as 0:00 (0%) but reached from
        // the other side, and reporting it as 0% made dragging the scrubber
        // to the far right look like it had snapped back to the start.
        F64 frac = (phase >= 1.0) ? 1.0 : std::fmod(phase, 1.0);
        if (frac < 0.0) frac += 1.0;

        static const F64 SECONDS_IN_DAY = 24.0 * 60.0 * 60.0;
        const S32 second_of_day = (S32)(frac * SECONDS_IN_DAY);
        S32 hour = second_of_day / 3600;
        const S32 minute = (second_of_day % 3600) / 60;

        std::string ap;
        if (!gSavedSettings.getBOOL("Use24HourClock"))
        {
            // hour 24 only ever comes from the exact-end-of-cycle case
            // above; it reads as midnight the same way 0 does.
            ap = (hour >= 12 && hour < 24) ? "PM" : "AM";
            if (hour == 0 || hour == 24) hour = 12;
            else if (hour > 12)          hour -= 12;
        }

        return llformat("%d:%02d%s (%d%%)", hour, minute, ap.c_str(), (S32)(frac * 100.0));
    }

    // Display names for the precipitation override's stored values, matching
    // the combo's own item labels so a ghost keyframe and the combo read the
    // same. Empty is the Auto entry; anything unrecognised (a hand-edited
    // notecard) shows its raw value rather than nothing.
    std::string precipDisplayName(const std::string& value)
    {
        if (value.empty())            return "Auto";
        if (value == "rain")          return "Rain";
        if (value == "snow")          return "Snow";
        if (value == "hail")          return "Hail";
        if (value == "blizzard")      return "Blizzard";
        if (value == "sleet")         return "Sleet";
        if (value == "freezing_rain") return "Freezing Rain";
        if (value == "slush_mix")     return "Wintry Mix";
        return value;
    }
}

//-----------------------------------------------------------------------------
// Shared keyframe-cluster machinery. Every row type - scalar slider, colour
// swatch, texture picker, xy_vector - has the same diamond/prev/next
// behaviour and differs only in how its own control is read and written, so
// that half lives here once rather than four times.
//-----------------------------------------------------------------------------

template <typename T>
void SSFloaterAtmoEnv::refreshKeyframeControls(const std::string& prefix,
                                               const SSAtmoEnvKeyframed<T>& field, F64 phase)
{
    const bool on_keyframe = field.hasKeyframeAt(phase);
    LLButton* kf_button = getChild<LLButton>(prefix + "_keyframe_button");
    kf_button->setToggleState(on_keyframe);
    kf_button->setLabel(std::string(on_keyframe ? FILLED_DIAMOND : HOLLOW_DIAMOND));

    // Chevrons are only meaningful once there is more than one keyframe to
    // move between - with none or one, "jump to the next" has nowhere to go.
    const bool can_jump = field.keyframeCount() > 1;
    getChild<LLUICtrl>(prefix + "_prev_button")->setEnabled(can_jump);
    getChild<LLUICtrl>(prefix + "_next_button")->setEnabled(can_jump);
}

template <typename T>
void SSFloaterAtmoEnv::toggleKeyframe(SSAtmoEnvKeyframed<T>& field)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    (void)mgr;
    field.toggleKeyframeAtHead(mPreviewPhase);
}

template <typename T>
void SSFloaterAtmoEnv::jumpKeyframe(const SSAtmoEnvKeyframed<T>& field, bool next)
{
    // refreshPreview() (called by every caller of this) is what
    // actually re-reads every control from mPreviewPhase - one place that
    // converts time to what the controls show, not one per row.
    mPreviewPhase = next ? field.nextKeyframeTime(mPreviewPhase)
                        : field.prevKeyframeTime(mPreviewPhase);
}

template <typename T>
void SSFloaterAtmoEnv::bindKeyframeButtons(const std::string& prefix,
                                           std::function<SSAtmoEnvKeyframed<T>&()> field)
{
    getChild<LLButton>(prefix + "_keyframe_button")->setClickedCallback(
        [this, field](LLUICtrl*, const LLSD&)
        { toggleKeyframe<T>(field()); refreshPreview(); refreshStatus(); });
    getChild<LLButton>(prefix + "_prev_button")->setClickedCallback(
        [this, field](LLUICtrl*, const LLSD&)
        { jumpKeyframe<T>(field(), false); refreshPreview(); });
    getChild<LLButton>(prefix + "_next_button")->setClickedCallback(
        [this, field](LLUICtrl*, const LLSD&)
        { jumpKeyframe<T>(field(), true); refreshPreview(); });
}

//-----------------------------------------------------------------------------
// Per-type control <-> field plumbing for the Water tab's non-scalar rows.
//-----------------------------------------------------------------------------

// Whether a colour picker is open anywhere.
//
// The picker is a separate floater that a swatch owns through a protected
// handle, so a swatch cannot be asked directly. Walking the floater view
// for a visible one answers the same question and is not fussy about which
// swatch opened it - which is what we want, since any open picker is one
// somebody is typing into.
static bool ss_color_picker_open()
{
    if (!gFloaterView) return false;

    for (LLView* child : *gFloaterView->getChildList())
    {
        LLFloaterColorPicker* picker = dynamic_cast<LLFloaterColorPicker*>(child);
        if (picker && picker->getVisible())
        {
            return true;
        }
    }
    return false;
}

void SSFloaterAtmoEnv::refreshColorRow(const KeyRow<LLColor3>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<LLColor3>& field = row.mField();

    // Divided into the swatch's own 0..1 space - see KeyRow::mScale. The
    // stored value is EEP's, which for several of these runs past 1.
    const F32 inv = (row.mScale > 0.f) ? (1.f / row.mScale) : 1.f;
    const LLColor3 value = field.valueAt(phase) * inv;

    // The swatch is an LLColor4 control; none of the keyframed colours has
    // a meaningful alpha of its own, so each round-trips through a fully
    // opaque colour and only the RGB is ever stored.
    //
    // Skipped entirely while a colour picker is open. The periodic refresh
    // would otherwise write the swatch every half second, the picker would
    // resync its own fields from it, and a hex code being typed in would be
    // replaced mid-keystroke - which is exactly what it looked like.
    if (!ss_color_picker_open())
    {
        getChild<LLColorSwatchCtrl>(row.mPrefix)->setValue(
            LLColor4(value.mV[0], value.mV[1], value.mV[2], 1.f).getValue());
    }

    refreshKeyframeControls<LLColor3>(row.mPrefix, field, phase);
}

void SSFloaterAtmoEnv::commitColorRow(const KeyRow<LLColor3>& row)
{
    const LLSD sd = getChild<LLColorSwatchCtrl>(row.mPrefix)->getValue();
    if (!sd.isArray() || sd.size() < 3) return;

    // ...and multiplied back out of it on the way in, so what is stored is
    // always EEP's own value whatever the swatch had to show.
    row.mField().setValueAtHead(mPreviewPhase,
        LLColor3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal()) * row.mScale);
}

void SSFloaterAtmoEnv::refreshVectorRow(const KeyRow<LLVector2>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<LLVector2>& field = row.mField();
    const LLVector2 value = field.valueAt(phase);

    // xy_vector's own LLSD form is a two-element array of reals, which is
    // exactly how ss_atmoenv_value_to_sd<LLVector2> stores it - so this is
    // a straight handover with no reshaping.
    LLSD sd = LLSD::emptyArray();
    sd.append((LLSD::Real)value.mV[0]);
    sd.append((LLSD::Real)value.mV[1]);
    getChild<LLUICtrl>(row.mPrefix)->setValue(sd);

    // The graphic's own X/Y entry bar is collapsed (edit_bar_height="0"):
    // at a parameter row's height there is no room for both it and a
    // usable touch area, and its two boxes would not line up with any
    // other row's number anyway. The numbers live in spinners on the left
    // in the same column as every other row's, and the graphic is purely
    // the direct-manipulation half.
    LLSpinCtrl* x_spin = getChild<LLSpinCtrl>(row.mPrefix + "_x_spinner");
    LLSpinCtrl* y_spin = getChild<LLSpinCtrl>(row.mPrefix + "_y_spinner");
    if (!x_spin->hasFocus()) x_spin->setValue(value.mV[0]);
    if (!y_spin->hasFocus()) y_spin->setValue(value.mV[1]);

    refreshKeyframeControls<LLVector2>(row.mPrefix, field, phase);
}

void SSFloaterAtmoEnv::commitVectorRow(const KeyRow<LLVector2>& row)
{
    const LLSD sd = getChild<LLUICtrl>(row.mPrefix)->getValue();
    if (!sd.isArray() || sd.size() < 2) return;

    row.mField().setValueAtHead(mPreviewPhase,
        LLVector2((F32)sd[0].asReal(), (F32)sd[1].asReal()));
}

void SSFloaterAtmoEnv::commitVectorSpinners(const KeyRow<LLVector2>& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        LLVector2((F32)getChild<LLUICtrl>(row.mPrefix + "_x_spinner")->getValue().asReal(),
                  (F32)getChild<LLUICtrl>(row.mPrefix + "_y_spinner")->getValue().asReal()));
}

void SSFloaterAtmoEnv::refreshTextureRow(const KeyRow<LLUUID>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<LLUUID>& field = row.mField();
    getChild<LLTextureCtrl>(row.mPrefix)->setValue(field.valueAt(phase));
    refreshKeyframeControls<LLUUID>(row.mPrefix, field, phase);
}

void SSFloaterAtmoEnv::commitTextureRow(const KeyRow<LLUUID>& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        getChild<LLTextureCtrl>(row.mPrefix)->getValue().asUUID());
}

void SSFloaterAtmoEnv::refreshStringRow(const KeyRow<std::string>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<std::string>& field = row.mField();

    // A value outside the combo's vocabulary (a hand-edited notecard, or
    // one from a build with a wider list) displays as Auto rather than
    // leaving the combo on whatever it showed last. Only the display: the
    // stored value is untouched until the user actually commits something.
    LLComboBox* combo = getChild<LLComboBox>(row.mPrefix);
    if (!combo->setSelectedByValue(field.valueAt(phase), true))
    {
        combo->setSelectedByValue(std::string(), true);
    }

    refreshKeyframeControls<std::string>(row.mPrefix, field, phase);
}

void SSFloaterAtmoEnv::commitStringRow(const KeyRow<std::string>& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        getChild<LLComboBox>(row.mPrefix)->getSelectedValue().asString());
}

void SSFloaterAtmoEnv::refreshFloatRow(const FloatRow& row, F64 phase)
{
    const SSAtmoEnvKeyframed<F32>& field = row.mField();
    const F32 value = field.valueAt(phase);

    getChild<LLUICtrl>(row.mPrefix + "_slider")->setValue(value);

    // Left alone while it has focus so a refresh landing mid-typing cannot
    // yank the caret or discard a half-entered number - the same rule the
    // name fields follow.
    LLSpinCtrl* spinner = getChild<LLSpinCtrl>(row.mPrefix + "_value_spinner");
    if (!spinner->hasFocus())
    {
        spinner->setValue(value);
    }

    refreshKeyframeControls<F32>(row.mPrefix, field, phase);
}

void SSFloaterAtmoEnv::commitFloatRowSpinner(const FloatRow& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        (F32)getChild<LLUICtrl>(row.mPrefix + "_value_spinner")->getValue().asReal());
}

void SSFloaterAtmoEnv::commitFloatRow(const FloatRow& row)
{
    const F32 value = (F32)getChild<LLUICtrl>(row.mPrefix + "_slider")->getValue().asReal();

    // This one call is the entire editing rule from the design doc: no
    // keyframes yet, this just becomes the plain value; head on an existing
    // keyframe edits it in place; head anywhere else inserts a new one.
    row.mField().setValueAtHead(mPreviewPhase, value);
}

void SSFloaterAtmoEnv::toggleFloatRowKeyframe(const FloatRow& row)
{
    toggleKeyframe<F32>(row.mField());
}

void SSFloaterAtmoEnv::jumpFloatRowPrev(const FloatRow& row)
{
    jumpKeyframe<F32>(row.mField(), false);
}

void SSFloaterAtmoEnv::jumpFloatRowNext(const FloatRow& row)
{
    jumpKeyframe<F32>(row.mField(), true);
}

void SSFloaterAtmoEnv::refreshPreview()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // This floater being open with a preview visible is what overrides the
    // live render's clock - see SSAtmoEnvManager::setPreviewPhaseOverride() and
    // SSAtmoEnvBridge::resolveActiveTrack(). Every call here (a user
    // scrub, a chevron jump, or just the periodic poll while nothing's
    // changed) re-asserts it, so the in-world preview always matches
    // whatever this floater is currently showing rather than drifting back
    // to the real wall clock the instant this stops being re-armed.
    mgr->setPreviewPhaseOverride(mPreviewPhase);

    const F64 day_length = mgr->asset().mTracks[mSelectedTrackIndex].mDayLengthSeconds;

    // The scrubber works in phase, 0..1, the same unit keyframes are stored
    // in - so changing a track's day length rescales nothing here and moves
    // nothing. It still snaps to the design doc's 32 slots across the
    // cycle; that grid is a fraction of the cycle too, not a duration.
    // The scrubber is a PERCENTAGE of the cycle, because that is what a
    // phase is: the track's day length scales it into a time of day
    // afterwards (see formatApparentTime) and has no business deciding what
    // the control's own range means. A scrubber calibrated in hours would
    // have to be rebuilt whenever a track's day length changed, and two
    // tracks running different day lengths could not share one.
    LLSliderCtrl* time_slider = getChild<LLSliderCtrl>("preview_time_slider");
    time_slider->setMinValue(0.f);
    time_slider->setMaxValue(100.f);
    time_slider->setIncrement(100.f / (F32)SS_ATMOENV_PREVIEW_STEPS);
    time_slider->setValue((F32)(llclamp(mPreviewPhase, 0.0, 1.0) * 100.0));

    getChild<LLTextBox>("preview_time_value_text")->setText(formatApparentTime(mPreviewPhase));

    for (const FloatRow& row : mFloatRows)
    {
        refreshFloatRow(row, mPreviewPhase);
    }
    for (const KeyRow<LLColor3>& row : mColorRows)
    {
        refreshColorRow(row, mPreviewPhase);
    }
    for (const KeyRow<LLVector2>& row : mVectorRows)
    {
        refreshVectorRow(row, mPreviewPhase);
    }
    for (const KeyRow<LLUUID>& row : mTextureRows)
    {
        refreshTextureRow(row, mPreviewPhase);
    }
    for (const KeyRow<std::string>& row : mStringRows)
    {
        refreshStringRow(row, mPreviewPhase);
    }

    // Phase 5 proof: the same resolve() call phase 5's renderer hookup will
    // eventually feed the particle/cloud/audio systems from.
    const SSAtmoEnvWeatherState resolved = SSAtmoEnvWeatherResolver::resolve(
        mgr->editable().mTracks[mSelectedTrackIndex].mWeather, mPreviewPhase);
    getChild<LLTextBox>("forecast_text")->setText(resolved.mForecastText);

    // Last, so the Auto groups can overwrite the authored values the row
    // loops above just displayed - see refreshAutoRows() - and so the
    // water tab's greying survives the row loops having just re-enabled
    // every keyframe cluster by keyframe count.
    refreshAutoRows();
    refreshWaterRows();
}

//-----------------------------------------------------------------------------
// Ghost keyframes on the preview scrubber - see the header.
//-----------------------------------------------------------------------------

template <typename T, typename FormatFn>
void SSFloaterAtmoEnv::buildGhosts(const std::vector<SSAtmoEnvKeyframe<T>>& keyframes,
                                   FormatFn format, std::vector<GhostKeyframe>& out)
{
    const S32 count = (S32)keyframes.size();
    for (S32 i = 0; i < count; ++i)
    {
        const SSAtmoEnvKeyframe<T>& kf = keyframes[i];

        GhostKeyframe ghost;
        ghost.mHold = (kf.mCurve == SSAtmoEnvCurve::HOLD);
        ghost.mLabel = format(kf.mValue);

        if (!ghost.mHold)
        {
            ghost.mDrawPhase = kf.mTime;
            ghost.mSpanStart = kf.mTime;
            ghost.mSpanEnd = kf.mTime;
        }
        else
        {
            // First keyframe's span reaches back to the start of the cycle
            // and the last one's runs to the end, rather than either
            // wrapping around: a marker that wrapped would be drawn at the
            // far end of the scrubber from the keyframe it belongs to,
            // which reads as a different keyframe entirely. Everything in
            // between runs to its successor, so the spans partition the
            // cycle with no gaps and no overlaps.
            ghost.mSpanStart = (i == 0) ? 0.0 : kf.mTime;
            ghost.mSpanEnd = (i == count - 1) ? 1.0 : keyframes[i + 1].mTime;
            ghost.mDrawPhase = 0.5 * (ghost.mSpanStart + ghost.mSpanEnd);
        }

        out.push_back(ghost);
    }
}

bool SSFloaterAtmoEnv::rowHovered(const std::string& prefix) const
{
    // The keyframe button is the one widget every row type has regardless
    // of what its actual control is, so its vertical extent stands in for
    // the row's band. Horizontally the whole tab counts: pointing at a
    // row's label or its slider is pointing at that row.
    LLView* tabs = findChild<LLView>("atmo_tabs");
    LLView* button = findChild<LLView>(prefix + "_keyframe_button");
    if (!tabs || !button) return false;

    // Must be on the tab that is actually showing. getVisible() only
    // reports the widget's own flag, so a row on a hidden tab still passes
    // it - and since every tab uses the same row grid, the Water row at a
    // given height would answer for the Weather row at that same height.
    // isInVisibleChain() is the question that was actually meant.
    if (!button->isInVisibleChain()) return false;

    S32 mouse_x = 0, mouse_y = 0;
    LLUI::getInstance()->getMousePositionScreen(&mouse_x, &mouse_y);

    const LLRect tab_rect = tabs->calcScreenRect();
    if (mouse_x < tab_rect.mLeft || mouse_x > tab_rect.mRight) return false;

    const LLRect band = button->calcScreenRect();
    return mouse_y >= band.mBottom && mouse_y <= band.mTop;
}

bool SSFloaterAtmoEnv::scrubberHovered() const
{
    LLView* scrubber = findChild<LLView>("preview_time_slider");
    if (!scrubber || !scrubber->getVisible()) return false;

    S32 mouse_x = 0, mouse_y = 0;
    LLUI::getInstance()->getMousePositionScreen(&mouse_x, &mouse_y);

    // Deliberately a padded copy of the rect rather than a taller widget.
    // Growing the slider itself would also grow what it captures on a
    // click-drag, so aiming near it would start scrubbing rather than just
    // revealing the overview - and it would push the layout below it
    // around. Only the hover test wants the extra room, so only the hover
    // test gets it.
    LLRect hover = scrubber->calcScreenRect();
    hover.stretch(HOVER_PAD_X, HOVER_PAD_Y);
    return hover.pointInRect(mouse_x, mouse_y);
}

bool SSFloaterAtmoEnv::collectHoveredKeyframes(std::vector<GhostKeyframe>& out, bool& out_labels) const
{
    out.clear();
    out_labels = true;

    // Hovering the scrubber itself asks a different question - "where is
    // there anything keyframed on this tab at all" - so it gathers every
    // animatable parameter currently on screen rather than one row. Rows
    // belonging to a tab that isn't showing are skipped: their keyframes
    // are real but they are not what is being looked at.
    const bool overview = scrubberHovered();
    if (overview) out_labels = false;

    auto wanted = [this, overview](const std::string& prefix)
    {
        // Auto-owned rows ghost nothing in either mode - see rowAutoOwned().
        if (rowAutoOwned(prefix)) return false;
        if (!overview) return rowHovered(prefix);
        LLView* probe = findChild<LLView>(prefix + "_keyframe_button");
        return probe && probe->isInVisibleChain();
    };

    bool found = false;

    for (const FloatRow& row : mFloatRows)
    {
        if (!wanted(row.mPrefix)) continue;
        // Formatted exactly as this row's own readout is, so a ghost and
        // the live value can be compared without mentally converting
        // between two notations.
        buildGhosts<F32>(row.mField().keyframes(),
            [&row](const F32& v) {
                return row.mIntegerDisplay ? llformat("%.0f", v) : llformat("%.2f", v);
            }, out);
        found = true;
        if (!overview) return true;
    }

    for (const KeyRow<LLColor3>& row : mColorRows)
    {
        if (!wanted(row.mPrefix)) continue;
        // Same scale the swatch shows, so a ghost and the control under it
        // read the same number - this is where <765, 765, 765> was coming
        // from for a plain white sunlight.
        const F32 ghost_inv = (row.mScale > 0.f) ? (255.f / row.mScale) : 255.f;
        buildGhosts<LLColor3>(row.mField().keyframes(),
            [ghost_inv](const LLColor3& v) {
                return llformat("%d %d %d", (S32)llround(v.mV[0] * ghost_inv),
                                            (S32)llround(v.mV[1] * ghost_inv),
                                            (S32)llround(v.mV[2] * ghost_inv));
            }, out);
        found = true;
        if (!overview) return true;
    }

    for (const KeyRow<LLVector2>& row : mVectorRows)
    {
        if (!wanted(row.mPrefix)) continue;
        buildGhosts<LLVector2>(row.mField().keyframes(),
            [](const LLVector2& v) { return llformat("%.1f, %.1f", v.mV[0], v.mV[1]); }, out);
        found = true;
        if (!overview) return true;
    }

    for (const KeyRow<LLUUID>& row : mTextureRows)
    {
        if (!wanted(row.mPrefix)) continue;
        buildGhosts<LLUUID>(row.mField().keyframes(),
            [](const LLUUID& v) { return std::string(v.isNull() ? "default" : "custom"); }, out);
        found = true;
        if (!overview) return true;
    }

    for (const KeyRow<std::string>& row : mStringRows)
    {
        if (!wanted(row.mPrefix)) continue;
        buildGhosts<std::string>(row.mField().keyframes(),
            [](const std::string& v) { return precipDisplayName(v); }, out);
        found = true;
        if (!overview) return true;
    }

    return found;
}

bool SSFloaterAtmoEnv::scrubberGeometry(LLRect& out_rect, S32& out_left_edge, S32& out_travel) const
{
    LLView* scrubber = findChild<LLView>("preview_time_slider");
    if (!scrubber || !scrubber->getVisible()) return false;

    // The scrubber is a direct child of the floater, so its rect is already
    // in the space these overlays draw in (LLFloater::draw has left the
    // origin at the floater's own bottom-left).
    out_rect = scrubber->getRect();

    // Match LLSlider::updateThumbRect()'s travel exactly, so an overlay
    // sits on the pixel the thumb would occupy at that phase rather than
    // merely near it. The thumb image is the one named by slider_bar.xml;
    // its width is the inset at each end.
    LLPointer<LLUIImage> thumb = LLUI::getUIImage("SliderThumb_Off");
    const S32 thumb_width = thumb.notNull() ? thumb->getWidth() : 16;
    out_left_edge = out_rect.mLeft + (thumb_width / 2);
    out_travel = (out_rect.mRight - (thumb_width / 2)) - out_left_edge;
    return out_travel > 0;
}

void SSFloaterAtmoEnv::drawRiseSetMarkers()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    const SSAtmoEnvPlanetary& planetary = asset.mTracks[mSelectedTrackIndex].mPlanetary;
    const S32 home = planetary.homeBodyIndex();
    if (home < 0) return; // no vantage point, so nothing rises or sets

    // The same two bodies the renderer will light the world with - asked of
    // the resolver rather than picked here, so a marker can never annotate
    // a different "sun" than the one that actually comes up.
    SSAtmoEnvResolvedBody sun;
    SSAtmoEnvResolvedBody moon;
    SSAtmoEnvPlanetaryResolver::resolveLightRoles(planetary, sun, moon);
    if (sun.mBodyIndex < 0 && moon.mBodyIndex < 0) return;

    LLRect rect;
    S32 left_edge = 0;
    S32 travel = 0;
    if (!scrubberGeometry(rect, left_edge, travel)) return;

    const SSAtmoEnvCelestialBody& home_body = planetary.mBodies[static_cast<size_t>(home)];
    LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 glyph_y = rect.getCenterY();

    // On the track line itself, HCENTER/VCENTER exactly like the ghost
    // diamonds - the ghosts' value labels live in the lanes above and
    // below, so the two overlays share the strip without colliding.
    auto mark = [&](const SSAtmoEnvResolvedBody& body, const LLColor4& colour,
                    const char* rise_glyph, const char* set_glyph)
    {
        if (body.mBodyIndex < 0) return;

        F64 rise = 0.0;
        F64 set = 0.0;
        // A body that never crosses the horizon draws nothing rather than
        // being pinned to an edge: on a world where the sun never sets
        // there is no sunrise to point at, and a marker parked at phase 0
        // would be a straight-up lie about that world.
        if (!SSAtmoEnvPlanetaryResolver::riseSetPhases(body.mDirection,
                home_body.mAxialTiltDeg, home_body.mLatitudeDeg, rise, set)) return;

        const S32 rise_x = left_edge + (S32)(llclamp(rise, 0.0, 1.0) * (F64)travel);
        const S32 set_x  = left_edge + (S32)(llclamp(set,  0.0, 1.0) * (F64)travel);

        font->renderUTF8(std::string(rise_glyph), 0, rise_x, glyph_y, colour,
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
        font->renderUTF8(std::string(set_glyph), 0, set_x, glyph_y, colour,
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
    };

    // Moon first: where the two coincide (a body rising as the other sets,
    // or a tight double), the sun's larger glyph reads through the smaller
    // one rather than the other way round.
    mark(moon, MOON_MARKER_COLOUR, RISE_TRIANGLE_SMALL, SET_TRIANGLE_SMALL);
    mark(sun, SUN_MARKER_COLOUR, RISE_TRIANGLE, SET_TRIANGLE);
}

void SSFloaterAtmoEnv::drawKeyframeGhosts()
{
    if (!SSAtmoEnvManager::getInstance()->hasAsset()) return;

    std::vector<GhostKeyframe> ghosts;
    bool show_labels = true;
    if (!collectHoveredKeyframes(ghosts, show_labels) || ghosts.empty()) return;

    LLRect rect;
    S32 left_edge = 0;
    S32 travel = 0;
    if (!scrubberGeometry(rect, left_edge, travel)) return;

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 glyph_y = rect.getCenterY();

    // Value labels alternate above and below the scrubber when they would
    // otherwise collide. Keyframes cluster - a dawn ramp is three of them
    // inside a few percent of the cycle - and overlapping labels there are
    // worse than useless, since two half-drawn numbers read as neither.
    //
    // Greedy, left to right: take the upper lane if it is clear, otherwise
    // the lower one, otherwise whichever has been free longest. That last
    // case only arises when labels are too dense for two lanes to hold,
    // where something has to overlap regardless; picking the emptier lane
    // at least keeps the damage to one pair.
    std::sort(ghosts.begin(), ghosts.end(),
              [](const GhostKeyframe& a, const GhostKeyframe& b) { return a.mDrawPhase < b.mDrawPhase; });

    const S32 LANE_GAP = 4; // pixels of clear space required between neighbours
    S32 lane_right[2] = { S32_MIN, S32_MIN };

    const S32 lane_y[2] = { rect.mTop + 2, rect.mBottom - 2 };
    const LLFontGL::VAlign lane_valign[2] = { LLFontGL::BOTTOM, LLFontGL::TOP };

    for (const GhostKeyframe& ghost : ghosts)
    {
        const F64 phase = llclamp(ghost.mDrawPhase, 0.0, 1.0);
        const S32 x = left_edge + (S32)(phase * (F64)travel);

        // Filled marks the keyframe currently in force, the same
        // filled/hollow distinction the row's own diamond button uses. For
        // a HOLD keyframe that means the head being anywhere in its span,
        // not just on its boundary - the whole point of drawing it as a
        // span is that the value is in force across all of it.
        const bool current = ghost.mHold
            ? (mPreviewPhase >= ghost.mSpanStart && mPreviewPhase < ghost.mSpanEnd)
            : (std::fabs(phase - mPreviewPhase) < SSAtmoEnvKeyframed<F32>::PHASE_EPSILON);

        const LLColor4 colour = current ? LLColor4::white : LLColor4(0.7f, 0.7f, 0.7f, 0.9f);

        font->renderUTF8(std::string(current ? FILLED_DIAMOND : HOLLOW_DIAMOND), 0,
                         x, glyph_y, colour, LLFontGL::HCENTER, LLFontGL::VCENTER);

        if (!show_labels) continue;

        const S32 half_width = font->getWidth(ghost.mLabel) / 2;
        const S32 label_left = x - half_width;

        S32 lane;
        if (label_left > lane_right[0] + LANE_GAP)      lane = 0;
        else if (label_left > lane_right[1] + LANE_GAP) lane = 1;
        else                                            lane = (lane_right[0] <= lane_right[1]) ? 0 : 1;

        lane_right[lane] = x + half_width;

        font->renderUTF8(ghost.mLabel, 0, x, lane_y[lane], colour,
                         LLFontGL::HCENTER, lane_valign[lane]);
    }
}

void SSFloaterAtmoEnv::drawSliderValueGhosts()
{
    if (!SSAtmoEnvManager::getInstance()->hasAsset()) return;

    // Only while the scrubber is hovered. Left on permanently these compete
    // with the thumb for attention on every row at once, and the thumb is
    // the thing you are usually reading. Tying them to the same gesture
    // that raises the scrubber overview also pairs the two halves: one
    // gesture, both axes - when each parameter has keyframes, and what
    // values they hold.
    if (!scrubberHovered()) return;

    // Widgets on a tab are positioned relative to that tab, but drawing
    // here happens in the floater's own space (LLFloater::draw has left the
    // origin at its bottom-left). Screen rects are the common ground: the
    // difference between a widget's and the floater's gives the widget in
    // floater-local coordinates without walking the view hierarchy.
    const LLRect floater_screen = calcScreenRect();

    LLPointer<LLUIImage> thumb_image = LLUI::getUIImage("SliderThumb_Off");
    const S32 thumb_width = thumb_image.notNull() ? thumb_image->getWidth() : 16;

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    for (const FloatRow& row : mFloatRows)
    {
        // Same suppression as the scrubber overlay - see rowAutoOwned().
        if (rowAutoOwned(row.mPrefix)) continue;

        LLSliderCtrl* slider = findChild<LLSliderCtrl>(row.mPrefix + "_slider");
        if (!slider || !slider->isInVisibleChain()) continue;

        const SSAtmoEnvKeyframed<F32>& field = row.mField();
        if (!field.hasKeyframes()) continue;

        const F32 lo = slider->getMinValue();
        const F32 hi = slider->getMaxValue();
        const F32 range = hi - lo;
        if (range <= 0.f) continue;

        const LLRect screen = slider->calcScreenRect();
        const S32 left = screen.mLeft - floater_screen.mLeft;
        const S32 bottom = screen.mBottom - floater_screen.mBottom;

        // Same travel LLSlider::updateThumbRect() uses, so a ghost lands on
        // the pixel the thumb would occupy at that value.
        const S32 left_edge = left + (thumb_width / 2);
        const S32 travel = screen.getWidth() - thumb_width;
        if (travel <= 0) continue;

        const S32 y = bottom + (screen.getHeight() / 2);

        for (const SSAtmoEnvKeyframe<F32>& kf : field.keyframes())
        {
            const F32 t = llclamp((kf.mValue - lo) / range, 0.f, 1.f);
            const S32 x = left_edge + (S32)(t * (F32)travel);

            // Filled marks the keyframe the head is actually on, matching
            // the row's own diamond button and the scrubber ghosts. When it
            // is filled it sits under the thumb by definition - that
            // coincidence is the point, not a collision.
            const bool current = std::fabs(kf.mTime - mPreviewPhase)
                                 < SSAtmoEnvKeyframed<F32>::PHASE_EPSILON;

            font->renderUTF8(std::string(current ? FILLED_DIAMOND : HOLLOW_DIAMOND), 0,
                             x, y,
                             current ? LLColor4::white : LLColor4(0.7f, 0.7f, 0.7f, 0.9f),
                             LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
}

void SSFloaterAtmoEnv::onCommitPreviewTime()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // Percent in, phase out - see refreshPreview for why the control is
    // calibrated that way. The readout beside it is what turns the phase
    // into a legible time of day.
    mPreviewPhase = llclamp(
        getChild<LLUICtrl>("preview_time_slider")->getValue().asReal() * 0.01, 0.0, 1.0);
    refreshPreview();
}

// </SS:Nexii>
