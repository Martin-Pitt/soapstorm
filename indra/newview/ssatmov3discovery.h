/**
 * @file ssatmov3discovery.h
 * @brief Atmo Magic v3: parcel-description discovery ("atmo:<uuid>", the
 *        exact marker the now-retired v2 weather layer used - see
 *        doc/atmo_magic_v3_environment.md's untangle plan, there is no live
 *        collision to disambiguate once v2 is gone) and the notecard fetch
 *        this triggers over the SL Bridge. Phase 8.
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

#ifndef SS_ATMOV3DISCOVERY_H
#define SS_ATMOV3DISCOVERY_H

// <SS:Nexii> Atmo Magic v3: parcel discovery and Bridge notecard fetch

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "llviewerparcelmgr.h"

#include <string>

// Watches the agent's current parcel for an "atmo:<uuid>" marker and
// requests the matching notecard over the SL Bridge when one appears.
// Auto-applies the result unless the v3 floater is open (treated as
// "mid-edit, don't clobber") - see doc/atmo_magic_v3_environment.md.
class SSAtmoV3DiscoveryMgr : public LLSingleton<SSAtmoV3DiscoveryMgr>, public LLParcelObserver
{
    LLSINGLETON(SSAtmoV3DiscoveryMgr);
    ~SSAtmoV3DiscoveryMgr();

public:
    // LLParcelObserver: fires both on crossing into a different parcel and
    // when the current parcel's properties are re-sent (editing the
    // description produces this too). Leaving a configured parcel does
    // nothing special by design - unlike v2, there is no implicit default
    // to fall back to, so a v3 environment picked up from a parcel just
    // persists until something else replaces it, the same as one loaded by
    // hand would.
    //
    // Retries on every call until the referenced UUID has actually been
    // applied, not merely attempted - a fetch that was suppressed because
    // the floater was open (see onFetchResult) is not "done" just because
    // it was asked for once; the next parcel event (crossing back in,
    // re-editing the description) is what gives it another chance.
    void changed() override;

    // "atmo:<uuid>" out of a parcel description, in any position and case -
    // same convention, same tolerance, as v2's own
    // SSAtmoTrackMgr::parseDescription.
    static LLUUID parseDescription(const std::string& desc);

private:
    // Notecards are immutable once created - the same asset uuid is
    // always the same content, forever - so a fetch first checks the
    // local cache (see ssatmov3discovery.cpp's cacheNotecardBody/
    // readCachedNotecardBody) and only asks the Bridge on a genuine miss.
    // Sends "FetchNotecard|<uuid>" via FSLSLBridge::viewerToLSL - an HTTP
    // POST under the hood, answered asynchronously by onFetchResult()
    // below. No-op (logged, not fatal) if the agent has no usable Bridge
    // attached.
    void requestFetch(const LLUUID& asset_id);

    // Callback for FSLSLBridge::viewerToLSL's success path. aData is the
    // full HTTP coroutine adapter result; the notecard body itself is at
    // aData[HttpCoroutineAdapter::HTTP_RESULTS_CONTENT], already parsed as
    // LLSD by the HTTP layer since the Bridge script sends it as an
    // "<llsd>...</llsd>" document, not a bare string, whenever the
    // notecard's own content already is one - which every v3 asset's is.
    // Caches the result (successful or not yet applied) before handing it
    // to applyText() below, so a floater-open suppression here still
    // leaves the content available locally for the retry a later
    // changed() triggers, with no second Bridge round trip.
    void onFetchResult(const LLUUID& asset_id, const LLSD& data);

    // Shared tail for both the cache-hit and freshly-fetched paths:
    // applies plain notecard text, honours "don't clobber an open
    // floater", and marks mAppliedAssetId on success. True on success.
    bool applyText(const LLUUID& asset_id, const std::string& text);

    // Deliberately two separate ids, not one: "asked for" and "actually
    // applied" are different states, and conflating them was a real bug -
    // a fetch suppressed by the floater being open would otherwise mark
    // the uuid as handled forever, with no later parcel event ever giving
    // it another chance.
    LLUUID mAppliedAssetId;  // uuid successfully applied - re-seeing it is a genuine no-op
    LLUUID mPendingAssetId;  // fetch currently in flight - avoids firing a duplicate concurrent request
};

// </SS:Nexii>

#endif // SS_ATMOV3DISCOVERY_H
