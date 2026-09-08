/**
 * @file ssworldfieldshapes.cpp
 * @brief See ssworldfieldshapes.h.
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

#include "ssworldfieldshapes.h"

#include "llfasttimer.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llprimitive.h"
#include "llphysicsshapebuilderutil.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llsurface.h"
#include "lltimer.h"
#include "llvector4a.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "llworld.h"

#include <cfloat>
#include <cmath>

// <SS:Nexii> The census envelope and rebuild policy (doc/atmo_magic_worldfield_competition.md 7.1):
// one snapshot around the query anchor, rebuilt when it rots - anchor drift, age, or a debounced
// edit fan-out - never a persistent mirror. The bucket grid is the broadphase; 64 m keeps a
// megaprim a handful of cells, not per-column work.
static constexpr F32 SS_SHAPES_BUCKET_M = 64.f;
static constexpr F64 SS_SHAPES_MAX_AGE = 10.0;
static constexpr F32 SS_SHAPES_REBUILD_MOVE = 48.f;
static constexpr F64 SS_SHAPES_DIRTY_DEBOUNCE = 2.0;
static constexpr S32 SS_SHAPES_PART_TRIS = 8192;
static constexpr S32 SS_SHAPES_PART_TRIS_LARGE = 65536;
static constexpr S32 SS_SHAPES_TOTAL_TRIS = 2000000;
static constexpr F32 SS_SHAPES_LARGE_PART_M = 16.f;
static constexpr F32 SS_SHAPES_TERRAIN_STEP_M = 4.f;
static constexpr S32 SS_SHAPES_TERRAIN_SAMPLES = 96;
static constexpr S32 SS_SHAPES_DDA_GUARD = 512;

static LLTrace::BlockTimerStatHandle FTM_SS_SHAPES_CENSUS("SS Shapes Census");

// Terrain height under an agent-space point, clamped into the region.
static F32 ss_terrain_z(LLViewerRegion* regionp, const LLVector3& pos_agent)
{
    LLVector3 region_pos = regionp->getPosRegionFromAgent(pos_agent);
    region_pos.mV[VX] = llclamp(region_pos.mV[VX], 0.f, 255.9f);
    region_pos.mV[VY] = llclamp(region_pos.mV[VY], 0.f, 255.9f);
    return regionp->getLand().resolveHeightRegion(region_pos.mV[VX], region_pos.mV[VY]);
}

// Volume-space vertex to a plain vector.
static LLVector3 ss_vert(const LLVector4a& vert)
{
    return LLVector3(vert.getF32ptr());
}

// World rotation of a part: its local rotation composed over its linkset ancestors.
static LLQuaternion ss_world_rotation(const LLViewerObject* vobj)
{
    LLQuaternion rot = vobj->getRotation();
    const LLViewerObject* cur = vobj;
    while (cur->getParent())
    {
        cur = cur->getParent();
        rot = cur->getRotation() * rot;
    }
    return rot;
}

// 64 m bucket key from cell coordinates, biased into unsigned space.
static U64 ss_bucket_key(S32 x, S32 y, S32 z)
{
    return ((U64)(U32)(x + (1 << 20)) << 42)
         | ((U64)(U32)(y + (1 << 20)) << 21)
         | (U64)(U32)(z + (1 << 20));
}

// Segment vs world AABB in segment parameter space [t0, t1] (0..1); tightens both bounds.
static bool ss_ray_aabb(const LLVector3& a, const LLVector3& dir,
                        const LLVector3& bmin, const LLVector3& bmax,
                        F32& t0, F32& t1)
{
    F32 lo = t0, hi = t1;
    for (U32 i = 0; i < 3; ++i)
    {
        const F32 d = dir.mV[i];
        if (fabsf(d) < 1e-9f)
        {
            if (a.mV[i] < bmin.mV[i] || a.mV[i] > bmax.mV[i]) return false;
            continue;
        }
        F32 tn = (bmin.mV[i] - a.mV[i]) / d;
        F32 tf = (bmax.mV[i] - a.mV[i]) / d;
        if (tn > tf) { F32 tmp = tn; tn = tf; tf = tmp; }
        if (tn > lo) lo = tn;
        if (tf < hi) hi = tf;
        if (lo > hi) return false;
    }
    t0 = lo;
    t1 = hi;
    return true;
}

// Segment vs triangle, two-sided Moeller-Trumbore.
static bool ss_ray_tri(const LLVector3& v0, const LLVector3& v1, const LLVector3& v2,
                       const LLVector3& a, const LLVector3& dir,
                       F32 t_min, F32 t_max, F32& out_t, LLVector3& out_n)
{
    const LLVector3 e1 = v1 - v0;
    const LLVector3 e2 = v2 - v0;
    const LLVector3 p = dir % e2;
    const F32 det = e1 * p;
    if (fabsf(det) < 1e-9f) return false;
    const F32 inv = 1.f / det;
    const LLVector3 tv = a - v0;
    const F32 u = (tv * p) * inv;
    if (u < -1e-6f || u > 1.000001f) return false;
    const LLVector3 q = tv % e1;
    const F32 v = (dir * q) * inv;
    if (v < -1e-6f || u + v > 1.000001f) return false;
    const F32 t = (e2 * q) * inv;
    if (t < t_min || t > t_max) return false;
    LLVector3 n = e1 % e2;
    if (n.magVecSquared() < 1e-18f) return false;
    n.normVec();
    if (n * dir > 0.f) n *= -1.f;
    out_t = t;
    out_n = n;
    return true;
}

// One frame of maintenance: gate off drops the census entirely; gate on rebuilds
// a rotted snapshot. Cheap every frame - the scan only runs on rebuild.
void SSWorldFieldShapes::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldFieldShapes", false);
    mNow = LLFrameTimer::getTotalSeconds();
    if (!enabled)
    {
        if (!mCensus.mRecords.empty())
        {
            mCensus = Census();
            mTriCount = 0;
        }
        return;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(LLViewerCamera::getInstance()->getOrigin());
    if (needsRebuild(regionp))
    {
        buildCensus(regionp);
    }
}

// The edit fan-out: record the dirty sphere; the next update past the debounce
// rebuilds if it touches the envelope. Never per-edit work here.
void SSWorldFieldShapes::markDirty(const LLVector3& pos_agent, F32 radius)
{
    SSWorldFieldShapes* self = getInstance();
    self->mDirtyCenter = pos_agent;
    self->mDirtyRadius = radius;
    self->mDirtyAt = LLFrameTimer::getTotalSeconds();
    self->mDirty = true;
}

// Whether the resident snapshot is stale or absent: no region, region switch,
// anchor drift, age, or an accepted dirty sphere inside the envelope.
bool SSWorldFieldShapes::needsRebuild(LLViewerRegion* regionp) const
{
    if (!regionp) return false;
    static LLCachedControl<F32> range(gSavedSettings, "SSWorldFieldShapesRange", 192.f);
    const F32 env = llclamp((F32)range, 32.f, 1024.f);
    const LLVector3 anchor = LLViewerCamera::getInstance()->getOrigin();
    if (mCensus.mRegionHandle != regionp->getHandle()) return true;
    if ((anchor - mCensus.mAnchor).magVec() > SS_SHAPES_REBUILD_MOVE) return true;
    if (mNow - mCensus.mBuildTime > SS_SHAPES_MAX_AGE) return true;
    if (mDirty && (mNow - mDirtyAt >= SS_SHAPES_DIRTY_DEBOUNCE))
    {
        if ((mDirtyCenter - mCensus.mAnchor).magVec() - mDirtyRadius < env) return true;
    }
    return false;
}

// The census scan: every volume part of the region inside the envelope becomes
// one declared-shape record. Object-list order - same snapshot, same records,
// every time; tie-breaks downstream are deterministic by construction.
void SSWorldFieldShapes::buildCensus(LLViewerRegion* regionp)
{
    if (!regionp) return;
    LL_RECORD_BLOCK_TIME(FTM_SS_SHAPES_CENSUS);
    LLTimer build_timer;

    static LLCachedControl<F32> range(gSavedSettings, "SSWorldFieldShapesRange", 192.f);
    const F32 env = llclamp((F32)range, 32.f, 1024.f);

    mCensus = Census();
    mCensus.mRegionHandle = regionp->getHandle();
    mCensus.mAnchor = LLViewerCamera::getInstance()->getOrigin();
    mCensus.mBuildTime = mNow;
    mTriCount = 0;
    mDirty = false;

    const S32 n = gObjectList.getNumObjects();
    for (S32 i = 0; i < n; ++i)
    {
        LLViewerObject* vobj = gObjectList.getObject(i);
        if (!vobj || vobj->isDead() || vobj->isOrphaned()) continue;
        if (vobj->getRegion() != regionp) continue;
        if (vobj->isAvatar() || vobj->isAttachment()) continue;
        const U32 pcode = vobj->getPCode();
        if (pcode == LLViewerObject::LL_VO_WATER || pcode == LLViewerObject::LL_VO_VOID_WATER) continue;
        if (pcode != LL_PCODE_VOLUME) continue;

        LLVOVolume* vov = (LLVOVolume*)vobj;
        if (vov->isFlexible() || vov->isRiggedMesh()) continue;
        if (!vov->getVolume()) continue;

        const LLVector3 pos = vov->getPositionAgent();
        const LLVector3 scale = vov->getScale();
        const F32 approx = 0.5f * llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ])) + 1.f;
        if ((pos - mCensus.mAnchor).magVec() - approx > env) continue;

        addPart(vov);
    }

    mLastBuildMS = build_timer.getElapsedTimeF32() * 1000.f;
}

// One volume part in: classify its declared shape exactly the way the physics
// debug renderer does (LLPhysicsShapeBuilderUtil + get_physics_detail), collect
// local-space triangles where the class needs them, and file the record.
void SSWorldFieldShapes::addPart(LLVOVolume* vov)
{
    LLVolume* vol = vov->getVolume();
    if (!vol) return;

    const LLVolumeParams& params = vol->getParams();
    const LLVector3 pos = vov->getPositionAgent();
    const LLQuaternion rot = ss_world_rotation(vov);
    const LLVector3 scale = vov->getScale();
    LLViewerObject* rootp = vov;
    while (rootp->getParent()) rootp = rootp->getParent();
    const bool phantom = rootp->flagPhantom();
    const U8 layer = phantom ? (U8)LAYER_DECLARED_PHANTOM : (U8)LAYER_DECLARED;

    // Never fire an ObjectPhysicsProperties request: unknown shape types read
    // as conservative boxes until the data arrives on its own.
    const bool shape_known = !vov->getPhysicsShapeUnknown();
    const S32 ptype = shape_known ? vov->getPhysicsShapeType() : -1;

    if (!shape_known)
    {
        addOBB(pos, rot, scale * 0.5f, layer, PROV_UNFETCHED);
        return;
    }
    if (ptype == LLViewerObject::PHYSICS_SHAPE_NONE)
    {
        // Declared non-colliding but very visible: geometry kept, provenance
        // marks it - walkability will filter, sound will not.
        addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
        return;
    }

    const S32 tri_cap = (llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ])) > SS_SHAPES_LARGE_PART_M)
                            ? SS_SHAPES_PART_TRIS_LARGE : SS_SHAPES_PART_TRIS;

    if (vov->isMesh())
    {
        const LLUUID mesh_id = params.getSculptID();
        LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
        const bool has_decomp = decomp && !decomp->mHull.empty();
        LLPhysicsVolumeParams phys_params(params, ptype == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
        LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification spec;
        LLPhysicsShapeBuilderUtil::determinePhysicsShape(phys_params, scale, has_decomp, spec);
        const S32 st = (S32)spec.getType();

        if (decomp)
        {
            if (!decomp->mHull.empty())
            {
                gMeshRepo.buildPhysicsMesh(*decomp);
                std::vector<LLVector3> soup;
                for (size_t h = 0; h < decomp->mMesh.size() && (S32)(soup.size() / 3) < tri_cap; ++h)
                {
                    const std::vector<LLVector3>& hull = decomp->mMesh[h].mPositions;
                    soup.insert(soup.end(), hull.begin(), hull.end());
                }
                if (!soup.empty() && (S32)(soup.size() / 3) <= tri_cap
                    && addTriangles(pos, rot, scale, soup, layer, PROV_HULL))
                {
                    return;
                }
                addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
                return;
            }
            if (!decomp->mPhysicsShapeMesh.empty())
            {
                if (addTriangles(pos, rot, scale, decomp->mPhysicsShapeMesh.mPositions, layer, PROV_TESSELLATED))
                {
                    return;
                }
            }
            else if (!decomp->mBaseHullMesh.empty())
            {
                if (addTriangles(pos, rot, scale, decomp->mBaseHullMesh.mPositions, layer, PROV_HULL))
                {
                    return;
                }
            }
            else
            {
                gMeshRepo.fetchPhysicsShape(mesh_id);
            }
        }
        addOBB(pos, rot, scale * 0.5f, layer, PROV_UNFETCHED);
        return;
    }

    // Prim: closed form for the three implicit specs, physics-detail geometry
    // for everything cut, hollow, twisted or otherwise non-implicit.
    LLPhysicsVolumeParams phys_params(params, ptype == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
    LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification spec;
    LLPhysicsShapeBuilderUtil::determinePhysicsShape(phys_params, scale, false, spec);
    const S32 st = (S32)spec.getType();
    const LLVector3 spec_center = pos + (spec.getCenter() * rot);
    const LLVector3 spec_half = spec.getScale() * 0.5f;

    if (st == (S32)LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::BOX)
    {
        addOBB(spec_center, rot, spec_half, layer, PROV_EXACT);
    }
    else if (st == (S32)LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::SPHERE)
    {
        addEllipsoid(spec_center, rot, spec_half, layer, PROV_EXACT);
    }
    else if (st == (S32)LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::CYLINDER)
    {
        const F32 radius = llmax(spec_half.mV[VX], spec_half.mV[VY]);
        addCylinder(spec_center, rot, radius, spec_half.mV[VZ], layer, PROV_EXACT);
    }
    else
    {
        // PRIM_MESH / PRIM_CONVEX / SCULPT / USER_CONVEX: the physics-detail
        // tessellation is the shape; hull points when Havok already built them.
        const S32 detail = get_physics_detail(params, scale);
        LLVolume* phys_vol = LLPrimitive::sVolumeManager->refVolume(params, detail);
        if (!phys_vol)
        {
            addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
            return;
        }

        std::vector<LLVector3> soup;
        bool truncated = false;

        if (phys_vol->mHullPoints && phys_vol->mHullIndices && phys_vol->mNumHullIndices >= 3)
        {
            for (S32 k = 0; k + 2 < phys_vol->mNumHullIndices; k += 3)
            {
                if ((S32)(soup.size() / 3) >= tri_cap) { truncated = true; break; }
                soup.push_back(ss_vert(phys_vol->mHullPoints[phys_vol->mHullIndices[k]]));
                soup.push_back(ss_vert(phys_vol->mHullPoints[phys_vol->mHullIndices[k + 1]]));
                soup.push_back(ss_vert(phys_vol->mHullPoints[phys_vol->mHullIndices[k + 2]]));
            }
        }
        if (soup.empty())
        {
            const S32 nf = phys_vol->getNumVolumeFaces();
            for (S32 f = 0; f < nf; ++f)
            {
                const LLVolumeFace& vf = phys_vol->getVolumeFace(f);
                if (!vf.mPositions || !vf.mIndices || vf.mNumIndices < 3 || vf.mNumIndices > 65535) continue;
                for (S32 k = 0; k + 2 < vf.mNumIndices; k += 3)
                {
                    if ((S32)(soup.size() / 3) >= tri_cap) { truncated = true; break; }
                    soup.push_back(ss_vert(vf.mPositions[vf.mIndices[k]]));
                    soup.push_back(ss_vert(vf.mPositions[vf.mIndices[k + 1]]));
                    soup.push_back(ss_vert(vf.mPositions[vf.mIndices[k + 2]]));
                }
                if (truncated) break;
            }
        }
        const bool from_hull = phys_vol->mHullPoints && phys_vol->mHullIndices && phys_vol->mNumHullIndices >= 3;
        LLPrimitive::sVolumeManager->unrefVolume(phys_vol);

        if (!truncated && !soup.empty()
            && addTriangles(pos, rot, scale, soup, layer, from_hull ? (U8)PROV_HULL : (U8)PROV_TESSELLATED))
        {
            return;
        }
        addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
    }
}

// Analytic record builders - AABBs derived from the frame so the bucket grid
// and the query pretests stay cheap.
void SSWorldFieldShapes::addOBB(const LLVector3& center, const LLQuaternion& rot,
                                const LLVector3& half, U8 layer, U8 prov)
{
    Record rec;
    rec.mClass = Record::CLASS_BOX;
    rec.mCenter = center;
    rec.mAxes[0] = LLVector3(1.f, 0.f, 0.f) * rot;
    rec.mAxes[1] = LLVector3(0.f, 1.f, 0.f) * rot;
    rec.mAxes[2] = LLVector3(0.f, 0.f, 1.f) * rot;
    rec.mHalf = half;
    LLVector3 ext;
    for (U32 i = 0; i < 3; ++i)
    {
        ext += rec.mAxes[i].scaledVec(LLVector3(fabsf(half.mV[VX]), fabsf(half.mV[VY]), fabsf(half.mV[VZ])));
    }
    rec.mBMin = center - ext;
    rec.mBMax = center + ext;
    rec.mLayer = layer;
    rec.mProv = prov;
    addRecord(rec);
}

void SSWorldFieldShapes::addEllipsoid(const LLVector3& center, const LLQuaternion& rot,
                                      const LLVector3& radii, U8 layer, U8 prov)
{
    Record rec;
    rec.mClass = Record::CLASS_SPHERE;
    rec.mCenter = center;
    rec.mAxes[0] = LLVector3(1.f, 0.f, 0.f) * rot;
    rec.mAxes[1] = LLVector3(0.f, 1.f, 0.f) * rot;
    rec.mAxes[2] = LLVector3(0.f, 0.f, 1.f) * rot;
    rec.mRadii = radii;
    LLVector3 ext;
    for (U32 i = 0; i < 3; ++i)
    {
        ext += rec.mAxes[i].scaledVec(radii);
    }
    rec.mBMin = center - ext;
    rec.mBMax = center + ext;
    rec.mLayer = layer;
    rec.mProv = prov;
    addRecord(rec);
}

void SSWorldFieldShapes::addCylinder(const LLVector3& center, const LLQuaternion& rot,
                                     F32 radius, F32 half_height, U8 layer, U8 prov)
{
    Record rec;
    rec.mClass = Record::CLASS_CYLINDER;
    rec.mCenter = center;
    rec.mAxisU = LLVector3(1.f, 0.f, 0.f) * rot;
    rec.mAxisV = LLVector3(0.f, 1.f, 0.f) * rot;
    rec.mAxes[2] = LLVector3(0.f, 0.f, 1.f) * rot;
    rec.mRadius = radius;
    rec.mHalfHeight = half_height;
    const LLVector3 ext = rec.mAxisU * radius + rec.mAxisV * radius + rec.mAxes[2] * half_height;
    rec.mBMin = center - ext;
    rec.mBMax = center + ext;
    rec.mLayer = layer;
    rec.mProv = prov;
    addRecord(rec);
}

// Triangle soup in (local, unit-space verts): transform to world, budget-check,
// AABB, file. Returns false when the soup was empty or over budget - the caller
// falls back to a box.
bool SSWorldFieldShapes::addTriangles(const LLVector3& pos, const LLQuaternion& rot,
                                      const LLVector3& scale, const std::vector<LLVector3>& local_soup,
                                      U8 layer, U8 prov)
{
    const size_t verts = local_soup.size() - local_soup.size() % 3;
    if (verts < 3) return false;
    if (mTriCount + (S32)(verts / 3) > SS_SHAPES_TOTAL_TRIS) return false;

    Record rec;
    rec.mClass = Record::CLASS_TRI;
    rec.mTri.resize(verts);
    rec.mBMin.setVec(FLT_MAX, FLT_MAX, FLT_MAX);
    rec.mBMax.setVec(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (size_t i = 0; i < verts; ++i)
    {
        const LLVector3 w = pos + (local_soup[i].scaledVec(scale) * rot);
        rec.mTri[i] = w;
        for (U32 c = 0; c < 3; ++c)
        {
            rec.mBMin.mV[c] = llmin(rec.mBMin.mV[c], w.mV[c]);
            rec.mBMax.mV[c] = llmax(rec.mBMax.mV[c], w.mV[c]);
        }
    }
    rec.mLayer = layer;
    rec.mProv = prov;
    mTriCount += (S32)(verts / 3);
    addRecord(rec);
    return true;
}

// File a record and register its AABB with every bucket cell it touches.
void SSWorldFieldShapes::addRecord(Record& rec)
{
    const F32 inv = 1.f / SS_SHAPES_BUCKET_M;
    S32 x0 = (S32)floorf(rec.mBMin.mV[VX] * inv);
    S32 y0 = (S32)floorf(rec.mBMin.mV[VY] * inv);
    S32 z0 = (S32)floorf(rec.mBMin.mV[VZ] * inv);
    S32 x1 = (S32)floorf(rec.mBMax.mV[VX] * inv);
    S32 y1 = (S32)floorf(rec.mBMax.mV[VY] * inv);
    S32 z1 = (S32)floorf(rec.mBMax.mV[VZ] * inv);
    if (x1 - x0 > 512) x1 = x0 + 512;
    if (y1 - y0 > 512) y1 = y0 + 512;
    if (z1 - z0 > 512) z1 = z0 + 512;

    const U32 index = (U32)mCensus.mRecords.size();
    mCensus.mRecords.push_back(rec);
    for (S32 z = z0; z <= z1; ++z)
    {
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                mCensus.mBuckets[ss_bucket_key(x, y, z)].push_back(index);
            }
        }
    }
}

// Exact segment cast: bucket DDA for candidates, analytic/triangle exact tests
// against each, heightfield march for terrain. Deterministic per census - the
// object-list build order and strict-closest tie-break fix the answer.
bool SSWorldFieldShapes::segmentCast(const LLVector3& a, const LLVector3& b, SegmentHit& out,
                                     bool include_phantom)
{
    out = SegmentHit();
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldFieldShapes", false);
    if (!enabled) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(a);
    if (!regionp) return false;
    if (needsRebuild(regionp)) buildCensus(regionp);
    if (mCensus.mRegionHandle != regionp->getHandle()) return false;

    const LLVector3 dir = b - a;
    const F32 len = dir.magVec();
    if (len < 1e-6f) return true;
    const LLVector3 dirn = dir * (1.f / len);

    F32 best_t = 1.f;
    LLVector3 best_n;
    U8 best_layer = LAYER_DECLARED;
    U8 best_prov = PROV_TERRAIN;
    bool hit = false;

    F32 terr_t = 0.f;
    LLVector3 terr_n;
    if (castTerrain(a, b, regionp, terr_t, terr_n))
    {
        best_t = terr_t;
        best_n = terr_n;
        hit = true;
    }

    const U32 epoch = ++mCensus.mVisitEpoch;
    S32 cx = (S32)floorf(a.mV[VX] / SS_SHAPES_BUCKET_M);
    S32 cy = (S32)floorf(a.mV[VY] / SS_SHAPES_BUCKET_M);
    S32 cz = (S32)floorf(a.mV[VZ] / SS_SHAPES_BUCKET_M);
    const S32 stepx = dir.mV[VX] > 0.f ? 1 : (dir.mV[VX] < 0.f ? -1 : 0);
    const S32 stepy = dir.mV[VY] > 0.f ? 1 : (dir.mV[VY] < 0.f ? -1 : 0);
    const S32 stepz = dir.mV[VZ] > 0.f ? 1 : (dir.mV[VZ] < 0.f ? -1 : 0);
    const F32 tdx = fabsf(dir.mV[VX]) > 1e-9f ? SS_SHAPES_BUCKET_M / fabsf(dir.mV[VX]) : F32_MAX;
    const F32 tdy = fabsf(dir.mV[VY]) > 1e-9f ? SS_SHAPES_BUCKET_M / fabsf(dir.mV[VY]) : F32_MAX;
    const F32 tdz = fabsf(dir.mV[VZ]) > 1e-9f ? SS_SHAPES_BUCKET_M / fabsf(dir.mV[VZ]) : F32_MAX;
    F32 tmaxx = fabsf(dir.mV[VX]) > 1e-9f ? ((dir.mV[VX] > 0.f ? (F32)(cx + 1) * SS_SHAPES_BUCKET_M - a.mV[VX]
                                                               : a.mV[VX] - (F32)cx * SS_SHAPES_BUCKET_M) / fabsf(dir.mV[VX])) : F32_MAX;
    F32 tmaxy = fabsf(dir.mV[VY]) > 1e-9f ? ((dir.mV[VY] > 0.f ? (F32)(cy + 1) * SS_SHAPES_BUCKET_M - a.mV[VY]
                                                               : a.mV[VY] - (F32)cy * SS_SHAPES_BUCKET_M) / fabsf(dir.mV[VY])) : F32_MAX;
    F32 tmaxz = fabsf(dir.mV[VZ]) > 1e-9f ? ((dir.mV[VZ] > 0.f ? (F32)(cz + 1) * SS_SHAPES_BUCKET_M - a.mV[VZ]
                                                               : a.mV[VZ] - (F32)cz * SS_SHAPES_BUCKET_M) / fabsf(dir.mV[VZ])) : F32_MAX;

    S32 guard = 0;
    while (true)
    {
        auto it = mCensus.mBuckets.find(ss_bucket_key(cx, cy, cz));
        if (it != mCensus.mBuckets.end())
        {
            for (U32 idx : it->second)
            {
                Record& rec = mCensus.mRecords[idx];
                if (rec.mVisit == epoch) continue;
                rec.mVisit = epoch;
                if (!include_phantom && rec.mLayer == LAYER_DECLARED_PHANTOM) continue;

                F32 t0 = 0.f, t1 = best_t;
                if (!ss_ray_aabb(a, dirn, rec.mBMin, rec.mBMax, t0, t1)) continue;

                F32 t = 0.f;
                LLVector3 n;
                if (castRecord(rec, a, dirn, t0, best_t, t, n))
                {
                    best_t = t;
                    best_n = n;
                    best_layer = rec.mLayer;
                    best_prov = rec.mProv;
                    hit = true;
                }
            }
        }

        if (tmaxx <= tmaxy && tmaxx <= tmaxz)
        {
            if (tmaxx > 1.f) break;
            cx += stepx;
            tmaxx += tdx;
        }
        else if (tmaxy <= tmaxz)
        {
            if (tmaxy > 1.f) break;
            cy += stepy;
            tmaxy += tdy;
        }
        else
        {
            if (tmaxz > 1.f) break;
            cz += stepz;
            tmaxz += tdz;
        }
        if (++guard > SS_SHAPES_DDA_GUARD) break;
    }

    if (hit)
    {
        out.mHit = true;
        out.mDistance = best_t * len;
        out.mPoint = a + dir * best_t;
        out.mNormal = best_n;
        out.mLayer = best_layer;
        out.mProvenance = best_prov;
    }
    return true;
}

// One record's exact test in segment parameter space; t_max doubles as the
// running best so triangle soups early-out.
bool SSWorldFieldShapes::castRecord(const Record& rec, const LLVector3& a, const LLVector3& dirn,
                                    F32 t_min, F32 t_max, F32& out_t, LLVector3& out_n) const
{
    switch (rec.mClass)
    {
        case Record::CLASS_BOX:
        {
            F32 t0 = t_min, t1 = t_max;
            S32 enter = -1;
            F32 enter_sign = 0.f;
            const LLVector3 rel = a - rec.mCenter;
            for (U32 i = 0; i < 3; ++i)
            {
                const F32 o = rel * rec.mAxes[i];
                const F32 d = dirn * rec.mAxes[i];
                const F32 h = rec.mHalf.mV[i];
                if (fabsf(d) < 1e-9f)
                {
                    if (fabsf(o) > h) return false;
                    continue;
                }
                F32 tn = (-h - o) / d;
                F32 tf = (h - o) / d;
                if (tn > tf) { F32 tmp = tn; tn = tf; tf = tmp; }
                if (tn > t0) { t0 = tn; enter = (S32)i; enter_sign = d > 0.f ? -1.f : 1.f; }
                if (tf < t1) t1 = tf;
                if (t0 > t1) return false;
            }
            if (enter < 0) return false;
            out_t = t0;
            out_n = rec.mAxes[enter] * enter_sign;
            return true;
        }
        case Record::CLASS_SPHERE:
        {
            const LLVector3 rel = a - rec.mCenter;
            F32 o[3], d[3];
            for (U32 i = 0; i < 3; ++i)
            {
                o[i] = (rel * rec.mAxes[i]) / rec.mRadii.mV[i];
                d[i] = (dirn * rec.mAxes[i]) / rec.mRadii.mV[i];
            }
            const F32 A = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const F32 B = 2.f * (o[0] * d[0] + o[1] * d[1] + o[2] * d[2]);
            const F32 C = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - 1.f;
            const F32 disc = B * B - 4.f * A * C;
            if (disc < 0.f || A < 1e-12f) return false;
            const F32 sq = sqrtf(disc);
            F32 t = (-B - sq) / (2.f * A);
            if (t < t_min) t = (-B + sq) / (2.f * A);
            if (t < t_min || t > t_max) return false;
            LLVector3 n;
            for (U32 i = 0; i < 3; ++i)
            {
                n += rec.mAxes[i] * ((o[i] + t * d[i]) / rec.mRadii.mV[i]);
            }
            n.normVec();
            out_t = t;
            out_n = n;
            return true;
        }
        case Record::CLASS_CYLINDER:
        {
            const LLVector3 rel = a - rec.mCenter;
            const F32 e0 = rel * rec.mAxisU, e1 = rel * rec.mAxisV, e2 = rel * rec.mAxes[2];
            const F32 d0 = dirn * rec.mAxisU, d1 = dirn * rec.mAxisV, d2 = dirn * rec.mAxes[2];
            const F32 r = rec.mRadius, h = rec.mHalfHeight;
            const F32 A = d0 * d0 + d1 * d1;
            if (A > 1e-12f)
            {
                const F32 B = 2.f * (e0 * d0 + e1 * d1);
                const F32 C = e0 * e0 + e1 * e1 - r * r;
                const F32 disc = B * B - 4.f * A * C;
                if (disc >= 0.f)
                {
                    const F32 sq = sqrtf(disc);
                    for (S32 k = 0; k < 2; ++k)
                    {
                        const F32 t = k == 0 ? (-B - sq) / (2.f * A) : (-B + sq) / (2.f * A);
                        if (t < t_min || t > t_max) continue;
                        const F32 z = e2 + t * d2;
                        if (fabsf(z) > h) continue;
                        out_n = (rec.mAxisU * (e0 + t * d0) + rec.mAxisV * (e1 + t * d1)).normVec();
                        out_t = t;
                        return true;
                    }
                }
            }
            if (fabsf(d2) > 1e-9f)
            {
                const F32 tc0 = (-h - e2) / d2;
                const F32 tc1 = (h - e2) / d2;
                const F32 t_lo = llmin(tc0, tc1);
                const F32 t_hi = llmax(tc0, tc1);
                const F32 tc = t_lo >= t_min ? t_lo : (t_hi >= t_min && t_hi <= t_max ? t_hi : F32_MAX);
                if (tc <= t_max)
                {
                    const F32 px = e0 + tc * d0, py = e1 + tc * d1;
                    if (px * px + py * py <= r * r)
                    {
                        out_n = rec.mAxes[2] * (d2 > 0.f ? -1.f : 1.f);
                        out_t = tc;
                        return true;
                    }
                }
            }
            return false;
        }
        case Record::CLASS_TRI:
        {
            const size_t nt = rec.mTri.size() / 3;
            for (size_t k = 0; k < nt; ++k)
            {
                if (ss_ray_tri(rec.mTri[k * 3], rec.mTri[k * 3 + 1], rec.mTri[k * 3 + 2],
                               a, dirn, t_min, t_max, out_t, out_n))
                {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

// Heightfield march: 4 m samples, sign flip, 10 bisection refinements; the
// normal from finite differences of the same heightfield.
bool SSWorldFieldShapes::castTerrain(const LLVector3& a, const LLVector3& b, LLViewerRegion* regionp,
                                     F32& out_t, LLVector3& out_normal) const
{
    const LLVector3 d = b - a;
    const F32 len = d.magVec();
    if (len < 1e-4f) return false;

    const S32 steps = llmin(SS_SHAPES_TERRAIN_SAMPLES, (S32)(len / SS_SHAPES_TERRAIN_STEP_M) + 1);
    F32 prev_t = 0.f;
    F32 prev_dz = a.mV[VZ] - ss_terrain_z(regionp, a);
    if (prev_dz <= 0.f)
    {
        out_t = 0.f;
        out_normal.setVec(0.f, 0.f, 1.f);
        return true;
    }

    for (S32 i = 1; i <= steps; ++i)
    {
        const F32 t = (F32)i / (F32)steps;
        const LLVector3 p = a + d * t;
        const F32 dz = p.mV[VZ] - ss_terrain_z(regionp, p);
        if (dz <= 0.f)
        {
            F32 lo = prev_t, hi = t;
            for (S32 k = 0; k < 10; ++k)
            {
                const F32 mid = 0.5f * (lo + hi);
                const LLVector3 pm = a + d * mid;
                if (pm.mV[VZ] - ss_terrain_z(regionp, pm) > 0.f) lo = mid; else hi = mid;
            }
            out_t = 0.5f * (lo + hi);

            const LLVector3 ph = a + d * out_t;
            const F32 hx0 = ss_terrain_z(regionp, ph + LLVector3(1.f, 0.f, 0.f));
            const F32 hx1 = ss_terrain_z(regionp, ph + LLVector3(-1.f, 0.f, 0.f));
            const F32 hy0 = ss_terrain_z(regionp, ph + LLVector3(0.f, 1.f, 0.f));
            const F32 hy1 = ss_terrain_z(regionp, ph + LLVector3(0.f, -1.f, 0.f));
            out_normal.setVec(-(hx0 - hx1), -(hy0 - hy1), 2.f);
            out_normal.normVec();
            return true;
        }
        prev_t = t;
    }
    return false;
}

// Eight horizontal wall distances - the exact form of the soundscape's side probes.
bool SSWorldFieldShapes::wallProfile(const LLVector3& pos, F32 range_m, F32 out[8],
                                     bool include_phantom)
{
    SegmentHit probe_hit;
    if (!segmentCast(pos, pos + LLVector3(1.f, 0.f, 0.f) * range_m, probe_hit, include_phantom))
    {
        return false;
    }
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 ang = (F32)i * (F_PI / 4.f);
        SegmentHit h;
        if (segmentCast(pos, pos + LLVector3(cosf(ang), sinf(ang), 0.f) * range_m, h, include_phantom)
            && h.mHit)
        {
            out[i] = h.mDistance;
        }
        else
        {
            out[i] = range_m;
        }
    }
    return true;
}

bool SSWorldFieldShapes::censusCurrent() const
{
    return mCensus.mRegionHandle != 0 && (mNow - mCensus.mBuildTime) <= SS_SHAPES_MAX_AGE;
}

// The census overlay: record boxes by layer and provenance, distance-thinned
// like the rest of the debug views.
void SSWorldFieldShapes::renderDebug()
{
    if (mCensus.mRecords.empty()) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);

    for (const Record& rec : mCensus.mRecords)
    {
        const LLVector3 centre = (rec.mBMin + rec.mBMax) * 0.5f;
        if ((centre - cam).magVec() > 256.f) continue;

        if (rec.mLayer == LAYER_DECLARED_PHANTOM)
        {
            gGL.color4f(1.f, 0.82f, 0.3f, 0.45f);
        }
        else
        {
            switch (rec.mProv)
            {
                case PROV_EXACT:        gGL.color4f(0.7f, 0.9f, 1.f, 0.45f); break;
                case PROV_HULL:         gGL.color4f(0.5f, 0.8f, 0.9f, 0.45f); break;
                case PROV_TESSELLATED:  gGL.color4f(0.5f, 1.f, 0.7f, 0.45f); break;
                case PROV_BBOX:         gGL.color4f(1.f, 0.6f, 0.3f, 0.45f); break;
                default:                gGL.color4f(1.f, 0.4f, 1.f, 0.45f); break;
            }
        }

        const LLVector3& mn = rec.mBMin;
        const LLVector3& mx = rec.mBMax;
        gGL.vertex3fv(mn.mV); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mn.mV[VZ]);
        gGL.vertex3fv(mn.mV); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mn.mV[VZ]);
        gGL.vertex3fv(mn.mV); gGL.vertex3f(mn.mV[VX], mn.mV[VY], mx.mV[VZ]);
        gGL.vertex3fv(mx.mV); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mx.mV[VZ]);
        gGL.vertex3fv(mx.mV); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]);
        gGL.vertex3fv(mx.mV); gGL.vertex3f(mx.mV[VX], mx.mV[VY], mn.mV[VZ]);
        gGL.vertex3f(mn.mV[VX], mx.mV[VY], mn.mV[VZ]); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mx.mV[VZ]);
        gGL.vertex3f(mn.mV[VX], mn.mV[VY], mx.mV[VZ]); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mx.mV[VZ]);
        gGL.vertex3f(mx.mV[VX], mn.mV[VY], mn.mV[VZ]); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]);
        gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]); gGL.vertex3f(mx.mV[VX], mx.mV[VY], mx.mV[VZ]);
        gGL.vertex3f(mx.mV[VX], mx.mV[VY], mn.mV[VZ]); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mn.mV[VZ]);
        gGL.vertex3f(mx.mV[VX], mx.mV[VY], mn.mV[VZ]); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mn.mV[VZ]);
        gGL.vertex3f(mn.mV[VX], mn.mV[VY], mx.mV[VZ]); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]);
    }

    gGL.end();
}
