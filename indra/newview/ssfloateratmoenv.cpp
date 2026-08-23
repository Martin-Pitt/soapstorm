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

#include "llbutton.h"
#include "llfloaterreg.h"
#include "llfloatersidepanelcontainer.h"
#include "llfocusmgr.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewerinventory.h"

// <SS:Nexii> Atmo Magic floater

static const F64 STATUS_POLL_INTERVAL = 0.5;

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
    getChild<LLButton>("add_track_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddTrack(); });
    getChild<LLButton>("remove_track_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveTrack(); });

    // Fixed track_row_0..3 - see the mSelectedTrackIndex comment in the
    // header for why this is four pre-declared rows rather than a
    // dynamically built list.
    for (S32 i = 0; i < SS_ATMOENV_MAX_TRACKS; ++i)
    {
        getChild<LLButton>(llformat("track_row_%d", i))->setClickedCallback(
            [this, i](LLUICtrl*, const LLSD&) { onClickTrackRow(i); });
    }

    getChild<LLUICtrl>("name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitName(); });

    getChild<LLUICtrl>("preview_time_slider")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitPreviewTime(); });

    // The ground track's Weather cube, one AE-style row per field - see
    // the FloatRow comment in the header. mField is a fresh accessor, not
    // a cached pointer/reference, so an add/remove track reallocating
    // SSAtmoEnvAsset::mTracks in between calls can never leave one dangling.
    mFloatRows = {
        { "moisture",     []() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[0].mWeather.mMoisture; },     "", false },
        { "convection",   []() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[0].mWeather.mConvection; },   "", false },
        { "temperature",  []() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[0].mWeather.mTemperatureC; }, "\xC2\xB0" "C", true },
        { "wind_heading", []() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[0].mWeather.mWindHeading; },  "\xC2\xB0", true },
        { "wind_speed",   []() -> SSAtmoEnvKeyframed<F32>& { return SSAtmoEnvManager::getInstance()->editable().mTracks[0].mWeather.mWindSpeed; },    " m/s", true },
    };
    for (const FloatRow& row : mFloatRows)
    {
        getChild<LLUICtrl>(row.mPrefix + "_slider")->setCommitCallback(
            [this, row](LLUICtrl*, const LLSD&) { commitFloatRow(row); refreshKeyframeProof(); refreshStatus(); });
        getChild<LLButton>(row.mPrefix + "_keyframe_button")->setClickedCallback(
            [this, row](LLUICtrl*, const LLSD&) { toggleFloatRowKeyframe(row); refreshKeyframeProof(); refreshStatus(); });
        getChild<LLButton>(row.mPrefix + "_prev_button")->setClickedCallback(
            [this, row](LLUICtrl*, const LLSD&) { jumpFloatRowPrev(row); refreshKeyframeProof(); });
        getChild<LLButton>(row.mPrefix + "_next_button")->setClickedCallback(
            [this, row](LLUICtrl*, const LLSD&) { jumpFloatRowNext(row); refreshKeyframeProof(); });
    }

    refreshVisibility();
    refreshTrackList();
    refreshStatus();
    refreshKeyframeProof();
    return true;
}

void SSFloaterAtmoEnv::onOpen(const LLSD& key)
{
    refreshVisibility();
    refreshTrackList();
    refreshStatus();
    refreshKeyframeProof();
}

void SSFloaterAtmoEnv::onClose(bool app_quitting)
{
    // Give the real wall clock back control the moment nobody's actually
    // looking at a preview - see SSAtmoEnvManager::setPreviewTimeOverride().
    // onVisibilityChange (below) already covers the common single-instance
    // hide-not-destroy case, but this catches actual destruction too.
    SSAtmoEnvManager::getInstance()->clearPreviewTimeOverride();
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
        refreshKeyframeProof();
    }
    else
    {
        SSAtmoEnvManager::getInstance()->clearPreviewTimeOverride();
    }
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
        refreshTrackList();
        // Skip the keyframe/slider refresh while any of its own widgets
        // has mouse capture (a slider mid-drag) - forcing every slider's
        // thumb back to its last-committed value out from under an active
        // drag, every 0.5s, made a real drag look like it was fighting
        // itself or "not sticking". A drag's own hover handler already
        // keeps mPreviewTime/the field values live via its continuous
        // commit, so skipping this one poll tick loses nothing.
        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            refreshKeyframeProof();
        }
    }

    LLFloater::draw();
}

//-----------------------------------------------------------------------------
// Drag and drop
//-----------------------------------------------------------------------------

bool SSFloaterAtmoEnv::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
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
                refreshTrackList();
                refreshKeyframeProof();
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
        "name_editor", "save_button", "revert_button",
        "track_panel", "atmo_tabs",
        "preview_time_caption", "preview_time_slider", "preview_time_value_text",
        "forecast_text"
    };
    for (const char* name : editing_widgets)
    {
        getChild<LLUICtrl>(name)->setVisible(has_asset);
    }
}

void SSFloaterAtmoEnv::refreshTrackList()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    const SSAtmoEnvAsset& asset = mgr->asset();

    // Fixed track_row_0..3 - see the mSelectedTrackIndex comment in the
    // header. Selection can't outlive a track being removed out from under
    // it (Remove Track always drops back to the ground track), so clamp
    // before it drives anything below.
    if (mSelectedTrackIndex >= (S32)asset.mTracks.size())
    {
        mSelectedTrackIndex = 0;
    }

    for (S32 i = 0; i < SS_ATMOENV_MAX_TRACKS; ++i)
    {
        LLButton* row = getChild<LLButton>(llformat("track_row_%d", i));
        const bool exists = i < (S32)asset.mTracks.size();
        row->setVisible(exists);
        if (!exists) continue;

        const SSAtmoEnvTrack& track = asset.mTracks[i];

        // FLT_MAX is "open ended" (see SSAtmoEnvTrack::mCeilingZ) - the
        // ground track's default, and any optional track before it's been
        // given a real ceiling. Printing it as a number is meaningless (a
        // multi-digit garbage figure, not an actual limit), so it gets its
        // own wording instead of being formatted like a real height.
        std::string range;
        if (track.mCeilingZ >= FLT_MAX)
        {
            range = (i == 0) ? "unlimited" : llformat("%.0fm+", track.mFloorZ);
        }
        else
        {
            range = (i == 0)
                ? llformat("to %.0fm", track.mCeilingZ)
                : llformat("%.0f-%.0fm", track.mFloorZ, track.mCeilingZ);
        }

        row->setLabel(track.mName + " (" + range + ")");
        row->setToggleState(i == mSelectedTrackIndex);
    }

    getChild<LLUICtrl>("add_track_button")->setEnabled(
        (S32)asset.mTracks.size() < SS_ATMOENV_MAX_TRACKS);
    getChild<LLUICtrl>("remove_track_button")->setEnabled(mSelectedTrackIndex > 0);

    LLLineEditor* name_editor = getChild<LLLineEditor>("name_editor");
    if (!name_editor->hasFocus())
    {
        name_editor->setText(asset.mName);
    }
}

void SSFloaterAtmoEnv::refreshStatus()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const bool modified = mgr->isModified();

    std::string name = mgr->configName();
    setTitle("ATMO MAGIC v3 - " + name + (modified ? " *" : ""));

    // No persistent status line - mgr->statusText() is still logged
    // (LL_WARNS/LL_INFOS) at the point something actually happens; a
    // steady-state string like "Ready." sitting in the UI didn't tell
    // anyone anything the rest of the floater's populated fields don't
    // already say. Real failures alert instead - see handleDragAndDrop().
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
    // is one more race to fall into for no reason: SSAtmoEnvAsset::makeDefault()
    // is deterministic, so regenerating it here and adopting it directly
    // is exactly the content that was written, no read-back needed at all.
    SSAtmoEnvManager::createDefaultNotecard(LLUUID::null,
        [](const LLUUID& item_id, const LLUUID& asset_id)
        {
            if (item_id.isNull() || asset_id.isNull()) return;
            SSAtmoEnvManager::getInstance()->adoptCreated(item_id, asset_id, SSAtmoEnvAsset::makeDefault());
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

void SSFloaterAtmoEnv::onClickRevert()
{
    SSAtmoEnvManager::getInstance()->revertToBaseline();
    refreshTrackList();
    refreshStatus();
    refreshKeyframeProof();
}

void SSFloaterAtmoEnv::onClickAddTrack()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    mgr->editable().addTrack();
    refreshTrackList();
    refreshStatus();
}

void SSFloaterAtmoEnv::onClickRemoveTrack()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (mSelectedTrackIndex <= 0) return; // ground track, or nothing selected

    mgr->editable().removeTrack(mSelectedTrackIndex);
    mSelectedTrackIndex = 0; // back to the one track that's always there
    refreshTrackList();
    refreshStatus();
}

void SSFloaterAtmoEnv::onClickTrackRow(S32 index)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;
    if (index >= (S32)mgr->asset().mTracks.size()) return;

    mSelectedTrackIndex = index;
    refreshTrackList();
}

void SSFloaterAtmoEnv::onCommitName()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    mgr->editable().mName = getChild<LLLineEditor>("name_editor")->getText();
    refreshStatus();
}

//-----------------------------------------------------------------------------
// Ground track's Weather cube - AE-style keyframe rows. See ssatmoenvkeyframe.h
// for the container every row exercises, and the FloatRow comment in the
// header for why mField is a fresh accessor rather than a cached reference.
//-----------------------------------------------------------------------------

namespace
{
    std::string formatHoursMinutes(F64 seconds)
    {
        if (seconds < 0.0) seconds = 0.0;
        const S32 total_minutes = (S32)llround(seconds / 60.0);
        return llformat("%dh %02dm", total_minutes / 60, total_minutes % 60);
    }
}

void SSFloaterAtmoEnv::refreshFloatRow(const FloatRow& row, F64 time, F64 day_length)
{
    const SSAtmoEnvKeyframed<F32>& field = row.mField();
    const F32 value = field.valueAt(time, day_length);

    getChild<LLUICtrl>(row.mPrefix + "_slider")->setValue(value);

    const std::string text = row.mIntegerDisplay
        ? llformat("%.0f%s", value, row.mUnitSuffix.c_str())
        : llformat("%.2f%s", value, row.mUnitSuffix.c_str());
    getChild<LLTextBox>(row.mPrefix + "_value_text")->setText(text);

    const bool on_keyframe = field.hasKeyframeAt(time);
    LLButton* kf_button = getChild<LLButton>(row.mPrefix + "_keyframe_button");
    kf_button->setToggleState(on_keyframe);
    kf_button->setLabel(std::string(on_keyframe ? "\xE2\x97\x86" : "\xE2\x97\x87")); // filled/hollow diamond

    // Debug diagnostic: dump the raw stored keyframe list as a tooltip on
    // the slider, so "what does the widget show" and "what is actually
    // stored" can be compared directly instead of inferred from the slider
    // thumb's position. Temporary - meant to close out the reported time-
    // scrub sync bug, not a permanent UI feature.
    std::string dump = llformat("now=%s -> %.3f | ", formatHoursMinutes(time).c_str(), value);
    if (!field.hasKeyframes())
    {
        dump += "(no keyframes, plain value)";
    }
    else
    {
        for (const SSAtmoEnvKeyframe<F32>& kf : field.keyframes())
        {
            dump += llformat("[%s=%.3f %s] ", formatHoursMinutes(kf.mTime).c_str(), kf.mValue,
                              ss_atmoenv_curve_name(kf.mCurve).c_str());
        }
    }
    getChild<LLUICtrl>(row.mPrefix + "_slider")->setToolTip(dump);
}

void SSFloaterAtmoEnv::commitFloatRow(const FloatRow& row)
{
    const F32 value = (F32)getChild<LLUICtrl>(row.mPrefix + "_slider")->getValue().asReal();

    // This one call is the entire editing rule from the design doc: no
    // keyframes yet, this just becomes the plain value; head on an existing
    // keyframe edits it in place; head anywhere else inserts a new one.
    row.mField().setValueAtHead(mPreviewTime, value);
}

void SSFloaterAtmoEnv::toggleFloatRowKeyframe(const FloatRow& row)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    row.mField().toggleKeyframeAtHead(mPreviewTime, mgr->asset().mTracks[0].mDayLengthSeconds);
}

void SSFloaterAtmoEnv::jumpFloatRowPrev(const FloatRow& row)
{
    // refreshKeyframeProof() (called by every caller of this) sets every
    // row's own slider from mPreviewTime, so there's nothing to set here
    // directly - one place that converts time to what a slider shows, not
    // one per row.
    mPreviewTime = row.mField().prevKeyframeTime(mPreviewTime);
}

void SSFloaterAtmoEnv::jumpFloatRowNext(const FloatRow& row)
{
    mPreviewTime = row.mField().nextKeyframeTime(mPreviewTime);
}

void SSFloaterAtmoEnv::refreshKeyframeProof()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // This floater being open with a preview visible is what overrides the
    // live render's clock - see SSAtmoEnvManager::setPreviewTimeOverride() and
    // SSAtmoEnvBridge::resolveActiveTrack(). Every call here (a user
    // scrub, a chevron jump, or just the periodic poll while nothing's
    // changed) re-asserts it, so the in-world preview always matches
    // whatever this floater is currently showing rather than drifting back
    // to the real wall clock the instant this stops being re-armed.
    mgr->setPreviewTimeOverride(mPreviewTime);

    const F64 day_length = mgr->asset().mTracks[0].mDayLengthSeconds;
    const F64 day_length_hours = day_length / 3600.0;

    // The slider itself works in hours (0..day length), not the raw 0..1
    // fraction it started as - snapped to the day-length-relative grid the
    // design doc specifies (day length / 32 keyframe slots), so a 4-hour
    // day snaps in 7.5-minute steps rather than an arbitrary fraction.
    LLSliderCtrl* time_slider = getChild<LLSliderCtrl>("preview_time_slider");
    time_slider->setMinValue(0.f);
    time_slider->setMaxValue((F32)day_length_hours);
    time_slider->setIncrement((F32)(day_length_hours / 32.0));
    time_slider->setValue((F32)llclamp(mPreviewTime / 3600.0, 0.0, day_length_hours));

    getChild<LLTextBox>("preview_time_value_text")->setText(formatHoursMinutes(mPreviewTime));

    for (const FloatRow& row : mFloatRows)
    {
        refreshFloatRow(row, mPreviewTime, day_length);
    }

    // Phase 5 proof: the same resolve() call phase 5's renderer hookup will
    // eventually feed the particle/cloud/audio systems from.
    const SSAtmoEnvWeatherState resolved = SSAtmoEnvWeatherResolver::resolve(
        mgr->editable().mTracks[0].mWeather, mPreviewTime, day_length);
    getChild<LLTextBox>("forecast_text")->setText(resolved.mForecastText);
}

void SSFloaterAtmoEnv::onCommitPreviewTime()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    // The slider is hours directly now, snapped to the day-length/32 grid
    // (see refreshKeyframeProof) - not the raw 0..1 fraction this started
    // as, which read as a meaningless percentage rather than a time of day.
    const F64 hours = getChild<LLUICtrl>("preview_time_slider")->getValue().asReal();
    mPreviewTime = hours * 3600.0;
    refreshKeyframeProof();
}

// </SS:Nexii>
