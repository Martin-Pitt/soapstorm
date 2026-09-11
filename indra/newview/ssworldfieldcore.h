/**
 * @file ssworldfieldcore.h
 * @brief Atmo Magic world field: the cell grid's pure core - span insertion and the
 *        air classification (reachability, the geometric reach budget, the labels).
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

// <SS:Nexii> A CORE header (lldefs.h + <cmath>/<cstdint> + POD containers only; NO
// llmath.h), the same split every other Atmo core follows: the two algorithms that
// decide what the world field says live here as pure functions of the cell grid, and
// ssworldfield.cpp owns the navmesh snapshot, the scheduling and the storage.
// Nothing here reads a setting, the camera or a system, so the scratch harness at
// V:\Scratch\atmo\worldfield pins the exact code the viewer runs.
// Coordinates: cells are region-anchored and indexed col = y * res + x; spans are
// [k * res * res + col] so a slot is one contiguous plane (the layout
// SSAcoustic::Snap walks); z is the absolute altitude, metres.

#ifndef SS_WORLDFIELD_CORE_H
#define SS_WORLDFIELD_CORE_H

#include "lldefs.h"

#include <cmath>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

namespace SSWorldFieldCore
{
    // An empty span slot. Matches ssworldfield.cpp's NO_SURFACE; a slot is
    // occupied when its top is above NO_SURFACE * 0.5.
    constexpr F32 NO_SURFACE = -3.402823466e+38F;

    // The air labels, value-for-value SSWorldField::EAirLabel. Plain constants so the
    // core never includes the shell's header; the shell static_asserts them.
    constexpr U8 AIR_SOLID     = 0;
    constexpr U8 AIR_OUTDOORS  = 1;
    constexpr U8 AIR_SHELTERED = 2;
    constexpr U8 AIR_INTERIOR  = 3;
    constexpr U8 AIR_UNKNOWN   = 4;

    // "Never reached", and the exclusive upper bound on a stored distance.
    constexpr U16 DEPTH_UNREACHED = 0xFFFFu;

    // <SS:Nexii> The assumed minimum slab: two bodies closer together than this merge
    // into one span rather than leaving an air gap nothing fits through, so a wall
    // standing on a floor never reads as a hollow shell and a room is one air interval
    // whatever band its floor and its ceiling were rasterised in.
    constexpr F32 SPAN_SLAB_M = 0.25f;

    // <SS:Nexii> How much further than the outdoors reach an opening's budget still
    // carries as SHELTER. One cot(theta) of gap height is the distance the sky is
    // still overhead at theta; four of them is the distance a covered space still
    // feels like it has a way out - about 14 degrees of grazing sky. Past it the space
    // is interior. A multiple rather than a second angle, because the two are the same
    // geometry and should not be tunable apart.
    constexpr F32 SHELTER_MULT = 4.f;

    // Two gaps are adjacent only when their z-intervals STRICTLY overlap by this much.
    // A corner touch where one gap ends exactly where the neighbour's begins is a wall
    // junction, not a door.
    constexpr F32 TOUCH_EPS = 0.05f;

    // <SS:Nexii> Inserts one solid body into a cell's span list: sorted position, union
    // with touching or overlapping neighbours, and over the span budget a collapse of
    // the thinnest air gap (the two spans around it merge - the gap becomes solid). The
    // list stays sorted and every gap in it at least the slab threshold tall.
    inline void spanInsert(F32* span_bottom, F32* span_top, U8* span_flags,
                           S32 max_spans, size_t layer, size_t col, F32 bot, F32 tp, U8 fl)
    {
        S32 n = 0;
        while (n < max_spans && span_top[(size_t)n * layer + col] > NO_SURFACE * 0.5f) ++n;

        S32 at = n;
        while (at > 0 && span_bottom[(size_t)(at - 1) * layer + col] > bot) --at;

        if (at > 0 && bot - span_top[(size_t)(at - 1) * layer + col] < SPAN_SLAB_M)
        {
            const size_t pi = (size_t)(at - 1) * layer + col;
            const bool higher = tp > span_top[pi];
            span_bottom[pi] = llmin(span_bottom[pi], bot);
            span_top[pi] = llmax(span_top[pi], tp);
            if (higher) span_flags[pi] = fl;
            return;
        }
        if (at < n && tp > span_bottom[(size_t)at * layer + col] - SPAN_SLAB_M)
        {
            const size_t ni = (size_t)at * layer + col;
            const bool higher = bot < span_bottom[ni];
            span_bottom[ni] = llmin(span_bottom[ni], bot);
            span_top[ni] = llmax(span_top[ni], tp);
            if (!higher) span_flags[ni] = fl;
            return;
        }
        if (n == max_spans)
        {
            S32 thinnest = 0;
            F32 best = 3.4e38f;
            for (S32 j = 0; j + 1 < n; ++j)
            {
                const F32 gap = span_bottom[(size_t)(j + 1) * layer + col] - span_top[(size_t)j * layer + col];
                if (gap < best) { best = gap; thinnest = j; }
            }
            span_top[(size_t)thinnest * layer + col] = span_top[(size_t)(thinnest + 1) * layer + col];
            for (S32 j = thinnest + 1; j + 1 < n; ++j)
            {
                span_bottom[(size_t)j * layer + col] = span_bottom[(size_t)(j + 1) * layer + col];
                span_top[(size_t)j * layer + col] = span_top[(size_t)(j + 1) * layer + col];
                span_flags[(size_t)j * layer + col] = span_flags[(size_t)(j + 1) * layer + col];
            }
            --n;
            at = n;
            while (at > 0 && span_bottom[(size_t)(at - 1) * layer + col] > bot) --at;
        }

        for (S32 j = n; j > at; --j)
        {
            span_bottom[(size_t)j * layer + col] = span_bottom[(size_t)(j - 1) * layer + col];
            span_top[(size_t)j * layer + col] = span_top[(size_t)(j - 1) * layer + col];
            span_flags[(size_t)j * layer + col] = span_flags[(size_t)(j - 1) * layer + col];
        }
        span_bottom[(size_t)at * layer + col] = bot;
        span_top[(size_t)at * layer + col] = tp;
        span_flags[(size_t)at * layer + col] = fl;
    }

    // <SS:Nexii> The air classification over the cell grid - the pass the whole field
    // exists to produce. Per cell, the intervals between, below and above its solid
    // spans are its air gaps; two gaps of neighbouring cells are adjacent when their
    // z-intervals strictly overlap. Three steps over that graph:
    //
    // 1. REACHABILITY. Every cell's top gap is open sky and every gap of a border cell
    //    can walk out sideways; a flood from those marks the air connected to outside
    //    at all. What the flood never touched is sealed, and is interior by
    //    construction.
    //
    // 2. THE REACH BUDGET (the owner's outdoors rule). A gap under something is
    //    COVERED. An opening - a reachable uncovered gap touching covered air - hands
    //    its covered neighbours a budget in METRES, and every cell step spends one cell
    //    width of it. The budget is capped at every step by the LOCAL gap's own
    //    capacity, gap_height * open_k * SHELTER_MULT, so a gap that pinches under a
    //    low beam cuts whatever it was carrying while a gap that stays tall keeps it.
    //    That single rule is what makes ground under a sky platform 200 m up behave
    //    like ground: its gap is 200 m tall, so outdoors carries clear across the
    //    footprint - while a 2.4 m room's door hands about 2.4 m of outdoors and
    //    roughly 10 m of shelter past that, and a sealed room gets nothing. It replaces
    //    the sqrt(aperture) seed, which measured the opening's pixel count and so moved
    //    whenever the cell size did.
    //
    // 3. THE LABEL. Covered air within ONE unmultiplied open_k reach of its opening
    //    still has the sky overhead at theta or better, so it reads OUTDOORS. Past that
    //    but still in budget it is SHELTERED. Out of budget, or never reached,
    //    INTERIOR. The covered distance travelled is stored per gap in DECIMETRES - the
    //    figure the enclosure ramp uses and the acoustic bake reads as
    //    travel-to-outdoors.
    //
    // Widest-path first (a max-heap on remaining budget) rather than plain BFS, because
    // the strongest opening must decide how deep the shelter reaches: the first pop of
    // a gap is final, since every later entry carries a smaller budget.
    //
    // open_k is cot(theta). cell_m is the grid's cell size in metres. ceiling is where
    // a cell's top gap ends.
    inline void classify(S32 res, S32 max_spans, F32 cell_m, F32 ceiling, F32 open_k,
                         const F32* span_top, const F32* span_bottom,
                         std::vector<U8>& gap_label, std::vector<U16>& gap_depth)
    {
        const size_t layer = (size_t)res * (size_t)res;
        const size_t per_col = (size_t)max_spans + 1;
        const size_t nodes = layer * per_col;
        gap_label.assign(nodes, AIR_SOLID);
        gap_depth.assign(nodes, DEPTH_UNREACHED);
        if (res < 1 || max_spans < 1) return;

        static const S32 DX[4] = { 1, -1, 0, 0 };
        static const S32 DY[4] = { 0, 0, 1, -1 };
        const F32 step_m = llmax(cell_m, 0.01f);

        // Gap bounds per node, precomputed once: gap k of a cell with n spans runs from
        // the span below (or the world floor) to the span above (or the ceiling). Slots
        // past a cell's span count are empty.
        std::vector<F32> gb0(nodes, 0.f);
        std::vector<F32> gb1(nodes, 0.f);
        std::vector<S32> span_count(layer, 0);
        for (size_t col = 0; col < layer; ++col)
        {
            S32 n = 0;
            while (n < max_spans && span_top[(size_t)n * layer + col] > NO_SURFACE * 0.5f) ++n;
            span_count[col] = n;

            for (S32 k = 0; k <= max_spans; ++k)
            {
                const size_t node = col * per_col + (size_t)k;
                if (k > n) continue;
                gb0[node] = (k == 0) ? 0.f : span_top[(size_t)(k - 1) * layer + col];
                gb1[node] = (k == n) ? ceiling : span_bottom[(size_t)k * layer + col];
            }
        }

        auto touches = [&](size_t node, auto&& fn)
        {
            const size_t col = node / per_col;
            const S32 x = (S32)(col % (size_t)res);
            const S32 y = (S32)(col / (size_t)res);
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;

                const size_t ncol = (size_t)ny * (size_t)res + (size_t)nx;
                for (S32 kj = 0; kj <= max_spans; ++kj)
                {
                    const size_t nnode = ncol * per_col + (size_t)kj;
                    if (gb1[nnode] <= gb0[nnode] + TOUCH_EPS) continue;
                    if (!(gb0[node] < gb1[nnode] - TOUCH_EPS && gb0[nnode] < gb1[node] - TOUCH_EPS)) continue;
                    fn(nnode);
                }
            }
        };

        // ---- 1. reachability ----
        std::vector<U8> reached(nodes, 0);
        std::vector<S32> queue;
        queue.reserve(nodes / 8 + 1);
        for (size_t col = 0; col < layer; ++col)
        {
            const S32 x = (S32)(col % (size_t)res);
            const S32 y = (S32)(col / (size_t)res);
            const bool border = x == 0 || y == 0 || x == res - 1 || y == res - 1;
            const S32 n = span_count[col];

            for (S32 k = 0; k <= max_spans; ++k)
            {
                const size_t node = col * per_col + (size_t)k;
                if (gb1[node] <= gb0[node] + TOUCH_EPS) continue;
                if (k != n && !border) continue;
                reached[node] = 1;
                queue.push_back((S32)node);
            }
        }

        for (size_t head = 0; head < queue.size(); ++head)
        {
            touches((size_t)queue[head], [&](size_t nnode)
            {
                if (reached[nnode]) return;
                reached[nnode] = 1;
                queue.push_back((S32)nnode);
            });
        }

        // Covered: reachable air with structure standing over it. A gap below a cell's
        // top span always has that structure; the top gap never does.
        std::vector<U8> covered(nodes, 0);
        for (size_t col = 0; col < layer; ++col)
        {
            const S32 n = span_count[col];
            for (S32 k = 0; k < n; ++k)
            {
                const size_t node = col * per_col + (size_t)k;
                if (gb1[node] <= gb0[node] + TOUCH_EPS) continue;
                if (reached[node]) covered[node] = 1;
            }
        }

        // ---- 2. the reach budget ----
        std::vector<F32> remaining(nodes, -1.f);
        std::vector<F32> travelled(nodes, 0.f);
        std::priority_queue<std::pair<F32, S32> > heap;
        auto capacity = [&](size_t node) { return (gb1[node] - gb0[node]) * open_k; };

        for (size_t node = 0; node < nodes; ++node)
        {
            if (!covered[node]) continue;

            // Seeded by any uncovered reachable neighbour: that is an opening.
            bool porch = false;
            touches(node, [&](size_t nnode)
            {
                if (!covered[nnode] && reached[nnode]) porch = true;
            });
            if (!porch) continue;

            const F32 r = capacity(node) * SHELTER_MULT - step_m;
            if (r <= remaining[node]) continue;
            remaining[node] = r;
            travelled[node] = step_m;
            heap.emplace(r, (S32)node);
        }

        while (!heap.empty())
        {
            const F32 b = heap.top().first;
            const size_t node = (size_t)heap.top().second;
            heap.pop();
            if (b != remaining[node]) continue;     // a stronger seed already passed
            if (b <= 0.f) continue;                 // nothing left to hand inward

            const F32 here = travelled[node];
            touches(node, [&](size_t nnode)
            {
                if (!covered[nnode]) return;
                const F32 r = llmin(b, capacity(nnode) * SHELTER_MULT) - step_m;
                if (r <= 0.f || r <= remaining[nnode]) return;
                remaining[nnode] = r;
                travelled[nnode] = here + step_m;
                heap.emplace(r, (S32)nnode);
            });
        }

        // ---- 3. the labels and the covered distance ----
        for (size_t node = 0; node < nodes; ++node)
        {
            if (gb1[node] <= gb0[node] + TOUCH_EPS) continue;    // no such gap
            if (!reached[node])
            {
                gap_label[node] = AIR_INTERIOR;
                continue;
            }
            if (!covered[node])
            {
                gap_label[node] = AIR_OUTDOORS;
                gap_depth[node] = 0;
                continue;
            }
            if (remaining[node] <= 0.f)
            {
                gap_label[node] = AIR_INTERIOR;
                continue;
            }

            const F32 d = travelled[node];
            // Within one unmultiplied open_k of the LOCAL gap height the sky is still
            // up there: that is outdoors, however much roof stands over it.
            gap_label[node] = (d <= capacity(node)) ? AIR_OUTDOORS : AIR_SHELTERED;
            // Decimetres, saturating one short of the sentinel so a genuinely 6553 m
            // walk can never read as "never visited".
            const U32 dm = (U32)(d * 10.f + 0.5f);
            gap_depth[node] = (U16)llmin(dm, (U32)DEPTH_UNREACHED - 1u);
        }
    }
}

#endif
