/**
 * @file ssobjectfacts.h
 * @brief A viewer-wide cache of what the simulator will tell us about individual objects, and metered ways to ask
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_OBJECTFACTS_H
#define SS_OBJECTFACTS_H

#include "lluuid.h"
#include "v3math.h"

#include <boost/signals2.hpp>

#include <functional>
#include <string>

class LLViewerRegion;

// An object update carries geometry and a handful of flags. Everything else the simulator knows about an object - its name, its description, who made it, which group it is set to, whether the region's own navigation mesh considers it permanent scenery - arrives by two entirely separate routes, and until now each feature that wanted any of it built its own way of asking and threw the rest away.
//
// This is the shared place for both routes and for everything they return:
//
//   ObjectProperties, elicited by a silent server-side select. Name, description, creator, owner, GROUP, permission masks, creation date. The group is the reason this exists - it is in no object update and has no flag in object_flags.h, and it decides whether parcel auto-return can take the object.
//
//   The region's pathfinding linksets, one HTTP GET against the ObjectLinksets capability, whole region at once, no selection and no per-object cost. Linkset use, land impact, scripted, modifiable, plus name, description and location for every linkset in the region. Not the owner UUID: LLPathfindingObject keeps it private behind a name lookup and exposes only "is it group owned", so the owner still has to come from the blob or from a select. `SS_LINKSET_STATIC_OBSTACLE` and `SS_LINKSET_WALKABLE` are the interesting ones: an object marked either is contributing to the region's navigation mesh, which is a statement by whoever built it that it is permanent, unmovable landscape. That is a far stronger claim than anything a viewer can infer from watching, and it costs one request per region.
//
// The two sources overlap and neither is complete: the pathfinding sweep covers a whole region for one request but reports only DEEDED group ownership, never "set to group", and only for the agent's own region. The select probe answers the group question exactly, for any region, one object at a time. Facts from both are merged into one record and `mHave` says which sources have spoken.
//
// Main thread only.

enum ESSLinksetUse : U8
{
    SS_LINKSET_UNKNOWN = 0,
    SS_LINKSET_WALKABLE,            // navmesh floor - permanent by construction
    SS_LINKSET_STATIC_OBSTACLE,     // navmesh wall - permanent by construction
    SS_LINKSET_DYNAMIC_OBSTACLE,    // collides but does not shape the navmesh, and may move
    SS_LINKSET_MATERIAL_VOLUME,
    SS_LINKSET_EXCLUSION_VOLUME,
    SS_LINKSET_DYNAMIC_PHANTOM,
};

enum ESSObjectFactsSource : U32
{
    SS_OBJFACTS_PROPERTIES  = 1 << 0,   // an ObjectProperties reply has been seen
    SS_OBJFACTS_PATHFINDING = 1 << 1,   // the region's linkset sweep named this object
};

struct SSObjectFacts
{
    SSObjectFacts();

    LLUUID      mObjectID;
    LLUUID      mOwnerID;
    LLUUID      mGroupID;        // the group the object is SET TO. Null is a real answer meaning it is set to none
    LLUUID      mCreatorID;
    std::string mName;
    std::string mDescription;
    U64         mCreationDate;
    U32         mBaseMask;
    U32         mOwnerMask;
    U32         mGroupMask;
    U32         mEveryoneMask;
    U32         mNextOwnerMask;

    U8          mLinksetUse;     // ESSLinksetUse
    U32         mLandImpact;
    bool        mGroupOwned;     // deeded. NOT the same as being set to a group
    bool        mScripted;
    bool        mModifiable;     // by this agent, as the pathfinding sweep reports it
    LLVector3   mLocation;       // region-local, from the pathfinding sweep

    U32         mHave;           // ESSObjectFactsSource bits

    // The one question the navigation mesh answers that nothing else does: is this object part of the region's permanent, unmovable landscape? Both walkable floor and static obstacle are statements that it shapes navigation, which no temporary or movable object may do.
    bool isNavmeshPermanent() const { return mLinksetUse == SS_LINKSET_WALKABLE || mLinksetUse == SS_LINKSET_STATIC_OBSTACLE; }
};

// Everything known about an object in a region, or false when nothing is. `mHave` on a true return says which sources have contributed.
bool ssObjectFactsGet(U64 region_handle, const LLUUID& object_id, SSObjectFacts& out);

// Evaluated at drain time, just before a message would be built, so a caller can withdraw a request that has since become pointless. Returning false costs nothing and is counted separately from an answer. Optional - an absent filter means "still wanted".
typedef std::function<bool()> ss_objfacts_filter_t;

// Ask for an object's ObjectProperties. Deduped against everything already known, already queued and already in flight, so it is cheap to call from a hot path. `urgent` jumps the settling delay and the per-region ceiling and goes to the head of the queue; it does not skip the rate limit.
void ssObjectFactsRequest(LLViewerRegion* regionp, U32 local_id, const LLUUID& object_id,
                          bool urgent = false, ss_objfacts_filter_t still_wanted = ss_objfacts_filter_t());

// True when a select probe for this object would actually be queued: already answered, already queued and already in flight all read false. This exists so a caller on a hot path can avoid building a request - and the filter functor that goes with it - only to have it thrown away, which for the region object cache is thousands of allocations a second it would otherwise pay for nothing.
bool ssObjectFactsWouldQueue(U64 region_handle, const LLUUID& object_id);

// Ask the region for its whole pathfinding linkset table. One HTTP GET, no selection, every linkset in the region. Only ever the agent's own region - the capability is not addressable for neighbours - and refused when the region has pathfinding turned off. Called automatically for the agent's region on a long refresh interval; exposed for a caller that wants it sooner.
void ssObjectFactsRequestPathfinding(LLViewerRegion* regionp);

// Prime the cache from a caller's own persistent store, so a fact learned in an earlier session is not paid for again. Marks the object as having been answered for.
void ssObjectFactsSeedGroup(U64 region_handle, const LLUUID& object_id, const LLUUID& group_id);

// One ObjectProperties reply, from LLSelectMgr::processObjectProperties. Returns true when this object was one of ours, which the caller uses to suppress its missing-node warning - a probed object is in no selection, so without that a thousand-object sweep writes a thousand warnings.
bool ssObjectFactsNoteProperties(const LLUUID& object_id, const LLUUID& owner_id, const LLUUID& group_id,
                                 const LLUUID& creator_id, U64 creation_date,
                                 U32 base_mask, U32 owner_mask, U32 group_mask, U32 everyone_mask, U32 next_owner_mask,
                                 const std::string& name, const std::string& description);

// Fired as each record gains something it did not have. The handle names the region the fact belongs to.
typedef boost::signals2::signal<void (U64, const SSObjectFacts&)> ss_object_facts_signal_t;
boost::signals2::connection ssObjectFactsAddListener(const ss_object_facts_signal_t::slot_type& cb);

void ssObjectFactsTick();
void ssObjectFactsForget(U64 handle);

void ssObjectFactsCounts(U32& known, U32& asked, U32& answered, U32& waiting, U32& withdrawn, U32& navmesh_known);
std::string ssObjectFactsMetricsString();

#endif // SS_OBJECTFACTS_H
