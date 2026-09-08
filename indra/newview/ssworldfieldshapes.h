/**
 * @file ssworldfieldshapes.h
 * @brief Atmo Magic: exact declared-shape queries over the census of physics shapes.
 *
 *        The column-span store quantizes indoor geometry to 0.25 m cells; the
 *        consumers that need wall-exact answers (the soundscape's live probes,
 *        shockwave and thunder paths, windflow apertures) read here instead:
 *        one census of the shapes objects declare - physics shape type and
 *        phantom status read respectively, phantom a layer not a filter -
 *        answered by exact segment/ray casts against analytic prims, physics
 *        detail tessellations and mesh decomposition hulls.
 *
 *        The census is build-scoped (doc/atmo_magic_worldfield_competition.md
 *        7.1): a snapshot taken on the main thread inside a query envelope,
 *        rasterized lazily per query, never a persistent mirror. Terrain is
 *        answered analytically off the heightfield at query time. Nothing here
 *        fires an ObjectPhysicsProperties request - shape type is read only
 *        when known, unknowns land on conservative OBBs until the data arrives.
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

#ifndef SS_WORLDFIELD_SHAPES_H
#define SS_WORLDFIELD_SHAPES_H

#include "llquaternion.h"
#include "llsingleton.h"
#include "v3math.h"

#include <unordered_map>
#include <vector>

class LLViewerRegion;

class SSWorldFieldShapes : public LLSingleton<SSWorldFieldShapes>
{
    LLSINGLETON_EMPTY_CTOR(SSWorldFieldShapes);

public:
    // Phantom is a layer, not an exclusion: which consumers may read a
    // declared shape is their call (a drop is stopped by a phantom deck,
    // walkability is not). Provenance records how the shape was derived so
    // consumers can filter by trust - walkability will exclude BBOX/UNFETCHED.
    enum ELayer : U8
    {
        LAYER_DECLARED = 0,         // non-phantom declared geometry
        LAYER_DECLARED_PHANTOM,     // phantom - visible, declared, non-colliding
        LAYER_COUNT
    };

    enum EProvenance : U8
    {
        PROV_EXACT = 0,             // closed-form prim (box/sphere/cylinder spec)
        PROV_HULL,                  // convex hull points / decomposition hull
        PROV_TESSELLATED,           // physics-detail volume tessellation
        PROV_BBOX,                  // conservative box of the prim bounds
        PROV_UNFETCHED,             // shape type or mesh physics not arrived yet
        PROV_TERRAIN                // the heightfield, answered analytically
    };

    struct SegmentHit
    {
        bool mHit = false;          // geometry met between a and b
        F32 mDistance = 0.f;        // metres from a
        LLVector3 mPoint;
        LLVector3 mNormal;
        U8 mLayer = LAYER_DECLARED;
        U8 mProvenance = PROV_EXACT;
    };

    // Per-frame maintenance: staleness checks against the query anchor, the
    // debounced markDirty fan-out, gate off = census dropped entirely.
    void update();

    // The edit fan-out, forwarded from SSWorldField::markDirty; rebuilds are
    // debounced, never per edit.
    static void markDirty(const LLVector3& pos_agent, F32 radius);

    // Exact segment cast a->b. Returns false when the census has no answer
    // (gate off, no census) and the caller keeps its own raycast; a false
    // out.mHit with a true return is a genuine miss along the segment.
    bool segmentCast(const LLVector3& a, const LLVector3& b, SegmentHit& out,
                     bool include_phantom = true);

    // Eight horizontal wall distances at ear height - the exact form of the
    // soundscape's cardinal side probes. False when the census has no answer.
    bool wallProfile(const LLVector3& pos, F32 range_m, F32 out[8],
                     bool include_phantom = true);

    // Stats
    bool censusCurrent() const;
    S32 recordCount() const { return (S32)mCensus.mRecords.size(); }
    S32 triangleCount() const { return mTriCount; }
    F32 lastBuildMS() const { return mLastBuildMS; }

    // The census overlay: record boxes by layer and provenance - view 6 of
    // SSWorldFieldDebugView.
    void renderDebug();

private:
    // One declared shape in world space. Analytic classes carry their frame;
    // TRI records carry a baked world-space triangle soup (3 verts per tri),
    // which is what mesh decompositions and physics-detail tessellations boil
    // down to. World-space baking costs memory and buys branch-free queries.
    struct Record
    {
        enum EClass : U8 { CLASS_BOX = 0, CLASS_SPHERE, CLASS_CYLINDER, CLASS_TRI } mClass = CLASS_BOX;

        LLVector3 mCenter;
        LLVector3 mAxes[3];         // BOX/SPHERE: prim local axes in world space
        LLVector3 mHalf;            // BOX: half extents per axis
        LLVector3 mRadii;           // SPHERE: per-axis radii (ellipsoid)
        LLVector3 mAxisU, mAxisV;   // CYLINDER: frame perpendicular to mAxes[2]
        F32 mRadius = 0.f;          // CYLINDER
        F32 mHalfHeight = 0.f;      // CYLINDER

        std::vector<LLVector3> mTri;    // CLASS_TRI: world-space soup

        LLVector3 mBMin, mBMax;         // world AABB
        U8 mLayer = LAYER_DECLARED;
        U8 mProv = PROV_EXACT;
        U32 mVisit = 0;                 // per-query dedupe stamp
    };

    // The build-scoped snapshot: records plus the 64 m bucket grid over them.
    // One census resident at a time - the queries come from one listener.
    struct Census
    {
        U64 mRegionHandle = 0;
        LLVector3 mAnchor;
        F64 mBuildTime = 0.0;
        std::vector<Record> mRecords;
        std::unordered_map<U64, std::vector<U32> > mBuckets;
        U32 mVisitEpoch = 0;
    };

    bool needsRebuild(LLViewerRegion* regionp) const;
    void buildCensus(LLViewerRegion* regionp);
    void addPart(class LLVOVolume* vov);
    void addOBB(const LLVector3& center, const LLQuaternion& rot, const LLVector3& half, U8 layer, U8 prov);
    void addEllipsoid(const LLVector3& center, const LLQuaternion& rot, const LLVector3& radii, U8 layer, U8 prov);
    void addCylinder(const LLVector3& center, const LLQuaternion& rot, F32 radius, F32 half_height, U8 layer, U8 prov);
    bool addTriangles(const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale,
                      const std::vector<LLVector3>& local_soup, U8 layer, U8 prov);
    void addRecord(Record& rec);
    bool castRecord(const Record& rec, const LLVector3& a, const LLVector3& dirn,
                    F32 t_min, F32 t_max, F32& out_t, LLVector3& out_n) const;
    bool castTerrain(const LLVector3& a, const LLVector3& b, LLViewerRegion* regionp,
                     F32& out_t, LLVector3& out_normal) const;

    Census mCensus;
    LLVector3 mDirtyCenter;
    F32 mDirtyRadius = 0.f;
    F64 mDirtyAt = 0.0;
    bool mDirty = false;

    F64 mNow = 0.0;
    F32 mLastBuildMS = 0.f;
    S32 mTriCount = 0;
};

#endif
