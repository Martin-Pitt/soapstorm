/**
 * @file ssparcelfacts.cpp
 * @brief A viewer-wide cache of what the simulator knows about each parcel, and a metered way to ask
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssparcelfacts.h"

#include "llagent.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"

#include <deque>
#include <iterator>
#include <map>
#include <vector>

SSParcelFacts::SSParcelFacts()
:   mLocalID(0),
    mGroupOwned(false),
    mCleanOtherTime(0),
    mArea(0),
    mParcelFlags(0),
    mCategory(0),
    mLearnedAt(0.0)
{
}

namespace
{
    // PARCEL_GRID_STEP_METERS by another name. Restated rather than pulling llparcel.h in for one constant, and the parcel grid has been 4m for as long as there has been a grid.
    constexpr F32 SS_PARCEL_CELL_M = 4.f;

    // 512m is the largest region this fork has been pointed at. The clamp is so a half-initialised region reporting a nonsense width cannot ask for an enormous cell map.
    constexpr S32 SS_PARCEL_MAX_EDGE = 128;

    // A region divided into more parcels than this is not one anybody is reasoning about parcel by parcel, and the cap keeps the facts index inside the S16 the cell map stores.
    constexpr size_t SS_PARCEL_MAX_FACTS = 2048;

    struct RegionParcels
    {
        U64 mHandle = 0;
        S32 mEdge   = 0;
        U32 mAsked  = 0;
        F64 mFirstWant = 0.0;

        std::vector<S16>  mCellFacts;   // index into mFacts, -1 while unresolved
        std::vector<U8>   mCellFlags;   // 1 queued, 2 asked
        std::deque<U32>   mQueue;       // a deque because an urgent request goes to the FRONT

        std::vector<SSParcelFacts> mFacts;
    };

    std::map<U64, RegionParcels> sRegions;
    ss_parcel_facts_signal_t     sSignal;

    F64 sLastTick   = 0.0;
    F32 sBudget     = 0.f;
    U32 sRoundRobin = 0;
    U32 sSent       = 0;
    U32 sAnswered   = 0;

    bool ssParcelProbeOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, "SSParcelFactsProbe", true);
        return on;
    }

    RegionParcels* ssParcelState(LLViewerRegion* regionp, bool create)
    {
        if (!regionp) return NULL;

        const U64 handle = regionp->getHandle();
        auto found = sRegions.find(handle);
        if (found != sRegions.end()) return &found->second;
        if (!create) return NULL;

        const S32 edge = llclamp((S32)(regionp->getWidth() / SS_PARCEL_CELL_M), 1, SS_PARCEL_MAX_EDGE);

        RegionParcels& rp = sRegions[handle];
        rp.mHandle    = handle;
        rp.mEdge      = edge;
        rp.mFirstWant = (F64)LLTimer::getElapsedSeconds();
        rp.mCellFacts.assign((size_t)edge * (size_t)edge, (S16)-1);
        rp.mCellFlags.assign((size_t)edge * (size_t)edge, (U8)0);
        return &rp;
    }

    // Region-local metres to a cell index, or -1 for anything outside the region.
    S32 ssParcelCell(const RegionParcels& rp, const LLVector3& pos)
    {
        const S32 cx = (S32)(pos.mV[VX] / SS_PARCEL_CELL_M);
        const S32 cy = (S32)(pos.mV[VY] / SS_PARCEL_CELL_M);
        if (cx < 0 || cy < 0 || cx >= rp.mEdge || cy >= rp.mEdge) return -1;
        return cy * rp.mEdge + cx;
    }

    bool ssParcelSend(LLViewerRegion* regionp, S32 cell, S32 edge)
    {
        if (!regionp || !gMessageSystem) return false;

        // A four metre square, exactly as LLViewerParcelMgr::requestHoveredParcel sends on mouse-over. The simulator answers with the WHOLE parcel that square falls in, AABB included, which is what makes one request per parcel enough rather than one per cell.
        const F32 west  = (F32)(cell % edge) * SS_PARCEL_CELL_M;
        const F32 south = (F32)(cell / edge) * SS_PARCEL_CELL_M;

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ParcelPropertiesRequest);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ParcelData);
        msg->addS32Fast(_PREHASH_SequenceID, SS_PARCELFACTS_SEQ_ID);
        msg->addF32Fast(_PREHASH_West,  west);
        msg->addF32Fast(_PREHASH_South, south);
        msg->addF32Fast(_PREHASH_East,  west  + SS_PARCEL_CELL_M);
        msg->addF32Fast(_PREHASH_North, south + SS_PARCEL_CELL_M);
        msg->addBOOL("SnapSelection", false);
        msg->sendReliable(regionp->getHost());
        return true;
    }
}

boost::signals2::connection ssParcelFactsAddListener(const ss_parcel_facts_signal_t::slot_type& cb)
{
    return sSignal.connect(cb);
}

void ssParcelFactsRequest(LLViewerRegion* regionp, const LLVector3& region_pos, bool urgent)
{
    if (!regionp || !ssParcelProbeOn()) return;

    RegionParcels* rp = ssParcelState(regionp, true);
    if (!rp) return;

    const S32 cell = ssParcelCell(*rp, region_pos);
    if (cell < 0) return;

    // Answered, queued or in flight. This is the branch nearly every call takes - a region's objects are thousands and its parcels are dozens - so it is one indexed byte and a return.
    if (rp->mCellFacts[cell] >= 0 || rp->mCellFlags[cell]) return;

    // The per-region ceiling is a ceiling on REQUESTS, not on parcels: a region carved into more parcels than this leaves the remainder unknown, which for every consumer means "no answer" rather than a wrong one. An urgent caller is acting for the user right now and is not made to wait behind a background sweep's budget.
    static LLCachedControl<U32> budget(gSavedSettings, "SSParcelFactsBudget", 64);
    if (!urgent && (size_t)rp->mAsked + rp->mQueue.size() >= (size_t)(U32)budget) return;

    rp->mCellFlags[cell] = 1;
    if (urgent) rp->mQueue.push_front((U32)cell);
    else        rp->mQueue.push_back((U32)cell);
}

void ssParcelFactsNoteReply(LLViewerRegion* regionp, const SSParcelFacts& facts)
{
    if (!regionp) return;

    // Not created here: a reply for a region nothing ever asked about is either a stale answer arriving after the region was forgotten, or another sender's sequence id colliding with ours, and neither should allocate a cell map.
    RegionParcels* rp = ssParcelState(regionp, false);
    if (!rp) return;

    if (rp->mFacts.size() >= SS_PARCEL_MAX_FACTS) return;

    const S16 index = (S16)rp->mFacts.size();
    rp->mFacts.push_back(facts);
    rp->mFacts.back().mLearnedAt = (F64)LLTimer::getElapsedSeconds();
    ++sAnswered;

    // One reply resolves the parcel's whole footprint. The AABB is region-local and is clamped rather than trusted: a malformed or empty one still marks the single cell it covers, and a wild one cannot walk off the map.
    const S32 x0 = llclamp((S32)(llmin(facts.mAABBMin.mV[VX], facts.mAABBMax.mV[VX]) / SS_PARCEL_CELL_M), 0, rp->mEdge - 1);
    const S32 y0 = llclamp((S32)(llmin(facts.mAABBMin.mV[VY], facts.mAABBMax.mV[VY]) / SS_PARCEL_CELL_M), 0, rp->mEdge - 1);
    const S32 x1 = llclamp((S32)(llmax(facts.mAABBMin.mV[VX], facts.mAABBMax.mV[VX]) / SS_PARCEL_CELL_M), 0, rp->mEdge - 1);
    const S32 y1 = llclamp((S32)(llmax(facts.mAABBMin.mV[VY], facts.mAABBMax.mV[VY]) / SS_PARCEL_CELL_M), 0, rp->mEdge - 1);

    for (S32 y = y0; y <= y1; ++y)
    {
        for (S32 x = x0; x <= x1; ++x)
        {
            const S32 cell = y * rp->mEdge + x;

            // A cell already attributed to another parcel is left alone. Parcel AABBs are bounding boxes and an L-shaped parcel's box overlaps its neighbour's, so the first answer for a cell is kept rather than the last - and the first answer is the one whose own request landed in it.
            if (rp->mCellFacts[cell] >= 0) continue;

            rp->mCellFacts[cell] = index;
            rp->mCellFlags[cell] = 0;
        }
    }

    sSignal(regionp, rp->mFacts[(size_t)index]);
}

bool ssParcelFactsGet(LLViewerRegion* regionp, const LLVector3& region_pos, SSParcelFacts& out)
{
    if (!regionp) return false;

    RegionParcels* rp = ssParcelState(regionp, false);
    if (!rp) return false;

    const S32 cell = ssParcelCell(*rp, region_pos);
    if (cell < 0) return false;

    const S16 index = rp->mCellFacts[cell];
    if (index < 0 || (size_t)index >= rp->mFacts.size()) return false;

    out = rp->mFacts[(size_t)index];
    return true;
}

bool ssParcelFactsGetByLocalID(LLViewerRegion* regionp, S32 parcel_local_id, SSParcelFacts& out)
{
    if (!regionp) return false;

    RegionParcels* rp = ssParcelState(regionp, false);
    if (!rp) return false;

    // Linear over the parcels of one region, which is dozens. A second index would cost more to maintain than it could ever save here.
    for (const SSParcelFacts& facts : rp->mFacts)
    {
        if (facts.mLocalID == parcel_local_id)
        {
            out = facts;
            return true;
        }
    }
    return false;
}

void ssParcelFactsTick()
{
    if (sRegions.empty()) return;

    if (!ssParcelProbeOn())
    {
        // Turned off mid-session. The maps are dropped rather than frozen, so switching it back on re-asks against current land rather than serving answers of unknown age.
        sRegions.clear();
        return;
    }

    static LLCachedControl<F32> per_sec(gSavedSettings, "SSParcelFactsPerSec", 2.f);
    static LLCachedControl<U32> delay_secs(gSavedSettings, "SSParcelFactsDelaySecs", 20);

    const F64 now = (F64)LLTimer::getElapsedSeconds();
    if (sLastTick <= 0.0)
    {
        sLastTick = now;
        return;
    }

    // Clamped because a frame that took a second - a texture thrash, a teleport - must not hand the drain a second's worth of accumulated permission to burst.
    const F64 dt = llclamp(now - sLastTick, 0.0, 1.0);
    sLastTick = now;

    // A cell map is edge squared bytes plus edge squared shorts - about 24KB for a 256m region - and at this fork's draw distance there can be sixty-two live at once and many more across a session of teleports. Regions the world no longer knows are dropped on a slow sweep rather than on a signal, because the region object cache's own removal hook runs BEFORE its promotion pass and promotion is the thing that reads this.
    static F64 s_last_prune = 0.0;
    if ((now - s_last_prune) > 30.0)
    {
        s_last_prune = now;
        for (auto it = sRegions.begin(); it != sRegions.end(); )
        {
            it = LLWorld::getInstance()->getRegionFromHandle(it->first) ? std::next(it) : sRegions.erase(it);
        }
        if (sRegions.empty()) return;
    }

    sBudget = llmin(sBudget + (F32)(dt * (F64)llmax(0.f, (F32)per_sec)), 4.f);
    if (sBudget < 1.f) return;

    // Round robin, because a std::map keyed on region handle is ordered by grid position: always draining from begin() would serve the south-west neighbour every tick and the region the agent is standing in only after it finished.
    const size_t count = sRegions.size();
    auto it = sRegions.begin();
    std::advance(it, (size_t)(sRoundRobin % (U32)count));
    ++sRoundRobin;

    for (size_t visited = 0; visited < count && sBudget >= 1.f; ++visited)
    {
        if (it == sRegions.end()) it = sRegions.begin();

        RegionParcels& rp = it->second;
        ++it;

        if (rp.mQueue.empty()) continue;

        // Nothing is asked while the region is still streaming its own object updates, because both messages ride the same circuit and a burst at entry competes with the rez this is usually being asked in service of.
        if ((now - rp.mFirstWant) < (F64)(U32)delay_secs) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(rp.mHandle);
        if (!regionp || !regionp->capabilitiesReceived())
        {
            // No circuit worth sending on. Left queued rather than dropped - a region still connecting will be ready in a few seconds.
            continue;
        }

        while (sBudget >= 1.f && !rp.mQueue.empty())
        {
            const S32 cell = (S32)rp.mQueue.front();
            rp.mQueue.pop_front();

            // Resolved by somebody else's reply while it waited. Costs no budget - the common case in a region with a few large parcels and thousands of objects spread across them.
            if (rp.mCellFacts[cell] >= 0)
            {
                rp.mCellFlags[cell] = 0;
                continue;
            }

            if (!ssParcelSend(regionp, cell, rp.mEdge)) break;

            rp.mCellFlags[cell] = 2;
            ++rp.mAsked;
            ++sSent;
            sBudget -= 1.f;
        }
    }
}

void ssParcelFactsForget(U64 handle)
{
    sRegions.erase(handle);
}

void ssParcelFactsCounts(U32& regions, U32& parcels_known, U32& asked, U32& answered, U32& waiting)
{
    regions  = (U32)sRegions.size();
    asked    = sSent;
    answered = sAnswered;

    parcels_known = 0;
    waiting       = 0;
    for (const auto& pair : sRegions)
    {
        parcels_known += (U32)pair.second.mFacts.size();
        waiting       += (U32)pair.second.mQueue.size();
    }
}

std::string ssParcelFactsMetricsString()
{
    U32 regions = 0, known = 0, asked = 0, answered = 0, waiting = 0;
    ssParcelFactsCounts(regions, known, asked, answered, waiting);

    return llformat("parcels: %u known across %u regions, %u asked, %u answered, %u waiting",
                    known, regions, asked, answered, waiting);
}
