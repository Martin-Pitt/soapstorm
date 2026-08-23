/**
 * @file ssfloateratmov3.cpp
 * @brief Atmo Magic v3 floater implementation - phase 1.
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

#include "ssfloateratmov3.h"
#include "ssatmov3mgr.h"
#include "ssatmov3weatherstate.h"

#include "llbutton.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "llviewerinventory.h"

// <SS:Nexii> Atmo Magic v3 floater

static const F64 STATUS_POLL_INTERVAL = 0.5;

SSFloaterAtmoV3::SSFloaterAtmoV3(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoV3::postBuild()
{
    getChild<LLButton>("create_new_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCreateNew(); });
    getChild<LLButton>("load_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickLoad(); });
    getChild<LLButton>("save_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickSave(); });
    getChild<LLButton>("revert_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRevert(); });
    getChild<LLButton>("add_track_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddTrack(); });
    getChild<LLButton>("remove_track_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveTrack(); });

    getChild<LLUICtrl>("name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitName(); });

    getChild<LLUICtrl>("preview_time_slider")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitPreviewTime(); });
    getChild<LLUICtrl>("moisture_slider")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitMoisture(); });
    getChild<LLButton>("moisture_keyframe_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickMoistureKeyframeToggle(); });
    getChild<LLButton>("moisture_prev_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickMoisturePrev(); });
    getChild<LLButton>("moisture_next_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickMoistureNext(); });

    refreshVisibility();
    refreshTrackList();
    refreshStatus();
    refreshKeyframeProof();
    return true;
}

void SSFloaterAtmoV3::onOpen(const LLSD& key)
{
    refreshVisibility();
    refreshTrackList();
    refreshStatus();
    refreshKeyframeProof();
}

void SSFloaterAtmoV3::draw()
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
        refreshTrackList();
        refreshKeyframeProof();
    }

    LLFloater::draw();
}

//-----------------------------------------------------------------------------
// Drag and drop
//-----------------------------------------------------------------------------

bool SSFloaterAtmoV3::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
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
        if (SSAtmoV3Mgr::getInstance()->loadFromInventory(item))
        {
            // The fetch is async; refreshStatus()/refreshVisibility() on the
            // next poll picks up the result either way
            refreshStatus();
            refreshTrackList();
            refreshKeyframeProof();
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// Population
//-----------------------------------------------------------------------------

void SSFloaterAtmoV3::refreshVisibility()
{
    const bool has_asset = SSAtmoV3Mgr::getInstance()->hasAsset();

    getChild<LLUICtrl>("create_new_button")->setVisible(!has_asset);
    getChild<LLUICtrl>("no_asset_text")->setVisible(!has_asset);

    const char* editing_widgets[] = {
        "name_editor", "save_button", "revert_button",
        "add_track_button", "remove_track_button", "track_list",
        "preview_time_slider", "moisture_slider", "moisture_keyframe_button",
        "moisture_prev_button", "moisture_next_button", "moisture_keyframe_count_text",
        "forecast_text"
    };
    for (const char* name : editing_widgets)
    {
        getChild<LLUICtrl>(name)->setVisible(has_asset);
    }
}

void SSFloaterAtmoV3::refreshTrackList()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("track_list");
    list->deleteAllItems();

    const SSAtmoV3Asset& asset = mgr->asset();
    for (size_t i = 0; i < asset.mTracks.size(); ++i)
    {
        const SSAtmoV3Track& track = asset.mTracks[i];

        // FLT_MAX is "open ended" (see SSAtmoV3Track::mCeilingZ) - the
        // ground track's default, and any optional track before it's been
        // given a real ceiling. Printing it as a number is meaningless (a
        // multi-digit garbage figure, not an actual limit), so it gets its
        // own wording instead of being formatted like a real height.
        std::string range;
        if (track.mCeilingZ >= FLT_MAX)
        {
            range = (i == 0) ? "ground, unlimited" : llformat("%.0fm and above", track.mFloorZ);
        }
        else
        {
            range = (i == 0)
                ? llformat("ground, to %.0fm", track.mCeilingZ)
                : llformat("%.0f to %.0fm", track.mFloorZ, track.mCeilingZ);
        }

        LLSD row;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = track.mName;
        row["columns"][1]["column"] = "range";
        row["columns"][1]["value"] = range;
        list->addElement(row);
    }

    getChild<LLUICtrl>("add_track_button")->setEnabled(
        (S32)asset.mTracks.size() < SS_ATMOV3_MAX_TRACKS);
    getChild<LLUICtrl>("remove_track_button")->setEnabled(
        list->getFirstSelectedIndex() > 0); // index 0 (ground) never removable

    LLLineEditor* name_editor = getChild<LLLineEditor>("name_editor");
    if (!name_editor->hasFocus())
    {
        name_editor->setText(asset.mName);
    }
}

void SSFloaterAtmoV3::refreshStatus()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    const bool modified = mgr->isModified();

    std::string name = mgr->configName();
    setTitle("ATMO MAGIC v3 - " + name + (modified ? " *" : ""));

    getChild<LLTextBox>("status_text")->setText(mgr->statusText());
    getChild<LLUICtrl>("revert_button")->setEnabled(modified);
    getChild<LLUICtrl>("save_button")->setEnabled(mgr->hasAsset());
}

//-----------------------------------------------------------------------------
// Buttons
//-----------------------------------------------------------------------------

void SSFloaterAtmoV3::onClickCreateNew()
{
    // Same underlying write as the inventory "New Settings > New Atmo
    // Magic" menu item (see menu_create_inventory_item's "atmov3" branch);
    // the only difference is this also adopts the result once it lands, so
    // opening the floater with nothing active never dead-ends.
    //
    // Deliberately does not re-fetch the notecard we just wrote to load it
    // back - gInventory.getItem()/loadFromInventory() would work, but only
    // once local inventory has actually caught up with the new item, which
    // is one more race to fall into for no reason: SSAtmoV3Asset::makeDefault()
    // is deterministic, so regenerating it here and adopting it directly
    // is exactly the content that was written, no read-back needed at all.
    SSAtmoV3Mgr::createDefaultNotecard(LLUUID::null,
        [](const LLUUID& item_id, const LLUUID& asset_id)
        {
            if (item_id.isNull() || asset_id.isNull()) return;
            SSAtmoV3Mgr::getInstance()->applyExternalLLSD(asset_id, SSAtmoV3Asset::makeDefault().asLLSD());
        });
}

void SSFloaterAtmoV3::onClickLoad()
{
    LLFloaterReg::showInstance("inventory");
}

void SSFloaterAtmoV3::onClickSave()
{
    std::string name = getChild<LLLineEditor>("name_editor")->getText();
    SSAtmoV3Mgr::getInstance()->saveAsNewNotecard(name);
    refreshStatus();
}

void SSFloaterAtmoV3::onClickRevert()
{
    SSAtmoV3Mgr::getInstance()->revertToBaseline();
    refreshTrackList();
    refreshStatus();
    refreshKeyframeProof();
}

void SSFloaterAtmoV3::onClickAddTrack()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;
    mgr->editable().addTrack();
    refreshTrackList();
    refreshStatus();
}

void SSFloaterAtmoV3::onClickRemoveTrack()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("track_list");
    const S32 index = list->getFirstSelectedIndex();
    if (index <= 0) return; // ground track, or nothing selected

    mgr->editable().removeTrack(index);
    refreshTrackList();
    refreshStatus();
}

void SSFloaterAtmoV3::onCommitName()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->editable().mName = getChild<LLLineEditor>("name_editor")->getText();
    refreshStatus();
}

//-----------------------------------------------------------------------------
// Phase 3 keyframe engine proof - ground track's Moisture field only. See
// ssatmov3keyframe.h for the container this exercises.
//-----------------------------------------------------------------------------

void SSFloaterAtmoV3::refreshKeyframeProof()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    SSAtmoV3Weather& weather = mgr->editable().mTracks[0].mWeather;

    getChild<LLUICtrl>("moisture_slider")->setValue(weather.mMoisture.valueAt(mPreviewTime));

    const bool on_keyframe = weather.mMoisture.hasKeyframeAt(mPreviewTime);
    getChild<LLButton>("moisture_keyframe_button")->setToggleState(on_keyframe);
    getChild<LLButton>("moisture_keyframe_button")->setLabel(
        std::string(on_keyframe ? "\xE2\x97\x86" : "\xE2\x97\x87")); // filled/hollow diamond

    getChild<LLTextBox>("moisture_keyframe_count_text")->setText(
        llformat("%d keyframe(s)", (S32)weather.mMoisture.keyframeCount()));

    // Phase 5 proof: the same resolve() call phase 5's renderer hookup will
    // eventually feed the particle/cloud/audio systems from.
    const SSAtmoV3WeatherState resolved = SSAtmoV3WeatherResolver::resolve(weather, mPreviewTime);
    getChild<LLTextBox>("forecast_text")->setText(resolved.mForecastText);
}

void SSFloaterAtmoV3::onCommitPreviewTime()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    // The slider is 0..1 of the loaded asset's day length, so moving it
    // means something even before the real timeline UI (still a plain
    // fraction display, not the full After-Effects-style track from the
    // design doc) exists.
    const F64 fraction = getChild<LLUICtrl>("preview_time_slider")->getValue().asReal();
    mPreviewTime = fraction * mgr->asset().mTracks[0].mDayLengthSeconds;
    refreshKeyframeProof();
}

void SSFloaterAtmoV3::onCommitMoisture()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    const F32 value = (F32)getChild<LLUICtrl>("moisture_slider")->getValue().asReal();

    // This one call is the entire editing rule from the design doc: no
    // keyframes yet, this just becomes the plain value; head on an existing
    // keyframe edits it in place; head anywhere else inserts a new one.
    mgr->editable().mTracks[0].mWeather.mMoisture.setValueAtHead(mPreviewTime, value);
    refreshKeyframeProof();
    refreshStatus();
}

void SSFloaterAtmoV3::onClickMoistureKeyframeToggle()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->editable().mTracks[0].mWeather.mMoisture.toggleKeyframeAtHead(mPreviewTime);
    refreshKeyframeProof();
    refreshStatus();
}

void SSFloaterAtmoV3::onClickMoisturePrev()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    mPreviewTime = mgr->asset().mTracks[0].mWeather.mMoisture.prevKeyframeTime(mPreviewTime);
    const F64 fraction = mgr->asset().mTracks[0].mDayLengthSeconds > 0.0
        ? mPreviewTime / mgr->asset().mTracks[0].mDayLengthSeconds : 0.0;
    getChild<LLUICtrl>("preview_time_slider")->setValue(fraction);
    refreshKeyframeProof();
}

void SSFloaterAtmoV3::onClickMoistureNext()
{
    SSAtmoV3Mgr* mgr = SSAtmoV3Mgr::getInstance();
    if (!mgr->hasAsset()) return;

    mPreviewTime = mgr->asset().mTracks[0].mWeather.mMoisture.nextKeyframeTime(mPreviewTime);
    const F64 fraction = mgr->asset().mTracks[0].mDayLengthSeconds > 0.0
        ? mPreviewTime / mgr->asset().mTracks[0].mDayLengthSeconds : 0.0;
    getChild<LLUICtrl>("preview_time_slider")->setValue(fraction);
    refreshKeyframeProof();
}

// </SS:Nexii>
