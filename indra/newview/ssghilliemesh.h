/**
 * @file ssghilliemesh.h
 * @brief Ghillie: off-thread mesh decomposition for software Hi-Z occluders.
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

#ifndef SS_GHILLIE_MESH_H
#define SS_GHILLIE_MESH_H

#include "lluuid.h"
#include "v3math.h"
#include "llquaternion.h"

#include <memory>
#include <vector>

// <SS:Nexii> Ghillie mesh decomposition
//
// Meshes are baked geometry - never rasterized live. Instead a low-priority
// worker thread decomposes a qualifying mesh (static, significant, closed)
// into a bounded set of conservative boxes in OBJECT-LOCAL space. The boxes
// are re-transformed per frame from the live position/rotation, so move and
// rotate are cheap; only a geometry/scale/params change invalidates the
// cache. The result is keyed by (mesh_id, params, scale, position class)
// so a mesh used a dozen times decomposes once.
//
// Everything fail-safe: no decomposition means no occluder contribution, and
// the boxes are never allowed to occlude occludees inside their own AABB or
// interior.

class LLMeshVOVolume;

namespace SS
{

// A single conservative occluder box, object-local. Replaced by the main
// thread with the object's live transform into agent space each frame.
struct GhillieMeshBox
{
    LLVector3 mPos;     // object-local box center (unit-object space)
    LLVector3 mHalf;    // object-local half extents - usual meaning
};

struct GhillieMeshDecomp
{
    std::vector<GhillieMeshBox> mBoxes;
    F32 mFill = 1.f;        // fraction of the box set's projection inside the mesh hull (0..1)
};

// Build a crash- and cost-bounded decomposition. `total_vertices` guards the
// per-call iteration budget; the worker pool owns wall-clock via
// SSGhillieMeshMaxMS. Unit-local coordinates only.
GhillieMeshDecomp ssGhillieDecomposeMesh(
    const std::vector<LLVector3>& unit_verts,     // one entry per used vertex (not per corner)
    const std::vector<U32>& triangles,            // indices, multiple of 3
    U32 loop_limit = 120000);

// Exact analytic decomposition of a square-profile line-path prim (box with
// hollow and/or cuts) into up to 4 thin wall boxes, in PROFILE space
// (unit half extents: 0.5 for a default box). This is the cheap exact path
// for the most common parametered prim; no worker pool.
//
// Params are the profile begin/end (fractions of the 4-side loop, each
// 0.25 = one side, matching LLProfileParams genNGon(4,-0.375)) and hollow
// (fraction of the FULL profile extent - so a 0.5-hollow box has walls of
// half-thickness 0.125). Cuts shrink the kept wall's SPAN (its length axis),
// not its thickness: the emitted wall box stays exactly aligned with the cut
// edge (cut-plane aligned) and never protrudes the kept solid.
//
// `half_scale` is the profile half extents (0.5,0.5,hz for a default box);
// the caller scales emitted boxes by the live drawable scale.
void ssGhillieDecomposeSquareBox(
    F32 begin_s, F32 end_s, F32 hollow,
    const LLVector3& half_scale,           // profile half extents
    std::vector<GhillieMeshBox>& out,      // emitted wall boxes, profile space
    U32& wall_count);                       // number of walls emitted

// Deterministic unit-local AABB helper, so the caller and the worker agree.
void ssGhillieUnitBounds(const std::vector<LLVector3>& verts,
                         LLVector3& lo, LLVector3& hi);

} // namespace SS

#endif // SS_GHILLIE_MESH_H
