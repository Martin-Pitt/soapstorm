/**
 * @file ssfloateratmo.cpp
 * @brief Atmo Magic floater implementation.
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

#include "ssfloateratmo.h"
#include "ssatmotrack.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "ssprecippreset.h"

// <SS:Nexii> Atmo Magic floater

static const F64 STATUS_POLL_INTERVAL = 0.5;   // status/follow refresh cadence

SSFloaterAtmoMagic::SSFloaterAtmoMagic(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoMagic::postBuild()
{
    // The impact controls are plain global settings and bind through
    // control_name; everything per-track is driven from here, because it lives
    // in the track config rather than in gSavedSettings. The two map solvers
    // moved out to the simulation floater.
    getChild<LLButton>("reload_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickReload(); });
    getChild<LLButton>("load_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickLoad(); });
    getChild<LLButton>("save_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickSave(); });
    getChild<LLButton>("revert_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRevert(); });
    getChild<LLButton>("defaults_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickDefaults(); });

    getChild<LLButton>("fx_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_fx"); });
    getChild<LLButton>("assets_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_assets"); });
    getChild<LLButton>("audio_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_audio"); });
    getChild<LLButton>("sim_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_sim"); });
    getChild<LLButton>("edit_preset_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickEditPreset(); });

    getChild<LLUICtrl>("track_combo")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitTrackSelect(); });

    getChild<LLUICtrl>("track_follow_check")->setCommitCallback(
        [this](LLUICtrl* ctrl, const LLSD&)
        {
            mFollowCamera = ctrl->getValue().asBoolean();
            if (mFollowCamera) refreshTrackUI();
        });

    // Every per-track field routes through the same commit: read all the
    // widgets back into the config and persist once
    const char* fields[] = {
        "track_enabled_check", "precip_slider", "turbulence_slider",
        "wind_heading_slider", "wind_elevation_slider", "windspeed_slider",
        "preset_combo", "ground_check", "ground_spinner", "fallthrough_slider"
    };
    for (const char* name : fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitTrackField(); });
    }

    getChild<LLUICtrl>("track_follow_check")->setValue(mFollowCamera);

    refreshTrackList();
    refreshPresets();
    refreshTrackUI();
    refreshStatus();
    return true;
}

void SSFloaterAtmoMagic::onOpen(const LLSD& key)
{
    // Presets and region altitudes can both change while this is closed
    refreshTrackList();
    refreshPresets();
    refreshTrackUI();
    refreshStatus();
}

void SSFloaterAtmoMagic::draw()
{
    // Cheap poll rather than a callback: the track manager has no change
    // signal, and this only matters while the panel is visible
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;

        if (mFollowCamera)
        {
            const S32 camera_track = SSAtmoTrackMgr::getInstance()->currentTrack();
            if (camera_track != mTrack)
            {
                mTrack = camera_track;
                getChild<LLComboBox>("track_combo")->selectByValue(LLSD(mTrack));
                refreshTrackUI();
            }
        }

        refreshStatus();
    }

    LLFloater::draw();
}

//-----------------------------------------------------------------------------
// Drag and drop
//-----------------------------------------------------------------------------

bool SSFloaterAtmoMagic::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                           EDragAndDropType cargo_type, void* cargo_data,
                                           EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_NOTECARD)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    *accept = ACCEPT_YES_SINGLE;

    if (drop)
    {
        const LLInventoryItem* item = (const LLInventoryItem*)cargo_data;
        if (SSAtmoTrackMgr::getInstance()->importFromInventory(item))
        {
            // The fetch is async; the status line reports the outcome
            refreshTrackUI();
            refreshStatus();
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// Population
//-----------------------------------------------------------------------------

void SSFloaterAtmoMagic::refreshTrackList()
{
    SSAtmoTrackMgr* tracks = SSAtmoTrackMgr::getInstance();
    LLComboBox* combo = getChild<LLComboBox>("track_combo");

    combo->removeall();
    for (S32 track = SS_TRACK_MIN; track <= SS_TRACK_MAX; ++track)
    {
        const F32 floor_z = tracks->trackFloor(track);
        const F32 ceil_z = tracks->trackCeiling(track);

        std::string label;
        if (track == SS_TRACK_MIN)
        {
            label = llformat("Track %d - ground, to %.0fm", track, ceil_z);
        }
        else if (track >= SS_TRACK_MAX)
        {
            label = llformat("Track %d - %.0fm and above", track, floor_z);
        }
        else
        {
            label = llformat("Track %d - %.0f to %.0fm", track, floor_z, ceil_z);
        }
        combo->add(label, LLSD(track));
    }

    combo->selectByValue(LLSD(mTrack));
}

void SSFloaterAtmoMagic::refreshPresets()
{
    LLComboBox* combo = getChild<LLComboBox>("preset_combo");
    const std::string current = combo->getSelectedValue().asString();

    combo->removeall();
    // An empty entry means "whatever the preset editor has selected", which is
    // what a track config that names no preset falls back to
    combo->add("(editor default)", LLSD(std::string()));
    for (const SSPrecipPreset& preset : SSPrecipPresetMgr::instance().presets())
    {
        combo->add(preset.mName, LLSD(preset.mName));
    }
    combo->selectByValue(LLSD(current));
}

void SSFloaterAtmoMagic::refreshTrackUI()
{
    SSAtmoTrackMgr* tracks = SSAtmoTrackMgr::getInstance();
    const SSAtmoTrackConfig& cfg = tracks->config(mTrack);

    mUpdating = true;

    getChild<LLUICtrl>("track_enabled_check")->setValue(cfg.mDefined && cfg.mEnabled);
    getChild<LLUICtrl>("precip_slider")->setValue(cfg.mPrecipitation);
    getChild<LLUICtrl>("turbulence_slider")->setValue(cfg.mTurbulence);
    getChild<LLUICtrl>("wind_heading_slider")->setValue(cfg.heading());
    getChild<LLUICtrl>("wind_elevation_slider")->setValue(cfg.elevation());
    getChild<LLUICtrl>("windspeed_slider")->setValue(cfg.mWindSpeed);
    getChild<LLUICtrl>("ground_check")->setValue(cfg.mHasGround);
    getChild<LLUICtrl>("fallthrough_slider")->setValue(cfg.mFallThrough);

    // Ground zero defaults to the band's own base when it is not pinned
    getChild<LLUICtrl>("ground_spinner")->setValue(
        cfg.mHasGround ? cfg.mGround : tracks->trackFloor(mTrack));

    LLComboBox* preset_combo = getChild<LLComboBox>("preset_combo");
    if (!preset_combo->selectByValue(LLSD(cfg.mPreset)))
    {
        // Config names a preset that is not installed here; keep the name
        // visible rather than silently snapping to something else
        preset_combo->setLabel(cfg.mPreset.empty() ? "(editor default)" : cfg.mPreset);
    }

    // A track that is off has nothing worth editing below it
    const bool track_on = cfg.mDefined && cfg.mEnabled;
    const bool sky = tracks->isSkyTrack(mTrack);

    const char* weather_fields[] = {
        "precip_slider", "turbulence_slider", "wind_heading_slider",
        "wind_elevation_slider", "windspeed_slider", "preset_combo"
    };
    for (const char* name : weather_fields)
    {
        getChild<LLUICtrl>(name)->setEnabled(track_on);
    }

    // Ground zero only means anything in a sky band; at ground level the
    // terrain and water surface already are the floor
    getChild<LLUICtrl>("ground_check")->setEnabled(track_on && sky);
    getChild<LLUICtrl>("ground_spinner")->setEnabled(track_on && sky && cfg.mHasGround);
    getChild<LLUICtrl>("fallthrough_slider")->setEnabled(track_on && sky);

    mUpdating = false;
}

void SSFloaterAtmoMagic::refreshStatus()
{
    SSAtmoTrackMgr* tracks = SSAtmoTrackMgr::getInstance();
    const bool modified = tracks->isModified();

    // Same idiom as the environment floaters: an asterisk means the working
    // set has drifted from whatever was loaded, and Revert will undo it.
    std::string name = tracks->configName();
    if (name.empty()) name = "Untitled";

    setTitle("ATMO MAGIC - " + name + (modified ? " *" : ""));

    std::string status = "Source: " + tracks->statusText();
    if (modified) status += "   (modified)";
    getChild<LLTextBox>("track_status_text")->setText(status);

    getChild<LLUICtrl>("revert_button")->setEnabled(modified);

    // Keep the name field in step unless it is being typed into
    LLLineEditor* editor = getChild<LLLineEditor>("config_name_editor");
    if (!editor->hasFocus())
    {
        editor->setText(tracks->configName());
    }
}

//-----------------------------------------------------------------------------
// Commits
//-----------------------------------------------------------------------------

void SSFloaterAtmoMagic::onCommitTrackSelect()
{
    mTrack = getChild<LLComboBox>("track_combo")->getSelectedValue().asInteger();
    mTrack = llclamp(mTrack, SS_TRACK_MIN, SS_TRACK_MAX);

    // Picking a track by hand means you want to look at that one
    mFollowCamera = false;
    getChild<LLUICtrl>("track_follow_check")->setValue(false);

    refreshTrackUI();
}

void SSFloaterAtmoMagic::onCommitTrackField()
{
    if (mUpdating) return;

    SSAtmoTrackMgr* tracks = SSAtmoTrackMgr::getInstance();
    SSAtmoTrackConfig& cfg = tracks->editable(mTrack);

    const bool on = getChild<LLUICtrl>("track_enabled_check")->getValue().asBoolean();

    // Turning a track on is what defines it. Turning it off leaves the config
    // in place but idle, so the settings survive toggling.
    if (on) cfg.mDefined = true;
    cfg.mEnabled = on;

    cfg.mPrecipitation = (F32)getChild<LLUICtrl>("precip_slider")->getValue().asReal();
    cfg.mTurbulence    = (F32)getChild<LLUICtrl>("turbulence_slider")->getValue().asReal();
    cfg.mWindSpeed     = (F32)getChild<LLUICtrl>("windspeed_slider")->getValue().asReal();
    cfg.mPreset        = getChild<LLComboBox>("preset_combo")->getSelectedValue().asString();
    cfg.mHasGround     = getChild<LLUICtrl>("ground_check")->getValue().asBoolean();
    cfg.mGround        = (F32)getChild<LLUICtrl>("ground_spinner")->getValue().asReal();
    cfg.mFallThrough   = (F32)getChild<LLUICtrl>("fallthrough_slider")->getValue().asReal();

    cfg.setHeadingElevation(
        (F32)getChild<LLUICtrl>("wind_heading_slider")->getValue().asReal(),
        (F32)getChild<LLUICtrl>("wind_elevation_slider")->getValue().asReal());

    tracks->commit();

    // Enable states depend on the values just written
    refreshTrackUI();
    refreshStatus();
}

//-----------------------------------------------------------------------------
// Buttons
//-----------------------------------------------------------------------------

void SSFloaterAtmoMagic::onClickEditPreset()
{
    // Open the editor on the preset this track is set to, not on whatever it
    // happened to be showing last. Passed through onOpen rather than the
    // floater key: the editor is single-instance, and keying it on the name
    // would make every preset its own floater.
    LLFloater* editor = LLFloaterReg::showInstance("ss_atmo_preset");
    const std::string name = getChild<LLComboBox>("preset_combo")->getValue().asString();
    if (editor && !name.empty())
    {
        editor->onOpen(LLSD(name));
    }
}

void SSFloaterAtmoMagic::onClickReload()
{
    SSAtmoTrackMgr::getInstance()->reload();
    refreshTrackUI();
    refreshStatus();
}

void SSFloaterAtmoMagic::onClickLoad()
{
    // There is no generic inventory item picker to call, so open inventory
    // filtered to notecards; dropping one onto this floater does the load.
    LLFloaterSidePanelContainer* inv = LLFloaterReg::showTypedInstance<LLFloaterSidePanelContainer>(
        "inventory", LLSD());
    if (inv)
    {
        LLInventoryPanel* panel = inv->findChild<LLInventoryPanel>("All Items", true);
        if (panel)
        {
            panel->getFilter().setFilterObjectTypes(0x1 << LLInventoryType::IT_NOTECARD);
        }
    }
}

void SSFloaterAtmoMagic::onClickSave()
{
    std::string name = getChild<LLLineEditor>("config_name_editor")->getText();
    LLStringUtil::trim(name);
    if (name.empty()) name = "Atmo Weather";

    SSAtmoTrackMgr::getInstance()->exportToNotecard(name);
    refreshStatus();
}

void SSFloaterAtmoMagic::onClickRevert()
{
    SSAtmoTrackMgr::getInstance()->revertToBaseline();
    refreshTrackUI();
    refreshStatus();
}

void SSFloaterAtmoMagic::onClickDefaults()
{
    SSAtmoTrackMgr::getInstance()->resetToDefaults();
    refreshTrackUI();
    refreshStatus();
}

// </SS:Nexii>
