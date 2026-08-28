/**
 * @file ssobjectfacts.cpp
 * @brief A viewer-wide cache of what the simulator will tell us about individual objects, and metered ways to ask
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssobjectfacts.h"

#include "llagent.h"
#include "llpathfindinglinkset.h"
#include "llpathfindingmanager.h"
#include "llpathfindingobjectlist.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"

#include <deque>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>

SSObjectFacts::SSObjectFacts()
:   mCreationDate(0),
    mBaseMask(0),
    mOwnerMask(0),
    mGroupMask(0),
    mEveryoneMask(0),
    mNextOwnerMask(0),
    mLinksetUse(SS_LINKSET_UNKNOWN),
    mLandImpact(0),
    mGroupOwned(false),
    mScripted(false),
    mModifiable(false),
    mHave(0)
{
}

namespace
{
    // One block is a U32 local id, so this is the same 255 FSAreaSearch uses (fsareasearch.cpp:72). The message system's own isSendFull check closes a packet earlier if it needs to.
    constexpr U32 SS_OBJFACTS_MAX_PER_PACKET = 255;

    // Records held for one region.
    constexpr size_t SS_OBJFACTS_MAX_KNOWN = 60000;

    struct Pending
    {
        U32                  mLocalID;
        LLUUID               mObjectID;
        ss_objfacts_filter_t mStillWanted;
    };

    struct RegionFacts
    {
        U64 mHandle = 0;
        F64 mFirstWant = 0.0;
        F64 mPathfindingAt = 0.0;      // when the linkset sweep last completed for this region
        bool mPathfindingInFlight = false;

        std::unordered_map<LLUUID, SSObjectFacts> mKnown;
        std::unordered_set<LLUUID> mQueued;
        std::deque<Pending>        mQueue;

        U32 mAsked     = 0;
        U32 mAnswered  = 0;
        U32 mWithdrawn = 0;
    };

    std::map<U64, RegionFacts> sRegions;
    ss_object_facts_signal_t   sSignal;

    // An ObjectProperties reply carries an object id and nothing that names a region, so the region has to be remembered from the send. This doubles as the in-flight count that paces the drain.
    std::unordered_map<LLUUID, U64> sAwaiting;

    F64 sLastTick   = 0.0;
    F32 sBudget     = 0.f;
    U32 sRoundRobin = 0;
    U32 sSent       = 0;
    U32 sAnswered   = 0;
    U32 sPathfindingRequestId = 0;

    bool ssSelectProbeOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, "SSObjectFactsProbe", true);
        return on;
    }

    RegionFacts* ssFactsState(U64 handle, bool create)
    {
        auto found = sRegions.find(handle);
        if (found != sRegions.end()) return &found->second;
        if (!create) return NULL;

        RegionFacts& rf = sRegions[handle];
        rf.mHandle    = handle;
        rf.mFirstWant = (F64)LLTimer::getElapsedSeconds();
        return &rf;
    }

    // The record for an object, created empty on first touch. Returns NULL only when the region is holding as many as it will.
    SSObjectFacts* ssFactsRecord(RegionFacts& rf, const LLUUID& object_id)
    {
        auto found = rf.mKnown.find(object_id);
        if (found != rf.mKnown.end()) return &found->second;
        if (rf.mKnown.size() >= SS_OBJFACTS_MAX_KNOWN) return NULL;

        SSObjectFacts& facts = rf.mKnown[object_id];
        facts.mObjectID = object_id;
        return &facts;
    }

    U8 ssLinksetUseFrom(LLPathfindingLinkset::ELinksetUse use)
    {
        switch (use)
        {
        case LLPathfindingLinkset::kWalkable:        return SS_LINKSET_WALKABLE;
        case LLPathfindingLinkset::kStaticObstacle:  return SS_LINKSET_STATIC_OBSTACLE;
        case LLPathfindingLinkset::kDynamicObstacle: return SS_LINKSET_DYNAMIC_OBSTACLE;
        case LLPathfindingLinkset::kMaterialVolume:  return SS_LINKSET_MATERIAL_VOLUME;
        case LLPathfindingLinkset::kExclusionVolume: return SS_LINKSET_EXCLUSION_VOLUME;
        case LLPathfindingLinkset::kDynamicPhantom:  return SS_LINKSET_DYNAMIC_PHANTOM;
        default:                                     return SS_LINKSET_UNKNOWN;
        }
    }

    // Select and immediately deselect, both as batched messages, which is the shape every other bulk property reader in this tree uses. The deselect matters: an object left selected server-side is state the simulator keeps on the agent's behalf, and a sweep that never released it would accumulate it for the whole session.
    void ssFactsSendBatch(LLViewerRegion* regionp, const std::vector<U32>& ids)
    {
        if (ids.empty() || !gMessageSystem) return;

        LLMessageSystem* msg = gMessageSystem;

        for (S32 pass = 0; pass < 2; ++pass)
        {
            const bool select = (pass == 0);
            bool  fresh  = true;
            S32   blocks = 0;

            for (U32 id : ids)
            {
                if (fresh)
                {
                    if (select) msg->newMessageFast(_PREHASH_ObjectSelect);
                    else        msg->newMessageFast(_PREHASH_ObjectDeselect);
                    msg->nextBlockFast(_PREHASH_AgentData);
                    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
                    fresh  = false;
                    blocks = 0;
                }

                msg->nextBlockFast(_PREHASH_ObjectData);
                msg->addU32Fast(_PREHASH_ObjectLocalID, id);
                ++blocks;

                if (msg->isSendFull(NULL) || blocks >= (S32)SS_OBJFACTS_MAX_PER_PACKET)
                {
                    msg->sendReliable(regionp->getHost());
                    fresh = true;
                }
            }

            if (!fresh) msg->sendReliable(regionp->getHost());
        }
    }

    // The linkset sweep's answer. The handle is captured by value rather than a region pointer because this lands an unbounded time later, quite possibly after the agent has left.
    void ssFactsPathfindingReply(U64 handle, LLPathfindingManager::ERequestStatus status, LLPathfindingObjectListPtr list)
    {
        RegionFacts* rf = ssFactsState(handle, false);

        if (status == LLPathfindingManager::kRequestStarted) return;

        if (rf) rf->mPathfindingInFlight = false;

        if (status != LLPathfindingManager::kRequestCompleted || !list || !rf) return;

        rf->mPathfindingAt = (F64)LLTimer::getElapsedSeconds();

        U32 learned = 0;
        for (LLPathfindingObjectList::const_iterator it = list->begin(); it != list->end(); ++it)
        {
            const LLPathfindingObjectPtr& objectp = it->second;
            if (!objectp) continue;

            const LLPathfindingLinkset* linkset = dynamic_cast<const LLPathfindingLinkset*>(objectp.get());

            SSObjectFacts* facts = ssFactsRecord(*rf, objectp->getUUID());
            if (!facts) break;

            // Name, description, owner and location come free with the sweep for every linkset in the region - the same fields the select probe pays a message each for. They are only written where the sweep actually has something, so a later ObjectProperties reply is never overwritten with less.
            if (!objectp->getName().empty())        facts->mName        = objectp->getName();
            if (!objectp->getDescription().empty()) facts->mDescription = objectp->getDescription();
            facts->mGroupOwned = objectp->isGroupOwned();
            facts->mLocation   = objectp->getLocation();

            if (linkset)
            {
                facts->mLinksetUse = ssLinksetUseFrom(linkset->getLinksetUse());
                facts->mLandImpact = linkset->getLandImpact();
                facts->mModifiable = linkset->isModifiable();
                if (linkset->hasIsScripted()) facts->mScripted = linkset->isScripted();
            }

            facts->mHave |= SS_OBJFACTS_PATHFINDING;
            ++learned;

            sSignal(handle, *facts);
        }

        LL_INFOS("SSFacts") << "Region " << handle << ": the pathfinding sweep described " << learned
                            << " linksets, no selection and one request" << LL_ENDL;
    }
}

boost::signals2::connection ssObjectFactsAddListener(const ss_object_facts_signal_t::slot_type& cb)
{
    return sSignal.connect(cb);
}

bool ssObjectFactsGet(U64 region_handle, const LLUUID& object_id, SSObjectFacts& out)
{
    RegionFacts* rf = ssFactsState(region_handle, false);
    if (!rf) return false;

    auto found = rf->mKnown.find(object_id);
    if (found == rf->mKnown.end()) return false;

    out = found->second;
    return true;
}

void ssObjectFactsSeedGroup(U64 region_handle, const LLUUID& object_id, const LLUUID& group_id)
{
    if (object_id.isNull()) return;

    RegionFacts* rf = ssFactsState(region_handle, true);
    if (!rf) return;

    SSObjectFacts* facts = ssFactsRecord(*rf, object_id);
    if (!facts) return;

    // Marked as answered even when the group is null, because "set to no group" is the answer for most objects and is exactly what stops them being asked about again on every future visit.
    facts->mGroupID = group_id;
    facts->mHave   |= SS_OBJFACTS_PROPERTIES;
}

bool ssObjectFactsWouldQueue(U64 region_handle, const LLUUID& object_id)
{
    if (object_id.isNull() || !ssSelectProbeOn()) return false;

    RegionFacts* rf = ssFactsState(region_handle, false);
    if (!rf) return true;

    auto known = rf->mKnown.find(object_id);
    if (known != rf->mKnown.end() && (known->second.mHave & SS_OBJFACTS_PROPERTIES)) return false;

    return rf->mQueued.find(object_id) == rf->mQueued.end();
}

void ssObjectFactsRequest(LLViewerRegion* regionp, U32 local_id, const LLUUID& object_id,
                          bool urgent, ss_objfacts_filter_t still_wanted)
{
    if (!regionp || !local_id || object_id.isNull() || !ssSelectProbeOn()) return;

    RegionFacts* rf = ssFactsState(regionp->getHandle(), true);
    if (!rf) return;

    // Already answered by a select, on this visit or a previous one whose answer was seeded back in. A pathfinding-only record is NOT an answer here: that sweep cannot report set-to-group.
    auto known = rf->mKnown.find(object_id);
    if (known != rf->mKnown.end() && (known->second.mHave & SS_OBJFACTS_PROPERTIES)) return;

    if (!rf->mQueued.insert(object_id).second) return;

    static LLCachedControl<U32> budget(gSavedSettings, "SSObjectFactsBudget", 5000);
    if (!urgent && (size_t)rf->mAsked + rf->mQueue.size() >= (size_t)(U32)budget)
    {
        rf->mQueued.erase(object_id);
        return;
    }

    Pending p;
    p.mLocalID     = local_id;
    p.mObjectID    = object_id;
    p.mStillWanted = still_wanted;

    if (urgent) rf->mQueue.push_front(p);
    else        rf->mQueue.push_back(p);
}

void ssObjectFactsRequestPathfinding(LLViewerRegion* regionp)
{
    static LLCachedControl<bool> on(gSavedSettings, "SSObjectFactsPathfinding", true);
    if (!on || !regionp) return;

    // The capability is only ever resolved for the region the agent is standing in (LLPathfindingManager::getCurrentRegion), so asking on behalf of a neighbour would silently return the agent region's table under the neighbour's handle - which would be worse than no answer.
    if (regionp != gAgent.getRegion()) return;
    if (!regionp->capabilitiesReceived()) return;
    if (!LLPathfindingManager::instanceExists()) return;
    if (!LLPathfindingManager::getInstance()->isPathfindingEnabledForRegion(regionp)) return;

    RegionFacts* rf = ssFactsState(regionp->getHandle(), true);
    if (!rf || rf->mPathfindingInFlight) return;

    rf->mPathfindingInFlight = true;

    const U64 handle = regionp->getHandle();
    LLPathfindingManager::getInstance()->requestGetLinksets(
        ++sPathfindingRequestId,
        [handle](LLPathfindingManager::request_id_t, LLPathfindingManager::ERequestStatus status, LLPathfindingObjectListPtr list)
        {
            ssFactsPathfindingReply(handle, status, list);
        });
}

bool ssObjectFactsNoteProperties(const LLUUID& object_id, const LLUUID& owner_id, const LLUUID& group_id,
                                 const LLUUID& creator_id, U64 creation_date,
                                 U32 base_mask, U32 owner_mask, U32 group_mask, U32 everyone_mask, U32 next_owner_mask,
                                 const std::string& name, const std::string& description)
{
    if (sAwaiting.empty()) return false;

    auto waiting = sAwaiting.find(object_id);
    if (waiting == sAwaiting.end()) return false;

    const U64 handle = waiting->second;
    sAwaiting.erase(waiting);
    ++sAnswered;

    RegionFacts* rf = ssFactsState(handle, false);
    if (!rf) return true;

    rf->mQueued.erase(object_id);
    ++rf->mAnswered;

    SSObjectFacts* facts = ssFactsRecord(*rf, object_id);
    if (!facts) return true;

    facts->mOwnerID       = owner_id;
    facts->mGroupID       = group_id;
    facts->mCreatorID     = creator_id;
    facts->mCreationDate  = creation_date;
    facts->mBaseMask      = base_mask;
    facts->mOwnerMask     = owner_mask;
    facts->mGroupMask     = group_mask;
    facts->mEveryoneMask  = everyone_mask;
    facts->mNextOwnerMask = next_owner_mask;
    facts->mName          = name;
    facts->mDescription   = description;
    facts->mHave         |= SS_OBJFACTS_PROPERTIES;

    sSignal(handle, *facts);
    return true;
}

void ssObjectFactsTick()
{
    const F64 now = (F64)LLTimer::getElapsedSeconds();

    // The pathfinding sweep is region-wide for one request and needs nothing queued, so it runs on its own schedule and outside the select probe's enable flag.
    {
        static LLCachedControl<U32> refresh_secs(gSavedSettings, "SSObjectFactsPathfindingRefreshSecs", 600);
        LLViewerRegion* agent_region = gAgent.getRegion();
        if (agent_region)
        {
            RegionFacts* rf = ssFactsState(agent_region->getHandle(), false);
            const bool never = !rf || rf->mPathfindingAt <= 0.0;
            if (never || (now - rf->mPathfindingAt) > (F64)(U32)refresh_secs)
            {
                ssObjectFactsRequestPathfinding(agent_region);
            }
        }
    }

    if (sRegions.empty()) return;

    // Turned off mid-session: stop asking, but keep what is known. Callers seed this cache from their own persistent stores and harvest it back at region exit, so clearing it here would turn a paused feature into silent data loss.
    if (!ssSelectProbeOn())
    {
        sAwaiting.clear();
        return;
    }

    static LLCachedControl<F32> packets_per_sec(gSavedSettings, "SSObjectFactsPacketsPerSec", 2.f);
    static LLCachedControl<U32> outstanding_cap(gSavedSettings, "SSObjectFactsOutstanding", 2000);
    static LLCachedControl<U32> delay_secs(gSavedSettings, "SSObjectFactsDelaySecs", 25);

    if (sLastTick <= 0.0)
    {
        sLastTick = now;
        return;
    }

    const F64 dt = llclamp(now - sLastTick, 0.0, 1.0);
    sLastTick = now;

    static F64 s_last_prune = 0.0;
    if ((now - s_last_prune) > 30.0)
    {
        s_last_prune = now;
        for (auto it = sRegions.begin(); it != sRegions.end(); )
        {
            it = LLWorld::getInstance()->getRegionFromHandle(it->first) ? std::next(it) : sRegions.erase(it);
        }

        // Requests whose region has gone, and - once the map has grown past several times the in-flight ceiling, which means answers are not coming back at all - everything. The queued mark is released with the request rather than left behind: an id that stays marked is one ssObjectFactsRequest will refuse to queue again, so abandoning a request without releasing it would make the viewer permanently silent about that object for the rest of the session.
        const bool drop_all = sAwaiting.size() > (size_t)(U32)outstanding_cap * 4;
        for (auto await = sAwaiting.begin(); await != sAwaiting.end(); )
        {
            RegionFacts* rf = ssFactsState(await->second, false);
            if (rf && !drop_all)
            {
                ++await;
                continue;
            }
            if (rf) rf->mQueued.erase(await->first);
            await = sAwaiting.erase(await);
        }

        if (sRegions.empty()) return;
    }

    sBudget = llmin(sBudget + (F32)(dt * (F64)llmax(0.f, (F32)packets_per_sec)), 3.f);
    if (sBudget < 1.f) return;

    // The pacing that actually matters. Not the request rate - a select packet is under a kilobyte and a person routinely selects a thousand objects by hand - but how many answers are allowed to be in flight, because the REPLIES are the expensive direction: an ObjectProperties block carries a name, description, permissions and sale info, and a thousand of them arriving at once lands on the same circuit as the region's object updates.
    if (sAwaiting.size() >= (size_t)(U32)outstanding_cap) return;

    const size_t count = sRegions.size();
    auto it = sRegions.begin();
    std::advance(it, (size_t)(sRoundRobin % (U32)count));
    ++sRoundRobin;

    for (size_t visited = 0; visited < count && sBudget >= 1.f; ++visited)
    {
        if (it == sRegions.end()) it = sRegions.begin();

        RegionFacts& rf = it->second;
        ++it;

        if (rf.mQueue.empty()) continue;
        if ((now - rf.mFirstWant) < (F64)(U32)delay_secs) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(rf.mHandle);
        if (!regionp || !regionp->capabilitiesReceived()) continue;

        std::vector<U32> batch;
        batch.reserve(SS_OBJFACTS_MAX_PER_PACKET);

        while (batch.size() < SS_OBJFACTS_MAX_PER_PACKET && !rf.mQueue.empty())
        {
            const Pending p = rf.mQueue.front();
            rf.mQueue.pop_front();

            auto known = rf.mKnown.find(p.mObjectID);
            if (known != rf.mKnown.end() && (known->second.mHave & SS_OBJFACTS_PROPERTIES))
            {
                // Answered while it waited, by an earlier batch or by a reply to somebody else's select that happened to name it.
                rf.mQueued.erase(p.mObjectID);
                continue;
            }

            // The caller's own last word, evaluated here rather than at request time because the thing it depends on - a parcel reply, a verdict reached elsewhere - usually has not arrived yet when the request is made.
            if (p.mStillWanted && !p.mStillWanted())
            {
                rf.mQueued.erase(p.mObjectID);
                ++rf.mWithdrawn;
                continue;
            }

            batch.push_back(p.mLocalID);
            sAwaiting[p.mObjectID] = rf.mHandle;
        }

        if (batch.empty()) continue;

        ssFactsSendBatch(regionp, batch);
        rf.mAsked += (U32)batch.size();
        sSent     += (U32)batch.size();
        sBudget   -= 1.f;
    }
}

void ssObjectFactsForget(U64 handle)
{
    sRegions.erase(handle);
}

void ssObjectFactsCounts(U32& known, U32& asked, U32& answered, U32& waiting, U32& withdrawn, U32& navmesh_known)
{
    asked    = sSent;
    answered = sAnswered;

    known = waiting = withdrawn = navmesh_known = 0;
    for (const auto& pair : sRegions)
    {
        known     += (U32)pair.second.mKnown.size();
        waiting   += (U32)pair.second.mQueue.size();
        withdrawn += pair.second.mWithdrawn;

        for (const auto& rec : pair.second.mKnown)
        {
            if (rec.second.isNavmeshPermanent()) ++navmesh_known;
        }
    }
}

std::string ssObjectFactsMetricsString()
{
    U32 known = 0, asked = 0, answered = 0, waiting = 0, withdrawn = 0, navmesh = 0;
    ssObjectFactsCounts(known, asked, answered, waiting, withdrawn, navmesh);

    return llformat("objects: %u known, %u asked, %u answered, %u waiting, %u withdrawn, %u navmesh-permanent",
                    known, asked, answered, waiting, withdrawn, navmesh);
}
