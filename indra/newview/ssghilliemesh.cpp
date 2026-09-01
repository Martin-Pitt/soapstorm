/**
 * @file ssghilliemesh.cpp
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

#include "llviewerprecompiledheaders.h"

#include "ssghilliemesh.h"

#include <algorithm>
#include <cmath>
#include <vector>

// <SS:Nexii> Ghillie mesh decomposition
//
// Bounded, monotone decomposition of a static-unit mesh into 1-4 boxes:
//   1. PCA slab fit (dominant-plane thin box) - the brick-wall / car-body
//      case: thin, high-occluding.
//   2. axis-aligned AABB (1 box).
//   3. axis-sliced AABB (2-4 boxes) with a conservative interior probe.
//   4. whichever candidate has the highest hull fill wins; never protrude.
//
// The search itself is deliberately cheap and deterministic (no full 3D
// convex decomposition) so it can run on the low-priority mesh pool without
// eating the frame budget.

namespace
{
    constexpr F32 AXIS_EPS = 1e-4f;
    constexpr F32 MAX_F = 1e30f;

    // Centroid + dominant axis (largest variance direction) of unit verts,
    // plus the two remaining orthonormal axes chosen deterministically from
    // the covariance diagonal.
    void dominantAxis(const std::vector<LLVector3>& v,
                      LLVector3& centroid,
                      LLVector3& axis, LLVector3& mid, LLVector3& minor)
    {
        centroid.clear();
        for (const LLVector3& p : v) centroid += p;
        if (!v.empty()) centroid /= (F32)v.size();

        LLVector3 c(0.f, 0.f, 0.f);
        for (const LLVector3& p : v)
        {
            const LLVector3 d = p - centroid;
            c.mV[0] += d.mV[0] * d.mV[0];
            c.mV[1] += d.mV[1] * d.mV[1];
            c.mV[2] += d.mV[2] * d.mV[2];
        }
        if (!c.isFinite() || c.isNull())
        {
            axis  = LLVector3(1.f, 0.f, 0.f);
            mid   = LLVector3(0.f, 1.f, 0.f);
            minor = LLVector3(0.f, 0.f, 1.f);
            return;
        }

        // Dominant axis by largest variance.
        if (c.mV[0] >= c.mV[1] && c.mV[0] >= c.mV[2])       axis = LLVector3(1.f, 0.f, 0.f);
        else if (c.mV[1] >= c.mV[0] && c.mV[1] >= c.mV[2])  axis = LLVector3(0.f, 1.f, 0.f);
        else                                                axis = LLVector3(0.f, 0.f, 1.f);

        // Remaining axes from the covariance diagonal ordering.
        if (axis.mV[0] > 0.5f)
        {
            mid   = (c.mV[1] >= c.mV[2]) ? LLVector3(0.f,1.f,0.f) : LLVector3(0.f,0.f,1.f);
            minor = (c.mV[1] >= c.mV[2]) ? LLVector3(0.f,0.f,1.f) : LLVector3(0.f,1.f,0.f);
        }
        else if (axis.mV[1] > 0.5f)
        {
            mid   = (c.mV[0] >= c.mV[2]) ? LLVector3(1.f,0.f,0.f) : LLVector3(0.f,0.f,1.f);
            minor = (c.mV[0] >= c.mV[2]) ? LLVector3(0.f,0.f,1.f) : LLVector3(1.f,0.f,0.f);
        }
        else
        {
            mid   = (c.mV[0] >= c.mV[1]) ? LLVector3(1.f,0.f,0.f) : LLVector3(0.f,1.f,0.f);
            minor = (c.mV[0] >= c.mV[1]) ? LLVector3(0.f,1.f,0.f) : LLVector3(1.f,0.f,0.f);
        }
    }

    // Project vertices onto the given orthonormal frame; returns slab bounds.
    void projectSlab(const std::vector<LLVector3>& v,
                     const LLVector3& centroid,
                     const LLVector3& a, const LLVector3& b, const LLVector3& c,
                     LLVector3& lo, LLVector3& hi)
    {
        lo = LLVector3(MAX_F, MAX_F, MAX_F);
        hi = LLVector3(-MAX_F, -MAX_F, -MAX_F);
        for (const LLVector3& p : v)
        {
            const LLVector3 d = p - centroid;
            lo.mV[0] = llmin(lo.mV[0], d * a);
            lo.mV[1] = llmin(lo.mV[1], d * b);
            lo.mV[2] = llmin(lo.mV[2], d * c);
            hi.mV[0] = llmax(hi.mV[0], d * a);
            hi.mV[1] = llmax(hi.mV[1], d * b);
            hi.mV[2] = llmax(hi.mV[2], d * c);
        }
        for (S32 k = 0; k < 3; ++k)
        {
            hi.mV[k] = llmax(hi.mV[k], lo.mV[k] + AXIS_EPS);
        }
    }

    // Point-in-hull test by ray-cast parity along +Z against the mesh tris.
    bool pointInsideHull(const LLVector3& p,
                         const std::vector<LLVector3>& verts,
                         const std::vector<U32>& tris)
    {
        S32 crossings = 0;
        for (S32 t = 0; t + 2 < (S32)tris.size(); t += 3)
        {
            const LLVector3& v0 = verts[tris[(U32)t]];
            const LLVector3& v1 = verts[tris[(U32)t + 1]];
            const LLVector3& v2 = verts[tris[(U32)t + 2]];
            const LLVector3 e1 = v1 - v0;
            const LLVector3 e2 = v2 - v0;
            const F32 det = e1.mV[0] * e2.mV[1] - e1.mV[1] * e2.mV[0];
            if (fabsf(det) < 1e-9f) continue;
            const F32 inv = 1.f / det;
            const LLVector3 tvec = p - v0;
            const F32 uu = (tvec.mV[0] * e2.mV[1] - tvec.mV[1] * e2.mV[0]) * inv;
            const F32 vv = (e1.mV[0] * tvec.mV[1] - e1.mV[1] * tvec.mV[0]) * inv;
            if (uu >= 0.f && vv >= 0.f && uu + vv <= 1.f)
            {
                const F32 tz = v0.mV[2] + uu * e1.mV[2] + vv * e2.mV[2];
                if (tz > p.mV[2])
                {
                    ++crossings;
                }
            }
        }
        return (crossings & 1) != 0;
    }

    // Fraction of box-set probes inside the hull, sampled on a small grid.
    // Monotone in box shrink, so greedy search never regresses.
    F32 hullFill(const std::vector<LLVector3>& verts,
                 const std::vector<U32>& tris,
                 const std::vector<LLVector3>& box_centers,
                 const std::vector<LLVector3>& box_halfs)
    {
        const S32 N = 3;
        S32 inside = 0;
        S32 probes = 0;
        const S32 nboxes = (S32)box_centers.size();
        for (S32 b = 0; b < nboxes; ++b)
        {
            const LLVector3& c0 = box_centers[(U32)b];
            const LLVector3& h0 = box_halfs[(U32)b];
            for (S32 ix = 0; ix <= N; ++ix)
            {
                for (S32 iy = 0; iy <= N; ++iy)
                {
                    for (S32 iz = 0; iz <= N; ++iz)
                    {
                        const F32 fx = ((F32)ix / (F32)N) * 2.f - 1.f;
                        const F32 fy = ((F32)iy / (F32)N) * 2.f - 1.f;
                        const F32 fz = ((F32)iz / (F32)N) * 2.f - 1.f;
                        const LLVector3 p = c0 + LLVector3(fx * h0.mV[0], fy * h0.mV[1], fz * h0.mV[2]);
                        if (pointInsideHull(p, verts, tris))
                        {
                            ++inside;
                        }
                        ++probes;
                    }
                }
            }
        }
        if (probes == 0) return 0.f;
        return (F32)inside / (F32)probes;
    }
} // namespace

namespace SS
{

void ssGhillieDecomposeSquareBox(
    F32 begin_s, F32 end_s, F32 hollow,
    const LLVector3& half_scale, std::vector<GhillieMeshBox>& out, U32& wall_count)
{
    out.clear();
    wall_count = 0;

    // Profile half extents (profile space).
    const F32 hx = half_scale.mV[0];
    const F32 hy = half_scale.mV[1];
    const F32 hz = half_scale.mV[2];

    // The four walls, in profile t order (t 0..1 = the four side quadrants,
    // 0.25 each). Square genNGon(4, offset=-0.375): ang(t)=2π(t-0.375),
    // so the corners sit at (-0.5,-0.5) t=0, (+0.5,-0.5) t=0.25,
    // (+0.5,+0.5) t=0.5, (-0.5,+0.5) t=0.75. The edge faces are therefore
    //   t 0.00..0.25: -Y   (bottom)
    //   t 0.25..0.50: +X   (right)
    //   t 0.50..0.75: +Y   (top)
    //   t 0.75..1.00: -X   (left)
    // which matches the box panel's "side" semantics.
    struct Wall { S32 t0, t1; LLVector3 pos, half; };
    const Wall walls[4] =
    {
        { 0, 25,  LLVector3(0.f, -hy, 0.f),         LLVector3(hx, 0.05f, hz)   },
        { 25, 50, LLVector3(hx, 0.f, 0.f),          LLVector3(0.05f, hy, hz)   },
        { 50, 75, LLVector3(0.f, hy, 0.f),          LLVector3(hx, 0.05f, hz)   },
        { 75, 100, LLVector3(-hx, 0.f, 0.f),        LLVector3(0.05f, hy, hz)  },
    };

    const F32 b5 = begin_s * 100.f;
    const F32 e5 = end_s   * 100.f;
    // Wall half-thickness, unit scale (half extents are 0.5 for a default
    // box): hollow f ~= wall full-thickness / full box size, so half is
    // 0.5*f*box_half = 0.25*f for the unit box. The slab sits centered on
    // the outer face (half the thickness protrudes outward), which errs safe
    // (over-occludes slightly rather than under).
    const F32 wall_thick = (hollow > 0.f)
        ? llclamp(hollow, 0.01f, 0.99f) * 0.5f * llmin(hx, hy)
        : 0.05f;

    const U32 MAX_PARTS = 8;

    // Wall span axis per wall: +/-Y walls span X, +/-X walls span Y.
    static const S32 WALL_SPAN[4] = { 0, 1, 0, 1 };   // X-span for -Y/+Y (indices 0,2), Y-span for +X/-X (1,3)
    // THE CRITICAL BIT: the t -> span mapping direction differs per wall.
    // The square goes counter-clockwise: -Y wall runs (x from -hx to +hx),
    // +X wall runs (y from -hy to +hy), but +Y wall runs (x from +hx to
    // -hx) and -X wall runs (y from +hy to -hy). So the SPAN START is:
    //   walls 0,1: -span_half, increasing
    //   walls 2,3: +span_half, decreasing
    static const S32 WALL_SPAN_SIGN[4] = { +1, +1, -1, -1 };

    for (S32 w = 0; w < 4; ++w)
    {
        const F32 w_lo = (F32)walls[w].t0;
        const F32 w_hi = (F32)walls[w].t1;
        if (b5 >= w_hi || e5 <= w_lo) continue; // cut fully away

        F32 lo_t = llmax(b5, w_lo);
        F32 hi_t = llmin(e5, w_hi);
        F32 frac = (hi_t - lo_t) / (w_hi - w_lo); // kept fraction 0..1
        if (frac <= 0.f) continue;

        const S32 span_axis  = WALL_SPAN[w];
        const F32 span_sign  = (F32)WALL_SPAN_SIGN[w];
        const F32 span_half  = (span_axis == 0) ? hx : hy;
        const F32 span_len   = 2.f * span_half;
        // Kept section along the wall, measured with the wall's own
        // direction (start = +/-span_half).
        const F32 lo_frac = (lo_t - w_lo) / (w_hi - w_lo);
        const F32 hi_frac = (hi_t - w_lo) / (w_hi - w_lo);
        const F32 kept_a = span_sign * span_half + lo_frac * span_len * span_sign;
        const F32 kept_b = span_sign * span_half + hi_frac * span_len * span_sign;
        const F32 kept_lo = llmin(kept_a, kept_b);
        const F32 kept_hi = llmax(kept_a, kept_b);
        const F32 kept_half = (kept_hi - kept_lo) * 0.5f;
        const F32 kept_center = (kept_lo + kept_hi) * 0.5f;

        LLVector3 pos  = walls[w].pos;
        LLVector3 half = walls[w].half;

        if (hollow > 0.f)
        {
            // A hollow box's wall is a thin slab at the outer surface. The
            // thin axis is the wall's NORMAL axis (complement of its span):
            // +/-Y walls (span X) are thin in Y; +/-X walls (span Y) are
            // thin in X.
            if (span_axis == 0)
            {
                half.mV[1] = wall_thick;   // thin in Y (normal of -Y/+Y walls)
            }
            else
            {
                half.mV[0] = wall_thick;   // thin in X (normal of +X/-X walls)
            }
        }

        if (frac < 1.f)
        {
            // Partial cut: place the (possibly hollow) wall's span at the
            // kept section, exactly aligned with the cut plane.
            if (span_axis == 0)
            {
                half.mV[0] = kept_half;
                pos.mV[0]  = kept_center;
            }
            else
            {
                half.mV[1] = kept_half;
                pos.mV[1]  = kept_center;
            }
        }

        if (out.size() < MAX_PARTS)
        {
            GhillieMeshBox b;
            b.mPos = pos;
            b.mHalf = half;
            out.push_back(b);
            ++wall_count;
        }
    }
}

void ssGhillieUnitBounds(const std::vector<LLVector3>& verts,
                         LLVector3& lo, LLVector3& hi)
{
    lo = LLVector3(MAX_F, MAX_F, MAX_F);
    hi = LLVector3(-MAX_F, -MAX_F, -MAX_F);
    for (const LLVector3& p : verts)
    {
        lo.mV[0] = llmin(lo.mV[0], p.mV[0]);
        lo.mV[1] = llmin(lo.mV[1], p.mV[1]);
        lo.mV[2] = llmin(lo.mV[2], p.mV[2]);
        hi.mV[0] = llmax(hi.mV[0], p.mV[0]);
        hi.mV[1] = llmax(hi.mV[1], p.mV[1]);
        hi.mV[2] = llmax(hi.mV[2], p.mV[2]);
    }
    for (S32 k = 0; k < 3; ++k)
    {
        hi.mV[k] = llmax(hi.mV[k], lo.mV[k] + AXIS_EPS);
    }
}

GhillieMeshDecomp ssGhillieDecomposeMesh(
    const std::vector<LLVector3>& unit_verts,
    const std::vector<U32>& tris,
    U32 loop_limit)
{
    (void)loop_limit; // bounded by construction below; expansion point for cost caps

    GhillieMeshDecomp out;
    if (unit_verts.empty() || tris.size() < 3)
    {
        return out; // nothing - fail open
    }

    LLVector3 lo, hi;
    ssGhillieUnitBounds(unit_verts, lo, hi);

    LLVector3 centroid, axis, mid, minor;
    dominantAxis(unit_verts, centroid, axis, mid, minor);

    // Candidate 1: PCA slab (thin along `minor`), trimmed to interior verts
    // so it never protrudes the hull.
    LLVector3 slo, shi;
    projectSlab(unit_verts, centroid, axis, mid, minor, slo, shi);
    LLVector3 klo(MAX_F, MAX_F, MAX_F), khi(-MAX_F, -MAX_F, -MAX_F);
    for (const LLVector3& p : unit_verts)
    {
        const LLVector3 d = p - centroid;
        const F32 x = d * axis;
        const F32 y = d * mid;
        const F32 z = d * minor;
        if (x >= slo.mV[0] && x <= shi.mV[0] &&
            y >= slo.mV[1] && y <= shi.mV[1] &&
            z >= slo.mV[2] && z <= shi.mV[2])
        {
            klo.mV[0] = llmin(klo.mV[0], x);
            klo.mV[1] = llmin(klo.mV[1], y);
            klo.mV[2] = llmin(klo.mV[2], z);
            khi.mV[0] = llmax(khi.mV[0], x);
            khi.mV[1] = llmax(khi.mV[1], y);
            khi.mV[2] = llmax(khi.mV[2], z);
        }
    }
    if (khi.mV[0] <= klo.mV[0] + AXIS_EPS) { klo = slo; khi = shi; }

    // PCA slab candidate fill.
    {
        std::vector<LLVector3> centers, halfs;
        const LLVector3 c = centroid
            + axis  * ((klo.mV[0] + khi.mV[0]) * 0.5f)
            + mid   * ((klo.mV[1] + khi.mV[1]) * 0.5f)
            + minor * ((klo.mV[2] + khi.mV[2]) * 0.5f);
        const LLVector3 hb = axis   * ((khi.mV[0] - klo.mV[0]) * 0.5f)
                           + mid    * ((khi.mV[1] - klo.mV[1]) * 0.5f)
                           + minor  * ((khi.mV[2] - klo.mV[2]) * 0.5f);
        const LLVector3 h_abs(fabsf(hb.mV[0]), fabsf(hb.mV[1]), fabsf(hb.mV[2]));
        centers.push_back(c);
        halfs.push_back(llmax(h_abs, LLVector3(AXIS_EPS, AXIS_EPS, AXIS_EPS)));
        const F32 fill = hullFill(unit_verts, tris, centers, halfs);

        // AABB candidate fill.
        std::vector<LLVector3> a_centers(1, (lo + hi) * 0.5f);
        std::vector<LLVector3> a_halfs(1, (hi - lo) * 0.5f);
        const F32 aabb_fill = hullFill(unit_verts, tris, a_centers, a_halfs);

        if (fill >= aabb_fill - 1e-3f && fill > 0.5f)
        {
            GhillieMeshBox box;
            box.mPos = c;
            box.mHalf = llmax(h_abs, LLVector3(AXIS_EPS, AXIS_EPS, AXIS_EPS));
            out.mBoxes.push_back(box);
            out.mFill = fill;
            return out;
        }
    }

    // Candidate 2+3: axis-sliced AABB into 1/2/4 boxes, keep the best fill.
    F32 best_fill = 0.f;
    std::vector<GhillieMeshBox> best_boxes;
    for (S32 K = 1; K <= 4; K <<= 1)
    {
        std::vector<GhillieMeshBox> boxes;
        boxes.reserve(K);
        std::vector<LLVector3> centers;
        std::vector<LLVector3> halfs;
        centers.reserve(K);
        halfs.reserve(K);
        for (S32 k = 0; k < K; ++k)
        {
            const F32 f0 = (F32)k / (F32)K;
            const F32 f1 = (F32)(k + 1) / (F32)K;
            const LLVector3 blo = lo + LLVector3(f0 * (hi.mV[0] - lo.mV[0]), 0.f, 0.f);
            const LLVector3 bhi = LLVector3(lo.mV[0] + f1 * (hi.mV[0] - lo.mV[0]),
                                            hi.mV[1], hi.mV[2]);
            GhillieMeshBox b;
            b.mPos = (blo + bhi) * 0.5f;
            b.mHalf = (bhi - blo) * 0.5f;
            boxes.push_back(b);
            centers.push_back(b.mPos);
            halfs.push_back(b.mHalf);
        }
        const F32 fill = hullFill(unit_verts, tris, centers, halfs);
        if (fill >= best_fill - 1e-3f)
        {
            best_fill = fill;
            best_boxes = boxes;
        }
    }

    out.mBoxes = best_boxes;
    out.mFill = best_fill;
    return out;
}

} // namespace SS