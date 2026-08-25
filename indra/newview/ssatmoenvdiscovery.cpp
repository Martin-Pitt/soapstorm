/**
 * @file ssatmoenvdiscovery.cpp
 * @brief Atmo Magic parcel discovery and Bridge notecard fetch
 *        implementation. See the header for the Bridge (LSL) contract this
 *        assumes - indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt's
 *        "FetchNotecard" command.
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

// <SS:Nexii> Atmo Magic: parcel discovery and Bridge notecard fetch

namespace
{
    const char* CONFIG_TAG = "atmo:";
    const char* FETCH_COMMAND = "FetchNotecard|";

    // Notecards are immutable once created - the same asset uuid is always the same content, forever - so caching a fetched one keyed purely by uuid never goes stale. Written wrapped in the same
    // Linden text container a real notecard asset already carries, so a cache entry is byte-shape-identical to whatever gAssetStorage would have fetched from the network; reading it back needs no
    // special-casing for "did this come from the Bridge or the real asset system".
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

    // The inverse of the above - null/empty if there's no cache entry or it doesn't parse as a notecard container.
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

SSAtmoEnvDiscoveryManager::SSAtmoEnvDiscoveryManager()
{
    // Fires when the agent crosses into a different parcel and when the current parcel's properties are re-sent, which is what an owner editing the description produces - same event-driven pattern
    // v2's own SSAtmoTrackManager already uses, no polling needed.
    LLViewerParcelMgr::getInstance()->addObserver(this);
}

SSAtmoEnvDiscoveryManager::~SSAtmoEnvDiscoveryManager()
{
    if (LLViewerParcelMgr::instanceExists())
    {
        LLViewerParcelMgr::getInstance()->removeObserver(this);
    }
}

// static
LLUUID SSAtmoEnvDiscoveryManager::parseDescription(const std::string& desc)
{
    // Same scan-for-marker-then-next-36-characters approach as v2's own SSAtmoTrackManager::parseDescription, same reason: keeps the marker findable inside ordinary prose so a parcel description
    // stays readable rather than needing to be exactly the tag and nothing else.
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

void SSAtmoEnvDiscoveryManager::changed()
{
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const std::string desc = parcel ? parcel->getDesc() : LLStringUtil::null;
    const LLUUID asset_id = parseDescription(desc);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    // Mid-edit is mid-edit, in both directions: the same courtesy that stops a parcel's environment clobbering an open editor also stops a parcel boundary yanking one away while it is being worked
    // on.
    const bool editing = editorIsOpen();

    if (asset_id.isNull())
    {
        // No marker here. Release what a parcel put on us, and only that - see the header.
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
        return; // already applied and still loaded - genuinely nothing to do
    }
    if (asset_id == mPendingAssetId) return; // already asked, still waiting on the reply

    // A DIFFERENT parcel environment: the one in hand is not the one this parcel is asking for, so it goes. Dropping it before the fetch rather than after means crossing onto a parcel whose notecard
    // cannot be reached shows the EEP fallback instead of the previous parcel's sky.
    if (!editing && mgr->hasAsset() && mgr->cameFromParcel() && mAppliedAssetId != asset_id)
    {
        mgr->unload();
        mAppliedAssetId.setNull();
    }

    requestFetch(asset_id);
}

void SSAtmoEnvDiscoveryManager::requestFetch(const LLUUID& asset_id)
{
    // Notecards are immutable - the same uuid is always the same content, forever - so a cache hit never needs the Bridge at all. If it can't be applied right now (floater open, content rejected),
    // that is the same "leave it retryable" rule as everywhere else here, not a reason to also go ask the Bridge for content already in hand.
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

    // Weak-ish capture by value is fine here: this is a singleton that outlives any request it starts, so there is no dangling-this risk to guard against the way there would be for a floater or
    // panel.
    FSLSLBridge::instance().viewerToLSL(
        std::string(FETCH_COMMAND) + asset_id.asString(),
        [this, asset_id](const LLSD& data) { onFetchResult(asset_id, data); });
}

void SSAtmoEnvDiscoveryManager::onFetchResult(const LLUUID& asset_id, const LLSD& data)
{
    // A newer request may have superseded this one while it was in flight.
    if (asset_id != mPendingAssetId) return;
    mPendingAssetId.setNull();

    if (!data.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT))
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 fetch for " << asset_id << " returned no content" << LL_ENDL;
        return;
    }

    const LLSD& content = data[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT];

    // Reduced to plain text once here, regardless of whether the Bridge passed through a parsed LLSD map (the common case: our own assets already start with "<llsd>") or a bare string (the wrapped
    // fallback) - one thing to cache, one thing to apply, rather than duplicating both branches in two places.
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

    // Cached unconditionally, before the apply attempt: even a floater- open suppression or a rejected parse still means we now have the content in hand, and the next retry (via changed()) should
    // read it back from here rather than hitting the Bridge a second time for something that never changes anyway.
    cacheNotecardBody(asset_id, text);

    applyText(asset_id, text);
}

// Whether the editor is open on screen. Auto-apply is suppressed while it is - the "New Atmo Magic" creation flow's own "mid-edit, don't clobber" rule, see doc/atmo_magic_environment.md - and so is
// the release that happens on leaving a configured parcel: an author working on a notecard should not have it taken away because they walked over a line.
bool SSAtmoEnvDiscoveryManager::editorIsOpen()
{
    LLFloater* floater = LLFloaterReg::findInstance("ss_atmo_env");
    return floater && floater->getVisible();
}

bool SSAtmoEnvDiscoveryManager::applyText(const LLUUID& asset_id, const std::string& text)
{
    if (editorIsOpen())
    {
        LL_INFOS("AtmoMagicEnv") << "Atmo v3 environment " << asset_id
                                 << " available but not applied - floater is open" << LL_ENDL;
        return false;
    }

    const bool applied = SSAtmoEnvManager::getInstance()->applyExternalNotecardText(asset_id, text);

    // Only a genuine success marks this uuid as handled. A rejected/ invalid notecard is left retryable too - the alternative (permanently giving up) is worse than occasionally re-attempting a
    // notecard that turns out to be broken.
    if (applied)
    {
        mAppliedAssetId = asset_id;

        // Marks this environment as the PARCEL's rather than the user's, so leaving the parcel knows it may release it - see changed().
        SSAtmoEnvManager::getInstance()->noteSource(asset_id, true);
    }
    else
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment " << asset_id << " fetched but rejected" << LL_ENDL;
    }
    return applied;
}

// </SS:Nexii>
