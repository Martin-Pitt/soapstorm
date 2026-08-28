# Shared fact caches — parcels and objects

Two viewer-wide caches for the things the simulator will tell you about the world but does not volunteer. Both are plain free functions, no singleton to construct, no floater to open, and neither belongs to the feature that needed it first.

- `ssparcelfacts.{h,cpp}` — everything in a `ParcelProperties` reply, per parcel, per region.
- `ssobjectfacts.{h,cpp}` — everything in an `ObjectProperties` reply plus the region's pathfinding linkset table, per object, per region.

Written 2026-08-28, out of the region object cache, at the owner's instruction: *"Everything that is pulled for information such as parcel information and object details attained by selection should be generically available."* The motivating second consumer is the weather branch, which reads an `atmo:uuid` tag out of a parcel **description** — a field the object cache has no use for and would never have stored.

## Why these are not free today

The stock viewer keeps exactly three parcels: the one under the agent, a hover copy and a selection copy. All three are overwritten as the user moves, and the reply router treats any unrecognised sequence id as an update to the agent's parcel (`llviewerparcelmgr.cpp:1690-1701`) whose tail then drives parcel media, the music stream and the land floater. So a feature that sends its own `ParcelPropertiesRequest` and gets it wrong does not merely fail — it starts a stranger's audio stream.

For objects it is worse. An object update carries geometry and flags. The **group an object is set to** is in no object update and has no flag in `object_flags.h`, and `FLAGS_OBJECT_GROUP_OWNED` means *deeded*, which is a different thing. The only viewer-side source is `ObjectProperties`, and the only way to make the simulator send one for an object you do not own is to select it.

## Reading

```cpp
SSParcelFacts parcel;
if (ssParcelFactsGet(regionp, region_pos, parcel))
{
    // parcel.mName, mDescription, mOwnerID, mGroupID, mGroupOwned,
    // mCleanOtherTime, mArea, mParcelFlags, mCategory, mAABBMin/Max, mLearnedAt
}

SSObjectFacts object;
if (ssObjectFactsGet(region_handle, object_id, object))
{
    // object.mName, mDescription, mOwnerID, mGroupID, mCreatorID, mCreationDate,
    // the five permission masks, mLinksetUse, mLandImpact, mScripted, mModifiable, mLocation
    // object.mHave says WHICH sources have spoken
}
```

`false` always means *unknown*, never *no*. Every consumer must treat it as the safe direction rather than as an answer.

## Asking

```cpp
ssParcelFactsRequest(regionp, pos);                    // background: metered, deduped, settles first
ssParcelFactsRequest(regionp, pos, /*urgent*/ true);   // acting for the user right now
ssObjectFactsRequest(regionp, local_id, object_id);
```

Both are cheap to call from a hot path — an answered, queued or in-flight target costs one lookup and a return. Requests can carry a **withdrawal filter**, a `std::function<bool()>` evaluated at drain time immediately before a message would be built, so a caller can cancel a request that has since become pointless. The object cache uses it to skip objects whose answer could not change its verdict; that removes most of a typical region before a packet exists. Build the filter only behind `ssObjectFactsWouldQueue()` — it captures state, so it heap-allocates, and this path runs thousands of times a second.

Prefer being told over polling:

```cpp
ssParcelFactsAddListener([](LLViewerRegion* r, const SSParcelFacts& p) { /* scan p.mDescription */ });
ssObjectFactsAddListener([](U64 handle, const SSObjectFacts& o) { /* ... */ });
```

## The pathfinding table

One HTTP GET against the region's `ObjectLinksets` capability returns **every linkset in the region** — no selection, no per-object cost, and it fires automatically for the agent's region on a long refresh. It carries name, description, location, land impact, scripted, modifiable and, above all, the linkset use.

`SS_LINKSET_WALKABLE` and `SS_LINKSET_STATIC_OBSTACLE` — together `isNavmeshPermanent()` — mean the object is shaping the region's navigation mesh. That is not an inference from watching something sit still; it is a declaration by whoever built it, enforced by the simulator, and **nothing that can move is permitted to make it**. For the object cache it is the strongest permanence signal available anywhere, and it is worth `SSROCNavmeshPermanentBonus` (0.25) in scoring.

Two limits, both structural. The capability resolves only for the region the agent is standing in, so neighbours get nothing until the agent walks there — asking on a neighbour's behalf would silently return the agent region's table under the wrong handle, which is worse than no answer. And `LLPathfindingObject` keeps the owner UUID private behind a name lookup, exposing only *is it group owned*, so the owner still has to come from the object blob or from a select.

## Metering, and why it is where it is

Requests are cheap; **replies are the expensive direction**. An `ObjectProperties` block carries a name, description, permissions and sale info, and a thousand of them arrive on the same circuit as the region's object updates. So the binding limit is `SSObjectFactsOutstanding` (2000 in flight), not the packet rate — selecting hundreds to a thousand objects at once is ordinary user behaviour and the batch size reflects that (255 blocks, the same figure FSAreaSearch uses).

Both caches leave a region alone for a settling delay before asking it anything, so a login or a teleport costs no messages at all while the region is streaming.

The traffic is not remarkable and the reasoning is written out in `region_object_cache.md`, decision 8: LSL cannot observe either message, the stock viewer already sends unattended per-parcel requests on mouse-over and unprompted selects from `PermissionsTracker`, and nothing on the probe path is visible to other avatars because it never enters the selection list.

## Lifetime

Session caches. Neither writes to disk. A feature that needs an answer to outlive the session persists its own distilled copy and seeds it back — the object cache stores set-to-group answers in its `[GROUPS]` section and calls `ssObjectFactsSeedGroup()` when a region file lands, so an object answered for in March is never asked about again. Regions the world no longer knows are dropped on a slow sweep rather than on a signal, because a consumer's own region-removal hook may still need to read them.
