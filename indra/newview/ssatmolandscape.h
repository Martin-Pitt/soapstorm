/**
 * @file ssatmolandscape.h
 * @brief Atmo Magic: the landscape world - owns the live scenery set.
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

#ifndef SS_ATMO_LANDSCAPE_H
#define SS_ATMO_LANDSCAPE_H

#include "llpointer.h"
#include "llsingleton.h"
#include "lldeadmantimer.h"

#include <string>
#include <vector>

#include "ssatmolandscapeobject.h"

class LLInventoryItem;

// <SS:Nexii> Atmo Magic landscape: mesh scenery owned by the environment asset instead of the
// region. The SSWaterWorld of the landscape family - one manager, N live runtime objects.
//
// Lifecycle: the working asset's active track owns the records; the live set mirrors the
// active track's record list, keyed by asset uuid. Anything that changes the record set
// (track crossing, environment load/revert, floater add/delete/reorder, agent region change)
// reshapes the live set; anything that changes a record's CONTENT (the author's edits) flows
// through the reconcile funnel - capture writes object state into the working asset, and the
// existing save path persists it.
//
// Everything is opt-in: the master SSAtmoEnabled switch plus this feature's own SSAtmoLandscape
// gate, and the applier must be actively driving the sky. Nothing exists when any of those are
// off.
class SSAtmoLandscapeWorld : public LLSingleton<SSAtmoLandscapeWorld>
{
    LLSINGLETON_EMPTY_CTOR(SSAtmoLandscapeWorld);
    ~SSAtmoLandscapeWorld() = default;

public:
    // Per-frame tick, called from the Atmo block in llviewerdisplay.
    void update();

    // Kills the live set (environment unload, master toggle, hard reset).
    void clearLandscapeObjects();

    // Live set introspection for the floater's list.
    S32 objectCount() const { return (S32)mObjects.size(); }
    SSAtmoLandscapeObject* objectAt(S32 index)
    {
        return (index >= 0 && index < (S32)mObjects.size()) ? mObjects[(size_t)index].get() : nullptr;
    }

    // The ACTIVE track's records - the list the floater mirrors. Index corresponds to
    // objectAt() while the live set is in sync (reconcile builds in record order).
    S32 recordCount() const;
    const SSAtmoEnvLandscape* recordAt(S32 index) const;

    // Flips a record's lock mode, converting its coordinates so the object does not jump on
    // the mode change (locked offsets become the current global, and vice versa), then
    // re-applies. Returns the new mode (true = locked).
    bool toggleRecordLock(S32 index);

    // The floater's add path: the R1 fullperm gate lives in the panel; this records the item
    // (asset id, metadata, default placement) into the active track and hydrates now. Returns
    // the record's index in the active track's list, or -1 with out_reason set.
    S32 addFromItem(const LLInventoryItem* item, std::string& out_reason);

    // Removes the record at index in the active track and reshapes now.
    bool removeRecord(S32 index);

    // Removes the active track's record whose mesh id matches (the pie-menu Delete path for
    // local-content objects). Returns false when nothing matched.
    bool removeByMesh(const LLUUID& mesh_id);

    // Force the live set to match the working asset next tick (floater reorder etc.).
    void invalidate() { mLastSignature.clear(); }

private:
    void reconcile(const SSAtmoEnvAsset& asset, S32 track_index, LLViewerRegion* regionp);
    SSAtmoLandscapeObject* createObject(LLViewerRegion* regionp, const SSAtmoEnvLandscape& record);
    void applyFacesToAll();
    void captureAll(std::vector<SSAtmoEnvLandscape>& records);

    std::vector<LLPointer<SSAtmoLandscapeObject>> mObjects;

    // The signature of the record set the live objects were shaped from - a mesh-id run.
    // Any change reshapes; content edits inside a record do not.
    std::string mLastSignature;

    // The region the live set is anchored to - changing it rebuilds so locked records
    // re-anchor to the new origin.
    U64 mAgentRegionHandle = 0;

    LLFrameTimer mCaptureTimer;
};

// The landscape floater's helpers: the fullperm drop gate and the live record lookup. The
// gate is shared so the panel and any future drop path ask the same question.
bool ss_landscape_item_fullperm(const LLInventoryItem* item);

// <SS:Nexii> Selection-node seeding: when a local-content object is selected, the node is
// seeded from its record (name, description, creator/last-owner, perms, creation date) so
// the stock editor's General tab, Inspect and texture panels read real values instead of
// the node's default blank server-wait state. Called from LLSelectMgr's node creation.
class LLSelectNode;
void ss_seed_local_select_node(LLSelectNode* nodep);

// The record backing a live object in the ACTIVE track - used by seating and the floater
// list. Null when the object's mesh is not in the active track at all.
const SSAtmoEnvLandscape* ss_landscape_record_for_mesh(const LLUUID& mesh_id);

// Name/desc write-back from the stock General tab. Called from LLSelectMgr's
// selectionSetObjectName/Description when the selection is a local-content landscape
// object: the send funnel ignores local content (no sim to tell), so the record - the
// authoritative store - is updated here instead. Scans all tracks (a same-mesh record may
// sit in another track); refreshes only the LIVE object's capture baseline, never the
// placement.
void ss_landscape_persist_name(const LLUUID& mesh_id, const std::string& name, const std::string& desc);
// </SS:Nexii>

#endif // SS_ATMO_LANDSCAPE_H