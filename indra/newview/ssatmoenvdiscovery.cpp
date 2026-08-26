/**
 * @file ssatmoenvdiscovery.cpp
 * @brief See ssatmoenvdiscovery.h.
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

#include "ssatmoenvdiscovery.h"

#include "fslslbridge.h"
#include "llcorehttputil.h"
#include "llfilesystem.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llnotecard.h"
#include "llparcel.h"
#include "llsdserialize.h"
#include "ssatmoenvmanager.h"

#include <sstream>

namespace
{
    const char* CONFIG_TAG = "atmo:";
    const char* FETCH_COMMAND = "FetchNotecard|";

    // Wraps fetched text in notecard format and caches it under the asset id, so later visits skip the Bridge.
    void cacheNotecardBody(const LLUUID& asset_id, const std::string& plain_body)
    {
        LLNotecard nc(LLNotecard::MAX_SIZE);
        nc.setText(plain_body);
        std::ostringstream wrapped;
        nc.exportStream(wrapped);
        const std::string wrapped_text = wrapped.str();

        LLFileSystem file(asset_id, LLAssetType::AT_NOTECARD, LLFileSystem::WRITE);
        file.write((const U8*)wrapped_text.data(), (S32)wrapped_text.size());
    }

    // Reads a cached notecard body back, unwrapping Linden notecard format when present.
    std::string readCachedNotecardBody(const LLUUID& asset_id)
    {
        if (!LLFileSystem::getExists(asset_id, LLAssetType::AT_NOTECARD)) return std::string();

        LLFileSystem file(asset_id, LLAssetType::AT_NOTECARD, LLFileSystem::READ);
        const S32 length = file.getSize();
        if (length <= 0) return std::string();

        std::vector<char> buffer(length + 1);
        file.read((U8*)buffer.data(), length);
        buffer[length] = '\0';

        std::string text(buffer.data(), length);
        if (length > 19 && strncmp(buffer.data(), "Linden text version", 19) == 0)
        {
            LLNotecard notecard;
            std::istringstream stream(text);
            if (!notecard.importStream(stream)) return std::string();
            text = notecard.getText();
        }
        return text;
    }
}

// Watches parcel changes from construction on.
SSAtmoEnvDiscoveryManager::SSAtmoEnvDiscoveryManager()
{
    LLViewerParcelMgr::getInstance()->addObserver(this);
}

// Stops watching; guarded because the parcel manager may already be gone at shutdown.
SSAtmoEnvDiscoveryManager::~SSAtmoEnvDiscoveryManager()
{
    if (LLViewerParcelMgr::instanceExists())
    {
        LLViewerParcelMgr::getInstance()->removeObserver(this);
    }
}

// Finds the first valid 'atmo:<uuid>' marker in a parcel description.
LLUUID SSAtmoEnvDiscoveryManager::parseDescription(const std::string& desc)
{
    const std::string lower = utf8str_tolower(desc);
    const size_t tag_len = strlen(CONFIG_TAG);
    size_t pos = 0;

    while ((pos = lower.find(CONFIG_TAG, pos)) != std::string::npos)
    {
        size_t start = pos + tag_len;
        while (start < desc.size() && isspace((unsigned char)desc[start])) ++start;

        if (start + UUID_STR_SIZE <= desc.size())
        {
            const std::string candidate = desc.substr(start, UUID_STR_SIZE);
            if (LLUUID::validate(candidate))
            {
                return LLUUID(candidate);
            }
        }
        pos = start;
    }

    return LLUUID::null;
}

// Parcel changed: apply, refetch or unload the parcel environment - never while the editor floater is open.
void SSAtmoEnvDiscoveryManager::changed()
{
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const std::string desc = parcel ? parcel->getDesc() : LLStringUtil::null;
    const LLUUID asset_id = parseDescription(desc);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    const bool editing = editorIsOpen();

    if (asset_id.isNull())
    {
        if (!editing && mgr->hasAsset() && mgr->cameFromParcel())
        {
            LL_INFOS("AtmoMagicEnv") << "Left the parcel that supplied the Atmo Magic"
                                        " environment; falling back to the EEP setting" << LL_ENDL;
            mgr->unload();
            mAppliedAssetId.setNull();
            mPendingAssetId.setNull();
        }
        return;
    }

    if (asset_id == mAppliedAssetId && mgr->hasAsset())
    {
        return;
    }
    if (asset_id == mPendingAssetId) return;

    if (!editing && mgr->hasAsset() && mgr->cameFromParcel() && mAppliedAssetId != asset_id)
    {
        mgr->unload();
        mAppliedAssetId.setNull();
    }

    requestFetch(asset_id);
}

// Cache first, then the LSL Bridge; without a Bridge the environment simply stays unloaded.
void SSAtmoEnvDiscoveryManager::requestFetch(const LLUUID& asset_id)
{
    const std::string cached = readCachedNotecardBody(asset_id);
    if (!cached.empty())
    {
        applyText(asset_id, cached);
        return;
    }

    if (!FSLSLBridge::instanceExists() || !FSLSLBridge::instance().canUseBridge())
    {
        LL_INFOS("AtmoMagicEnv") << "No SL Bridge available - cannot fetch parcel-referenced "
                                   "Atmo v3 notecard " << asset_id << LL_ENDL;
        return;
    }

    mPendingAssetId = asset_id;

    FSLSLBridge::instance().viewerToLSL(
        std::string(FETCH_COMMAND) + asset_id.asString(),
        [this, asset_id](const LLSD& data) { onFetchResult(asset_id, data); });
}

// Bridge reply: cache and apply the fetched notecard text, ignoring stale replies.
void SSAtmoEnvDiscoveryManager::onFetchResult(const LLUUID& asset_id, const LLSD& data)
{
    if (asset_id != mPendingAssetId) return;
    mPendingAssetId.setNull();

    if (!data.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT))
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 fetch for " << asset_id << " returned no content" << LL_ENDL;
        return;
    }

    const LLSD& content = data[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT];

    std::string text;
    if (content.isMap())
    {
        std::ostringstream out;
        LLSDSerialize::toXML(content, out);
        text = out.str();
    }
    else
    {
        text = content.asString();
    }

    cacheNotecardBody(asset_id, text);

    applyText(asset_id, text);
}

// The editor owns the environment while visible - discovery must not stomp an edit in progress.
bool SSAtmoEnvDiscoveryManager::editorIsOpen()
{
    LLFloater* floater = LLFloaterReg::findInstance("ss_atmo_env");
    return floater && floater->getVisible();
}

// Hands fetched text to the manager and records where it came from; refused while the editor is open.
bool SSAtmoEnvDiscoveryManager::applyText(const LLUUID& asset_id, const std::string& text)
{
    if (editorIsOpen())
    {
        LL_INFOS("AtmoMagicEnv") << "Atmo v3 environment " << asset_id
                                 << " available but not applied - floater is open" << LL_ENDL;
        return false;
    }

    const bool applied = SSAtmoEnvManager::getInstance()->applyExternalNotecardText(asset_id, text);

    if (applied)
    {
        mAppliedAssetId = asset_id;

        SSAtmoEnvManager::getInstance()->noteSource(asset_id, true);
    }
    else
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment " << asset_id << " fetched but rejected" << LL_ENDL;
    }
    return applied;
}
