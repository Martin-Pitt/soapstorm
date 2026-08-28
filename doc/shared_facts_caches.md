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

## Pathfinding: what is free, what is filtered, and what is not reachable at all

**Free, and the one most callers want.** `FLAGS_AFFECTS_NAVMESH` (`object_flags.h:44`) is an ordinary object update flag on every object in every region. It answers "does this object shape the region's navigation mesh", which is a declaration by whoever built it, enforced by the simulator, that the object is permanent and unmovable — nothing that can move is permitted to make it. `LLViewerObject::flagObjectPermanent()` reads it, and it is what the build floater shows as "Pathfinding attributes: Permanent". No capability, no request, no cache. **Do not come to this file for that** — test the flag.

**Permission filtered.** The `ObjectLinksets` capability returns only objects the agent **owns or can modify**, and only for the agent's own region (it resolves through `LLPathfindingManager::getCurrentRegion`). Verified in the field 2026-08-28: refreshing it in an unowned region does not return that region's objects. So it is not a region sweep and nothing may be built on it that has to be true of other people's builds.

What it does uniquely carry, for your own objects, is the **category**: `SS_LINKSET_WALKABLE`, `SS_LINKSET_STATIC_OBSTACLE`, `SS_LINKSET_MATERIAL_VOLUME`, `SS_LINKSET_EXCLUSION_VOLUME` — plus land impact, modifiable and scripted. No update flag distinguishes those. It also keeps the owner UUID private behind a name lookup, exposing only *is it group owned*.

**Not reachable: the navigation mesh itself.** It was worth checking whether the baked navmesh could be downloaded, cached and handed to other features — a wind flow map wants exactly that kind of static obstacle field. It cannot, for two independent reasons:

1. **No decoder ships in this tree.** `indra/llphysicsextensionsos/llpathinglib.cpp` is the open-source stub: `isFunctional()` returns false and `getInstance()` returns NULL, which is why every caller in the viewer is written as `if (LLPathingLib::getInstance() != NULL)`. The proprietary library is not in `autobuild.xml` and is not linked. The navmesh arrives as an opaque zlib-compressed `LLSD::Binary` and nothing here can open it.
2. **Even the closed library would not help.** Its entire interface is `extractNavMeshSrcFromLLSD`, `processNavMeshData`, `generatePath` and a set of `renderNavMesh*` calls (`llpathinglib.h:147-182`). It consumes the blob and draws it or paths through it. It never hands geometry back, so there is nothing for a third feature to read.

Caching the raw blob would therefore cache bytes nobody can interpret.

**The alternative, if a static obstacle field is what you actually need.** Every object already carries `FLAGS_AFFECTS_NAVMESH`, a position and a scale, and the object cache already stores all three per record for whole regions across sessions. That is a coarse obstacle volume set, region-wide, neighbours included, needing no capability and no decoder. It is blockier than a navmesh and it is available today.

## Layering with live objects

A reasonable question, asked by the owner while wiring the wind flow map, which already walks live volume objects: should these facts hang off `LLViewerObject` instead of a side cache?

**No, and the split is not arbitrary — it falls exactly along "does this change".**

*The object owns what changes.* Update flags, region position, scale. Caching any of it makes it a lie the moment the object moves, and all of it is already free on the object. `FLAGS_AFFECTS_NAVMESH`, physics, phantom, temporary, character, scripted are all bits of `mFlags`.

*The cache owns what was answered.* Name, description, group, creator, permission masks, creation date, linkset category, and everything about a parcel. None of it is on `LLViewerObject` and none of it can be, for three reasons:

1. **There is nowhere to put it, and the tree has already answered this twice.** An object's name and description live on `LLSelectNode` (`llselectmgr.h:235-236`), which dies with the selection. That is why FSAreaSearch maintains its own `std::map<LLUUID, FSObjectProperties> mObjectDetails` (`fsareasearch.h:129`) rather than writing to the objects. Two existing side maps, for want of a field.
2. **Lifetime, and it is fatal rather than inconvenient.** `LLWorld::resetClass` calls `gObjectList.destroy()` (`llworld.cpp:139`) *before* its `removeRegion` loop. At shutdown every `LLViewerObject` is already gone when the object cache's region-exit hook runs — facts stored on objects would vanish at precisely the moment something wants to persist them. More generally, remembering objects that are *not* live is the entire point of that consumer.
3. **Memory, and it is the opposite of the intuition.** A field on `LLViewerObject` is paid by every live object whether anyone asked about it or not. Two `std::string`s cost ~64 bytes each even when empty, so name and description alone run to megabytes across a 2048 m draw distance before a single one is filled. The side cache holds an entry only for objects something actually asked about — and keeps it after the object dies, which is when it is most valuable.

**So use `ssObjectFactsResolve()` and stop thinking about the seam:**

```cpp
SSObjectFacts facts;
if (ssObjectFactsResolve(objectp, facts))
{
    if (facts.affectsNavmesh()) { /* live flag, free, always true */ }
    if (facts.mHave & SS_OBJFACTS_PROPERTIES) { /* facts.mDescription, from a reply */ }
}
```

It reads the flags, position and scale off the live object and fills the rest from the cache, returning true for any live object because the object is itself an answer. `mHave` says how complete the rest is. `ssObjectFactsRequestFor(objectp)` queues a probe for the missing half.

One thing it deliberately does not do is read `LLViewerObject::getPhysicsShapeType()`. That getter sends an `ObjectPhysicsProperties` request when the value is unknown (`llviewerobject.cpp:7568-7573`), so calling it across a region's worth of objects fires thousands of messages from what reads like a field access. Ask the object directly if you want it, and mean it.

## Metering, and why it is where it is

Requests are cheap; **replies are the expensive direction**. An `ObjectProperties` block carries a name, description, permissions and sale info, and a thousand of them arrive on the same circuit as the region's object updates. So the binding limit is `SSObjectFactsOutstanding` (2000 in flight), not the packet rate — selecting hundreds to a thousand objects at once is ordinary user behaviour and the batch size reflects that (255 blocks, the same figure FSAreaSearch uses).

Both caches leave a region alone for a settling delay before asking it anything, so a login or a teleport costs no messages at all while the region is streaming.

The traffic is not remarkable and the reasoning is written out in `region_object_cache.md`, decision 8: LSL cannot observe either message, the stock viewer already sends unattended per-parcel requests on mouse-over and unprompted selects from `PermissionsTracker`, and nothing on the probe path is visible to other avatars because it never enters the selection list.

## Lifetime

Session caches. Neither writes to disk. A feature that needs an answer to outlive the session persists its own distilled copy and seeds it back — the object cache stores set-to-group answers in its `[GROUPS]` section and calls `ssObjectFactsSeedGroup()` when a region file lands, so an object answered for in March is never asked about again. Regions the world no longer knows are dropped on a slow sweep rather than on a signal, because a consumer's own region-removal hook may still need to read them.
