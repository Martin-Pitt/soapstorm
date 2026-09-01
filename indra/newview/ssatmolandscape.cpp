/**
 * @file ssatmolandscape.cpp
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

#include "llviewerprecompiledheaders.h"

#include "ssatmolandscape.h"

#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"

#include "llpermissionsflags.h"
#include "llselectmgr.h"

#include "pipeline.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvmanager.h"

namespace
{
    // The capture throttle: the reconcile funnel writes the working asset at a few Hz, so a
    // mid-drag object churns the notecard save only when the author actually rests.
    constexpr F32 SS_LANDSCAPE_CAPTURE_INTERVAL = 0.25f;
}

bool ss_landscape_item_fullperm(const LLInventoryItem* item)
{
    const LLViewerInventoryItem* viewer_item = dynamic_cast<const LLViewerInventoryItem*>(item);
    return viewer_item && viewer_item->getIsFullPerm();
}

const SSAtmoEnvLandscape* ss_landscape_record_for_mesh(const LLUUID& mesh_id)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    if (!mgr->hasAsset() || !applier->isActive())
    {
        return nullptr;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        return nullptr;
    }
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    for (const SSAtmoEnvLandscape& record : records)
    {
        if (record.mMeshId == mesh_id)
        {
            return &record;
        }
    }
    return nullptr;
}

void ss_seed_local_select_node(LLSelectNode* nodep)
{
    if (!nodep || !nodep->getObject() || !nodep->getObject()->ssIsLocalContent())
    {
        return;
    }

    // Records are keyed by mesh asset uuid; only landscape objects carry one.
    const SSAtmoLandscapeObject* landscape = dynamic_cast<const SSAtmoLandscapeObject*>(nodep->getObject());
    const SSAtmoEnvLandscape* record = landscape
        ? ss_landscape_record_for_mesh(landscape->meshId()) : nullptr;
    if (!record)
    {
        return;
    }

    // The node is the stock editor's view of an object: seed what the sim would normally
    // reply with. Permission AND-masks make every stock gate treat the object as the
    // full-perm, agent-owned object it is.
    nodep->mValid = true;
    nodep->mName = record->mName;
    nodep->mDescription = record->mDesc;
    nodep->mCreationDate = (U64)record->mCreated;
    nodep->mPermissions->init(record->mCreator, gAgentID, record->mLastOwner, LLUUID::null);
    nodep->mPermissions->initMasks(PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL, PERM_ALL);
}

void SSAtmoLandscapeWorld::clearLandscapeObjects()
{
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull())
        {
            gObjectList.killObject(objp);
        }
    }
    mObjects.clear();
}

void SSAtmoLandscapeWorld::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);
    static LLCachedControl<bool> landscape_enabled(gSavedSettings, "SSAtmoLandscape", false);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();

    const bool want_active = enabled && landscape_enabled && mgr->hasAsset() && applier->isActive();
    if (!want_active)
    {
        if (!mObjects.empty())
        {
            clearLandscapeObjects();
        }
        mLastSignature.clear();
        mAgentRegionHandle = 0;
        return;
    }

    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    // The author's hand is on a scenery object: defer reshapes (track/region/record-set
    // changes) until nothing is selected, so a mid-drag reconciliation cannot snap the
    // object back to the stale record. Capture continues - the drag is preserved at
    // capture cadence, and the deferred reshape then adopts the captured state.
    bool editing = false;
    for (const LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull() && objp->isSelected())
        {
            editing = true;
            break;
        }
    }

    // Agent region change rebuilds the whole set: objects are anchored to a region origin
    // (locked records re-anchor by construction), and kill-and-recreate is the SSWaterWorld
    // idiom - the mesh repo's cache makes recreation cheap and pop-free.
    const U64 handle = region->getHandle();
    if (!editing && handle != mAgentRegionHandle)
    {
        mAgentRegionHandle = handle;
        clearLandscapeObjects();
        mLastSignature.clear();
    }

    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        track = 0;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;

    // The signature is the active track's mesh-id run PLUS the track index itself: any reshape
    // (track crossing, load, revert, floater add/delete/reorder) reshapes the live set;
    // content edits inside a record (the reconcile's own capture writes) deliberately do not.
    std::string sig;
    sig += llformat("t%d;", track);
    for (const SSAtmoEnvLandscape& r : records)
    {
        sig += r.mMeshId.asString();
        sig += ';';
    }
    if (!editing && sig != mLastSignature)
    {
        mLastSignature = sig;
        reconcile(asset, track, region);
    }

    applyFacesToAll();

    // Capture only while the live set matches the record set. While an object is selected a
    // reshape is deferred by design, and writing the OLD objects' state into the NEW track's
    // records would be cross-track contamination - so the capture skips that window and
    // resumes once the deferred reshape has adopted the new records.
    if (!editing && mCaptureTimer.getElapsedTimeF32() > SS_LANDSCAPE_CAPTURE_INTERVAL)
    {
        mCaptureTimer.reset();
        captureAll(records);
    }
}

void SSAtmoLandscapeWorld::reconcile(const SSAtmoEnvAsset& asset, S32 track_index, LLViewerRegion* regionp)
{
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track_index)].mLandscapes;

    std::vector<LLPointer<SSAtmoLandscapeObject>> next;
    next.reserve(records.size());

    // Pairing consumes: each live object is matched at most once, in record order, so two
    // records holding the same mesh each get their own instance (and keep their own state)
    // instead of both collapsing onto the first match.
    std::vector<bool> taken(mObjects.size(), false);

    for (const SSAtmoEnvLandscape& record : records)
    {
        // UUID adoption: a live object with the same mesh keeps its instance and its loaded
        // mesh, adopting the record's placement + face state - no re-download, no rebuild pop.
        SSAtmoLandscapeObject* match = nullptr;
        for (size_t oi = 0; oi < mObjects.size(); ++oi)
        {
            if (!taken[oi] && mObjects[oi].notNull() && mObjects[oi]->meshId() == record.mMeshId)
            {
                match = mObjects[oi].get();
                taken[oi] = true;
                break;
            }
        }

        if (match)
        {
            match->applyRecord(record);
        }
        else
        {
            match = createObject(regionp, record);
        }
        next.push_back(match);
    }

    // Kill whatever did not survive the pairing - including surplus duplicates of a mesh id.
    for (size_t oi = 0; oi < mObjects.size(); ++oi)
    {
        if (!taken[oi] && mObjects[oi].notNull())
        {
            gObjectList.killObject(mObjects[oi]);
        }
    }

    mObjects = std::move(next);
}

SSAtmoLandscapeObject* SSAtmoLandscapeWorld::createObject(LLViewerRegion* regionp, const SSAtmoEnvLandscape& record)
{
    LLUUID id;
    id.generate();

    SSAtmoLandscapeObject* objp = new SSAtmoLandscapeObject(id, regionp, record);
    if (!gObjectList.adoptViewerObject(objp))
    {
        delete objp;
        return nullptr;
    }
    gPipeline.createObject(objp);
    return objp;
}

void SSAtmoLandscapeWorld::applyFacesToAll()
{
    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull())
        {
            objp->applyFaces();
        }
    }
}

void SSAtmoLandscapeWorld::captureAll(std::vector<SSAtmoEnvLandscape>& records)
{
    // Reconcile builds mObjects in record order and capture only runs when the sets are
    // matched, so index pairing is exact - including for two records holding the same mesh,
    // which each keep their own instance and their own captured state. The size guard is a
    // belt-and-braces fallback to safe first-match pairing for any transient mismatch.
    if (mObjects.size() == records.size())
    {
        for (size_t i = 0; i < mObjects.size(); ++i)
        {
            if (mObjects[i].notNull())
            {
                mObjects[i]->captureToRecord(records[i]);
            }
        }
        return;
    }

    for (LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.isNull())
        {
            continue;
        }
        for (SSAtmoEnvLandscape& record : records)
        {
            if (record.mMeshId == objp->meshId())
            {
                objp->captureToRecord(record);
                break;
            }
        }
    }
}

S32 SSAtmoLandscapeWorld::recordCount() const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return 0;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return 0;
    }
    return (S32)asset.mTracks[static_cast<size_t>(track)].mLandscapes.size();
}

const SSAtmoEnvLandscape* SSAtmoLandscapeWorld::recordAt(S32 index) const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return nullptr;
    }
    const SSAtmoEnvAsset& asset = mgr->asset();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return nullptr;
    }
    const std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    if (index < 0 || index >= (S32)records.size())
    {
        return nullptr;
    }
    return &records[static_cast<size_t>(index)];
}

bool SSAtmoLandscapeWorld::toggleRecordLock(S32 index)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return true;
    }
    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track < 0 || track >= (S32)asset.mTracks.size())
    {
        return true;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    if (index < 0 || index >= (S32)records.size())
    {
        return true;
    }
    SSAtmoEnvLandscape& record = records[static_cast<size_t>(index)];

    if (record.mLocked)
    {
        // Locked -> free: the global position is what the object is currently at.
        record.mFreeGlobal = gAgent.getRegion()
            ? gAgent.getRegion()->getPosGlobalFromRegion(record.mLockedOffset)
            : LLVector3d(record.mLockedOffset.mV[0], record.mLockedOffset.mV[1], record.mLockedOffset.mV[2]);
    }
    else
    {
        // Free -> locked: the region-local offset is where the object currently is.
        record.mLockedOffset = gAgent.getRegion()
            ? gAgent.getRegion()->getPosRegionFromGlobal(record.mFreeGlobal)
            : LLVector3((F32)record.mFreeGlobal.mdV[VX], (F32)record.mFreeGlobal.mdV[VY], (F32)record.mFreeGlobal.mdV[VZ]);
    }
    record.mLocked = !record.mLocked;

    // Re-apply so the object follows the new mode this frame - content edits do not trip the
    // reconcile signature, so this must apply directly. Index-paired like capture; the
    // size-guarded fallback covers a transient mismatch.
    SSAtmoLandscapeObject* objp = nullptr;
    if (index >= 0 && index < (S32)mObjects.size())
    {
        objp = mObjects[static_cast<size_t>(index)].get();
    }
    if (!objp)
    {
        for (LLPointer<SSAtmoLandscapeObject>& o : mObjects)
        {
            if (o.notNull() && o->meshId() == record.mMeshId)
            {
                objp = o.get();
                break;
            }
        }
    }
    if (objp)
    {
        objp->applyRecord(record);
    }

    return record.mLocked;
}

S32 SSAtmoLandscapeWorld::addFromItem(const LLInventoryItem* item, std::string& out_reason)
{
    out_reason.clear();

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    if (!mgr->hasAsset() || !applier->isActive())
    {
        out_reason = "no active environment";
        return -1;
    }
    if (!gAgent.getRegion())
    {
        out_reason = "no region";
        return -1;
    }

    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, applier->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        out_reason = "no active track";
        return -1;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;

    if ((S32)records.size() >= SS_ATMOENV_MAX_LANDSCAPE_PER_TRACK)
    {
        out_reason = "track landscape cap reached";
        return -1;
    }
    S32 total = 0;
    for (SSAtmoEnvTrack& t : asset.mTracks)
    {
        total += (S32)t.mLandscapes.size();
    }
    if (total >= SS_ATMOENV_MAX_LANDSCAPE_TOTAL)
    {
        out_reason = "environment landscape cap reached";
        return -1;
    }

    // <SS:Nexii> The drop queue: this exact flow is the inventory-drop contract the floater
    // panel uses. The full-perm gate is the panel's job (ss_landscape_item_fullperm) - this
    // method records and hydrates only.
    SSAtmoEnvLandscape record;
    record.mMeshId = item->getAssetUUID();
    record.mName = item->getName();
    record.mDesc = item->getDescription();
    record.mCreator = item->getCreatorUUID();
    record.mLastOwner = item->getPermissions().getLastOwner();
    record.mCreated = (F64)item->getCreationDate();
    record.mLocked = true;
    record.mRotation = LLQuaternion();

    // Default placement: where the agent stands, so the mesh appears in front of the author
    // and headline-first. The editor moves it from there like any object.
    if (gAgent.getRegion())
    {
        record.mLockedOffset = gAgent.getPositionRegion();
        record.mLockedOffset.mV[VZ] += 8.f;
        record.mFreeGlobal = gAgent.getPositionGlobal();
        record.mFreeGlobal.mdV[VZ] += 8.0;
    }

    const S32 index = (S32)records.size();
    records.push_back(record);

    // Hydrate now: the floater wants the scenery visible this click, not next frame. But a
    // drop lands mid-edit (another scenery object selected): defer like any reshape so an
    // in-flight drag never snaps - the drop just waits out the drag.
    bool editing = false;
    for (const LLPointer<SSAtmoLandscapeObject>& objp : mObjects)
    {
        if (objp.notNull() && objp->isSelected())
        {
            editing = true;
            break;
        }
    }
    if (editing)
    {
        mLastSignature.clear();
    }
    else
    {
        reconcile(asset, track, gAgent.getRegion());
    }

    return index;
}

bool SSAtmoLandscapeWorld::removeRecord(S32 index)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset())
    {
        return false;
    }
    SSAtmoEnvAsset& asset = mgr->editable();
    S32 track = llmax(0, SSAtmoEnvApplier::getInstance()->primaryTrackIndex());
    if (track >= (S32)asset.mTracks.size())
    {
        return false;
    }
    std::vector<SSAtmoEnvLandscape>& records = asset.mTracks[static_cast<size_t>(track)].mLandscapes;
    if (index < 0 || index >= (S32)records.size())
    {
        return false;
    }

    records.erase(records.begin() + index);
    clearLandscapeObjects();
    mLastSignature.clear();
    return true;
}