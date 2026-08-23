/**
 * @file ssatmov3mgr.h
 * @brief Atmo Magic v3: loads, holds and persists the unified environment
 *        asset. Deliberately has no implicit default: unlike the v2 track
 *        manager, nothing is active until something has actually been
 *        created or loaded, per doc/atmo_magic_v3_environment.md ("v3 is
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

#ifndef SS_ATMOV3MGR_H
#define SS_ATMOV3MGR_H

// <SS:Nexii> Atmo Magic v3: environment manager

#include "llassettype.h"
#include "llextendedstatus.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <functional>
#include <string>

#include "ssatmov3asset.h"

class LLInventoryItem;

class SSAtmoV3Mgr : public LLSingleton<SSAtmoV3Mgr>
{
    LLSINGLETON(SSAtmoV3Mgr);
    ~SSAtmoV3Mgr() = default;

public:
    // True once something has actually been created or loaded this
    // session. False means "nothing to show" - the floater's one-time
    // create button, not an edit panel, is what a caller should be showing.
    bool hasAsset() const { return mHasAsset; }

    const SSAtmoV3Asset& asset() const { return mWorking; }
    // Caller must check hasAsset() first; mutating before anything exists
    // would silently manufacture the implicit default this deliberately
    // does not have.
    SSAtmoV3Asset& editable() { return mWorking; }

    // Compares serialised LLSD rather than a hand-written operator== on
    // every nested struct - one place to get the "is this actually
    // different" question right instead of one per field.
    bool isModified() const;
    void revertToBaseline();

    // Fire-and-forget: writes a fresh notecard pre-filled with
    // SSAtmoV3Asset::makeDefault(), independent of whatever is or isn't
    // currently loaded. This is the one action both the inventory "New
    // Atmo Magic" menu item and the floater's create button funnel into -
    // see menu_create_inventory_item's "atmov3" branch in
    // llviewerinventory.cpp. parent_id null means the default Notecards
    // folder.
    //
    // on_created fires only once the asset body has actually finished
    // uploading, with both the item id and the asset id - not right after
    // the bare inventory-item-metadata create step, which was the bug an
    // earlier version of this had: the item existed but its content
    // hadn't landed yet, so an immediate load attempt against it could
    // silently do nothing. Either id may be null on failure.
    static void createDefaultNotecard(const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id)> on_created);

    // Load a notecard the user picked or dropped onto the floater. False
    // means the permission check failed immediately; a true return still
    // means the fetch is in flight, not that it succeeded - watch
    // statusText().
    bool loadFromInventory(const LLInventoryItem* item);

    // Load a bare asset id directly (phase 8: parcel-referenced notecard).
    // Unlike loadFromInventory, this does not check the agent's own
    // permissions on any inventory item, because there isn't necessarily
    // one - see the open note in doc/atmo_magic_v3_environment.md about
    // whether this path is even the right one for phase 8 before it ships.
    void loadFromAssetId(const LLUUID& asset_id);

    // Applies notecard content obtained some other way than the two fetch
    // paths above - specifically, SSAtmoV3DiscoveryMgr's Bridge-relayed
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
    // a caller like SSAtmoV3DiscoveryMgr needs to know the difference
    // between "applied" and "rejected" to decide whether the same source
    // is worth trying again later.
    bool applyExternalLLSD(const LLUUID& source_id, const LLSD& sd);
    bool applyExternalNotecardText(const LLUUID& source_id, const std::string& text);

    // Writes the current working asset out to a brand new notecard. This
    // mirrors v2's SSAtmoTrackMgr::exportToNotecard: every "save" is a new
    // inventory item, there is no update-in-place yet. Caller must have
    // hasAsset() true.
    void saveAsNewNotecard(const std::string& name);

    const std::string& statusText() const { return mStatus; }
    const std::string& configName() const { return mHasAsset ? mWorking.mName : mNoAssetName; }
    const LLUUID& configAssetId() const { return mAssetID; }

private:
    static void onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                               void* user_data, S32 status, LLExtStat ext_status);
    bool applyNotecardText(const std::string& text, bool from_inventory_permission_check);

    // The common tail of every apply path: validate via
    // SSAtmoV3Asset::fromLLSD and, on success, adopt as both baseline and
    // working. Factored out so parsed-LLSD-in-hand (applyExternalLLSD) and
    // parse-text-first (applyNotecardText) share one place that decides
    // what counts as a valid asset, rather than each re-deciding it.
    bool adoptParsedAsset(const LLSD& sd);

    // The asset actually in the notecard last loaded/saved, vs. mWorking
    // which is that plus whatever has been edited on top. Same baseline/
    // working split as v2, for the same reason: an asterisk in the floater
    // title and a Revert button that means something.
    SSAtmoV3Asset mBaseline;
    SSAtmoV3Asset mWorking;
    bool mHasAsset = false;

    LLUUID mAssetID;      // notecard currently applied; null if none
    LLUUID mPendingID;    // fetch in flight; null if none

    std::string mStatus = "no environment loaded";
    const std::string mNoAssetName = "(none)";
};

// </SS:Nexii>

#endif // SS_ATMOV3MGR_H
