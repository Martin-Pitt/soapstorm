/**
 * @file ssatmoenvdiscovery.h
 * @brief Atmo Magic: parcel discovery and notecard fetch.
 *
 *        Fetch is a plain HTTP round trip through the existing Bridge
 *        plumbing (FSLSLBridge::viewerToLSL), not a chat-chunked relay -
 *        see indra/newview/fs_resources/EBEDD1D2-...-D47BBCA5DFB.lsltxt's
 *        "FetchNotecard" command for the LSL side: it reads the notecard
 *        synchronously (llGetNotecardLineSync, no object-inventory step
 *        needed) and replies over llHTTPResponse with the notecard body,
 *        passed straight through as the response's own LLSD document when
 *        it already starts with "<llsd>" (true for every v3 asset, since
 *        that's exactly what LLSDSerialize::toPrettyXML emits) - which
 *        means the viewer-side callback below gets the asset's LLSD map
 *        directly, no text reassembly step of any kind.
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

#ifndef SS_ATMOENVDISCOVERY_H
#define SS_ATMOENVDISCOVERY_H

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "llviewerparcelmgr.h"

#include <string>

class SSAtmoEnvDiscoveryManager : public LLSingleton<SSAtmoEnvDiscoveryManager>, public LLParcelObserver
{
    LLSINGLETON(SSAtmoEnvDiscoveryManager);
    ~SSAtmoEnvDiscoveryManager();

public:
    void changed() override;

    static bool editorIsOpen();

    static LLUUID parseDescription(const std::string& desc);

private:
    void requestFetch(const LLUUID& asset_id);

    void onFetchResult(const LLUUID& asset_id, const LLSD& data);

    bool applyText(const LLUUID& asset_id, const std::string& text);

    LLUUID mAppliedAssetId;
    LLUUID mPendingAssetId;
};

#endif
