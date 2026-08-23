/**
 * @file ssatmoenvmanager.h
 * @brief Atmo Magic: loads, holds and persists the unified environment
 *        asset. Deliberately has no implicit default: unlike the v2 track
 *        manager, nothing is active until something has actually been
 *        created or loaded, per doc/atmo_magic_environment.md ("v3 is
 *        opt-in end to end"). No rendering hookup yet - this is phase 1,
 *        schema and notecard round-trip only.
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

#ifndef SS_ATMOENVMGR_H
#define SS_ATMOENVMGR_H

// <SS:Nexii> Atmo Magic: environment manager

#include "llassettype.h"
#include "llextendedstatus.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <functional>
#include <string>

#include "ssatmoenvasset.h"

class LLInventoryItem;

class SSAtmoEnvManager : public LLSingleton<SSAtmoEnvManager>
{
    LLSINGLETON(SSAtmoEnvManager);
    ~SSAtmoEnvManager() = default;

public:
    // True once something has actually been created or loaded this
    // session. False means "nothing to show" - the floater's one-time
    // create button, not an edit panel, is what a caller should be showing.
    bool hasAsset() const { return mHasAsset; }

    const SSAtmoEnvAsset& asset() const { return mWorking; }
    // Caller must check hasAsset() first; mutating before anything exists
    // would silently manufacture the implicit default this deliberately
    // does not have.
    SSAtmoEnvAsset& editable() { return mWorking; }

    // Compares serialised LLSD rather than a hand-written operator== on
    // every nested struct - one place to get the "is this actually
    // different" question right instead of one per field.
    bool isModified() const;
    void revertToBaseline();

    // Fire-and-forget: writes a fresh notecard pre-filled with
    // SSAtmoEnvAsset::makeDefault(), independent of whatever is or isn't
    // currently loaded. This is the one action both the inventory "New
    // Atmo Magic" menu item and the floater's create button funnel into -
    // see menu_create_inventory_item's "atmoenv" branch in
    // llviewerinventory.cpp. parent_id null means the "Atmo Magic" subfolder
    // under the Settings system folder (see atmoFolderId() below), created
    // first if it doesn't exist yet.
    //
    // on_created fires only once the asset body has actually finished
    // uploading, with both the item id and the asset id - not right after
    // the bare inventory-item-metadata create step, which was the bug an
    // earlier version of this had: the item existed but its content
    // hadn't landed yet, so an immediate load attempt against it could
    // silently do nothing. Either id may be null on failure.
    static void createDefaultNotecard(const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id)> on_created);

    // Adopts a just-created notecard (createDefaultNotecard's own callback)
    // as the active asset, recording its item id as well as its asset id -
    // unlike applyExternalLLSD, this one *is* something the agent owns and
    // can update in place, so saveNotecard() has an item to target rather
    // than always minting a new one.
    void adoptCreated(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset);

    // Resolves the "Atmo Magic" subfolder under the Settings system folder,
    // creating it first if it doesn't already exist. Notecards are notes,
    // not real Settings-typed assets, but grouping v3 environments
    // alongside the Sky/Water/Day Cycle items EEP's own "New Settings"
    // menu already creates there keeps everything environment-related in
    // one place instead of scattered loose in the general Notecards
    // folder. Async because creating the folder (on a genuine first use)
    // is a server round trip; on_ready always fires exactly once, with a
    // valid id either way.
    static void atmoFolderId(std::function<void(const LLUUID&)> on_ready);

    // Load a notecard the user picked or dropped onto the floater. False
    // means the permission check failed immediately - on_complete never
    // fires in that case, since nothing was actually started. A true
    // return means the fetch is in flight; on_complete (optional) fires
    // exactly once when it resolves, with whether it actually parsed as a
    // valid v3 asset - the floater uses this to alert on a load *it*
    // asked for, without also alerting for SSAtmoEnvDiscoveryManager's silent
    // parcel-driven fetches, which don't pass one.
    bool loadFromInventory(const LLInventoryItem* item, std::function<void(bool success)> on_complete = nullptr);

    // Load a bare asset id directly (phase 8: parcel-referenced notecard).
    // Unlike loadFromInventory, this does not check the agent's own
    // permissions on any inventory item, because there isn't necessarily
    // one - see the open note in doc/atmo_magic_environment.md about
    // whether this path is even the right one for phase 8 before it ships.
    void loadFromAssetId(const LLUUID& asset_id);

    // Applies notecard content obtained some other way than the two fetch
    // paths above - specifically, SSAtmoEnvDiscoveryManager's Bridge-relayed
    // parcel discovery (phase 8), which never touches gAssetStorage at
    // all. source_id is recorded as the applied asset id the same as
    // either fetch path would.
    //
    // applyExternalLLSD is the common case: the Bridge script passes a v3
    // notecard's content straight through as its HTTP response whenever it
    // already starts with "<llsd>" - true for every v3 asset - so the HTTP
    // layer has already parsed it by the time it gets here, no text step
    // at all. applyExternalNotecardText is the fallback for content that
    // arrives as plain text instead (a hand-edited notecard, or a Bridge
    // response that took the wrapped-string path) and needs the same
    // LLSD-XML/notation parsing onAssetLoaded()'s text handling does.
    //
    // Both return whether the content actually validated and got adopted -
    // a caller like SSAtmoEnvDiscoveryManager needs to know the difference
    // between "applied" and "rejected" to decide whether the same source
    // is worth trying again later.
    bool applyExternalLLSD(const LLUUID& source_id, const LLSD& sd);
    bool applyExternalNotecardText(const LLUUID& source_id, const std::string& text);

    // Saves the current working asset. Updates the currently loaded/created
    // notecard in place (renaming its inventory item if the name changed,
    // re-uploading its content to the same item) whenever one is actually
    // owned - mItemID null (nothing loaded yet, or the active asset came
    // from someone else's parcel via SSAtmoEnvDiscoveryManager, which the agent
    // doesn't have a copy of) falls back to minting a brand new notecard,
    // the same as this always used to do. Caller must have hasAsset() true.
    void saveNotecard(const std::string& name);

    const std::string& statusText() const { return mStatus; }
    const std::string& configName() const { return mHasAsset ? mWorking.mName : mNoAssetName; }
    const LLUUID& configAssetId() const { return mAssetID; }

    // Preview time override - what the Atmo Magic floater's preview time
    // slider actually does while it's open, mirroring EEP's Edit Day Cycle
    // floater overriding LLEnvironment's live time: SSAtmoEnvBridge
    // uses this instead of SSAtmoEnvTrack::currentDayCycleTime()'s wall-clock
    // formula whenever it's active, so scrubbing the floater's slider is
    // actually previewing what gets rendered, not just what the floater's
    // own widgets show. The floater sets this on every preview-time change
    // while visible and clears it on hide/close, so the real wall clock
    // silently takes back over the moment nobody's looking at a preview.
    void setPreviewTimeOverride(F64 time_seconds) { mPreviewOverrideActive = true; mPreviewOverrideTime = time_seconds; }
    void clearPreviewTimeOverride() { mPreviewOverrideActive = false; }
    bool hasPreviewTimeOverride() const { return mPreviewOverrideActive; }
    F64 previewTimeOverride() const { return mPreviewOverrideTime; }

private:
    static void onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                               void* user_data, S32 status, LLExtStat ext_status);
    bool applyNotecardText(const std::string& text, bool from_inventory_permission_check);

    // The common tail of every apply path: validate via
    // SSAtmoEnvAsset::fromLLSD and, on success, adopt as both baseline and
    // working. Factored out so parsed-LLSD-in-hand (applyExternalLLSD) and
    // parse-text-first (applyNotecardText) share one place that decides
    // what counts as a valid asset, rather than each re-deciding it.
    bool adoptParsedAsset(const LLSD& sd);

    // saveNotecard()'s in-place branch: renames mItemID's inventory item to
    // `name` if it differs, then re-uploads mWorking's serialised content to
    // that same item id - no create_inventory_item call at all, so the
    // notecard the user has been looking at in their inventory stays the
    // same item instead of a new one appearing next to it on every save.
    void updateExistingNotecard(const std::string& name);

    // Invokes and clears mLoadCompleteCallback, if one is set. The one
    // place onAssetLoaded's several early-return paths (permission,
    // network failure, unreadable, invalid LLSD) and its success path all
    // funnel through, so "did the load I asked for actually work" is
    // answered in exactly one place rather than at every return site.
    void finishLoad(bool success);

    // The asset actually in the notecard last loaded/saved, vs. mWorking
    // which is that plus whatever has been edited on top. Same baseline/
    // working split as v2, for the same reason: an asterisk in the floater
    // title and a Revert button that means something.
    SSAtmoEnvAsset mBaseline;
    SSAtmoEnvAsset mWorking;
    bool mHasAsset = false;

    LLUUID mAssetID;      // notecard currently applied; null if none
    LLUUID mItemID;       // inventory item backing mAssetID, if the agent owns one; null if none (e.g. a parcel-discovered notecard nobody handed a copy of)
    LLUUID mPendingID;    // fetch in flight; null if none
    LLUUID mPendingItemID; // item id for the load mPendingID belongs to, if any - copied to mItemID once it lands
    std::function<void(bool)> mLoadCompleteCallback; // set only for loadFromInventory's caller, if it asked

    std::string mStatus = "no environment loaded";
    const std::string mNoAssetName = "(none)";

    bool mPreviewOverrideActive = false;
    F64 mPreviewOverrideTime = 0.0;
};

// </SS:Nexii>

#endif // SS_ATMOENVMGR_H
