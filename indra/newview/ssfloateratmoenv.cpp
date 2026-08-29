/**
 * @file ssfloateratmoenv.cpp
 * @brief See ssfloateratmoenv.h.
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
#include "ssatmoenvapplier.h" // <SS:Nexii> the auto dome altitude the greyed-out row shows
#include "ssfloateratmoplanetary.h"
#include "ssfloateratmoinfluence.h"
#include "ssfloateratmoskyimport.h"
#include "ssfloateratmoenvcreate.h"
#include "ssprecippreset.h"
#include "ssatmoenvbridge.h"

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
#include "lltabcontainer.h"
#include "lltexturectrl.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

#include <algorithm>

static const F64 STATUS_POLL_INTERVAL = 0.5;

static const char* const FILLED_DIAMOND = "\xE2\x97\x86";
static const char* const HOLLOW_DIAMOND = "\xE2\x97\x87";

static const char* const RISE_TRIANGLE = "\xE2\x96\xB2";
static const char* const SET_TRIANGLE = "\xE2\x96\xBC";
static const char* const RISE_TRIANGLE_SMALL = "\xE2\x96\xB4";
static const char* const SET_TRIANGLE_SMALL = "\xE2\x96\xBE";

static const LLColor4 SUN_MARKER_COLOUR(1.f, 0.8f, 0.4f, 0.8f);
static const LLColor4 MOON_MARKER_COLOUR(0.72f, 0.78f, 0.95f, 0.75f);

static const S32 HOVER_PAD_X = 6;
static const S32 HOVER_PAD_Y = 10;

// Floater shell; all content is wired in postBuild.
SSFloaterAtmoEnv::SSFloaterAtmoEnv(const LLSD& key) :
    LLFloater(key)
{
}

// <SS:Nexii> A tab_container matches on the panel's own name, which is either the name given in the
// tab entry or the one inside the loaded panel file depending on which params win. Both are tried
// so a rename in one place cannot silently stop the rail selecting tabs.
static bool ss_select_tab(LLTabContainer* tabs, const char* entry_name, const char* panel_name)
{
    if (!tabs) return false;
    if (tabs->getPanelByName(entry_name)) return tabs->selectTabByName(entry_name);
    return tabs->selectTabByName(panel_name);
}

static bool ss_tab_is(const LLPanel* panel, const char* entry_name, const char* panel_name)
{
    if (!panel) return false;
    const std::string& name = panel->getName();
    return name == entry_name || name == panel_name;
}

// Wires the whole editor: toolbar, altitude rail, every tab's rows, keyframe buttons, preview scrubber.
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

    for (S32 slot = 1; slot < SS_ATMOENV_MAX_TRACKS; ++slot)
    {
        getChild<LLButton>(llformat("track_name_button_%d", slot))->setClickedCallback(
            [this, slot](LLUICtrl*, const LLSD&) { selectTrack(slot); });
    }

    LLMultiSliderCtrl* alt_slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    alt_slider->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitAltitudeSlider(); });
    alt_slider->setSliderMouseUpCallback(
        [this](LLUICtrl*, const LLSD&) { onMouseUpAltitudeSlider(); });

    getChild<LLUICtrl>("name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitName(); });

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

    getChild<LLUICtrl>("preview_time_slider")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitPreviewTime(); });

    getChild<LLUICtrl>("settings_button")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("ss_atmo"); });

    getChild<LLUICtrl>("preview_play_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickPreviewPlay(); });

    getChild<LLUICtrl>("track_name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitTrackName(); });

    // <SS:Nexii> World templates. Built from the table rather than listed in the XUI so the two
    // cannot drift: adding an archetype is one row in ssAtmoEnvTemplates() and nothing else.
    LLComboBox* template_combo = getChild<LLComboBox>("track_template_combo");
    template_combo->clearRows();
    for (const SSAtmoEnvTemplate& tmpl : ssAtmoEnvTemplates())
    {
        template_combo->add(tmpl.mLabel, LLSD(tmpl.mKey));
    }
    template_combo->selectFirstItem();
    getChild<LLButton>("track_template_apply_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickApplyTemplate(); });
    const char* day_cycle_fields[] = { "day_length_slider", "day_offset_slider",
                                      "day_length_value_spinner", "day_offset_value_spinner" };
    for (const char* name : day_cycle_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitDayCycle(); });
    }

    const char* planetary_scale_fields[] = { "sun_planet_scale_slider", "sun_planet_scale_spinner",
                                             "planet_moon_scale_slider", "planet_moon_scale_spinner" };
    for (const char* name : planetary_scale_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitPlanetaryScales(); });
    }
    getChild<LLButton>("open_planetary_designer_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickOpenPlanetaryDesigner(); });

    getChild<LLUICtrl>("water_enabled_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWaterEnabled(); });
    getChild<LLUICtrl>("ucloud_enabled_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitUnderEnabled(); });
    getChild<LLUICtrl>("ucloud_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitUnderAuto(); });

    getChild<LLUICtrl>("gust_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitGustAuto(); });
    getChild<LLUICtrl>("lightning_enabled_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitLightningFlags(); });
    getChild<LLUICtrl>("lightning_charge_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitLightningFlags(); });
    getChild<LLUICtrl>("lightning_sparks_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitLightningFlags(); });
    getChild<LLUICtrl>("lightning_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitLightningAuto(); });
    getChild<LLUICtrl>("cloud_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitCloudAuto(); });
    getChild<LLUICtrl>("dome_auto_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitDomeAuto(); });
    getChild<LLUICtrl>("atmo_horizon_clip_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitHorizonClip(); });

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
        { "lightning_core_white", [this]() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mLightningCoreWhite; }, false },
    };

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

    auto clouds = [this]() -> SSAtmoEnvCloudField& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mCloudField;
    };
    const std::vector<FloatRow> cloud_rows = {
        { "cloud_base_height", [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mBaseHeightM; },    true },
        { "cloud_thickness",   [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mBaseThicknessM; }, true },
        { "cloud_coverage",    [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mCoverageScale; },  false },
        { "cloud_puff_density",[clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mPuffDensity; },     false },
        { "cloud_storm_dark",  [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mStormDarkening; },  false },
        { "cloud_texture_mix", [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mTextureMix; },      false },
        { "cloud_detail_scale",[clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mDetailScale; },     false },
        { "cloud_drift_rate",  [clouds]() -> SSAtmoEnvKeyframed<F32>& { return clouds().mDriftRate; },       false },
    };
    mFloatRows.insert(mFloatRows.end(), cloud_rows.begin(), cloud_rows.end());

    auto under = [this]() -> SSAtmoEnvCloudField& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mUnderField;
    };
    const std::vector<FloatRow> under_rows = {
        { "ucloud_base_height", [under]() -> SSAtmoEnvKeyframed<F32>& { return under().mBaseHeightM; },    true },
        { "ucloud_thickness",   [under]() -> SSAtmoEnvKeyframed<F32>& { return under().mBaseThicknessM; }, true },
        { "ucloud_coverage",    [under]() -> SSAtmoEnvKeyframed<F32>& { return under().mCoverageScale; },  false },
        { "ucloud_puff_density",[under]() -> SSAtmoEnvKeyframed<F32>& { return under().mPuffDensity; },     false },
        { "ucloud_storm_dark",  [under]() -> SSAtmoEnvKeyframed<F32>& { return under().mStormDarkening; },  false },
        { "ucloud_texture_mix", [under]() -> SSAtmoEnvKeyframed<F32>& { return under().mTextureMix; },      false },
        { "ucloud_detail_scale",[under]() -> SSAtmoEnvKeyframed<F32>& { return under().mDetailScale; },     false },
        { "ucloud_drift_rate",  [under]() -> SSAtmoEnvKeyframed<F32>& { return under().mDriftRate; },       false },
    };
    mFloatRows.insert(mFloatRows.end(), under_rows.begin(), under_rows.end());

    auto dome = [this]() -> SSAtmoEnvCloudDome& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mCloudDome;
    };
    const std::vector<FloatRow> dome_rows = {
        { "dome_height",    [dome]() -> SSAtmoEnvKeyframed<F32>& { return dome().mHeightM; },  true },
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

    auto atmos = [this]() -> SSAtmoEnvAtmosphere& {
        return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mAtmosphere;
    };
    const std::vector<FloatRow> atmos_rows = {
        { "atmo_haze_horizon",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mHazeHorizon; },        false },
        { "atmo_haze_density",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mHazeDensity; },        false },
        { "atmo_rainbow",        [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSkyMoistureLevel; },   false },
        { "atmo_droplet_radius",  [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSkyDropletRadius; },   false },
        { "atmo_ice_level",       [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mSkyIceLevel; },        false },
        { "atmo_density_mult",    [atmos]() -> SSAtmoEnvKeyframed<F32>& { return atmos().mDensityMultiplier; },  false, 0.001f },
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

    // <SS:Nexii> The slider keeps its honest near-surface dial (SS_ATMOENV_WATER_CEILING); the
    // spinner takes the whole authored range by hand, so a sky-themed build can put its ocean
    // kilometres below the platform. Values past the slider's ends read there pinned at the rail.
    getChild<LLSliderCtrl>("water_height_slider")->setMaxValue(SS_ATMOENV_WATER_CEILING);
    getChild<LLSpinCtrl>("water_height_value_spinner")->setMinValue(SS_ATMOENV_WATER_MIN);
    getChild<LLSpinCtrl>("water_height_value_spinner")->setMaxValue(SS_ATMOENV_WATER_MAX);

    static const F32 SCALE_SUN_AMBIENT = 3.f;
    static const F32 SCALE_BLUE = 2.f;

    mColorRows = {
        { "water_fog_color", [water]() -> SSAtmoEnvKeyframed<LLColor3>& { return water().mFogColor; } },
        { "atmo_ambient",        [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mAmbientColor; }, SCALE_SUN_AMBIENT },
        { "atmo_blue_horizon",   [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mBlueHorizon; }, SCALE_BLUE },
        { "atmo_blue_density",   [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mBlueDensity; }, SCALE_BLUE },
        { "atmo_sunlight_color", [atmos]() -> SSAtmoEnvKeyframed<LLColor3>& { return atmos().mSunlightColor; }, SCALE_SUN_AMBIENT },
        { "dome_color",          [dome]() -> SSAtmoEnvKeyframed<LLColor3>& { return dome().mColor; } },
        { "lightning_color",     [this]() -> SSAtmoEnvKeyframed<LLColor3>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mLightningColor; } },
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
        { "cloud_field_image", [clouds]() -> SSAtmoEnvKeyframed<LLUUID>& { return clouds().mBaseTexture; } },
        { "cloud_detail_image",[clouds]() -> SSAtmoEnvKeyframed<LLUUID>& { return clouds().mDetailTexture; } },
        { "ucloud_field_image", [under]() -> SSAtmoEnvKeyframed<LLUUID>& { return under().mBaseTexture; } },
        { "ucloud_detail_image",[under]() -> SSAtmoEnvKeyframed<LLUUID>& { return under().mDetailTexture; } },
    };
    for (const KeyRow<LLUUID>& row : mTextureRows)
    {
        getChild<LLUICtrl>(row.mPrefix)->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitTextureRow(row); refreshPreview(); refreshStatus(); });
        bindKeyframeButtons<LLUUID>(row.mPrefix, row.mField);
    }

    LLTextureCtrl* dome_image = getChild<LLTextureCtrl>("dome_image");
    dome_image->setDefaultImageAssetID(LLSettingsSky::GetDefaultCloudNoiseTextureId());
    dome_image->setAllowNoTexture(true);

    mStringRows = {
        { "precipitation_combo", [this]() -> SSAtmoEnvKeyframed<std::string>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[mSelectedTrackIndex].mWeather.mPrecipitationOverride; } },
    };
    for (const KeyRow<std::string>& row : mStringRows)
    {
        getChild<LLUICtrl>(row.mPrefix)->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitStringRow(row); refreshPreview(); refreshStatus(); });
        bindKeyframeButtons<std::string>(row.mPrefix, row.mField);
    }

    // <SS:Nexii> The rail's mode follows the selected tab and nothing else, so the tab container is
    // the only thing that drives it - no separate zoom control to keep in step with the selection.
    // The sync first, though: the rail's markers select tabs, so a tab clicked directly has to
    // select or clear its marker the same way, or a pressed marker survives a tab click that
    // moved elsewhere and a direct click never shows what it linked to.
    getChild<LLTabContainer>("atmo_tabs")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            syncSelectionToTab();
            refreshRailMode();
        });
    // The Clouds sub-tabs are part of the same bargain: Main/Under press their deck marker,
    // Dome - like Space, a pinned anchor rather than a layer - clears it.
    getChild<LLTabContainer>("clouds_sub_tabs")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            syncSelectionToTab();
            refreshRailMode();
        });

    for (S32 layer = 0; layer < LAYER_COUNT; ++layer)
    {
        getChild<LLUICtrl>(llformat("layer_name_button_%d", layer + 1))->setCommitCallback(
            [this, layer](LLUICtrl*, const LLSD&) { onClickLayerMarker(layer); });
    }
    getChild<LLUICtrl>("space_anchor_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            ss_select_tab(getChild<LLTabContainer>("atmo_tabs"), "space_tab",
                          "panel_ss_atmo_env_space");
            mSelectedLayer = LAYER_NONE;
            refreshRailMode();
        });
    getChild<LLUICtrl>("dome_anchor_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            ss_select_tab(getChild<LLTabContainer>("atmo_tabs"), "clouds_tab",
                          "panel_ss_atmo_env_clouds");
            ss_select_tab(getChild<LLTabContainer>("clouds_sub_tabs"), "clouds_dome_tab",
                          "panel_ss_atmo_env_clouds_dome");
            mSelectedLayer = LAYER_NONE;
            refreshRailMode();
        });
    getChild<LLUICtrl>("weather_marker_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&)
        {
            ss_select_tab(getChild<LLTabContainer>("atmo_tabs"), "weather_tab",
                          "panel_ss_atmo_env_weather");
            ss_select_tab(getChild<LLTabContainer>("weather_sub_tabs"),
                          "weather_precipitation_tab",
                          "panel_ss_atmo_env_weather_precipitation");
            mSelectedLayer = LAYER_NONE;
            refreshRailMode();
        });
    getChild<LLUICtrl>("add_deck_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddDeck(); });
    getChild<LLUICtrl>("remove_deck_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveDeck(); });
    getChild<LLUICtrl>("weather_source_combo")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWeatherSource(); });
    getChild<LLUICtrl>("precip_new_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickNewPrecipType(); });
    getChild<LLUICtrl>("precip_edit_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickEditPrecipTypes(); });

    refreshVisibility();
    refreshWeatherSource();
    refreshPrecipitationTypes();
    refreshRailMode();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
    return true;
}

// Full refresh on open.
void SSFloaterAtmoEnv::onOpen(const LLSD& key)
{
    refreshVisibility();
    refreshWeatherSource();
    refreshPrecipitationTypes();
    refreshRailMode();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

// Clears the preview override so live time resumes.
void SSFloaterAtmoEnv::onClose(bool app_quitting)
{
    SSAtmoEnvManager::getInstance()->clearPreviewPhaseOverride();
    LLFloater::onClose(app_quitting);
}

// Hiding behaves like closing for the preview override.
void SSFloaterAtmoEnv::onVisibilityChange(bool new_visibility)
{
    LLFloater::onVisibilityChange(new_visibility);

    if (new_visibility)
    {
        refreshPreview();
    }
    else
    {
        SSAtmoEnvManager::getInstance()->clearPreviewPhaseOverride();
    }
}

// Keeps the rail markers placed through resizes.
void SSFloaterAtmoEnv::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);

    repositionRailMarkers();
}

// Per-frame: preview playback, ghosts, markers, status polling.
void SSFloaterAtmoEnv::draw()
{
    advancePreviewPlayback();

    const F64 now = LLTimer::getElapsedSeconds();

    // <SS:Nexii> The rail's zoom between the region scale and the selected track's own contents.
    // Smoothstepped so it reads as diving into the track rather than as the widget cutting to a
    // different set of numbers; markers glide while it runs and the thumbs land at the end.
    if (mRailZooming)
    {
        static const F64 RAIL_ZOOM_SECONDS = 0.2;
        F32 t = (F32)llclamp((now - mRailZoomStart) / RAIL_ZOOM_SECONDS, 0.0, 1.0);
        const F32 eased = cubic_step(t);

        mRailMin = lerp(mRailMinFrom, mRailMinTo, eased);
        mRailMax = lerp(mRailMaxFrom, mRailMaxTo, eased);

        if (t >= 1.f)
        {
            mRailZooming = false;
            mRailMin = mRailMinTo;
            mRailMax = mRailMaxTo;
        }

        if (mRailMode == ERailMode::LAYER)
        {
            refreshLayerRail();
        }
        else
        {
            refreshTrackRail();
        }
    }

    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;
        refreshStatus();
        refreshVisibility();
        refreshTrackTab();
        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            refreshRailMode();
            refreshPlanetaryScales();
            refreshPreview();
        }
    }

    LLFloater::draw();

    drawWeatherBracket();
    drawRiseSetMarkers();
    drawKeyframeGhosts();
    drawSliderValueGhosts();
}

// Accepts EEP settings and notecard drops onto the floater. With an environment loaded a
// settings drop stamps into it; with nothing loaded a settings drop becomes a SEED for a new
// environment, the same choices the Create button's chooser offers.
bool SSFloaterAtmoEnv::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                        EDragAndDropType cargo_type, void* cargo_data,
                                        EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type == DAD_SETTINGS)
    {
        *accept = ACCEPT_YES_SINGLE;
        if (drop)
        {
            if (!SSAtmoEnvManager::getInstance()->hasAsset())
            {
                handleCreateDrop((const LLInventoryItem*)cargo_data);
            }
            else
            {
                handleSettingsDrop((const LLInventoryItem*)cargo_data);
            }
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
                if (!success)
                {
                    LLNotificationsUtil::add("GenericAlert", LLSD().with(
                        "MESSAGE", "That notecard could not be loaded as an Atmo Magic environment."));
                }
                refreshVisibility();
                refreshPrecipitationTypes();
                mSelectedTrackIndex = 0;
                refreshTrackRail();
                refreshTrackTab();
                refreshPlanetaryScales();
                refreshPreview();
            });
        if (started)
        {
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

// A dropped EEP sky/water is fetched, sky-checked, then offered to the import dialog - the
// author picks which groupings of it to take before anything is stamped into the track.
void SSFloaterAtmoEnv::handleSettingsDrop(const LLInventoryItem* drop_item)
{
    const LLViewerInventoryItem* item =
        drop_item ? gInventory.getItem(drop_item->getUUID()) : nullptr;
    if (!item || item->getAssetUUID().isNull()) return;

    if (!item->checkPermissionsSet(PERM_ITEM_UNRESTRICTED))
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with(
            "MESSAGE", "That setting isn't full permission - only full-perm settings can be imported."));
        return;
    }

    const S32 track_index = mSelectedTrackIndex;
    const F64 phase = mPreviewPhase;
    const std::string item_name = item->getName();
    LLHandle<LLFloater> handle = getHandle();

    LLSettingsVOBase::getSettingsAsset(item->getAssetUUID(),
        [handle, track_index, phase, item_name](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
        {
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
                LLNotificationsUtil::add("GenericAlert", LLSD().with(
                    "MESSAGE", "Only sky settings can be imported for now."));
                return;
            }

            SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
            if (!mgr->hasAsset()
                || track_index < 0 || track_index >= (S32)mgr->editable().mTracks.size())
            {
                return;
            }

            // The fetched sky carries no home of its own - hand it to the import dialog, which
            // asks which groupings to take before anything is stamped. The dialog re-resolves
            // the track at OK time, so the checks above are only an early out for the common case.
            SSFloaterAtmoSkyImport::show(sky, track_index, phase, item_name, handle);
        });
}

// A settings drop with nothing loaded: the dropped setting seeds a NEW environment. The asset is
// fetched first and classified by what it actually is - an item alone cannot be trusted to say
// sky vs day cycle - then handed to the matching seed. Water presets are refused: they have no
// seed that makes sense yet.
void SSFloaterAtmoEnv::handleCreateDrop(const LLInventoryItem* drop_item)
{
    const LLViewerInventoryItem* item =
        drop_item ? gInventory.getItem(drop_item->getUUID()) : nullptr;
    if (!item || item->getAssetUUID().isNull()) return;

    if (!item->checkPermissionsSet(PERM_ITEM_UNRESTRICTED))
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with(
            "MESSAGE", "That setting isn't full permission - only full-perm settings can be used to create an environment."));
        return;
    }

    const std::string item_name = item->getName();
    LLHandle<LLFloater> handle = getHandle();

    // The same adoption shape the chooser and the old create button use: take the written
    // notecard in as the live asset and refresh every panel that reads it.
    auto on_created = [handle](const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)
    {
        if (item_id.isNull() || asset_id.isNull()) return;
        SSAtmoEnvManager::getInstance()->adoptCreated(item_id, asset_id, asset);

        SSFloaterAtmoEnv* self = (SSFloaterAtmoEnv*)handle.get();
        if (self)
        {
            self->mSelectedTrackIndex = 0;
            self->refreshVisibility();
            self->refreshPrecipitationTypes();
            self->refreshTrackRail();
            self->refreshTrackTab();
            self->refreshPlanetaryScales();
            self->refreshStatus();
            self->refreshPreview();
        }
    };

    LLSettingsVOBase::getSettingsAsset(item->getAssetUUID(),
        [handle, item_name, on_created](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
        {
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

            if (std::dynamic_pointer_cast<LLSettingsDay>(settings))
            {
                SSAtmoEnvManager::createFromDayCycle(asset_id, LLUUID::null, on_created);
                return;
            }

            if (std::dynamic_pointer_cast<LLSettingsSky>(settings))
            {
                SSAtmoEnvManager::createFromSkies({ asset_id }, { item_name }, LLUUID::null, on_created);
                return;
            }

            LLNotificationsUtil::add("GenericAlert", LLSD().with(
                "MESSAGE", "Only sky settings and day cycles can seed a new environment."));
        });
}

// Shows the editor or the no-asset landing state.
void SSFloaterAtmoEnv::refreshVisibility()
{
    const bool has_asset = SSAtmoEnvManager::getInstance()->hasAsset();

    getChild<LLUICtrl>("create_new_button")->setVisible(!has_asset);
    getChild<LLUICtrl>("no_asset_text")->setVisible(!has_asset);

    const char* editing_widgets[] = {
        "name_editor", "save_button", "revert_button", "unload_button",
        "track_panel", "atmo_tabs",
        "preview_time_caption", "preview_play_button", "preview_time_slider",
        "preview_time_value_text",
        "forecast_text"
    };
    for (const char* name : editing_widgets)
    {
        getChild<LLUICtrl>(name)->setVisible(has_asset);
    }
}

// Rebuilds the vertical altitude rail: one row per track at its scaled height.
void SSFloaterAtmoEnv::refreshTrackRail()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();

    if (mSelectedTrackIndex >= (S32)asset.mTracks.size())
    {
        mSelectedTrackIndex = 0;
    }

    // <SS:Nexii> Called from a dozen places that just want the rail brought up to date. In layer
    // mode the markers belong to refreshLayerRail(); the asset-level chrome below is shared, so
    // only the track markers themselves are conditional.
    if (mRailMode == ERailMode::LAYER)
    {
        refreshLayerRail();

        LLLineEditor* layer_name_editor = getChild<LLLineEditor>("name_editor");
        if (!layer_name_editor->hasFocus())
        {
            layer_name_editor->setText(asset.mName);
        }
        return;
    }

    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    slider->clear();

    // Layer mode retunes the rail to whatever the selected track spans, so track mode puts the
    // authored region scale back rather than assuming it survived.
    slider->setMinValue(0.f);
    slider->setMaxValue(SS_ATMOENV_REGION_CEILING);
    slider->setIncrement(64.f);
    slider->setOverlapThreshold(304.f);

    const S32 optional_count = (S32)asset.mTracks.size() - 1;
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

// Re-places the camera and selection markers on the rail.
void SSFloaterAtmoEnv::repositionRailMarkers()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    if (mRailMode == ERailMode::LAYER)
    {
        refreshLayerRail();
        return;
    }

    const S32 centre = railCentreForValue(0.f);
    centreViewOn(getChild<LLUICtrl>("track_ground_button"), centre);
    centreViewOn(getChild<LLUICtrl>("track_ground_alt_label"), centre);

    const SSAtmoEnvAsset& asset = mgr->asset();
    for (S32 slot = 1; slot < (S32)asset.mTracks.size() && slot < SS_ATMOENV_MAX_TRACKS; ++slot)
    {
        refreshAltitudeLabel(slot);
    }
}

// Altitude to rail pixel. Maps through mRailMin/mRailMax rather than the slider's own range: the
// two agree except mid-zoom, when the markers glide and the slider carries no thumbs at all.
S32 SSFloaterAtmoEnv::railCentreForValue(F32 value) const
{
    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    const LLRect sld_rect = slider->getRect();

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

    const F32 range = mRailMax - mRailMin;
    F32 t = (range > 0.f) ? (value - mRailMin) / range : 0.f;
    t = llclamp(t, 0.f, 1.f);

    return sld_rect.mBottom + bottom_edge + (S32)(t * (F32)(top_edge - bottom_edge));
}

// What a deck actually resolves to at the preview phase: the auto derivation off the track's
// weather when the field owns its numbers, else the authored keyframes. The rail reads this
// rather than the raw keyframes so its markers and fit follow the deck the renderer draws - an
// auto deck's height wanders with moisture and convection, and the row it greys out does not.
void SSFloaterAtmoEnv::effectiveDeckSpan(const SSAtmoEnvTrack& track, bool under_deck,
                                         F32& out_base, F32& out_thickness) const
{
    const SSAtmoEnvCloudField& field = under_deck ? track.mUnderField : track.mCloudField;

    if (field.mAuto)
    {
        F32 coverage, dark;
        SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(
            track.mWeather.mMoisture.valueAt(mPreviewPhase),
            track.mWeather.mConvection.valueAt(mPreviewPhase),
            out_base, out_thickness, coverage, dark);
        return;
    }

    out_base      = field.mBaseHeightM.valueAt(mPreviewPhase);
    out_thickness = field.mBaseThicknessM.valueAt(mPreviewPhase);
}

// The altitude band the rail covers in layer mode: everything placed inside the selected track,
// padded, with a floor on the span so a flat stack does not collapse to a point. The dome is
// excluded deliberately - it is a backdrop pinned above the scale, and a cirrus dome at 6km would
// otherwise squash the decks being edited into the bottom eighth of the rail.
void SSFloaterAtmoEnv::railRangeForTrack(F32& out_min, F32& out_max) const
{
    out_min = 0.f;
    out_max = SS_ATMOENV_REGION_CEILING;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    F32 lo = track.mFloorZ;
    F32 hi = track.mFloorZ;

    auto include = [&lo, &hi](F32 value)
    {
        lo = llmin(lo, value);
        hi = llmax(hi, value);
    };

    const F64 phase = mPreviewPhase;

    F32 base, thickness;

    effectiveDeckSpan(track, false, base, thickness);
    include(base);
    include(base + thickness);

    if (track.mUnderField.mEnabled)
    {
        effectiveDeckSpan(track, true, base, thickness);
        include(base);
        include(base + thickness);
    }

    if (track.mWater.mEnabled)
    {
        include(track.mWater.mHeight.valueAt(phase));
    }

    static const F32 RAIL_MIN_SPAN = 200.f;
    if (hi - lo < RAIL_MIN_SPAN)
    {
        const F32 centre = 0.5f * (lo + hi);
        lo = centre - 0.5f * RAIL_MIN_SPAN;
        hi = centre + 0.5f * RAIL_MIN_SPAN;
    }

    const F32 pad = 0.08f * (hi - lo);
    out_min = lo - pad;
    out_max = hi + pad;

    // Quantise the fit to 256m blocks. The fit re-runs on every poll, and an auto deck's height
    // wanders with moisture and convection, so an exact fit glides the whole scale under even a
    // small drift; rounding keeps it still until the content actually crosses a block boundary.
    // Rounding can land both bounds of a near-minimum span on one block, so spread a collapsed
    // fit back out symmetrically.
    static const F32 FIT_BLOCK = 256.f;
    out_min = ll_round(out_min, FIT_BLOCK);
    out_max = ll_round(out_max, FIT_BLOCK);
    if (out_max - out_min < RAIL_MIN_SPAN)
    {
        out_min -= 0.5f * FIT_BLOCK;
        out_max += 0.5f * FIT_BLOCK;
    }
}

F32 SSFloaterAtmoEnv::weatherReferenceSurface() const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return 0.f;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return 0.f;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    F32 surface = track.mFloorZ;
    if (track.mWater.mEnabled)
    {
        surface = llmax(surface, track.mWater.mHeight.valueAt(mPreviewPhase));
    }
    return surface;
}

// The deck precipitation falls from. Derived by default as the lowest enabled deck above the
// reference surface, which resolves to the main deck for a sky build because the under deck hangs
// below the platform floor. An authored override that names a deck which has since been disabled
// falls back to derivation rather than leaving weather with no source.
S32 SSFloaterAtmoEnv::weatherDeliveringDeck() const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return LAYER_NONE;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return LAYER_NONE;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    if (track.mWeatherSourceDeck == SS_ATMOENV_DECK_UNDER && track.mUnderField.mEnabled)
    {
        return LAYER_UNDER;
    }
    if (track.mWeatherSourceDeck == SS_ATMOENV_DECK_MAIN)
    {
        return LAYER_MAIN;
    }

    const F32 surface = weatherReferenceSurface();
    F32 main_base, main_thickness, under_base, under_thickness;
    effectiveDeckSpan(track, false, main_base, main_thickness);
    effectiveDeckSpan(track, true, under_base, under_thickness);

    const bool under_above = track.mUnderField.mEnabled && under_base > surface;
    if (under_above && under_base <= main_base) return LAYER_UNDER;
    if (main_base > surface) return LAYER_MAIN;
    if (under_above) return LAYER_UNDER;
    return LAYER_NONE;
}

F32 SSFloaterAtmoEnv::layerAltitude(S32 layer) const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return 0.f;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return 0.f;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    switch (layer)
    {
    case LAYER_WATER: return track.mWater.mHeight.valueAt(mPreviewPhase);
    case LAYER_MAIN:
    case LAYER_UNDER:
    {
        F32 base, thickness;
        effectiveDeckSpan(track, layer == LAYER_UNDER, base, thickness);
        return base;
    }
    default: return 0.f;
    }
}

bool SSFloaterAtmoEnv::layerPresent(S32 layer) const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return false;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    switch (layer)
    {
    case LAYER_WATER: return track.mWater.mEnabled;
    case LAYER_MAIN:  return true;
    case LAYER_UNDER: return track.mUnderField.mEnabled;
    default: return false;
    }
}

// Which mode the rail should be in, driven entirely by the selected tab, and the widget swap that
// goes with it. The author never asks for a zoom level: Track shows the region, everything else
// shows the track you are inside.
// Tab to rail, the reverse half of selectTrack/selectLayer. A tab clicked directly resolves the
// marker it is linked to: Water presses the water marker, Clouds presses whichever deck its
// sub-tabs are showing, and tabs with nothing on the rail - Weather, Sky, Space, Look, and Dome
// inside Clouds, a pinned anchor rather than a layer - clear the pressed one. Track reselects the
// selected track's own marker, which refreshTrackRail re-applies from mSelectedTrackIndex. Runs
// ahead of refreshRailMode, which re-applies the toggle states from the resolved selection.
void SSFloaterAtmoEnv::syncSelectionToTab()
{
    LLTabContainer* tabs = getChild<LLTabContainer>("atmo_tabs");
    const LLPanel* current = tabs->getCurrentPanel();

    if (ss_tab_is(current, "water_tab", "panel_ss_atmo_env_water"))
    {
        mSelectedLayer = layerPresent(LAYER_WATER) ? LAYER_WATER : LAYER_NONE;
        return;
    }

    if (ss_tab_is(current, "clouds_tab", "panel_ss_atmo_env_clouds"))
    {
        const LLPanel* sub = getChild<LLTabContainer>("clouds_sub_tabs")->getCurrentPanel();
        if (ss_tab_is(sub, "clouds_under_tab", "panel_ss_atmo_env_clouds_under"))
        {
            // The main deck is always present, so a stale Under selection on a track whose
            // second deck went away still has something legitimate to fall back to.
            mSelectedLayer = layerPresent(LAYER_UNDER) ? LAYER_UNDER : LAYER_MAIN;
        }
        else if (ss_tab_is(sub, "clouds_main_tab", "panel_ss_atmo_env_clouds_main"))
        {
            mSelectedLayer = LAYER_MAIN;
        }
        else
        {
            mSelectedLayer = LAYER_NONE;
        }
        return;
    }

    mSelectedLayer = LAYER_NONE;
}

void SSFloaterAtmoEnv::refreshRailMode()
{
    LLTabContainer* tabs = getChild<LLTabContainer>("atmo_tabs");
    const LLPanel* current = tabs->getCurrentPanel();
    const bool on_track = ss_tab_is(current, "track_tab", "panel_ss_atmo_env_track");

    const ERailMode wanted = on_track ? ERailMode::TRACK : ERailMode::LAYER;
    const bool changed = (wanted != mRailMode);
    mRailMode = wanted;

    const bool layer = (mRailMode == ERailMode::LAYER);

    getChild<LLUICtrl>("tracks_label")->setValue(layer ? "Layers" : "Tracks");

    for (S32 slot = 1; slot <= SS_ATMOENV_MAX_TRACKS - 1; ++slot)
    {
        if (layer)
        {
            getChild<LLUICtrl>(llformat("track_name_button_%d", slot))->setVisible(false);
            getChild<LLUICtrl>(llformat("track_alt_label_%d", slot))->setVisible(false);
        }
    }
    getChild<LLUICtrl>("track_ground_button")->setVisible(!layer);
    getChild<LLUICtrl>("track_ground_alt_label")->setVisible(!layer);
    getChild<LLUICtrl>("add_track_button")->setVisible(!layer);
    getChild<LLUICtrl>("remove_track_button")->setVisible(!layer);

    getChild<LLUICtrl>("space_anchor_button")->setVisible(layer);
    getChild<LLUICtrl>("dome_anchor_button")->setVisible(layer);
    getChild<LLUICtrl>("dome_anchor_alt_label")->setVisible(layer);
    getChild<LLUICtrl>("weather_marker_button")->setVisible(layer);
    getChild<LLUICtrl>("add_deck_button")->setVisible(layer);
    getChild<LLUICtrl>("remove_deck_button")->setVisible(layer);
    if (!layer)
    {
        for (S32 i = 0; i < LAYER_COUNT; ++i)
        {
            getChild<LLUICtrl>(llformat("layer_name_button_%d", i + 1))->setVisible(false);
            getChild<LLUICtrl>(llformat("layer_alt_label_%d", i + 1))->setVisible(false);
        }
    }

    F32 want_min = 0.f;
    F32 want_max = SS_ATMOENV_REGION_CEILING;
    if (layer)
    {
        railRangeForTrack(want_min, want_max);
    }

    if (changed || want_min != mRailMinTo || want_max != mRailMaxTo)
    {
        mRailMinFrom = mRailMin;
        mRailMaxFrom = mRailMax;
        mRailMinTo = want_min;
        mRailMaxTo = want_max;
        // Only the mode switch is worth animating. A range that merely re-fits because a deck
        // moved should follow the drag, not lag behind it.
        if (changed)
        {
            mRailZooming = true;
            mRailZoomStart = LLTimer::getElapsedSeconds();
        }
        else
        {
            mRailMin = want_min;
            mRailMax = want_max;
        }
    }

    if (layer)
    {
        refreshLayerRail();
    }
    else
    {
        refreshTrackRail();
    }
}

// Populates the rail with the selected track's contents. Thumbs are only added once the zoom has
// settled: the slider clamps values to its own range, so adding them mid-flight would snap a deck
// sitting outside the interpolated window onto its edge and then write that back.
void SSFloaterAtmoEnv::refreshLayerRail()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    // A refresh runs mid-drag (the live commit re-runs it on every move), and addSlider hands
    // "current" to each thumb it adds - without saving it here the last rebuilt thumb would steal
    // the drag off the one under the cursor, so a water drag would end up dragging the main deck.
    const std::string drag_slider = slider->getCurSlider();
    slider->clear();

    const F32 range = llmax(1.f, mRailMax - mRailMin);
    slider->setMinValue(mRailMin);
    slider->setMaxValue(mRailMax);
    slider->setIncrement(llmax(1.f, range / 256.f));
    // The authored 304m gap is sized for the 0-4096m scale and would reject every marker on a
    // fitted one, so it scales with the window too.
    slider->setOverlapThreshold(range / 24.f);

    static const char* const LAYER_LABELS[LAYER_COUNT] = { "Water", "Main Deck", "Under Deck" };

    for (S32 i = 0; i < LAYER_COUNT; ++i)
    {
        LLButton*  button = getChild<LLButton>(llformat("layer_name_button_%d", i + 1));
        LLTextBox* label  = getChild<LLTextBox>(llformat("layer_alt_label_%d", i + 1));

        const bool present = layerPresent(i);
        button->setVisible(present);
        label->setVisible(present);
        if (!present) continue;

        const F32 altitude = layerAltitude(i);

        if (!mRailZooming)
        {
            slider->addSlider(llclamp(altitude, mRailMin, mRailMax), llformat("layer%d", i));
        }

        button->setLabel(std::string(LAYER_LABELS[i]));
        button->setToggleState(i == mSelectedLayer);
        label->setText(llformat("%.0fm", altitude));

        const S32 centre = railCentreForValue(altitude);
        centreViewOn(button, centre);
        centreViewOn(label, centre);
    }

    // Put the drag (or last-touched thumb) back: setCurSlider no-ops when the name did not
    // survive the rebuild, so a zoom that drops thumbs mid-flight lands here harmless.
    if (!drag_slider.empty() && slider->getCurSlider() != drag_slider)
    {
        slider->setCurSlider(drag_slider);
    }

    // The dome reads out its height but keeps its pinned position - it is not on the scale.
    getChild<LLTextBox>("dome_anchor_alt_label")->setText(
        llformat("%.0fm", track.mCloudDome.mHeightM.valueAt(mPreviewPhase)));
    getChild<LLButton>("dome_anchor_button")->setToggleState(false);
    getChild<LLButton>("space_anchor_button")->setToggleState(false);

    const S32 delivering = weatherDeliveringDeck();
    LLButton* weather_marker = getChild<LLButton>("weather_marker_button");
    weather_marker->setVisible(delivering != LAYER_NONE);
    if (delivering != LAYER_NONE)
    {
        const F32 surface = weatherReferenceSurface();
        const F32 top = layerAltitude(delivering);
        centreViewOn(weather_marker, railCentreForValue(0.5f * (surface + top)));
    }

    getChild<LLUICtrl>("add_deck_button")->setEnabled(!track.mUnderField.mEnabled);
    getChild<LLUICtrl>("remove_deck_button")->setEnabled(track.mUnderField.mEnabled);
}

// Rail marker to tab. The markers are the tab selector in layer mode exactly as the track buttons
// are in track mode, which is the whole reason the two share one widget.
void SSFloaterAtmoEnv::selectLayer(S32 layer)
{
    mSelectedLayer = layer;

    LLTabContainer* tabs = getChild<LLTabContainer>("atmo_tabs");
    switch (layer)
    {
    case LAYER_WATER:
        ss_select_tab(tabs, "water_tab", "panel_ss_atmo_env_water");
        break;
    case LAYER_MAIN:
    case LAYER_UNDER:
        ss_select_tab(tabs, "clouds_tab", "panel_ss_atmo_env_clouds");
        if (layer == LAYER_MAIN)
        {
            ss_select_tab(getChild<LLTabContainer>("clouds_sub_tabs"),
                          "clouds_main_tab", "panel_ss_atmo_env_clouds_main");
        }
        else
        {
            ss_select_tab(getChild<LLTabContainer>("clouds_sub_tabs"),
                          "clouds_under_tab", "panel_ss_atmo_env_clouds_under");
        }
        break;
    default:
        break;
    }
    refreshRailMode();
}

void SSFloaterAtmoEnv::onClickLayerMarker(S32 layer)
{
    if (!layerPresent(layer))
    {
        refreshLayerRail();
        return;
    }
    selectLayer(layer);
}

// Add Deck reads generically but bottoms out on the under deck's enable flag: the model carries two
// named decks with distinct semantics rather than a vector. If a third is ever wanted the rail does
// not change - only the asset and the deck panels do. See doc/atmo_magic_env_ui.md.
void SSFloaterAtmoEnv::onClickAddDeck()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
    if (asset.mTracks[mSelectedTrackIndex].mUnderField.mEnabled) return;

    asset.mTracks[mSelectedTrackIndex].mUnderField.mEnabled = true;
    selectLayer(LAYER_UNDER);
    refreshAutoRows();
    refreshStatus();
    refreshPreview();
}

void SSFloaterAtmoEnv::onClickRemoveDeck()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];
    if (!track.mUnderField.mEnabled) return;

    track.mUnderField.mEnabled = false;
    // An override naming the deck that just went away would otherwise leave weather sourceless.
    if (track.mWeatherSourceDeck == SS_ATMOENV_DECK_UNDER)
    {
        track.mWeatherSourceDeck = SS_ATMOENV_DECK_DERIVED;
    }
    if (mSelectedLayer == LAYER_UNDER)
    {
        selectLayer(LAYER_MAIN);
    }
    refreshWeatherSource();
    refreshAutoRows();
    refreshRailMode();
    refreshStatus();
    refreshPreview();
}

void SSFloaterAtmoEnv::onCommitWeatherSource()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mWeatherSourceDeck =
        getChild<LLComboBox>("weather_source_combo")->getSelectedValue().asInteger();

    refreshRailMode();
    refreshStatus();
    refreshPreview();
}

// <SS:Nexii> Rebuilds the type combo from both tiers. The shipped vocabulary is captured from the
// XUI on the first pass rather than duplicated in code, so the panel stays the one place the
// derivation names are written down; the environment's own types are appended after a separator.
void SSFloaterAtmoEnv::refreshPrecipitationTypes()
{
    LLComboBox* combo = getChild<LLComboBox>("precipitation_combo");

    if (mBuiltinPrecipItems.empty())
    {
        // LLComboBox exposes its rows only through the selection, so walk it once and put the
        // selection back. Once is all it takes - the shipped vocabulary is fixed at build time.
        const S32 was = combo->getCurrentIndex();
        for (S32 i = 0; i < combo->getItemCount(); ++i)
        {
            if (!combo->setCurrentByIndex(i)) continue;
            mBuiltinPrecipItems.emplace_back(combo->getSelectedItemLabel(),
                                             combo->getSelectedValue().asString());
        }
        combo->setCurrentByIndex(llmax(0, was));
    }

    const std::string selected = combo->getSelectedValue().asString();

    combo->removeall();
    for (const auto& entry : mBuiltinPrecipItems)
    {
        combo->add(entry.first, LLSD(entry.second));
    }

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (mgr->hasAsset())
    {
        for (const auto& entry : mgr->asset().mPrecipitationTypes)
        {
            // Marked so an author can tell at a glance which types travel with this environment.
            combo->add(entry.first + "  (this environment)", LLSD(entry.first));
        }
    }

    if (!combo->setSelectedByValue(LLSD(selected), true))
    {
        combo->setSelectedByValue(LLSD(std::string()), true);
    }
}

// Derives a new environment type from whatever is selected and opens it for editing.
void SSFloaterAtmoEnv::onClickNewPrecipType()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with(
            "MESSAGE", "Load or create an Atmo environment first - custom precipitation types are "
                       "saved into it."));
        return;
    }

    const std::string selected = getChild<LLComboBox>("precipitation_combo")
        ->getSelectedValue().asString();

    // Auto has no definition to derive from, so a blank selection starts from the shipped rain.
    const std::string parent_type = selected.empty() ? std::string("rain") : selected;
    const std::string parent_name = SSAtmoEnvBridge::presetNameForType(parent_type);

    const SSPrecipPreset* parent = SSPrecipPresetManager::instance().find(parent_name);
    if (!parent)
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with(
            "MESSAGE", "That precipitation type has no definition to copy."));
        return;
    }

    SSPrecipPreset derived = *parent;
    derived.mBuiltIn = false;
    derived.mFromEnvironment = true;

    std::map<std::string, LLSD>& types = mgr->editable().mPrecipitationTypes;
    std::string name = parent->mName + " copy";
    for (S32 i = 2; types.count(name) && i < 1000; ++i)
    {
        name = parent->mName + " copy " + llformat("%d", i);
    }
    derived.mName = name;

    types[name] = derived.asLLSD();
    ssAtmoEnvStagePrecipTypes(mgr->asset());

    refreshPrecipitationTypes();
    refreshStatus();

    LLFloaterReg::showInstance("ss_atmo_preset",
        LLSD().with("scope", "environment").with("name", name));
}

void SSFloaterAtmoEnv::onClickEditPrecipTypes()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset() || mgr->asset().mPrecipitationTypes.empty())
    {
        LLNotificationsUtil::add("GenericAlert", LLSD().with(
            "MESSAGE", "This environment has no precipitation types of its own yet. Use "
                       "\"New from this...\" to derive one from a shipped type."));
        return;
    }

    const std::string selected = getChild<LLComboBox>("precipitation_combo")
        ->getSelectedValue().asString();
    const std::string want = mgr->asset().mPrecipitationTypes.count(selected)
        ? selected : mgr->asset().mPrecipitationTypes.begin()->first;

    LLFloaterReg::showInstance("ss_atmo_preset",
        LLSD().with("scope", "environment").with("name", want));
}

void SSFloaterAtmoEnv::refreshWeatherSource()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    LLComboBox* combo = getChild<LLComboBox>("weather_source_combo");
    combo->clearRows();
    combo->add("Derived", LLSD(SS_ATMOENV_DECK_DERIVED));
    combo->add("Main Deck", LLSD(SS_ATMOENV_DECK_MAIN));
    if (track.mUnderField.mEnabled)
    {
        combo->add("Under Deck", LLSD(SS_ATMOENV_DECK_UNDER));
    }
    if (!combo->setSelectedByValue(LLSD(track.mWeatherSourceDeck), true))
    {
        combo->setSelectedByValue(LLSD(SS_ATMOENV_DECK_DERIVED), true);
    }
}

// Centres a rail widget on a pixel.
void SSFloaterAtmoEnv::centreViewOn(LLView* view, S32 centre_y)
{
    LLRect rect = view->getRect();
    const S32 height = rect.getHeight();
    rect.mBottom = centre_y - (height / 2);
    rect.mTop = rect.mBottom + height;
    view->setRect(rect);
}

// One rail row's altitude label.
void SSFloaterAtmoEnv::refreshAltitudeLabel(S32 slot)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    LLButton*  name_button = getChild<LLButton>(llformat("track_name_button_%d", slot));
    LLTextBox* alt_label   = getChild<LLTextBox>(llformat("track_alt_label_%d", slot));

    const F32 value = slider->getSliderValue(llformat("track%d", slot));

    const S32 centre = railCentreForValue(value);
    centreViewOn(name_button, centre);
    centreViewOn(alt_label, centre);

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (slot < (S32)asset.mTracks.size())
    {
        name_button->setLabel(asset.mTracks[slot].mName);
        name_button->setToggleState(slot == mSelectedTrackIndex);
    }
    alt_label->setText(llformat("%.0fm", value));
}

// Live drag of a track floor: preview the altitude while dragging.
void SSFloaterAtmoEnv::onCommitAltitudeSlider()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    LLMultiSliderCtrl* slider = getChild<LLMultiSliderCtrl>("track_altitude_slider");
    const std::string cur = slider->getCurSlider();

    // Layer mode drags a placed layer, not a track floor. Writing straight to the field's plain
    // value is deliberate: a deck's altitude can be keyframed, but dragging it on the rail is a
    // structural placement, so it sets the value the track holds rather than stamping a keyframe.
    if (mRailMode == ERailMode::LAYER)
    {
        if (cur.rfind("layer", 0) != 0) return;

        const S32 layer = atoi(cur.c_str() + 5);
        SSAtmoEnvAsset& layer_asset = mgr->editable();
        if (mSelectedTrackIndex >= (S32)layer_asset.mTracks.size()) return;
        SSAtmoEnvTrack& track = layer_asset.mTracks[mSelectedTrackIndex];

        const F32 value = slider->getCurSliderValue();
        switch (layer)
        {
        case LAYER_WATER:
            track.mWater.mHeight.setValueAtHead(mPreviewPhase,
                llclamp(value, SS_ATMOENV_WATER_MIN, SS_ATMOENV_WATER_MAX));
            break;
        case LAYER_MAIN:
            track.mCloudField.mAuto = false;
            track.mCloudField.mBaseHeightM.setValueAtHead(mPreviewPhase, value);
            break;
        case LAYER_UNDER:
            track.mUnderField.mAuto = false;
            track.mUnderField.mBaseHeightM.setValueAtHead(mPreviewPhase, value);
            break;
        default:
            return;
        }

        if (layer != mSelectedLayer)
        {
            selectLayer(layer);
        }
        refreshLayerRail();
        refreshAutoRows();
        refreshStatus();
        refreshPreview();
        return;
    }

    if (cur.size() <= 5) return;

    const S32 slot = atoi(cur.c_str() + 5);
    SSAtmoEnvAsset& asset = mgr->editable();
    if (slot < 1 || slot >= (S32)asset.mTracks.size()) return;

    F32 value = slider->getCurSliderValue();
    if (value < SS_ATMOENV_MIN_TRACK_FLOOR)
    {
        value = SS_ATMOENV_MIN_TRACK_FLOOR;
        slider->setCurSliderValue(value);
    }
    asset.mTracks[slot].mFloorZ = value;

    if (slot != mSelectedTrackIndex)
    {
        selectTrack(slot);
    }

    refreshAltitudeLabel(slot);
    refreshTrackTab();
    refreshStatus();
}

// Drag released: commit the floor, enforce spacing, re-sort tracks.
void SSFloaterAtmoEnv::onMouseUpAltitudeSlider()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    if (mRailMode == ERailMode::LAYER)
    {
        // Re-fit only now the drag is over, so the axis cannot move under the cursor.
        F32 want_min = 0.f;
        F32 want_max = SS_ATMOENV_REGION_CEILING;
        railRangeForTrack(want_min, want_max);
        mRailMin = mRailMinTo = want_min;
        mRailMax = mRailMaxTo = want_max;
        refreshLayerRail();
        refreshStatus();
        return;
    }

    mSelectedTrackIndex = mgr->editable().sortTracksByAltitude(mSelectedTrackIndex);
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();

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

// Opens the influence sub-floater on the edited track.
void SSFloaterAtmoEnv::onClickWeatherInfluence()
{
    LLFloaterReg::showInstance("ss_atmo_influence", LLSD(mSelectedTrackIndex));
}

// Selects the ground track.
void SSFloaterAtmoEnv::onClickGroundRow()
{
    selectTrack(0);
}

// Switches the edited track and refreshes every tab.
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

    SSFloaterAtmoPlanetary* designer =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoPlanetary>("ss_atmo_planetary");
    if (designer)
    {
        designer->setTrack(index);
    }

    SSFloaterAtmoInfluence* influence =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoInfluence>("ss_atmo_influence");
    if (influence)
    {
        influence->setTrack(index);
    }
}

// The header status line: source, modified state.
void SSFloaterAtmoEnv::refreshStatus()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const bool modified = mgr->isModified();

    setTitle(std::string("Edit Atmo Magic Environment") + (modified ? " - Unsaved changes*" : ""));

    getChild<LLUICtrl>("revert_button")->setEnabled(modified);
    getChild<LLUICtrl>("save_button")->setEnabled(mgr->hasAsset());
}

// Opens the create chooser: empty defaults, the stock day cycle, a list of skies, or an EEP day
// cycle. What "new" means is a choice, so the button no longer seeds immediately.
void SSFloaterAtmoEnv::onClickCreateNew()
{
    SSFloaterAtmoEnvCreate::show();
}

// Inventory picker for an environment notecard.
void SSFloaterAtmoEnv::onClickLoad()
{
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

// Saves to the loaded notecard (or a new one).
void SSFloaterAtmoEnv::onClickSave()
{
    std::string name = getChild<LLLineEditor>("name_editor")->getText();
    SSAtmoEnvManager::getInstance()->saveNotecard(name);
    refreshStatus();
}

// Unloads the environment and closes down to the landing state.
void SSFloaterAtmoEnv::onClickUnload()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->unload();

    mSelectedTrackIndex = 0;
    refreshVisibility();
    refreshPrecipitationTypes();
    refreshTrackRail();
    refreshTrackTab();
    refreshStatus();
}

// Back to the load-time baseline.
void SSFloaterAtmoEnv::onClickRevert()
{
    SSAtmoEnvManager::getInstance()->revertToBaseline();
    mSelectedTrackIndex = 0;
    // Reverting restores the baseline's precipitation types too, so they have to be restaged and
    // relisted - otherwise a type added since the load would linger in the combo and in the
    // resolver's live set.
    ssAtmoEnvStagePrecipTypes(SSAtmoEnvManager::getInstance()->asset());
    refreshPrecipitationTypes();
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

// Adds an altitude track and selects it.
void SSFloaterAtmoEnv::onClickAddTrack()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (!asset.addTrack()) return;

    mSelectedTrackIndex = (S32)asset.mTracks.size() - 1;
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

// Removes the selected track.
void SSFloaterAtmoEnv::onClickRemoveTrack()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex <= 0) return;

    mgr->editable().removeTrack(mSelectedTrackIndex);
    mSelectedTrackIndex = 0;
    refreshTrackRail();
    refreshTrackTab();
    refreshPlanetaryScales();
    refreshStatus();
    refreshPreview();
}

// <SS:Nexii> Seeds the selected track from a world archetype. Confirmed first because it overwrites
// the track wholesale, keyframes included - there is no partial apply and no undo beyond Revert.
void SSFloaterAtmoEnv::onClickApplyTemplate()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    LLComboBox* combo = getChild<LLComboBox>("track_template_combo");
    const std::string key = combo->getSelectedValue().asString();
    if (key.empty()) return;

    LLSD args;
    args["MESSAGE"] = "Seed \"" + combo->getSelectedItemLabel() + "\" onto this track? Its water, "
                      "cloud decks, sky and weather are replaced, keyframes included.";
    // The floater can be closed while the confirmation is up, so the callback goes through a
    // handle rather than capturing this - same pattern as the dropped-settings load above.
    LLHandle<LLFloater> handle = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(),
        [handle, key](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;

            SSFloaterAtmoEnv* self = (SSFloaterAtmoEnv*)handle.get();
            if (!self) return;

            SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
            if (!mgr->hasAsset()) return;

            SSAtmoEnvAsset& asset = mgr->editable();
            if (self->mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
            if (!ssAtmoEnvApplyTemplate(asset.mTracks[self->mSelectedTrackIndex], key)) return;

            self->refreshTrackRail();
            self->refreshTrackTab();
            self->refreshWaterRows();
            self->refreshAutoRows();
            self->refreshLightningRows();
            self->refreshPlanetaryScales();
            self->refreshStatus();
            self->refreshPreview();
        });
}

// Asset name field into the asset.
void SSFloaterAtmoEnv::onCommitName()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->editable().mName = getChild<LLLineEditor>("name_editor")->getText();
    refreshStatus();
}

// Rewrites the Track tab from the selected track.
void SSFloaterAtmoEnv::refreshTrackTab()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->asset();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;
    const SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

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

    getChild<LLUICtrl>("water_enabled_check")->setValue(track.mWater.mEnabled);
    getChild<LLUICtrl>("ucloud_enabled_check")->setValue(track.mUnderField.mEnabled);
    getChild<LLUICtrl>("ucloud_auto_check")->setValue(track.mUnderField.mAuto);

    getChild<LLUICtrl>("gust_auto_check")->setValue(track.mWeather.mGustAuto);
    getChild<LLUICtrl>("lightning_enabled_check")->setValue(track.mWeather.mLightningEnabled);
    getChild<LLUICtrl>("lightning_charge_check")->setValue(track.mWeather.mLightningCharge);
    getChild<LLUICtrl>("lightning_sparks_check")->setValue(track.mWeather.mLightningSparks);
    getChild<LLUICtrl>("lightning_auto_check")->setValue(track.mWeather.mLightningAuto);
    getChild<LLUICtrl>("cloud_auto_check")->setValue(track.mCloudField.mAuto);
    getChild<LLUICtrl>("dome_auto_check")->setValue(track.mCloudDome.mAuto);
    getChild<LLUICtrl>("atmo_horizon_clip_check")->setValue(track.mAtmosphere.mHorizonClip);
    refreshAutoRows();
    refreshLightningRows();
    refreshWaterRows();
}

// Track name field into the track.
void SSFloaterAtmoEnv::onCommitTrackName()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    std::string name = getChild<LLLineEditor>("track_name_editor")->getText();
    LLStringUtil::trim(name);
    if (name.empty()) name = asset.nextDefaultTrackName();

    asset.mTracks[mSelectedTrackIndex].mName = name;

    refreshTrackRail();
    refreshStatus();
}

// Day cycle length/offset into the track.
void SSFloaterAtmoEnv::onCommitDayCycle()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    SSAtmoEnvTrack& track = asset.mTracks[mSelectedTrackIndex];

    const bool from_spinner =
        getChild<LLUICtrl>("day_length_value_spinner")->hasFocus() ||
        getChild<LLUICtrl>("day_offset_value_spinner")->hasFocus();
    const char* length_src = from_spinner ? "day_length_value_spinner" : "day_length_slider";
    const char* offset_src = from_spinner ? "day_offset_value_spinner" : "day_offset_slider";

    track.mDayLengthSeconds = getChild<LLUICtrl>(length_src)->getValue().asReal() * 3600.0;
    track.mDayOffsetSeconds = getChild<LLUICtrl>(offset_src)->getValue().asReal() * 3600.0;

    refreshTrackTab();
    refreshStatus();
}

// Water plane toggle into the track.
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

// Under layer toggle into the track.
void SSFloaterAtmoEnv::onCommitUnderEnabled()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mUnderField.mEnabled =
        getChild<LLUICtrl>("ucloud_enabled_check")->getValue().asBoolean();
    refreshWaterRows();
    refreshStatus();
}

// Under layer auto toggle into the track.
void SSFloaterAtmoEnv::onCommitUnderAuto()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mUnderField.mAuto =
        getChild<LLUICtrl>("ucloud_auto_check")->getValue().asBoolean();
    refreshAutoRows();
    refreshWaterRows();
    refreshStatus();
}

// Gust auto toggle; manual rows enable accordingly.
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

// Lightning toggles into the weather block.
void SSFloaterAtmoEnv::onCommitLightningFlags()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    SSAtmoEnvWeather& weather = asset.mTracks[mSelectedTrackIndex].mWeather;
    weather.mLightningEnabled =
        getChild<LLUICtrl>("lightning_enabled_check")->getValue().asBoolean();
    weather.mLightningCharge =
        getChild<LLUICtrl>("lightning_charge_check")->getValue().asBoolean();
    weather.mLightningSparks =
        getChild<LLUICtrl>("lightning_sparks_check")->getValue().asBoolean();

    refreshLightningRows();
    refreshStatus();
}

// Enables the lightning rows per the auto flag.
void SSFloaterAtmoEnv::refreshLightningRows()
{
    const bool on = getChild<LLUICtrl>("lightning_enabled_check")->getValue().asBoolean();
    getChild<LLUICtrl>("lightning_charge_check")->setEnabled(on);
    getChild<LLUICtrl>("lightning_sparks_check")->setEnabled(on);
}

// Lightning auto toggle.
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

// Dome altitude auto toggle; the height row enables accordingly.
void SSFloaterAtmoEnv::onCommitDomeAuto()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mCloudDome.mAuto =
        getChild<LLUICtrl>("dome_auto_check")->getValue().asBoolean();
    refreshAutoRows();
    refreshStatus();
}

// Horizon clip toggle; the render side reads it straight off the applier.
void SSFloaterAtmoEnv::onCommitHorizonClip()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    asset.mTracks[mSelectedTrackIndex].mAtmosphere.mHorizonClip =
        getChild<LLUICtrl>("atmo_horizon_clip_check")->getValue().asBoolean();
    refreshStatus();
}

// Cloud field auto toggle; authored rows enable accordingly.
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
    std::string precipDisplayName(const std::string& value);
}

// Enables every auto-owned row set per its flag.
void SSFloaterAtmoEnv::refreshAutoRows()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return;

    const SSAtmoEnvTrack& track = mgr->asset().mTracks[mSelectedTrackIndex];
    const SSAtmoEnvWeather& weather = track.mWeather;

    const SSAtmoEnvWeatherState resolved = SSAtmoEnvWeatherResolver::resolve(weather, mPreviewPhase);

    F32 auto_height = 0.f, auto_thickness = 0.f, auto_coverage = 0.f, auto_dark = 0.f;
    SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(
        weather.mMoisture.valueAt(mPreviewPhase),
        weather.mConvection.valueAt(mPreviewPhase),
        auto_height, auto_thickness, auto_coverage, auto_dark);

    const struct { const char* mPrefix; bool mAuto; F32 mComputed; } rows[] = {
        { "gust_depth",          weather.mGustAuto,       resolved.mGustDepth },
        { "gust_length",         weather.mGustAuto,       resolved.mGustLength },
        { "gust_veer",           weather.mGustAuto,       resolved.mGustVeer },
        { "lightning_intensity", weather.mLightningAuto,  resolved.mLightningIntensity },
        { "cloud_base_height",   track.mCloudField.mAuto, auto_height },
        { "cloud_thickness",     track.mCloudField.mAuto, auto_thickness },
        { "cloud_coverage",      track.mCloudField.mAuto, auto_coverage },
        { "cloud_storm_dark",    track.mCloudField.mAuto, auto_dark },
        { "ucloud_base_height",  track.mUnderField.mAuto, auto_height },
        { "ucloud_thickness",    track.mUnderField.mAuto, auto_thickness },
        { "ucloud_coverage",     track.mUnderField.mAuto, auto_coverage },
        { "ucloud_storm_dark",   track.mUnderField.mAuto, auto_dark },
        { "dome_height",         track.mCloudDome.mAuto,  SSAtmoEnvApplier::instance().cirrusAltitudeMetres() },
    };
    for (const auto& row : rows)
    {
        const std::string prefix(row.mPrefix);
        getChild<LLUICtrl>(prefix + "_slider")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_value_spinner")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_keyframe_button")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_prev_button")->setEnabled(!row.mAuto);
        getChild<LLUICtrl>(prefix + "_next_button")->setEnabled(!row.mAuto);

        if (row.mAuto)
        {
            getChild<LLUICtrl>(prefix + "_slider")->setValue(row.mComputed);
            getChild<LLUICtrl>(prefix + "_value_spinner")->setValue(row.mComputed);
        }
    }

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

// Whether the water rows are moot (no water plane).
bool SSFloaterAtmoEnv::waterRowsInactive() const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return false;

    return !mgr->asset().mTracks[mSelectedTrackIndex].mWater.mEnabled;
}

// Rewrites the Water tab rows, and gates the Under Layer tab's rows on its enable flag (the
// auto derivation's own grey-out from refreshAutoRows composes with this: a row is live only
// when its field is on and not auto-owned).
void SSFloaterAtmoEnv::refreshWaterRows()
{
    const bool enabled = !waterRowsInactive();

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const bool under_enabled = mgr->hasAsset()
        && mSelectedTrackIndex < (S32)mgr->asset().mTracks.size()
        && mgr->asset().mTracks[mSelectedTrackIndex].mUnderField.mEnabled;

    auto setRow = [this](const std::string& prefix, LLUICtrl* primary, bool on)
    {
        if (primary) primary->setEnabled(on);

        static const char* const CLUSTER[] = {
            "_keyframe_button", "_prev_button", "_next_button"
        };
        for (const char* suffix : CLUSTER)
        {
            LLUICtrl* part = findChild<LLUICtrl>(prefix + suffix);
            if (part) part->setEnabled(on);
        }
    };

    auto isWater = [](const std::string& prefix)
    {
        return prefix.compare(0, 6, "water_") == 0;
    };

    auto isUnder = [](const std::string& prefix)
    {
        return prefix.compare(0, 7, "ucloud_") == 0;
    };

    auto rowEnabled = [&](const std::string& prefix)
    {
        if (isWater(prefix)) return enabled;
        if (isUnder(prefix)) return under_enabled && !rowAutoOwned(prefix);
        return true;
    };

    for (const FloatRow& row : mFloatRows)
    {
        if (!isWater(row.mPrefix) && !isUnder(row.mPrefix)) continue;
        const bool on = rowEnabled(row.mPrefix);
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix + "_slider"), on);
        LLUICtrl* spinner = findChild<LLUICtrl>(row.mPrefix + "_value_spinner");
        if (spinner) spinner->setEnabled(on);
    }
    for (const KeyRow<LLColor3>& row : mColorRows)
    {
        if (!isWater(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix), enabled);
    }
    for (const KeyRow<LLVector2>& row : mVectorRows)
    {
        if (!isWater(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix), enabled);
        for (const char* suffix : { "_x_spinner", "_y_spinner" })
        {
            LLUICtrl* part = findChild<LLUICtrl>(row.mPrefix + suffix);
            if (part) part->setEnabled(enabled);
        }
    }
    for (const KeyRow<LLUUID>& row : mTextureRows)
    {
        if (!isWater(row.mPrefix) && !isUnder(row.mPrefix)) continue;
        setRow(row.mPrefix, findChild<LLUICtrl>(row.mPrefix), rowEnabled(row.mPrefix));
    }
}

// Whether a row is currently owned by an auto derivation.
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
    if (prefix == "ucloud_base_height" || prefix == "ucloud_thickness"
        || prefix == "ucloud_coverage" || prefix == "ucloud_storm_dark")
    {
        return track.mUnderField.mAuto;
    }
    if (prefix == "dome_height")
    {
        return track.mCloudDome.mAuto;
    }
    if (prefix.compare(0, 6, "water_") == 0)
    {
        return !track.mWater.mEnabled;
    }
    return false;
}

// Rewrites the planetary scale spinners.
void SSFloaterAtmoEnv::refreshPlanetaryScales()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex >= (S32)mgr->asset().mTracks.size()) return;

    const SSAtmoEnvPlanetary& planetary = mgr->asset().mTracks[mSelectedTrackIndex].mPlanetary;

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

// Planetary scale spinners into the asset.
void SSFloaterAtmoEnv::onCommitPlanetaryScales()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    SSAtmoEnvPlanetary& planetary = asset.mTracks[mSelectedTrackIndex].mPlanetary;

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

// Opens the planetary designer on the edited track.
void SSFloaterAtmoEnv::onClickOpenPlanetaryDesigner()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    LLFloaterReg::showInstance("ss_atmo_planetary", LLSD(mSelectedTrackIndex));
}

namespace
{
    // Phase as a clock-time label.
    std::string formatApparentTime(F64 phase)
    {
        F64 frac = (phase >= 1.0) ? 1.0 : std::fmod(phase, 1.0);
        if (frac < 0.0) frac += 1.0;

        static const F64 SECONDS_IN_DAY = 24.0 * 60.0 * 60.0;
        const S32 second_of_day = (S32)(frac * SECONDS_IN_DAY);
        S32 hour = second_of_day / 3600;
        const S32 minute = (second_of_day % 3600) / 60;

        std::string ap;
        if (!gSavedSettings.getBOOL("Use24HourClock"))
        {
            ap = (hour >= 12 && hour < 24) ? "PM" : "AM";
            if (hour == 0 || hour == 24) hour = 12;
            else if (hour > 12)          hour -= 12;
        }

        return llformat("%d:%02d%s (%d%%)", hour, minute, ap.c_str(), (S32)(frac * 100.0));
    }

    // Precipitation type key to display name.
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

template <typename T>
// One row's keyframe buttons: dot state, prev/next enables.
void SSFloaterAtmoEnv::refreshKeyframeControls(const std::string& prefix,
                                               const SSAtmoEnvKeyframed<T>& field, F64 phase)
{
    const bool on_keyframe = field.hasKeyframeAt(phase);
    LLButton* kf_button = getChild<LLButton>(prefix + "_keyframe_button");
    kf_button->setToggleState(on_keyframe);
    kf_button->setLabel(std::string(on_keyframe ? FILLED_DIAMOND : HOLLOW_DIAMOND));

    const bool can_jump = field.keyframeCount() > 1;
    getChild<LLUICtrl>(prefix + "_prev_button")->setEnabled(can_jump);
    getChild<LLUICtrl>(prefix + "_next_button")->setEnabled(can_jump);
}

// The keyframe dot: add/edit at the head, or remove the one under it.
template <typename T>
void SSFloaterAtmoEnv::toggleKeyframe(SSAtmoEnvKeyframed<T>& field)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    (void)mgr;
    field.toggleKeyframeAtHead(mPreviewPhase);
}

// Scrub the head to the neighbouring keyframe.
template <typename T>
void SSFloaterAtmoEnv::jumpKeyframe(const SSAtmoEnvKeyframed<T>& field, bool next)
{
    mPreviewPhase = next ? field.nextKeyframeTime(mPreviewPhase)
                        : field.prevKeyframeTime(mPreviewPhase);
}

template <typename T>
// Wires one row's keyframe buttons to a field.
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

// Whether a colour picker is up - committing under one fights the swatch.
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

// Colour row out of the field at the phase.
void SSFloaterAtmoEnv::refreshColorRow(const KeyRow<LLColor3>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<LLColor3>& field = row.mField();

    const F32 inv = (row.mScale > 0.f) ? (1.f / row.mScale) : 1.f;
    const LLColor3 value = field.valueAt(phase) * inv;

    if (!ss_color_picker_open())
    {
        getChild<LLColorSwatchCtrl>(row.mPrefix)->setValue(
            LLColor4(value.mV[0], value.mV[1], value.mV[2], 1.f).getValue());
    }

    refreshKeyframeControls<LLColor3>(row.mPrefix, field, phase);
}

// Colour row into the field.
void SSFloaterAtmoEnv::commitColorRow(const KeyRow<LLColor3>& row)
{
    const LLSD sd = getChild<LLColorSwatchCtrl>(row.mPrefix)->getValue();
    if (!sd.isArray() || sd.size() < 3) return;

    row.mField().setValueAtHead(mPreviewPhase,
        LLColor3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal()) * row.mScale);
}

// Vector row out of the field at the phase.
void SSFloaterAtmoEnv::refreshVectorRow(const KeyRow<LLVector2>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<LLVector2>& field = row.mField();
    const LLVector2 value = field.valueAt(phase);

    LLSD sd = LLSD::emptyArray();
    sd.append((LLSD::Real)value.mV[0]);
    sd.append((LLSD::Real)value.mV[1]);
    getChild<LLUICtrl>(row.mPrefix)->setValue(sd);

    LLSpinCtrl* x_spin = getChild<LLSpinCtrl>(row.mPrefix + "_x_spinner");
    LLSpinCtrl* y_spin = getChild<LLSpinCtrl>(row.mPrefix + "_y_spinner");
    if (!x_spin->hasFocus()) x_spin->setValue(value.mV[0]);
    if (!y_spin->hasFocus()) y_spin->setValue(value.mV[1]);

    refreshKeyframeControls<LLVector2>(row.mPrefix, field, phase);
}

// Vector row into the field.
void SSFloaterAtmoEnv::commitVectorRow(const KeyRow<LLVector2>& row)
{
    const LLSD sd = getChild<LLUICtrl>(row.mPrefix)->getValue();
    if (!sd.isArray() || sd.size() < 2) return;

    row.mField().setValueAtHead(mPreviewPhase,
        LLVector2((F32)sd[0].asReal(), (F32)sd[1].asReal()));
}

// Vector spinner pair into the field.
void SSFloaterAtmoEnv::commitVectorSpinners(const KeyRow<LLVector2>& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        LLVector2((F32)getChild<LLUICtrl>(row.mPrefix + "_x_spinner")->getValue().asReal(),
                  (F32)getChild<LLUICtrl>(row.mPrefix + "_y_spinner")->getValue().asReal()));
}

// Texture row out of the field at the phase.
void SSFloaterAtmoEnv::refreshTextureRow(const KeyRow<LLUUID>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<LLUUID>& field = row.mField();
    getChild<LLTextureCtrl>(row.mPrefix)->setValue(field.valueAt(phase));
    refreshKeyframeControls<LLUUID>(row.mPrefix, field, phase);
}

// Texture row into the field.
void SSFloaterAtmoEnv::commitTextureRow(const KeyRow<LLUUID>& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        getChild<LLTextureCtrl>(row.mPrefix)->getValue().asUUID());
}

// String row out of the field at the phase.
void SSFloaterAtmoEnv::refreshStringRow(const KeyRow<std::string>& row, F64 phase)
{
    const SSAtmoEnvKeyframed<std::string>& field = row.mField();

    LLComboBox* combo = getChild<LLComboBox>(row.mPrefix);
    if (!combo->setSelectedByValue(field.valueAt(phase), true))
    {
        combo->setSelectedByValue(std::string(), true);
    }

    refreshKeyframeControls<std::string>(row.mPrefix, field, phase);
}

// String row into the field.
void SSFloaterAtmoEnv::commitStringRow(const KeyRow<std::string>& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        getChild<LLComboBox>(row.mPrefix)->getSelectedValue().asString());
}

// Float row (slider + spinner) out of the field at the phase.
void SSFloaterAtmoEnv::refreshFloatRow(const FloatRow& row, F64 phase)
{
    const SSAtmoEnvKeyframed<F32>& field = row.mField();

    const F32 inv = (row.mScale > 0.f) ? (1.f / row.mScale) : 1.f;
    const F32 value = field.valueAt(phase) * inv;

    getChild<LLUICtrl>(row.mPrefix + "_slider")->setValue(value);

    LLSpinCtrl* spinner = getChild<LLSpinCtrl>(row.mPrefix + "_value_spinner");
    if (!spinner->hasFocus())
    {
        spinner->setValue(value);
    }

    refreshKeyframeControls<F32>(row.mPrefix, field, phase);
}

// Float spinner into the field.
void SSFloaterAtmoEnv::commitFloatRowSpinner(const FloatRow& row)
{
    row.mField().setValueAtHead(mPreviewPhase,
        (F32)getChild<LLUICtrl>(row.mPrefix + "_value_spinner")->getValue().asReal() * row.mScale);
}

// Float slider into the field.
void SSFloaterAtmoEnv::commitFloatRow(const FloatRow& row)
{
    const F32 value = (F32)getChild<LLUICtrl>(row.mPrefix + "_slider")->getValue().asReal() * row.mScale;

    row.mField().setValueAtHead(mPreviewPhase, value);
}

// Float row's keyframe dot.
void SSFloaterAtmoEnv::toggleFloatRowKeyframe(const FloatRow& row)
{
    toggleKeyframe<F32>(row.mField());
}

// Float row's previous-keyframe jump.
void SSFloaterAtmoEnv::jumpFloatRowPrev(const FloatRow& row)
{
    jumpKeyframe<F32>(row.mField(), false);
}

// Float row's next-keyframe jump.
void SSFloaterAtmoEnv::jumpFloatRowNext(const FloatRow& row)
{
    jumpKeyframe<F32>(row.mField(), true);
}

// Rewrites the scrubber and time label, and pushes the preview phase to the applier.
void SSFloaterAtmoEnv::refreshPreview()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->setPreviewPhaseOverride(mPreviewPhase);

    const F64 day_length = mgr->asset().mTracks[mSelectedTrackIndex].mDayLengthSeconds;

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

    const SSAtmoEnvWeatherState resolved = SSAtmoEnvWeatherResolver::resolve(
        mgr->editable().mTracks[mSelectedTrackIndex].mWeather, mPreviewPhase);
    getChild<LLTextBox>("forecast_text")->setText(resolved.mForecastText);

    refreshAutoRows();
    refreshWaterRows();
}

// Collects a field's keyframes as scrubber ghosts.
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
            ghost.mSpanStart = (i == 0) ? 0.0 : kf.mTime;
            ghost.mSpanEnd = (i == count - 1) ? 1.0 : keyframes[i + 1].mTime;
            ghost.mDrawPhase = 0.5 * (ghost.mSpanStart + ghost.mSpanEnd);
        }

        out.push_back(ghost);
    }
}

// Whether the mouse is over a row - its ghosts show while it is.
bool SSFloaterAtmoEnv::rowHovered(const std::string& prefix) const
{
    LLView* tabs = findChild<LLView>("atmo_tabs");
    LLView* button = findChild<LLView>(prefix + "_keyframe_button");
    if (!tabs || !button) return false;

    if (!button->isInVisibleChain()) return false;

    S32 mouse_x = 0, mouse_y = 0;
    LLUI::getInstance()->getMousePositionScreen(&mouse_x, &mouse_y);

    const LLRect tab_rect = tabs->calcScreenRect();
    if (mouse_x < tab_rect.mLeft || mouse_x > tab_rect.mRight) return false;

    const LLRect band = button->calcScreenRect();
    return mouse_y >= band.mBottom && mouse_y <= band.mTop;
}

// Whether the mouse is over the scrubber.
bool SSFloaterAtmoEnv::scrubberHovered() const
{
    LLView* scrubber = findChild<LLView>("preview_time_slider");
    if (!scrubber || !scrubber->getVisible()) return false;

    S32 mouse_x = 0, mouse_y = 0;
    LLUI::getInstance()->getMousePositionScreen(&mouse_x, &mouse_y);

    LLRect hover = scrubber->calcScreenRect();
    hover.stretch(HOVER_PAD_X, HOVER_PAD_Y);
    return hover.pointInRect(mouse_x, mouse_y);
}

// Every ghost the hover state asks to show.
bool SSFloaterAtmoEnv::collectHoveredKeyframes(std::vector<GhostKeyframe>& out, bool& out_labels) const
{
    out.clear();
    out_labels = true;

    const bool overview = scrubberHovered();
    if (overview) out_labels = false;

    auto wanted = [this, overview](const std::string& prefix)
    {
        if (rowAutoOwned(prefix)) return false;
        if (!overview) return rowHovered(prefix);
        LLView* probe = findChild<LLView>(prefix + "_keyframe_button");
        return probe && probe->isInVisibleChain();
    };

    bool found = false;

    for (const FloatRow& row : mFloatRows)
    {
        if (!wanted(row.mPrefix)) continue;
        const F32 ghost_inv = (row.mScale > 0.f) ? (1.f / row.mScale) : 1.f;
        buildGhosts<F32>(row.mField().keyframes(),
            [&row, ghost_inv](const F32& v) {
                const F32 shown = v * ghost_inv;
                return row.mIntegerDisplay ? llformat("%.0f", shown) : llformat("%.2f", shown);
            }, out);
        found = true;
        if (!overview) return true;
    }

    for (const KeyRow<LLColor3>& row : mColorRows)
    {
        if (!wanted(row.mPrefix)) continue;
        const F32 ghost_inv = (row.mScale > 0.f) ? (255.f / row.mScale) : 255.f;
        buildGhosts<LLColor3>(row.mField().keyframes(),
            [ghost_inv](const LLColor3& v) {
                return llformat("%d %d %d", ll_round(v.mV[0] * ghost_inv),
                                            ll_round(v.mV[1] * ghost_inv),
                                            ll_round(v.mV[2] * ghost_inv));
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

// The scrubber's pixel geometry, shared by markers and ghosts.
bool SSFloaterAtmoEnv::scrubberGeometry(LLRect& out_rect, S32& out_left_edge, S32& out_travel) const
{
    LLView* scrubber = findChild<LLView>("preview_time_slider");
    if (!scrubber || !scrubber->getVisible()) return false;

    out_rect = scrubber->getRect();

    LLPointer<LLUIImage> thumb = LLUI::getUIImage("SliderThumb_Off");
    const S32 thumb_width = thumb.notNull() ? thumb->getWidth() : 16;
    out_left_edge = out_rect.mLeft + (thumb_width / 2);
    out_travel = (out_rect.mRight - (thumb_width / 2)) - out_left_edge;
    return out_travel > 0;
}

// The weather bracket: precipitation occupies a span rather than a height, so it draws as an extent
// from the reference surface up to the delivering deck rather than as another thumb. Rail
// geometry is panel-local, and this runs after LLFloater::draw() in floater space, so the track
// panel's own origin has to be added back on.
void SSFloaterAtmoEnv::drawWeatherBracket()
{
    if (mRailMode != ERailMode::LAYER) return;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const S32 delivering = weatherDeliveringDeck();
    if (delivering == LAYER_NONE) return;

    LLView* panel = findChild<LLView>("track_panel");
    LLView* slider = findChild<LLView>("track_altitude_slider");
    if (!panel || !slider || !panel->getVisible()) return;

    const LLRect panel_rect = panel->getRect();
    const LLRect sld_rect = slider->getRect();

    const S32 surface_y = panel_rect.mBottom + railCentreForValue(weatherReferenceSurface());
    const S32 deck_y    = panel_rect.mBottom + railCentreForValue(layerAltitude(delivering));
    if (deck_y <= surface_y) return;

    const S32 centre_x = panel_rect.mLeft + sld_rect.getCenterX();

    static const LLColor4 BRACKET_COLOUR(0.55f, 0.72f, 0.95f, 0.55f);
    static const S32 STEM_HALF = 1;
    static const S32 CAP_HALF = 5;

    gl_rect_2d(centre_x - STEM_HALF, deck_y, centre_x + STEM_HALF, surface_y,
               BRACKET_COLOUR, true);
    gl_rect_2d(centre_x - CAP_HALF, deck_y + 1, centre_x + CAP_HALF, deck_y - 1,
               BRACKET_COLOUR, true);
    gl_rect_2d(centre_x - CAP_HALF, surface_y + 1, centre_x + CAP_HALF, surface_y - 1,
               BRACKET_COLOUR, true);
}

// Sun and moon rise/set/culmination markers on the scrubber, from the resolver's arc math.
void SSFloaterAtmoEnv::drawRiseSetMarkers()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->editable();
    if (mSelectedTrackIndex < 0 || mSelectedTrackIndex >= (S32)asset.mTracks.size()) return;

    const SSAtmoEnvPlanetary& planetary = asset.mTracks[mSelectedTrackIndex].mPlanetary;
    const S32 home = planetary.homeBodyIndex();
    if (home < 0) return;

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

    auto mark = [&](const SSAtmoEnvResolvedBody& body, const LLColor4& colour,
                    const char* rise_glyph, const char* set_glyph)
    {
        if (body.mBodyIndex < 0) return;

        F64 rise = 0.0;
        F64 set = 0.0;
        if (!SSAtmoEnvPlanetaryResolver::riseSetPhases(body.mDirection,
                home_body.mAxialTiltDeg, home_body.mLatitudeDeg, rise, set)) return;

        const S32 rise_x = left_edge + (S32)(llclamp(rise, 0.0, 1.0) * (F64)travel);
        const S32 set_x  = left_edge + (S32)(llclamp(set,  0.0, 1.0) * (F64)travel);

        font->renderUTF8(std::string(rise_glyph), 0, rise_x, glyph_y, colour,
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
        font->renderUTF8(std::string(set_glyph), 0, set_x, glyph_y, colour,
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
    };

    mark(moon, MOON_MARKER_COLOUR, RISE_TRIANGLE_SMALL, SET_TRIANGLE_SMALL);
    mark(sun, SUN_MARKER_COLOUR, RISE_TRIANGLE, SET_TRIANGLE);
}

// Draws the hovered row's keyframes on the scrubber.
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

    std::sort(ghosts.begin(), ghosts.end(),
              [](const GhostKeyframe& a, const GhostKeyframe& b) { return a.mDrawPhase < b.mDrawPhase; });

    const S32 LANE_GAP = 4;
    S32 lane_right[2] = { S32_MIN, S32_MIN };

    const S32 lane_y[2] = { rect.mTop + 2, rect.mBottom - 2 };
    const LLFontGL::VAlign lane_valign[2] = { LLFontGL::BOTTOM, LLFontGL::TOP };

    for (const GhostKeyframe& ghost : ghosts)
    {
        const F64 phase = llclamp(ghost.mDrawPhase, 0.0, 1.0);
        const S32 x = left_edge + (S32)(phase * (F64)travel);

        const bool current = ghost.mHold
            ? (mPreviewPhase >= ghost.mSpanStart && mPreviewPhase < ghost.mSpanEnd)
            : (llabs(phase - mPreviewPhase) < SSAtmoEnvKeyframed<F32>::PHASE_EPSILON);

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

// Draws a hovered row's keyframe VALUES as ghosts on its own slider.
void SSFloaterAtmoEnv::drawSliderValueGhosts()
{
    if (!SSAtmoEnvManager::getInstance()->hasAsset()) return;

    if (!scrubberHovered()) return;

    const LLRect floater_screen = calcScreenRect();

    LLPointer<LLUIImage> thumb_image = LLUI::getUIImage("SliderThumb_Off");
    const S32 thumb_width = thumb_image.notNull() ? thumb_image->getWidth() : 16;

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    for (const FloatRow& row : mFloatRows)
    {
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

        const S32 left_edge = left + (thumb_width / 2);
        const S32 travel = screen.getWidth() - thumb_width;
        if (travel <= 0) continue;

        const S32 y = bottom + (screen.getHeight() / 2);

        for (const SSAtmoEnvKeyframe<F32>& kf : field.keyframes())
        {
            const F32 t = llclamp((kf.mValue - lo) / range, 0.f, 1.f);
            const S32 x = left_edge + (S32)(t * (F32)travel);

            const bool current = llabs(kf.mTime - mPreviewPhase)
                                 < SSAtmoEnvKeyframed<F32>::PHASE_EPSILON;

            font->renderUTF8(std::string(current ? FILLED_DIAMOND : HOLLOW_DIAMOND), 0,
                             x, y,
                             current ? LLColor4::white : LLColor4(0.7f, 0.7f, 0.7f, 0.9f),
                             LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
    }
}

// Starts/stops preview playback.
void SSFloaterAtmoEnv::onClickPreviewPlay()
{
    mPreviewPlaying = !mPreviewPlaying;
    mPreviewPlayLast = LLTimer::getElapsedSeconds();

    LLButton* button = getChild<LLButton>("preview_play_button");
    button->setImageOverlay(mPreviewPlaying ? "Pause_Off" : "Play_Off");
    button->setToolTip(std::string(mPreviewPlaying
        ? "Stop the preview where it is"
        : "Run the preview through the cycle"));
}

// Advances the preview phase while playing.
void SSFloaterAtmoEnv::advancePreviewPlayback()
{
    if (!mPreviewPlaying) return;

    if (!SSAtmoEnvManager::getInstance()->hasAsset())
    {
        onClickPreviewPlay();
        return;
    }

    static const F64 PLAY_LAP_SECONDS = 60.0;

    const F64 now = LLTimer::getElapsedSeconds();
    const F64 elapsed = now - mPreviewPlayLast;
    mPreviewPlayLast = now;

    if (elapsed <= 0.0 || elapsed > 1.0) return;

    mPreviewPhase += elapsed / PLAY_LAP_SECONDS;
    while (mPreviewPhase >= 1.0) mPreviewPhase -= 1.0;

    refreshPreview();
}

// Scrubber commit into the preview phase.
void SSFloaterAtmoEnv::onCommitPreviewTime()
{
    if (mPreviewPlaying) onClickPreviewPlay();

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    mPreviewPhase = llclamp(
        getChild<LLUICtrl>("preview_time_slider")->getValue().asReal() * 0.01, 0.0, 1.0);
    refreshPreview();
}
