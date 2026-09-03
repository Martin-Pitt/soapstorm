/**
 * @file ssparcelfacts.h
 * @brief A viewer-wide cache of what the simulator knows about each parcel, and a metered way to ask
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_PARCELFACTS_H
#define SS_PARCELFACTS_H

#include "lluuid.h"
#include "v3math.h"

#include <boost/signals2.hpp>

#include <string>

class LLViewerRegion;

// The only parcel the stock viewer keeps is the one the agent is standing on, plus a hover copy and a selection copy - all three of which are overwritten as the user moves and none of which can be asked a question about anywhere else. Anything that wants to know who owns the land under a point, what the parcel is called, what its description says or whether auto-return is armed there has had to either wait for the agent to walk onto it or send its own request and race the others for `LLViewerParcelMgr`'s single agent-parcel slot.
//
// This is that missing thing: a per-region, per-parcel cache that any feature can read and any feature can ask to fill. It is deliberately not owned by the feature that needed it first. Two consumers exist already and they want completely different fields - the region object cache reads owner, group and auto-return to decide whether a remembered object can be swept away, and the weather work reads the parcel DESCRIPTION looking for its own tags - which is exactly the argument for the whole reply being kept rather than the six fields one caller happened to need.
//
// Asking is metered and deduped: one request per DISTINCT parcel, because the reply carries the parcel's AABB and resolves every 4m cell inside it at once, and a private sequence id so a reply can never be mistaken for an update to the parcel the user is standing on. See doc/region_object_cache.md decision 8 for why this traffic is ordinary rather than remarkable.
//
// Main thread only. No locks, and none needed: requests come from feature code, the drain from LLWorld::updateRegions, the replies from the message handler.

// The reply router in LLViewerParcelMgr::processParcelProperties treats sequence id 0, or any id above mAgentParcelSequenceID, as an update to the parcel the agent is STANDING ON (llviewerparcelmgr.cpp:1690-1701), and its tail drives parcel media, the music stream and the land floater off whatever it filled. A private negative id follows the convention the stock viewer already uses for its own out-of-band requests (llparcel.h:91-95) and is dispatched to nothing else.
constexpr S32 SS_PARCELFACTS_SEQ_ID = -70000;

// One parcel, as the simulator described it. Everything in the ParcelProperties reply that is a property OF THE LAND is kept; the per-visit counters that are really about the viewer's own selection are not.
struct SSParcelFacts
{
    SSParcelFacts();

    S32         mLocalID;          // the simulator's own id for this parcel within the region
    LLUUID      mOwnerID;          // the owner, or the GROUP when mGroupOwned - a deeded parcel's group is packed into the owner field
    LLUUID      mGroupID;          // the group the parcel is set to, deeded or not
    bool        mGroupOwned;       // deeded to the group. NOT the same as an object being "set to group"
    S32         mCleanOtherTime;   // auto-return in minutes; 0 means disarmed AND ALSO means withheld, and nothing in the reply separates the two
    S32         mArea;             // square metres
    U32         mParcelFlags;      // PF_* from llparcelflags.h - build, entry, damage, fly, push, voice, and the rest
    U8          mCategory;         // LLParcel::ECategory - the search category the owner chose
    std::string mName;
    std::string mDescription;      // the field the weather work reads its own tags out of
    LLVector3   mAABBMin;          // region-local, and the reason one reply resolves a whole parcel rather than one cell
    LLVector3   mAABBMax;
    F64         mLearnedAt;        // LLTimer::getElapsedSeconds when the reply landed, so a caller can decide the answer is too old for it

    bool isValid() const { return mLearnedAt > 0.0; }
};

// The answer for the parcel under a region-local position, or false when it has not been learned. False is always the safe reading: it means unknown, never "no".
bool ssParcelFactsGet(LLViewerRegion* regionp, const LLVector3& region_pos, SSParcelFacts& out);

// The same by the simulator's own parcel id, for a caller holding one from elsewhere.
bool ssParcelFactsGetByLocalID(LLViewerRegion* regionp, S32 parcel_local_id, SSParcelFacts& out);

// Ask about the parcel under a position. Cheap to call repeatedly and from a hot path: a parcel already known, already queued or already asked about costs one indexed byte and a return.
//
// `urgent` is for a caller acting on the user's behalf right now - a floater opening, a tag being looked up for something about to be drawn. It skips the settling delay and the per-region ceiling, and goes to the head of the queue. It does NOT skip the rate limit, because nothing does.
void ssParcelFactsRequest(LLViewerRegion* regionp, const LLVector3& region_pos, bool urgent = false);

// Fired once per parcel as its reply lands, for consumers that would rather be told than poll. The region pointer is valid for the duration of the call and no longer.
typedef boost::signals2::signal<void (LLViewerRegion*, const SSParcelFacts&)> ss_parcel_facts_signal_t;
boost::signals2::connection ssParcelFactsAddListener(const ss_parcel_facts_signal_t::slot_type& cb);

// Called from the private branch in LLViewerParcelMgr::processParcelProperties.
void ssParcelFactsNoteReply(LLViewerRegion* regionp, const SSParcelFacts& facts);

// Drains the queue at the metered rate. Called from LLWorld::updateRegions.
void ssParcelFactsTick();

void ssParcelFactsForget(U64 handle);
void ssParcelFactsCounts(U32& regions, U32& parcels_known, U32& asked, U32& answered, U32& waiting);
std::string ssParcelFactsMetricsString();

#endif // SS_PARCELFACTS_H
