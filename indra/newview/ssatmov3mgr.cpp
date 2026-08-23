/**
 * @file ssatmov3mgr.cpp
 * @brief Atmo Magic v3 environment manager implementation.
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

#include "ssatmov3mgr.h"

#include <cstring>
#include <sstream>

#include "llagent.h"
#include "llassetstorage.h"
#include "llfilesystem.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llpermissionsflags.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "roles_constants.h"

// <SS:Nexii> Atmo Magic v3: environment manager

SSAtmoV3Mgr::SSAtmoV3Mgr()
{
    // No baseline, no working set. See doc/atmo_magic_v3_environment.md -
    // there is deliberately nothing to fall back to here.
}

bool SSAtmoV3Mgr::isModified() const
{
    if (!mHasAsset) return false;
    return !llsd_equals(mWorking.asLLSD(), mBaseline.asLLSD());
}

void SSAtmoV3Mgr::revertToBaseline()
{
    if (!mHasAsset) return;
    mWorking = mBaseline;
}

//-----------------------------------------------------------------------------
// Notecard body helpers - shared by createDefaultNotecard() and
// saveAsNewNotecard(), which differ only in which asset gets serialised.
//-----------------------------------------------------------------------------

namespace
{
    // Wraps a v3 asset's LLSD in the Linden notecard container so the result
    // opens like any other notecard, and fires create_inventory_item +
    // an asset upload the same way v2's SSAtmoTrackMgr::exportToNotecard
    // does. name is both the inventory item's name and, if the caller wants
    // it, worth also writing into the asset's own mName before calling this.
    //
    // on_created fires once the asset body has actually finished
    // uploading - not right after the bare inventory-item-metadata create
    // step. Calling it that early was a real bug: the item existed but its
    // content hadn't landed yet, so a caller that immediately tried to load
    // it back (the floater's Create button did exactly this) could race
    // against an item with no asset on it yet and silently do nothing.
    // Either id is null if creation or upload failed.
    void writeAssetAsNotecard(const SSAtmoV3Asset& asset, const std::string& name,
                               const LLUUID& parent_id_in,
                               std::function<void(const LLUUID& item_id, const LLUUID& asset_id)> on_created)
    {
        std::ostringstream body;
        LLSDSerialize::toPrettyXML(asset.asLLSD(), body);

        LLNotecard nc(LLNotecard::MAX_SIZE);
        nc.setText(body.str());
        std::ostringstream wrapped;
        nc.exportStream(wrapped);
        const std::string asset_text = wrapped.str();

        const LLUUID parent_id = parent_id_in.notNull()
            ? parent_id_in
            : gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);

        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
            [asset_text, name, on_created](const LLUUID& new_item_id)
            {
                LLViewerRegion* region = gAgent.getRegion();
                const std::string url = region
                    ? region->getCapability("UpdateNotecardAgentInventory")
                    : std::string();
                if (new_item_id.isNull() || url.empty())
                {
                    LL_WARNS("AtmoMagicV3") << "Could not create Atmo v3 notecard item" << LL_ENDL;
                    if (on_created) on_created(LLUUID::null, LLUUID::null);
                    return;
                }

                LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
                    new_item_id, LLAssetType::AT_NOTECARD, asset_text,
                    [name, new_item_id, on_created](LLUUID, LLUUID new_asset_id, LLUUID, LLSD)
                    {
                        LL_INFOS("AtmoMagicV3") << "Saved Atmo v3 environment '" << name
                                                 << "' as asset " << new_asset_id << LL_ENDL;
                        if (on_created) on_created(new_item_id, new_asset_id);
                    },
                    nullptr);

                LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            });

        create_inventory_item(gAgentID, gAgentSessionID, parent_id, LLTransactionID::tnull,
                               name, "Atmo Magic v3 environment", LLAssetType::AT_NOTECARD,
                               LLInventoryType::IT_NOTECARD, NO_INV_SUBTYPE, PERM_ALL, cb);
    }
}

//-----------------------------------------------------------------------------
// Creation
//-----------------------------------------------------------------------------

// static
void SSAtmoV3Mgr::createDefaultNotecard(const LLUUID& parent_id,
                                         std::function<void(const LLUUID& item_id, const LLUUID& asset_id)> on_created)
{
    const SSAtmoV3Asset def = SSAtmoV3Asset::makeDefault();
    writeAssetAsNotecard(def, def.mName, parent_id, on_created);
}

void SSAtmoV3Mgr::saveAsNewNotecard(const std::string& name)
{
    if (!mHasAsset) return;

    std::string save_name = name;
    LLStringUtil::trim(save_name);
    if (save_name.empty()) save_name = "Atmo Environment";

    mWorking.mName = save_name;
    writeAssetAsNotecard(mWorking, save_name, LLUUID::null, nullptr);

    // Saving adopts the just-written state as the new baseline; there is no
    // separate "confirm the upload actually landed" step here, matching the
    // v2 floater's behaviour (exportToNotecard has the same shape).
    mBaseline = mWorking;
}

//-----------------------------------------------------------------------------
// Loading
//-----------------------------------------------------------------------------

bool SSAtmoV3Mgr::loadFromInventory(const LLInventoryItem* item)
{
    if (!item || item->getAssetUUID().isNull()) return false;

    if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE)
        && !gAgent.isGodlike())
    {
        mStatus = "no permission to read that notecard";
        return false;
    }

    mPendingID = item->getAssetUUID();
    mStatus = "loading environment...";

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        return false;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoV3Mgr::onAssetLoaded, nullptr, true);
    return true;
}

void SSAtmoV3Mgr::loadFromAssetId(const LLUUID& asset_id)
{
    // See the header note and the open item in doc/atmo_magic_v3_environment.md:
    // this path does not check ownership, because a parcel-referenced
    // notecard is not necessarily something the agent has a copy of. Kept
    // separate from loadFromInventory() rather than folded together so that
    // distinction stays visible at the call site, not buried in a flag.
    mPendingID = asset_id;
    mStatus = "loading environment...";

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        return;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoV3Mgr::onAssetLoaded, nullptr, true);
}

// static
void SSAtmoV3Mgr::onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status)
{
    SSAtmoV3Mgr* self = SSAtmoV3Mgr::getInstance();

    // A newer request may have superseded this fetch while it was in flight
    if (asset_id != self->mPendingID) return;
    self->mPendingID.setNull();

    if (status != 0)
    {
        LL_WARNS("AtmoMagicV3") << "Atmo v3 environment notecard " << asset_id
                                 << " failed to load, status " << status << LL_ENDL;
        self->mStatus = "notecard unavailable";
        return;
    }

    LLFileSystem file(asset_id, type, LLFileSystem::READ);
    const S32 length = file.getSize();
    if (length <= 0)
    {
        self->mStatus = "notecard empty";
        return;
    }

    std::vector<char> buffer(length + 1);
    file.read((U8*)buffer.data(), length);
    buffer[length] = '\0';

    // Notecard assets are wrapped in the Linden text container; unwrap to
    // the plain body, but tolerate a bare text asset too - same tolerance
    // v2's SSAtmoTrackMgr::onNotecardLoaded applies.
    std::string text(buffer.data(), length);
    if (length > 19 && strncmp(buffer.data(), "Linden text version", 19) == 0)
    {
        LLNotecard notecard;
        std::istringstream stream(text);
        if (!notecard.importStream(stream))
        {
            LL_WARNS("AtmoMagicV3") << "Could not parse Atmo v3 notecard " << asset_id << LL_ENDL;
            self->mStatus = "notecard unreadable";
            return;
        }
        text = notecard.getText();
    }

    self->mAssetID = asset_id;
    self->applyNotecardText(text, false);
}

bool SSAtmoV3Mgr::applyNotecardText(const std::string& text, bool /*from_inventory_permission_check*/)
{
    LLSD sd;
    std::istringstream stream(text);

    bool parsed = false;
    if (text.find("<llsd") != std::string::npos)
    {
        parsed = (LLSDSerialize::fromXML(sd, stream) != LLSDParser::PARSE_FAILURE);
    }
    if (!parsed)
    {
        std::istringstream retry(text);
        parsed = (LLSDSerialize::fromNotation(sd, retry, (S32)text.size()) != LLSDParser::PARSE_FAILURE);
    }

    if (!parsed)
    {
        LL_WARNS("AtmoMagicV3") << "Atmo v3 environment notecard is not valid LLSD" << LL_ENDL;
        mStatus = "notecard is not valid LLSD";
        return false;
    }

    return adoptParsedAsset(sd);
}

bool SSAtmoV3Mgr::applyExternalLLSD(const LLUUID& source_id, const LLSD& sd)
{
    mAssetID = source_id;
    return adoptParsedAsset(sd);
}

bool SSAtmoV3Mgr::applyExternalNotecardText(const LLUUID& source_id, const std::string& text)
{
    mAssetID = source_id;
    return applyNotecardText(text, false);
}

bool SSAtmoV3Mgr::adoptParsedAsset(const LLSD& sd)
{
    SSAtmoV3Asset parsed_asset;
    std::string error;
    if (!parsed_asset.fromLLSD(sd, error))
    {
        LL_WARNS("AtmoMagicV3") << "Atmo v3 environment notecard rejected: " << error << LL_ENDL;
        mStatus = "notecard invalid: " + error;
        return false;
    }

    mBaseline = parsed_asset;
    mWorking = parsed_asset;
    mHasAsset = true;
    mStatus = "loaded";
    return true;
}

// </SS:Nexii>
