/**
 * @file ssatmoenvmanager.h
 * @brief Atmo Magic: environment asset load/hold/persist.
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
    bool hasAsset() const { return mHasAsset; }

    void unload();

    const LLUUID& sourceAssetId() const { return mSourceAssetId; }
    bool cameFromParcel() const { return mFromParcel; }
    void noteSource(const LLUUID& asset_id, bool from_parcel)
    {
        mSourceAssetId = asset_id;
        mFromParcel = from_parcel;
    }

    const SSAtmoEnvAsset& asset() const { return mWorking; }
    SSAtmoEnvAsset& editable() { return mWorking; }

    bool isModified() const;
    void revertToBaseline();

    static void createDefaultNotecard(const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created);

    void adoptCreated(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset);

    static void atmoFolderId(std::function<void(const LLUUID&)> on_ready);

    bool loadFromInventory(const LLInventoryItem* item, std::function<void(bool success)> on_complete = nullptr);

    void loadFromAssetId(const LLUUID& asset_id);

    bool applyExternalLLSD(const LLUUID& source_id, const LLSD& sd);
    bool applyExternalNotecardText(const LLUUID& source_id, const std::string& text);

    void saveNotecard(const std::string& name);

    const std::string& statusText() const { return mStatus; }
    const std::string& configName() const { return mHasAsset ? mWorking.mName : mNoAssetName; }
    const LLUUID& configAssetId() const { return mAssetID; }

    void setPreviewPhaseOverride(F64 phase) { mPreviewOverrideActive = true; mPreviewOverridePhase = phase; }
    void clearPreviewPhaseOverride() { mPreviewOverrideActive = false; }
    bool hasPreviewPhaseOverride() const { return mPreviewOverrideActive; }
    F64 previewPhaseOverride() const { return mPreviewOverridePhase; }

private:
    static void onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                               void* user_data, S32 status, LLExtStat ext_status);
    bool applyNotecardText(const std::string& text, bool from_inventory_permission_check);

    bool adoptParsedAsset(const LLSD& sd);

    void updateExistingNotecard(const std::string& name);

    void finishLoad(bool success);

    SSAtmoEnvAsset mBaseline;
    SSAtmoEnvAsset mWorking;
    bool mHasAsset = false;

    LLUUID mSourceAssetId;
    bool mFromParcel = false;

    LLUUID mAssetID;
    LLUUID mItemID;
    LLUUID mPendingID;
    LLUUID mPendingItemID;
    std::function<void(bool)> mLoadCompleteCallback;

    std::string mStatus = "no environment loaded";
    const std::string mNoAssetName = "(none)";

    bool mPreviewOverrideActive = false;
    F64 mPreviewOverridePhase = 0.0;
};

#endif
