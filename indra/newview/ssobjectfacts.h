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
#include "object_flags.h"
#include "v3math.h"

#include <boost/signals2.hpp>

#include <functional>
#include <string>

class LLViewerObject;
class LLViewerRegion;

// An object update carries geometry and a handful of flags. Everything else the simulator knows about an object - its name, its description, who made it, which group it is set to, whether the region's own navigation mesh considers it permanent scenery - arrives by two entirely separate routes, and until now each feature that wanted any of it built its own way of asking and threw the rest away.
//
// This is the shared place for both routes and for everything they return:
//
//   ObjectProperties, elicited by a silent server-side select. Name, description, creator, owner, GROUP, permission masks, creation date. The group is the reason this exists - it is in no object update and has no flag in object_flags.h, and it decides whether parcel auto-return can take the object.
//
//   The region's pathfinding linksets, one HTTP GET against the ObjectLinksets capability. Linkset use, land impact, scripted, modifiable, plus name, description and location.
//
// **What that second source is actually worth, measured rather than assumed (owner, 2026-08-28).** It is permission filtered: it returns only objects the agent owns or can modify, and refreshing it in an unowned region does not produce the region's object list. It is also only addressable for the agent's OWN region, since the capability resolves through LLPathfindingManager::getCurrentRegion. So it is not the free region-wide sweep an earlier version of this comment claimed, and nothing may be built on it that needs to be true of other people's builds.
//
// What it does still give, for your own objects, is the one thing no other source has: the CATEGORY. Walkable floor, static obstacle, material volume, exclusion volume - `ESSLinksetUse` below. The mere fact that an object shapes the navmesh at all needs none of this and costs nothing: FLAGS_AFFECTS_NAVMESH is an ordinary object update flag (object_flags.h:44), present on every object in every region, and it is exactly what the build floater reports as "Pathfinding attributes: Permanent".
//
// Neither source is complete and they overlap: the sweep reports only DEEDED group ownership, never "set to group". The select probe answers the group question exactly, for any region, one object at a time. Facts from both are merged into one record and `mHave` says which sources have spoken.
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
    SS_OBJFACTS_LIVE        = 1 << 2,   // filled from a live LLViewerObject, so the flags, position and scale are current rather than remembered
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

    // Straight off the live object where there was one. Nothing here is ever cached, because all of it changes: a cached position is a lie the moment the object moves, and the whole point of separating these is that the caller can tell which half it is holding.
    U32         mUpdateFlags;    // the simulator's own object update flags - navmesh, physics, phantom, temporary, character, scripted, and the ownership bits
    LLVector3   mScale;

    U32         mHave;           // ESSObjectFactsSource bits

    // Whether this object is navmesh floor or navmesh wall specifically, which is only ever known for objects the agent owns or can modify - see the note at the top of this file. For the far more useful question of whether it shapes the navmesh AT ALL, use affectsNavmesh() below: that is free and true everywhere.
    bool isNavmeshPermanent() const { return mLinksetUse == SS_LINKSET_WALKABLE || mLinksetUse == SS_LINKSET_STATIC_OBSTACLE; }

    // The update-flag questions, restated here so a consumer holding one of these structs does not have to know which bit lives where. All of them require SS_OBJFACTS_LIVE - a record assembled only from replies has no update flags and every one of these reads false.
    bool affectsNavmesh() const { return (mUpdateFlags & FLAGS_AFFECTS_NAVMESH) != 0; }
    bool isPhysical() const     { return (mUpdateFlags & FLAGS_USE_PHYSICS) != 0; }
    bool isPhantom() const      { return (mUpdateFlags & FLAGS_PHANTOM) != 0; }
    bool isTemporary() const    { return (mUpdateFlags & FLAGS_TEMPORARY_ON_REZ) != 0; }
    bool isCharacter() const    { return (mUpdateFlags & FLAGS_CHARACTER) != 0; }
    bool isScripted() const     { return (mUpdateFlags & FLAGS_SCRIPTED) != 0; }
};

// Everything known about an object in a region, or false when nothing is. `mHave` on a true return says which sources have contributed.
bool ssObjectFactsGet(U64 region_handle, const LLUUID& object_id, SSObjectFacts& out);

// Evaluated at drain time, just before a message would be built, so a caller can withdraw a request that has since become pointless. Returning false costs nothing and is counted separately from an answer. Optional - an absent filter means "still wanted".
typedef std::function<bool()> ss_objfacts_filter_t;

// Ask for an object's ObjectProperties. Deduped against everything already known, already queued and already in flight, so it is cheap to call from a hot path. `urgent` jumps the settling delay and the per-region ceiling and goes to the head of the queue; it does not skip the rate limit.
void ssObjectFactsRequest(LLViewerRegion* regionp, U32 local_id, const LLUUID& object_id,
                          bool urgent = false, ss_objfacts_filter_t still_wanted = ss_objfacts_filter_t());

// The call for a feature that is ALREADY walking live objects - a wind flow map over the region's volumes, say. Reads the update flags, region position and scale straight off the object, then fills in from the cache whatever only a reply could have supplied. Returns true for any live object, because the object is itself an answer; `mHave` says how complete the rest is.
//
// Deliberately does NOT read LLViewerObject::getPhysicsShapeType(): that getter sends an ObjectPhysicsProperties request when the value is unknown (llviewerobject.cpp:7568-7573), so calling it across a region's worth of objects would fire thousands of messages from what reads like a field access. A caller that wants it should ask the object directly and mean it.
bool ssObjectFactsResolve(LLViewerObject* objectp, SSObjectFacts& out);

// Queue a select probe for a live object. Same as the local-id form, for callers that already hold the object.
void ssObjectFactsRequestFor(LLViewerObject* objectp, bool urgent = false, ss_objfacts_filter_t still_wanted = ss_objfacts_filter_t());

// True when a select probe for this object would actually be queued: already answered, already queued and already in flight all read false. This exists so a caller on a hot path can avoid building a request - and the filter functor that goes with it - only to have it thrown away, which for the region object cache is thousands of allocations a second it would otherwise pay for nothing.
bool ssObjectFactsWouldQueue(U64 region_handle, const LLUUID& object_id);

// Ask the region for its pathfinding linkset table. One HTTP GET, no selection. Two hard limits, both measured rather than assumed: the reply covers only objects the agent OWNS OR CAN MODIFY, and the capability is only addressable for the agent's own region. Called automatically for the agent's region on a long refresh interval; exposed for a caller that wants it sooner.
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
