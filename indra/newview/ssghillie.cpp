/**
 * @file ssghillie.cpp
 * @brief Ghillie: multi-threaded software Hi-Z object occlusion culling.
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

#include "ssghillie.h"
#include "ssghilliemesh.h"

#include "llagent.h"            // gAgent, LLAgent::TELEPORT_NONE
#include "llagentcamera.h"      // gAgentCamera, cameraMouselook()
#include "llappviewer.h"        // gFrameCount, gUseWireframe
#include "lldrawable.h"
#include "llenvironment.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llfile.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llspatialpartition.h" // LLCullResult
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvieweroctree.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "llworld.h"
#include "m3math.h"
#include "pipeline.h"
#include "threadpool.h"
#include "workqueue.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <thread>

// <SS:Nexii> Ghillie: offloaded object occlusion culling
//
// One worker job per frame ranks sound occluders, rasterizes them into a
// low resolution integer-depth Hi-Z pyramid and tests the agent-space AABBs
// of every octree node the traversal visited, all off the render thread.
// Published verdicts are merged into per-node streaks on the main thread and
// consumed by LLOctreeCull::earlyFail. Everything fails open to "visible".
//
// v2 (this file):
//   - occluders: exact parametric boxes + static AABB fallback + mesh
//     decomposition boxes (separate low-priority pool) + a worker-safe
//     terrain/water base layer snapshotted from the heightfield at gather;
//   - static-landscape tier: how long an object has not moved decides what
//     may use AABB/mesh occluders and tile persistence; teleports/death edge
//     the camera into a fast full-rebuild so respawn stays flat;
//   - activity + proximity gate defers close non-static occluders during
//     active camera motion (walk/run/mouselook/vehicle) so FPS does not tank
//     exactly when the user turns a corner;
//   - parallax-ranked occluder budget keeps per-frame raster work bounded.

bool SSGhillie::sActive = false;

extern bool gCubeSnapshot; // defined in llviewerdisplay.cpp

//static
bool SSGhillie::activeForCurrentPass()
{
    // only the main world camera pass is owned by Ghillie; shadow,
    // reflection, snapshot and HUD passes keep the stock GL query machine
    return sActive
        && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD
        && !LLPipeline::sShadowRender
        && !LLPipeline::sRenderingHUDs
        && !gCubeSnapshot;
}

namespace
{
    constexpr F32 CLIP_W_EPSILON = 1e-3f;
    constexpr U32 DEPTH_INIT = 0x00FFFFFF;      // far plane, 24 bit fixed point
    constexpr U32 WATER_MARK = 1u << 24;        // tag bit for water-plane occluders
    constexpr U32 DEPTH_MASK = DEPTH_INIT;      // drops the tag
    constexpr F32 DEPTH_SCALE_F = 16777215.f;
    constexpr U32 DEPTH_BIAS = 8;               // depth slack before declaring occluded
    constexpr F32 EYE_FUDGE = 0.5f;             // stock SG_OCCLUSION_FUDGE * 2
    constexpr F32 MAX_OCCLUDEE_RADIUS = 256.f;  // never occlusion cull huge nodes
    constexpr F32 MAX_OCCLUDER_RADIUS = 512.f;
    constexpr U32 MAX_DRAWABLE_SCAN = 8192;

    // Combat / proximity gates (see design review)
    constexpr F32 PEEK_DEFER_RADIUS = 5.f;              // walls inside this get deferred while active
    constexpr F32 TELEPORT_DELTA = 100.f;               // camera jump past this = teleport
    constexpr F32 FAST_REBUILD_ROT_THRESHOLD = 0.5f;    // radians/frame at which a camera jump forces fast rebuild
    constexpr U32 MODE_PERSIST_FRAMES = 12;             // hold a mode for this many frames before switching

    // Occluder kinds (OccluderSolid::mKind)
    constexpr U8 OC_SEG_BOX = 0;        // exact parametric box (segment)
    constexpr U8 OC_STATIC_BOX = 2;     // AABB fallback for parametered static objects
    constexpr U8 OC_STATIC_SLAB = 3;    // mesh decomposition box (static only)

    const S32 CUBE_CORNERS[8][3] =
    {
        { -1, -1, -1 }, { 1, -1, -1 }, { -1,  1, -1 }, { 1,  1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { -1,  1,  1 }, { 1,  1,  1 },
    };

    struct ScreenVert
    {
        F32 x, y, u;
    };

    inline U32 depthFromNdc(F32 zndc)
    {
        const F32 u01 = llclamp((zndc + 1.f) * 0.5f, 0.f, 1.f);
        return (U32)(u01 * DEPTH_SCALE_F + 0.5f);
    }

    // Sutherland-Hodgman clip against clip.w >= epsilon; returns vertex count
    S32 clipPolyW(const LLVector4* in, S32 nIn, LLVector4* out)
    {
        S32 n = 0;
        for (S32 i = 0; i < nIn; ++i)
        {
            const LLVector4& a = in[i];
            const LLVector4& b = in[(i + 1) % nIn];
            const bool a_in = a.mV[3] >= CLIP_W_EPSILON;
            const bool b_in = b.mV[3] >= CLIP_W_EPSILON;
            if (a_in && n < 8)
            {
                out[n++] = a;
            }
            if (a_in != b_in && n < 8)
            {
                const F32 t = (CLIP_W_EPSILON - a.mV[3]) / (b.mV[3] - a.mV[3]);
                const LLVector4 clipped(a.mV[0] + (b.mV[0] - a.mV[0]) * t,
                                        a.mV[1] + (b.mV[1] - a.mV[1]) * t,
                                        a.mV[2] + (b.mV[2] - a.mV[2]) * t,
                                        CLIP_W_EPSILON);
                out[n++] = clipped;
            }
        }
        return n;
    }

    // Rasterize one clip-space polygon (fan triangulated) into a horizontal
    // strip of the HZB. Depth is interpolated screen-linearly, which
    // over-estimates a plane's perspective depth, so the written occluder
    // depth always errs on the safe (too far) side.
    void rasterPoly(const LLVector4* poly, S32 nIn, U32* hzb, U32 width, U32 height, U32 y0, U32 y1)
    {
        LLVector4 clipped[8];
        const S32 n = clipPolyW(poly, nIn, clipped);
        if (n < 3) return;

        ScreenVert sv[8];
        F32 minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (S32 i = 0; i < n; ++i)
        {
            const LLVector4& v = clipped[i];
            const F32 iw = 1.f / v.mV[3];
            const F32 x = (v.mV[0] * iw * 0.5f + 0.5f) * (F32)width;
            const F32 y = (v.mV[1] * iw * 0.5f + 0.5f) * (F32)height;
            sv[i].x = x;
            sv[i].y = y;
            sv[i].u = (F32)depthFromNdc(v.mV[2] * iw);
            minx = llmin(minx, x);
            maxx = llmax(maxx, x);
            miny = llmin(miny, y);
            maxy = llmax(maxy, y);
        }

        S32 px0 = llclamp((S32)floorf(minx), 0, (S32)width - 1);
        S32 px1 = llclamp((S32)ceilf(maxx), 0, (S32)width - 1);
        S32 py0 = llclamp((S32)floorf(miny), (S32)y0, (S32)y1 - 1);
        S32 py1 = llclamp((S32)ceilf(maxy), (S32)y0, (S32)y1 - 1);
        if (px0 > px1 || py0 > py1) return;

        for (S32 i = 1; i < n - 1; ++i)
        {
            const ScreenVert& a = sv[0];
            const ScreenVert& b = sv[i];
            const ScreenVert& c = sv[i + 1];
            const F32 area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (fabsf(area) < 1e-9f) continue;
            const F32 inv = 1.f / area;
            for (S32 py = py0; py <= py1; ++py)
            {
                const F32 fy = (F32)py + 0.5f;
                U32* row = hzb + (U32)py * width;
                for (S32 px = px0; px <= px1; ++px)
                {
                    const F32 fx = (F32)px + 0.5f;
                    const F32 w0 = ((c.x - b.x) * (fy - b.y) - (c.y - b.y) * (fx - b.x)) * inv;
                    const F32 w1 = ((a.x - c.x) * (fy - c.y) - (a.y - c.y) * (fx - c.x)) * inv;
                    const F32 w2 = ((b.x - a.x) * (fy - a.y) - (b.y - a.y) * (fx - a.x)) * inv;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                    const F32 u = w0 * a.u + w1 * b.u + w2 * c.u;
                    const U32 ui = (U32)llclamp(u, 0.f, (F32)DEPTH_INIT);
                    U32& dst = row[px];
                    if (ui > dst) dst = ui;
                }
            }
        }
    }

    // Rasterize a box given agent-space pos/rot/half into the strip.
    // Exact analytic box decomposition for square-profile line-path prims.
    //
    // profile t spans 4 quadrants of the unit square, one per "side"
    // (t_step = 0.25 each, matching the box panel's "side" semantics):
    //   t 0.00..0.25: -X wall
    //   t 0.25..0.50: -Y wall
    //   t 0.50..0.75: +X wall
    //   t 0.75..1.00: +Y wall
    //
    // begin/end cut those sides sequentially (each whole 0.25 interval =
    // the side's full removal), and an exact box with cuts resolves to at
    // most 3 whole-wall boxes + 1 truncated partial box.
    //
    // hollow: the remaining walls become 4 thin side wall boxes at the
    // given thickness (hollow is a fraction of the half profile).
    //
    // Cuts are applied after hollow: a wall the cut fully removes is
    // dropped, a partial cut is clamped onto the wall box's extent.
    //
    // Boxes are emitted in profile-local orientation (the four walls are
    // axis-aligned in profile space); the drawable's full scale/rotation
    // is applied by the caller so profile (unit-ish) maps to world exactly.
    struct BoxPart
    {
        LLVector3 mPosLocal;    // offset in profile scale (scale = full drawable scale)
        LLVector3 mHalfLocal;   // extents in profile scale
    };
    void decomposeSquareBox(F32 begin_s, F32 end_s, F32 hollow,
                            LLVector3& half_scale, std::vector<BoxPart>& out, U32& wall_count)
    {
        // Profile (half extents, profile space).
        const F32 hx = half_scale.mV[0];
        const F32 hy = half_scale.mV[1];
        const F32 hz = half_scale.mV[2];

        // Walls, in profile t order (see comment above).
        struct Wall { S32 t0, t1; LLVector3 pos, half; };   // pos/half in profile space
        const Wall walls[4] =
        {
            { 0, 25, LLVector3(-hx, 0.f, 0.f),          LLVector3(0.05f, hy, hz)     },
            { 25, 50, LLVector3(0.f, -hy, 0.f),         LLVector3(hx, 0.05f, hz)    },
            { 50, 75, LLVector3(hx, 0.f, 0.f),          LLVector3(0.05f, hy, hz)    },
            { 75, 100, LLVector3(0.f, hy, 0.f),         LLVector3(hx, 0.05f, hz)   },
        };

        // Which walls are present (fully or partially) and their t ranges.
        // begin/end are fractions of the full 4-side loop.
        const F32 b5 = begin_s * 100.f;
        const F32 e5 = end_s   * 100.f;

        const F32 wall_thick = (hollow > 0.f)
            ? llclamp(hollow, 0.01f, 0.99f) * 0.5f * llmin(hx, hy)
            : 0.05f;

        const U32 MAX_PARTS = 8;
        out.clear();

        for (S32 w = 0; w < 4; ++w)
        {
            const F32 w_lo = (F32)walls[w].t0;
            const F32 w_hi = (F32)walls[w].t1;
            // The wall is cut away when begin >= w_hi or end <= w_lo.
            if (b5 >= w_hi || e5 <= w_lo) continue;

            // Partial cut: clamp the wall extent by how much of it is kept.
            F32 lo_t = llmax(b5, w_lo);
            F32 hi_t = llmin(e5, w_hi);
            F32 frac = (hi_t - lo_t) / (w_hi - w_lo);   // kept fraction 0..1
            if (frac <= 0.f) continue;

            LLVector3 pos = walls[w].pos;
            LLVector3 half = walls[w].half;

            if (hollow > 0.f)
            {
                // A hollow box's wall is a thin slab at the outer surface.
                if (walls[w].half.mV[0] <= 0.1f)  // -X/+X wall: slab in X
                {
                    half.mV[0] = wall_thick;
                }
                else                               // -Y/+Y wall: slab in Y
                {
                    half.mV[1] = wall_thick;
                }
                // For hollow, cuts apply to the slab's length/width.
                if (frac < 1.f)
                {
                    // Assess which length axis the cut is along (the wall's
                    // primary span in Y or X). For -X/+X walls the span is Y.
                    if (walls[w].half.mV[0] <= 0.1f)
                    {
                        half.mV[1] *= frac;
                        if (lo_t > w_lo)
                        {
                            pos.mV[1] = -hy + (lo_t - w_lo) / (w_hi - w_lo) * (2.f * hy) + half.mV[1];
                        }
                        else
                        {
                            pos.mV[1] = -hy + half.mV[1];
                        }
                    }
                    else
                    {
                        half.mV[0] *= frac;
                        if (lo_t > w_lo)
                        {
                            pos.mV[0] = -hx + (lo_t - w_lo) / (w_hi - w_lo) * (2.f * hx) + half.mV[0];
                        }
                        else
                        {
                            pos.mV[0] = -hx + half.mV[0];
                        }
                    }
                }
            }
            else if (frac < 1.f)
            {
                // Solid box: a partial cut leaves a box whose span is the
                // kept fraction. (Full-removal walls were skipped above.)
                if (walls[w].half.mV[0] <= 0.1f)
                {
                    half.mV[1] *= frac;
                    pos.mV[1] = -hy + (lo_t - w_lo) / (w_hi - w_lo) * (2.f * hy) + half.mV[1];
                }
                else
                {
                    half.mV[0] *= frac;
                    pos.mV[0] = -hx + (lo_t - w_lo) / (w_hi - w_lo) * (2.f * hx) + half.mV[0];
                }
            }

            if (out.size() < MAX_PARTS)
            {
                out.push_back(BoxPart{ pos, half });
                ++wall_count;
            }
        }
    }

    void rasterCullBox(const LLVector3& pos, const LLQuaternion& rot, const LLVector3& half,
                       const LLMatrix4& modelview, const LLMatrix4& projection,
                       const LLVector3& eye, U32* hzb, U32 width, U32 height, U32 y0, U32 y1)
    {
        const LLMatrix3 rot3(rot);
        LLVector3 corners[8];
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3 local(half.mV[0] * (F32)CUBE_CORNERS[i][0],
                                  half.mV[1] * (F32)CUBE_CORNERS[i][1],
                                  half.mV[2] * (F32)CUBE_CORNERS[i][2]);
            corners[i] = pos + local * rot3;
        }

        for (S32 axis = 0; axis < 3; ++axis)
        {
            for (S32 sign = -1; sign <= 1; sign += 2)
            {
                LLVector3 axis_local(0.f, 0.f, 0.f);
                axis_local.mV[axis] = (F32)sign;
                const LLVector3 axis_world = axis_local * rot3;
                const LLVector3 face_center = pos + axis_world * half.mV[axis];
                if (dot(axis_world, face_center - eye) >= 0.f) continue; // back facing

                LLVector4 poly[4];
                S32 cnt = 0;
                for (S32 i = 0; i < 8 && cnt < 4; ++i)
                {
                    if (CUBE_CORNERS[i][axis] == sign)
                    {
                        const LLVector3& c3 = corners[i];
                        const LLVector4 world(c3.mV[0], c3.mV[1], c3.mV[2], 1.f);
                        const LLVector4 view = world * modelview;
                        poly[cnt++] = view * projection;
                    }
                }
                if (cnt == 4)
                {
                    rasterPoly(poly, 4, hzb, width, height, y0, y1);
                }
            }
        }
    }

    // Rasterize a water-plane quad at the given height, only when the eye is on
    // the other side. Conservative: the plane only ever occludes what lies
    // below the waterline (testOccludee applies the poke-above exemption via
    // the WATER_MARK bit - the depth is tagged, not just written).
    void rasterCullPlaneZ(F32 water_z, const LLVector3& eye,
                          const LLVector3& region_origin_agent,
                          const LLMatrix4& modelview, const LLMatrix4& projection,
                          U32* hzb, U32 width, U32 height, U32 y0, U32 y1)
    {
        if (fabsf(eye.mV[2] - water_z) < 0.1f) return; // in the plane: no occlusion
        const F32 W = 4096.f;
        const F32 cx = region_origin_agent.mV[0];
        const F32 cy = region_origin_agent.mV[1];
        const LLVector4 quad[4] =
        {
            LLVector4(cx - W, cy - W, water_z, 1.f),
            LLVector4(cx + W, cy - W, water_z, 1.f),
            LLVector4(cx + W, cy + W, water_z, 1.f),
            LLVector4(cx - W, cy + W, water_z, 1.f),
        };
        LLVector4 clip[4];
        for (S32 i = 0; i < 4; ++i) clip[i] = quad[i] * modelview * projection;
        // Raster with the WATER_MARK bit set so the test can exempt nodes
        // that rise above the surface.
        LLVector4 poly[4];
        const S32 n = clipPolyW(clip, 4, poly);
        if (n < 3) return;
        ScreenVert sv[8];
        F32 minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (S32 i = 0; i < n; ++i)
        {
            const LLVector4& v = poly[i];
            const F32 iw = 1.f / v.mV[3];
            const F32 x = (v.mV[0] * iw * 0.5f + 0.5f) * (F32)width;
            const F32 y = (v.mV[1] * iw * 0.5f + 0.5f) * (F32)height;
            sv[i].x = x;
            sv[i].y = y;
            sv[i].u = (F32)depthFromNdc(v.mV[2] * iw);
            minx = llmin(minx, x);
            maxx = llmax(maxx, x);
            miny = llmin(miny, y);
            maxy = llmax(maxy, y);
        }
        S32 px0 = llclamp((S32)floorf(minx), 0, (S32)width - 1);
        S32 px1 = llclamp((S32)ceilf(maxx), 0, (S32)width - 1);
        S32 py0 = llclamp((S32)floorf(miny), (S32)y0, (S32)y1 - 1);
        S32 py1 = llclamp((S32)ceilf(maxy), (S32)y0, (S32)y1 - 1);
        if (px0 > px1 || py0 > py1) return;
        for (S32 py = py0; py <= py1; ++py)
        {
            U32* row = hzb + (U32)py * width;
            for (S32 px = px0; px <= px1; ++px)
            {
                const U32 marked = (U32)sv[0].u | WATER_MARK;
                U32& dst = row[px];
                if (marked > dst) dst = marked;
            }
        }
    }

    // Terrain: for each texel in the strip, binary-search the ray from the
    // eye through the texel against the heightfield snapshot (region-local
    // grid). Writes the terrain's depth when the ray crosses it, else leaves
    // the texel at far. Conservative around steep banks by the DEPTH_BIAS and
    // by clamping the search window to never cross the near plane.
    void rasterCullTerrain(const std::vector<F32>& heights, S32 grid, F32 meters_per_grid,
                           const LLVector3& region_origin,
                           const LLMatrix4& modelview, const LLMatrix4& projection,
                           const LLVector3& eye, U32* hzb, U32 width, U32 height, U32 y0, U32 y1)
    {
        if (grid < 2 || heights.size() < (size_t)(grid * grid)) return;

        auto terrainZ = [&](F32 x_agent, F32 y_agent) -> F32
        {
            const F32 x = x_agent - region_origin.mV[0];
            const F32 y = y_agent - region_origin.mV[1];
            const F32 f = 1.f / meters_per_grid;
            const F32 xf = x * f;
            const F32 yf = y * f;
            S32 x0 = (S32)floorf(xf);
            S32 y0 = (S32)floorf(yf);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x0 > grid - 2) x0 = grid - 2;
            if (y0 > grid - 2) y0 = grid - 2;
            const F32 frac_x = llclamp(xf - (F32)x0, 0.f, 1.f);
            const F32 frac_y = llclamp(yf - (F32)y0, 0.f, 1.f);
            const F32 h00 = heights[(size_t)y0     * grid + x0];
            const F32 h10 = heights[(size_t)y0     * grid + x0 + 1];
            const F32 h01 = heights[(size_t)(y0+1) * grid + x0];
            const F32 h11 = heights[(size_t)(y0+1) * grid + x0 + 1];
            const F32 hx0 = h00 + (h10 - h00) * frac_x;
            const F32 hx1 = h01 + (h11 - h01) * frac_x;
            return hx0 + (hx1 - hx0) * frac_y;
        };

        for (U32 py = y0; py <= y1; ++py)
        {
            const F32 fy = ((F32)py + 0.5f) / (F32)height * 2.f - 1.f;
            U32* row = hzb + (U32)py * width;
            for (U32 px = 0; px < width; ++px)
            {
                const F32 fx = ((F32)px + 0.5f) / (F32)width * 2.f - 1.f;

                // Build the ray direction in view space from NDC inverse.
                const F32 znear = 0.5f; // NDC z of the near plane (projection agnostic for the ray)
                LLVector3 dir_view(fx, fy, 1.f);
                dir_view -= LLVector3(0.f, 0.f, znear);
                dir_view.normVec();
                if (!dir_view.isFinite()) continue;

                // dir_view is in camera space (Z forward). Convert to agent.
                // The view matrix columns are left/up/back; inverse is its
                // transpose (orthonormal), so agent dir = (dir.dot(left), ...)
                const LLMatrix4& mv = modelview;
                const LLVector3 left(mv.mMatrix[0][0], mv.mMatrix[1][0], mv.mMatrix[2][0]);
                const LLVector3 up  (mv.mMatrix[0][1], mv.mMatrix[1][1], mv.mMatrix[2][1]);
                const LLVector3 back(mv.mMatrix[0][2], mv.mMatrix[1][2], mv.mMatrix[2][2]);
                const LLVector3 dir_agent = left * dir_view.mV[0] + up * dir_view.mV[1] + back * dir_view.mV[2];
                if (dir_agent.mV[2] >= -1e-4f)
                {
                    // Ray does not descend: can't hit terrain. A ridge in
                    // front that rises above the eye is handled because it is
                    // below the ray's horizon only if the ray descends; leave
                    // the texel untouched (safe).
                    continue;
                }

                // Binary search t where ray_z == terrainZ.
                F32 t_lo = 0.1f;
                F32 t_hi = 4096.f;
                const F32 y0_ray = eye.mV[2];
                const F32 y1_ray = eye.mV[2] + dir_agent.mV[2] * t_hi;
                const LLVector3 p_far = eye + dir_agent * t_hi;
                const F32 h_far = terrainZ(p_far.mV[0], p_far.mV[1]);
                if (y1_ray >= h_far)
                {
                    continue; // ray stays above terrain all the way
                }
                // Find the crossing t.
                F32 t_cross = t_hi;
                for (S32 iter = 0; iter < 14; ++iter)
                {
                    const F32 t_mid = (t_lo + t_hi) * 0.5f;
                    const LLVector3 p = eye + dir_agent * t_mid;
                    const F32 h = terrainZ(p.mV[0], p.mV[1]);
                    const F32 ray_z = eye.mV[2] + dir_agent.mV[2] * t_mid;
                    if (ray_z < h)
                    {
                        t_hi = t_mid; // ray is under the terrain: move back
                    }
                    else
                    {
                        t_lo = t_mid;
                    }
                }
                t_cross = t_hi;

                // Project the crossing point and write its depth.
                const LLVector3 p = eye + dir_agent * t_cross;
                const LLVector4 world(p.mV[0], p.mV[1], p.mV[2], 1.f);
                const LLVector4 clip = (world * modelview) * projection;
                if (clip.mV[3] < CLIP_W_EPSILON) continue;
                const F32 u = (F32)depthFromNdc(clip.mV[2] / clip.mV[3]);
                const U32 ui = (U32)llclamp(u, 0.f, (F32)DEPTH_INIT);
                U32& dst = row[px];
                if (ui > dst) dst = ui;
            }
        }
    }
} // namespace

//static
void SSGhillie::preCull(const LLViewerCamera& camera)
{
    sActive = enabledSetting() && !gUseWireframe && LLPipeline::sUseOcclusion != 0;
    if (!sActive)
    {
        if (!mNodeStates.empty())
        {
            clearNodeStates();
        }
        mSeenNodes.clear();
        mConsumedGen = mPublishedGen.load(); // discard anything stale
        return;
    }

    updateCameraMode(camera);

    // watchdog: a stalled or superseded job must fail open. Never recycle
    // mJob here: stale worker tasks may still be executing. Mark the job
    // abandoned; workers finish their countdown cheaply and publish a null
    // result, after which the next frame can post a fresh job.
    if (mJobInFlight.load(std::memory_order_acquire) && gFrameCount - mLastJobFrame > 2)
    {
        mJob.mAbandon.store(true, std::memory_order_release);
        clearNodeStates();
    }

    const U32 published = mPublishedGen.load(std::memory_order_acquire);
    if (published != NO_GENERATION && published != mConsumedGen)
    {
        Result& result = mResults[mPublishedIdx.load(std::memory_order_acquire)];
        if (result.mGeneration == published)
        {
            const LLVector3 delta_pos = camera.getOrigin() - result.mEye;
            const F32 delta_at = 1.f - dot(camera.getAtAxis(), result.mAtAxis);
            const bool teleport_style = delta_pos.magVec() > TELEPORT_DELTA
                || delta_at > FAST_REBUILD_ROT_THRESHOLD;
            if (teleport_style || delta_pos.magVec() > 1.f || delta_at > 0.02f)
            {   // camera moved too far since the test frame: stale, fail open
                clearNodeStates();
            }
            else
            {
                static LLCachedControl<U32> hysteresis_setting(gSavedSettings, "SSGhillieHysteresis", 2);
                const U32 hysteresis = llclamp((U32)hysteresis_setting, 1u, 16u);
                U64 occluded = 0;
                for (std::vector<Verdict>::const_iterator it = result.mVerdicts.begin(); it != result.mVerdicts.end(); ++it)
                {
                    if (!it->mGroup) continue;
                    NodeState& state = mNodeStates[it->mGroup];
                    state.mVerdictFrame = gFrameCount;
                    if (it->mOccluded)
                    {
                        state.mStreak += 1;
                        ++occluded;
                    }
                    else
                    {
                        state.mStreak = 0;
                    }
                }
                mTotalNodesOccluded = occluded;
            }
        }
        mConsumedGen = published;
    }

    if (gFrameCount - mLastPruneFrame > 256)
    {
        mLastPruneFrame = gFrameCount;
        for (std::unordered_map<LLViewerOctreeGroup*, NodeState>::iterator it = mNodeStates.begin(); it != mNodeStates.end();)
        {
            if (it->second.mLastSeenFrame + 256 < gFrameCount)
            {
                it = mNodeStates.erase(it); // keyed by pointer, never dereferenced
            }
            else
            {
                ++it;
            }
        }
    }
}

SSGhillie::SSGhillie()
{
}

void SSGhillie::cleanupSingleton()
{
    if (mThreadPool)
    {
        mThreadPool->close();
        mThreadPool.reset();
    }
    if (mMeshPool)
    {
        mMeshPool->close();
        mMeshPool.reset();
    }
    sActive = false;
    mJobInFlight.store(false, std::memory_order_release);
    mPublishedGen.store(NO_GENERATION);
    mConsumedGen = NO_GENERATION;
    clearNodeStates();
    mSeenNodes.clear();
    mObjectRecords.clear();
    {
        std::lock_guard<std::mutex> lock(mMeshCacheMutex);
        mMeshCache.clear();
    }
}

// static-ish helper
bool SSGhillie::enabledSetting() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSGhillieEnabled", false);
    return enabled;
}

void SSGhillie::clearNodeStates()
{
    mNodeStates.clear();
}

void SSGhillie::updateCameraMode(const LLViewerCamera& camera)
{
    CameraActivity new_mode = CAMERA_RELAXED;

    const F32 avg_speed = camera.getAverageSpeed();
    const F32 avg_ang = camera.getAverageAngularSpeed();
    const F32 ang = 1.f - dot(mCameraState.mLastAt, camera.getAtAxis());
    const LLVector3 delta = camera.getOrigin() - mCameraState.mLastPos;
    const F32 dist = delta.magVec();

    // Teleport / death / region-change / any huge camera jump.
    if (dist > TELEPORT_DELTA || (ang > FAST_REBUILD_ROT_THRESHOLD && dist > 1.f))
    {
        new_mode = CAMERA_FAST_REBUILD;
    }
    else if (avg_ang > 0.10f || avg_speed > 2.f)
    {
        new_mode = CAMERA_VEHICLE;
    }
    else if (gAgentCamera.cameraMouselook() || avg_ang > 0.025f)
    {
        new_mode = CAMERA_MLOOK;
    }
    else if (avg_speed > 0.2f || dist < 1e-3f)
    {
        new_mode = CAMERA_WALK_RUN;
    }
    else
    {
        new_mode = CAMERA_RELAXED; // stillness / relaxed look around
    }

    // Teleport / fast-rebuild is immediate (it must not be smoothed by
    // MODE_PERSIST_FRAMES, or respawn takes too long to re-cull).
    if (new_mode == CAMERA_FAST_REBUILD)
    {
        mCameraState.mActivity = new_mode;
        mCameraState.mModeFrame = gFrameCount;
        mLastTeleportFrame = gFrameCount;
    }
    else if (mCameraState.mActivity != new_mode
             && gFrameCount - mCameraState.mModeFrame > MODE_PERSIST_FRAMES)
    {
        mCameraState.mActivity = new_mode;
        mCameraState.mModeFrame = gFrameCount;
    }

    mCameraState.mLastPos = camera.getOrigin();
    mCameraState.mLastAt = camera.getAtAxis();
    mCameraState.mLastFrame = gFrameCount;
    mCameraState.mDeferNearPeek = (mCameraState.mActivity == CAMERA_MLOOK
        || mCameraState.mActivity == CAMERA_VEHICLE
        || mCameraState.mActivity == CAMERA_WALK_RUN
        || mCameraState.mActivity == CAMERA_FAST_REBUILD);
}

void SSGhillie::updateStaticRecords(LLCullResult& cull)
{
    static LLCachedControl<bool> static_setting(gSavedSettings, "SSGhillieStatic", true);
    if (!static_setting) return;
    static LLTimer cadence_timer;
    static U32 last_cadence_frame = 0;
    if (gFrameCount - last_cadence_frame < 60)
    {
        return;
    }
    last_cadence_frame = gFrameCount;

    static LLCachedControl<F32> fast_secs(gSavedSettings, "SSGhillieStaticFastSecs", 30.f);
    static LLCachedControl<F32> static_minutes(gSavedSettings, "SSGhillieStaticMinutes", 2.f);
    const F32 fast_secs_v = llclamp((F32)fast_secs, 1.f, 120.f);
    const F32 static_secs_v = llclamp((F32)static_minutes, 1.f, 10.f) * 60.f;

    S32 scan = 0;
    const bool teleporting = (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE);
    for (LLCullResult::drawable_iterator iter = cull.beginVisibleList(); iter != cull.endVisibleList(); ++iter)
    {
        if (++scan > 1024) break;
        LLDrawable* drawable = *iter;
        if (!drawable || drawable->isDead()) continue;
        LLViewerObject* vobj = drawable->getVObj();
        if (!vobj) continue;

        const LLUUID& id = vobj->getID();
        const LLVector3 pos = drawable->getPositionAgent();
        const LLVector3 scale = drawable->getScale();

        ObjectRecord& rec = mObjectRecords[id];
        if (rec.mObjectID.isNull()) rec.mObjectID = id;

        const LLVector3 move = pos - rec.mLastPos;
        const LLVector3 scale_delta = scale - rec.mLastScale;
        const bool moved = move.magVecSquared() > 0.01f || scale_delta.magVecSquared() > 0.001f;
        if (rec.mFirstSeenFrame == 0) rec.mFirstSeenFrame = gFrameCount;

        if (moved
            || vobj->flagScripted()
            || (vobj->getVelocity().magVecSquared() > 0.01f)
            || teleporting)
        {
            rec.mLastMoveFrame = gFrameCount;
            rec.mStillSeconds = 0.f;
            rec.mNeverMoved = false;
            rec.mTeleported = teleporting;
        }
        else
        {
            rec.mStillSeconds += cadence_timer.getElapsedSeconds();
            if (rec.mStillSeconds >= static_secs_v)
            {
                rec.mLastMoveFrame = 0; // static
            }
        }

        if (rec.mNeverMoved && gFrameCount - rec.mFirstSeenFrame > 8)
        {
            // Never observed moving: static early.
            rec.mStillSeconds = fast_secs_v;
            rec.mLastMoveFrame = 0;
        }

        rec.mLastPos = pos;
        rec.mLastScale = scale;
    }

    if ((gFrameCount & 0x7F) == 0)
    {
        for (std::unordered_map<LLUUID, ObjectRecord>::iterator it = mObjectRecords.begin();
             it != mObjectRecords.end();)
        {
            if (it->second.mLastMoveFrame != 0 && gFrameCount - it->second.mLastMoveFrame > 256)
            {
                it = mObjectRecords.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void SSGhillie::resolveMeshOccluders(LLCullResult& cull, std::vector<OccluderSolid>& out)
{
    static LLCachedControl<bool> mesh_setting(gSavedSettings, "SSGhillieMesh", false);
    static LLCachedControl<F32> min_area(gSavedSettings, "SSGhillieMeshMinArea", 4.f);
    static LLCachedControl<U32> max_boxes(gSavedSettings, "SSGhillieMeshMaxBoxes", 4);
    if (!mesh_setting) return;

    const F32 min_area_v = llmax((F32)min_area, 0.5f);
    const U32 max_boxes_v = llclamp((U32)max_boxes, 1u, 8u);
    if (!mMeshPool)
    {
        size_t width = 1;
        mMeshPool.reset(new LL::ThreadPool("SSCullMesh", width));
        mMeshPool->start();
    }

    LL::WorkQueue* queue = &mMeshPool->getQueue();

    S32 scanned = 0;
    for (LLCullResult::drawable_iterator iter = cull.beginVisibleList(); iter != cull.endVisibleList(); ++iter)
    {
        if (++scanned > 256) break;
        LLDrawable* drawable = *iter;
        if (!drawable || drawable->isDead()) continue;
        if (drawable->isState(LLDrawable::HAS_ALPHA)) continue;
        LLVOVolume* vov = drawable->getVOVolume();
        if (!vov || vov->isDead()) continue;
        if (vov->isRiggedMeshFast() || vov->isAnimatedObjectFast() || vov->isSculptedFast()) continue;
        if (!vov->isMeshFast()) continue;

        // Static gate: only objects that have stood still long enough (and
        // are not rigged/animated) may contribute mesh occluders. Fail open.
        const LLUUID& id = vov->getID();
        const std::unordered_map<LLUUID, ObjectRecord>::const_iterator rec_it = mObjectRecords.find(id);
        if (rec_it == mObjectRecords.end() || rec_it->second.mLastMoveFrame != 0)
        {
            continue;
        }

        const LLVector3 scale = drawable->getScale();
        const F32 area = 2.f * (scale.mV[0] * scale.mV[2] + scale.mV[1] * scale.mV[2] + scale.mV[0] * scale.mV[1]);
        if (area < min_area_v * 2.f) continue; // not significant (wall ~= 0.5*8*4*2 = 32 m²)

        const LLVector3 pos_agent = drawable->getPositionAgent();
        const LLQuaternion rot = drawable->getWorldRotation();
        const LLVector3 pos_class(llfloor(pos_agent.mV[0]), llfloor(pos_agent.mV[1]), llfloor(pos_agent.mV[2]));

        // Cache key: the mesh's own ID (stable across instances), plus scale.
        const LLUUID& mesh_id = vov->getMeshID();
        std::lock_guard<std::mutex> lock(mMeshCacheMutex);
        std::unordered_map<LLUUID, MeshCacheEntry>::iterator entry_it = mMeshCache.find(mesh_id);
        if (entry_it != mMeshCache.end()
            && entry_it->second.mScale == scale
            && entry_it->second.mReady)
        {
            entry_it->second.mPosClass = pos_class;
            const SS::GhillieMeshDecomp& decomp = entry_it->second.mDecomp;
            S32 n = 0;
            for (std::vector<SS::GhillieMeshBox>::const_iterator b = decomp.mBoxes.begin();
                 b != decomp.mBoxes.end() && n < (S32)max_boxes_v; ++b, ++n)
            {
                OccluderSolid solid;
                solid.mKind = OC_STATIC_SLAB;
                // Object-local box -> agent space via the live transform.
                // The mesh faces are in unit-object space; the drawable scale
                // maps them to meters, and the rotation/position place them.
                const LLVector3 box_pos = b->mPos;
                solid.mPos = pos_agent + (box_pos * rot).scaledVec(scale);
                solid.mRot = rot;
                solid.mHalf = LLVector3(fabsf(b->mHalf.mV[0] * scale.mV[0]),
                                        fabsf(b->mHalf.mV[1] * scale.mV[1]),
                                        fabsf(b->mHalf.mV[2] * scale.mV[2]));
                solid.mProfile = 0;
                out.push_back(solid);
            }
            continue;
        }

        if (entry_it == mMeshCache.end())
        {
            entry_it = mMeshCache.insert(std::make_pair(mesh_id, MeshCacheEntry())).first;
            entry_it->second.mMeshID = mesh_id;
            entry_it->second.mScale = scale;
            entry_it->second.mPosClass = pos_class;
        }
        if (entry_it->second.mReady) continue; // entry resolved while we were away

        // Snapshot the mesh geometry into worker-owned PODs (never touch the
        // live LLVolume from a worker thread).
        LLVolume* volume = vov->getVolume();
        if (!volume) continue;
        const S32 num_faces = volume->getNumVolumeFaces();
        std::vector<LLVector3> unit_verts;
        std::vector<U32> tris;
        unit_verts.reserve(num_faces * 4);
        tris.reserve(num_faces * 6);
        for (S32 f = 0; f < num_faces; ++f)
        {
            const LLVolumeFace& face = volume->getVolumeFace(f);
            if (!face.mPositions || face.mNumIndices <= 0) continue;
            const U32 base = (U32)unit_verts.size();
            for (S32 i = 0; i < face.mNumVertices; ++i)
            {
                unit_verts.push_back(LLVector3(face.mPositions[i].mV[0],
                                               face.mPositions[i].mV[1],
                                               face.mPositions[i].mV[2]));
            }
            for (U32 i = 0; i + 2 < (U32)face.mNumIndices; i += 3)
            {
                tris.push_back(base + (U32)face.mIndices[i]);
                tris.push_back(base + (U32)face.mIndices[i+1]);
                tris.push_back(base + (U32)face.mIndices[i+2]);
            }
        }
        if (unit_verts.empty() || tris.size() < 3) continue;

        const LLUUID snapshot_mesh = mesh_id;
        const LLVector3 snapshot_scale = scale;
        queue->post([this, snapshot_mesh, snapshot_scale, unit_verts, tris]()
            {
                const SS::GhillieMeshDecomp decomp = ssGhillieDecomposeMesh(unit_verts, tris);
                std::lock_guard<std::mutex> lock(mMeshCacheMutex);
                std::unordered_map<LLUUID, MeshCacheEntry>::iterator it = mMeshCache.find(snapshot_mesh);
                if (it != mMeshCache.end())
                {
                    it->second.mDecomp = decomp;
                    it->second.mReady = true;
                    it->second.mScale = snapshot_scale;
                }
            });
    }
}

void SSGhillie::gatherAndPost(const LLViewerCamera& camera, LLCullResult& cull)
{
    if (!sActive)
    {
        mSeenNodes.clear();
        return;
    }
    if (mJobInFlight.load(std::memory_order_acquire))
    {   // previous job still running: shed this frame, verdicts fail open
        ++mSkipFrames;
        mSeenNodes.clear();
        return;
    }

    static LLCachedControl<U32> max_occluders(gSavedSettings, "SSGhillieMaxOccluders", 256);
    static LLCachedControl<U32> hzb_width_setting(gSavedSettings, "SSGhillieHZBWidth", 640);
    static LLCachedControl<U32> fast_latency(gSavedSettings, "SSGhillieFastLatencyFrames", 4);

    ensurePool();
    updateStaticRecords(cull);

    Job& job = mJob;

    // Terrain/water base layer snapshot (main thread - safe to touch the
    // region heightfield here; workers only read the copy).
    static LLCachedControl<bool> terrain_setting(gSavedSettings, "SSGhillieTerrain", true);
    static LLCachedControl<bool> water_setting(gSavedSettings, "SSGhillieWater", true);
    LLViewerRegion* region = gAgent.getRegion();
    if (region && (terrain_setting || water_setting))
    {
        const LLSurface& land = region->getLand();
        job.mTerrainOrigin = region->getOriginAgent();
        job.mWaterEnabled = (bool)water_setting;
        job.mWaterZ = (water_setting) ? LLEnvironment::instance().getWaterHeight() : 0.f;
        if (terrain_setting)
        {
            const S32 grid = land.getGridsPerEdge();
            job.mTerrainGrid = grid;
            job.mTerrainMetersPerGrid = land.getMetersPerGrid();
            job.mTerrainHeights.resize((size_t)grid * (size_t)grid);
            for (S32 j = 0; j < grid; ++j)
            {
                for (S32 i = 0; i < grid; ++i)
                {
                    job.mTerrainHeights[(size_t)j * (size_t)grid + (size_t)i] = land.getZ(i, j);
                }
            }
        }
    }

    job.mGeneration = ++mNextGeneration;
    if (job.mGeneration == NO_GENERATION)
    {
        job.mGeneration = ++mNextGeneration; // skip the reserved value on wrap
    }

    // Build the view/projection pair from camera state only (row-vector
    // convention, LL's v * M). Deliberately independent of the current GL
    // matrices, which may already belong to a shadow pass at this point.
    job.mEye = camera.getOrigin();
    job.mAtAxis = camera.getAtAxis();

    const LLVector3 left = camera.getLeftAxis();
    const LLVector3 up = camera.getUpAxis();
    const LLVector3 back = -camera.getAtAxis();
    LLMatrix4& view = job.mModelview;
    view.initRows(LLVector4(left.mV[0], up.mV[0], back.mV[0], 0.f),
                  LLVector4(left.mV[1], up.mV[1], back.mV[1], 0.f),
                  LLVector4(left.mV[2], up.mV[2], back.mV[2], 0.f),
                  LLVector4(-dot(job.mEye, left), -dot(job.mEye, up), -dot(job.mEye, back), 1.f));

    const F32 fov = llclamp(camera.getView(), 0.01f, 3.f);
    F32 aspect = camera.getAspect();
    aspect = (aspect < 0.01f) ? (16.f / 9.f) : aspect;
    const F32 near_z = llmax(camera.getNear(), 0.01f);
    const F32 far_z = llmax(camera.getRenderFarPlane(), near_z * 2.f);
    const F32 proj_f = 1.f / tanf(fov * 0.5f);
    const F32 proj_a = (far_z + near_z) / (near_z - far_z);
    const F32 proj_b = 2.f * far_z * near_z / (near_z - far_z);
    LLMatrix4& projection = job.mProjection;
    // row-vector convention (v * M): clip.z = w*(a*z/w - 1) = a*z - w,
    // clip.w = b*z, so NDC z = (a*z - w)/(b*z) with a=(f+n)/(n-f), b=2fn/(n-f)
    projection.initRows(LLVector4(proj_f / aspect, 0.f, 0.f, 0.f),
                        LLVector4(0.f, proj_f, 0.f, 0.f),
                        LLVector4(0.f, 0.f, proj_a, -1.f),
                        LLVector4(0.f, 0.f, proj_b, 0.f));

    job.mHzbWidth = llclamp((U32)hzb_width_setting, 64u, 2048u);
    U32 hzb_height = (U32)llround((F32)job.mHzbWidth / aspect);
    job.mHzbHeight = llclamp(hzb_height, 64u, 2048u);
    S32 tile_count = mThreadPool ? (S32)mThreadPool->getWidth() : 1;
    job.mTileCount = llclamp(tile_count, 1, 16);
    job.mTileCount = llmin(job.mTileCount, (S32)job.mHzbHeight);

    // Occluder candidate assembly.
    job.mOccluders.clear();
    mCandScratch.clear();
    mCandDistScratch.clear();

    // Parallax rank: how quickly an occluder's screen projection changes per
    // frame. Close or fast-revolving objects rank hot (they occlude the most
    // during maneuvers); far or static objects rank cool.
    const F32 camera_speed = camera.getAverageSpeed();
    const F32 camera_ang = camera.getAverageAngularSpeed();
    const F32 ang_mass = llmax(camera_ang, 0.02f);

    struct RankedSolid
    {
        OccluderSolid solid;
        F32 dist_sq;
        F32 rank;
    };
    std::vector<RankedSolid> ranked;
    ranked.reserve(MAX_DRAWABLE_SCAN);

    const bool defer_near_peek = mCameraState.mDeferNearPeek;
    const bool fast_rebuild = (mCameraState.mActivity == CAMERA_FAST_REBUILD);
    U32 wall_count = 0;

    S32 scanned = 0;
    for (LLCullResult::drawable_iterator iter = cull.beginVisibleList(); iter != cull.endVisibleList(); ++iter)
    {
        if (++scanned > (S32)MAX_DRAWABLE_SCAN) break;
        LLDrawable* drawable = *iter;
        if (!drawable || drawable->isDead()) continue;
        if (drawable->isState(LLDrawable::HAS_ALPHA)) continue;
        LLVOVolume* vov = drawable->getVOVolume();
        if (!vov || vov->isDead()) continue;
        if (vov->isFlexibleFast() || vov->isSculptedFast() || vov->isAnimatedObjectFast()) continue;
        if (vov->isParticleSource()) continue;

        const LLVector3 pos = drawable->getPositionAgent();
        const F32 dist_sq = (pos - job.mEye).magVecSquared();
        if (dist_sq > MAX_OCCLUDER_RADIUS * MAX_OCCLUDER_RADIUS) continue;
        if (dist_sq > 0.01f)
        {
            const F32 fwd = (pos - job.mEye) * camera.getAtAxis();
            if (fwd <= 0.05f * sqrtf(dist_sq)) continue; // behind or at the camera plane
        }

        // Static-tier accent: static objects may use AABB / mesh occluders.
        const LLUUID& id = vov->getID();
        const std::unordered_map<LLUUID, ObjectRecord>::const_iterator rec_it = mObjectRecords.find(id);
        const bool static_obj = (rec_it != mObjectRecords.end()) && rec_it->second.mLastMoveFrame == 0;

        // Peek gate: while the camera is actively moving, close non-static
        // occluders are deferred - they are the ones that tank FPS when the
        // user turns a corner. Static objects are stable even then.
        if (defer_near_peek && !static_obj && dist_sq < PEEK_DEFER_RADIUS * PEEK_DEFER_RADIUS)
        {
            continue;
        }

        LLVolume* volume = vov->getVolume();
        if (!volume) continue;
        const LLVolumeParams& params = volume->getParams();
        if (params.isSculpt()) continue;

        const U8 profile = params.getProfileParams().getCurveType() & LL_PCODE_PROFILE_MASK;
        const U8 path = params.getPathParams().getCurveType();
        const F32 begin_s = params.getProfileParams().getBegin();
        const F32 end_s = params.getProfileParams().getEnd();
        const F32 hollow = params.getProfileParams().getHollow();
        const F32 begin_t = params.getPathParams().getBegin();
        const F32 end_t = params.getPathParams().getEnd();
        const F32 scale_x = params.getPathParams().getScaleX();
        const F32 scale_y = params.getPathParams().getScaleY();
        const F32 shear_x = params.getPathParams().getShearX();
        const F32 shear_y = params.getPathParams().getShearY();
        const F32 taper_x = params.getPathParams().getTaperX();
        const F32 taper_y = params.getPathParams().getTaperY();
        const F32 twist_begin = params.getPathParams().getTwistBegin();
        const F32 twist_end = params.getPathParams().getTwistEnd();
        const F32 radius_offset = params.getPathParams().getRadiusOffset();
        const F32 revolutions = params.getPathParams().getRevolutions();
        const F32 skew = params.getPathParams().getSkew();

        const bool exact_profile = (begin_s == 0.f && end_s == 1.f && hollow == 0.f);
        const bool exact_path = (begin_t == 0.f && end_t == 1.f
                                 && scale_x == 1.f && scale_y == 1.f
                                 && shear_x == 0.f && shear_y == 0.f
                                 && taper_x == 0.f && taper_y == 0.f
                                 && twist_begin == 0.f && twist_end == 0.f
                                 && radius_offset == 0.f && revolutions == 1.f && skew == 0.f);

        const LLVector3 scale = drawable->getScale();
        const LLVector3 half(scale.mV[0] * 0.5f, scale.mV[1] * 0.5f, scale.mV[2] * 0.5f);
        const F32 half_radius_sq = half.magVecSquared();
        if (half_radius_sq <= 0.f) continue;

        const LLQuaternion rot = drawable->getWorldRotation();

        // Box profile + line path is exact for the occluder. Everything else
        // becomes a conservative AABB fallback when the object is static.
        const bool is_box = (profile == LL_PCODE_PROFILE_SQUARE);
        const bool shape_exact = (path == LL_PCODE_PATH_LINE) && is_box;

        if (shape_exact && exact_profile && exact_path)
        {
            OccluderSolid solid;
            solid.mKind = OC_SEG_BOX;
            solid.mPos = pos;
            solid.mRot = rot;
            solid.mHalf = half;
            solid.mProfile = profile;
            RankedSolid rs;
            rs.solid = solid;
            rs.dist_sq = dist_sq;
            rs.rank = (1.f + camera_speed) * (1.f + 3.f * ang_mass) / (1.f + dist_sq);
            ranked.push_back(rs);
        }
        else if (shape_exact && (hollow > 0.f || begin_s != 0.f || end_s != 1.f))
        {
            // Hollow / cut box: decompose exactly into thin wall boxes
            // aligned with the cut edges (cheap, analytic; works for static
            // AND moving boxes). The decompose works in unit profile space
            // (half extents 0.5 for a default box); the live scale/rotation/
            // position map that to agent space here.
            std::vector<SS::GhillieMeshBox> parts;
            U32 parts_count = 0;
            SS::ssGhillieDecomposeSquareBox(begin_s, end_s, hollow, LLVector3(0.5f, 0.5f, 0.5f),
                                            parts, parts_count);
            for (U32 p = 0; p < parts.size() && p < 8; ++p)
            {
                // Profile-space box (unit half extents) -> agent space via the
                // live scale + rotation + position.
                const LLVector3 pos_full = pos + (parts[p].mPos * rot).scaledVec(scale);
                const LLVector3 half_full(fabsf(parts[p].mHalf.mV[0] * scale.mV[0]),
                                          fabsf(parts[p].mHalf.mV[1] * scale.mV[1]),
                                          fabsf(parts[p].mHalf.mV[2] * scale.mV[2]));
                OccluderSolid solid;
                solid.mKind = OC_SEG_BOX;
                solid.mPos = pos_full;
                solid.mRot = rot;
                solid.mHalf = half_full;
                solid.mProfile = profile;
                RankedSolid rs;
                rs.solid = solid;
                rs.dist_sq = dist_sq;
                rs.rank = (1.f + camera_speed) * (1.f + 3.f * ang_mass) / (1.f + dist_sq);
                ranked.push_back(rs);
            }
        }
        else if (static_obj)
        {
            // Static objects with any params get the conservative AABB.
            OccluderSolid solid;
            solid.mKind = OC_STATIC_BOX;
            solid.mPos = pos;
            solid.mRot = rot;
            solid.mHalf = half;
            solid.mProfile = profile;
            RankedSolid rs;
            rs.solid = solid;
            rs.dist_sq = dist_sq;
            rs.rank = (1.f + camera_speed) * (1.f + 3.f * ang_mass) / (1.f + dist_sq);
            rs.rank *= 0.5f; // static occluders are cheaper to keep
            ranked.push_back(rs);
        }
        // else: not exact, not static: not a sound occluder - skip.
    }

    // Mesh occluders (static gate applied inside).
    resolveMeshOccluders(cull, job.mOccluders);

    // Parallax budget: cap the number of occluders re-rastered this frame,
    // keeping the hottest. Fast rebuilds (teleport/death) pull the whole set
    // so the re-earned cull lands as early as possible after respawn.
    U32 occluder_cap = llclamp((U32)max_occluders, 16u, 4096u);
    if (fast_rebuild)
    {
        occluder_cap = llmin((U32)ranked.size(), occluder_cap * (U32)fast_latency);
    }
    if (ranked.size() > occluder_cap)
    {
        std::vector<S32> order(ranked.size());
        std::iota(order.begin(), order.end(), 0);
        std::nth_element(order.begin(), order.begin() + (S32)occluder_cap, order.end(),
                         [&ranked](S32 a, S32 b) { return ranked[a].rank > ranked[b].rank; });
        std::vector<RankedSolid> picked;
        picked.reserve(occluder_cap);
        for (S32 i = 0; i < (S32)occluder_cap; ++i)
        {
            picked.push_back(ranked[order[i]]);
        }
        ranked.swap(picked);
    }

    for (std::vector<RankedSolid>::const_iterator it = ranked.begin(); it != ranked.end(); ++it)
    {
        job.mOccluders.push_back(it->solid);
    }

    // Occludees: octree nodes visited by this frame's traversal
    job.mOccludees.clear();
    job.mOccludees.reserve(mSeenNodes.size());
    for (std::vector<LLViewerOctreeGroup*>::const_iterator it = mSeenNodes.begin(); it != mSeenNodes.end(); ++it)
    {
        LLViewerOctreeGroup* group = *it;
        if (!group) continue;
        const LLVector4a* bounds = group->getBounds();
        if (!bounds) continue;
        Occludee occludee;
        occludee.mGroup = group;
        occludee.mCenter.setVec(bounds[0][0], bounds[0][1], bounds[0][2]);
        occludee.mHalfSize.setVec(bounds[1][0], bounds[1][1], bounds[1][2]);
        job.mOccludees.push_back(occludee);
    }
    mSeenNodes.clear();

    if (job.mOccluders.empty() || job.mOccludees.empty())
    {
        return; // nothing to rank against: frame fails open
    }

    postJob();
}

void SSGhillie::ensurePool()
{
    if (mThreadPool) return;

    size_t cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    // leave a core for the render thread and one for system housekeeping;
    // the ThreadPoolSizes["SSCull"] map overrides this default
    const size_t width = (size_t)llclamp<S32>((S32)cores - 2, 1, 32);
    mThreadPool.reset(new LL::ThreadPool("SSCull", width));
    mThreadPool->start();
    LL_INFOS("Ghillie") << "worker pool started, " << mThreadPool->getWidth() << " threads" << LL_ENDL;
}

void SSGhillie::postJob()
{
    Job& job = mJob;

    mBackBufferIdx.store(mBackBufferIdx.load(std::memory_order_relaxed) ^ 1, std::memory_order_relaxed);
    Result& out = mResults[mBackBufferIdx.load(std::memory_order_relaxed)];
    out.mGeneration = NO_GENERATION;
    out.mEye = job.mEye;
    out.mAtAxis = job.mAtAxis;
    out.mVerdicts.assign(job.mOccludees.size(), Verdict());
    out.mOccluderCount = 0;
    out.mOccludedCount = 0;
    out.mHiZMS = 0.f;
    out.mTotalMS = 0.f;
    out.mGhillieBoxes = (U32)job.mOccluders.size();

    job.mHzb.assign((size_t)job.mHzbWidth * (size_t)job.mHzbHeight, DEPTH_INIT);
    job.mMips.clear();

    job.mTileCountdown.store(job.mTileCount);
    job.mTestCountdown.store(0);
    job.mAbandon.store(false, std::memory_order_release);
    job.mTimer.reset();
    mLastJobFrame = gFrameCount;
    mJobInFlight.store(true, std::memory_order_release);

    LL::WorkQueue* queue = mThreadPool ? &mThreadPool->getQueue() : nullptr;
    if (!queue)
    {   // no worker pool: run the whole DAG inline (rare, fail safe)
        for (S32 tile = 0; tile < job.mTileCount; ++tile)
        {
            runTileJob(tile);
        }
        return;
    }

    const U32 generation = job.mGeneration;
    S32 posted = 0;
    for (S32 tile = 0; tile < job.mTileCount; ++tile)
    {
        if (queue->post([this, generation, tile]()
            {
                if (mJob.mGeneration != generation || !mJobInFlight.load(std::memory_order_acquire)) return;
                runTileJob(tile);
            }))
        {
            ++posted;
        }
        else
        {
            break; // queue closed (shutdown race): finish inline below
        }
    }
    for (S32 tile = posted; tile < job.mTileCount; ++tile)
    {
        runTileJob(tile);
    }
}

void SSGhillie::runTileJob(S32 tile)
{
    Job& job = mJob;
    U32* hzb = job.mHzb.data();
    const U32 width = job.mHzbWidth;
    const U32 height = job.mHzbHeight;
    const U32 strip_y0 = (height * (U32)tile) / (U32)job.mTileCount;
    const U32 strip_y1 = (height * (U32)(tile + 1)) / (U32)job.mTileCount;

    if (!job.mAbandon.load(std::memory_order_acquire))
    {
        // Base layers first: terrain and water (farther-wins, the raster
        // below is the conservative surface-occluder path).
        if (job.mTerrainGrid > 0 || job.mWaterEnabled)
        {
            if (job.mWaterEnabled)
            {
                rasterCullPlaneZ(job.mWaterZ, job.mEye, job.mTerrainOrigin,
                                 job.mModelview, job.mProjection,
                                 hzb, width, height, strip_y0, strip_y1);
            }
            if (job.mTerrainGrid > 0)
            {
                rasterCullTerrain(job.mTerrainHeights, job.mTerrainGrid,
                                  job.mTerrainMetersPerGrid, job.mTerrainOrigin,
                                  job.mModelview, job.mProjection, job.mEye,
                                  hzb, width, height, strip_y0, strip_y1);
            }
        }

        for (std::vector<OccluderSolid>::const_iterator it = job.mOccluders.begin(); it != job.mOccluders.end(); ++it)
        {
            rasterCullBox(it->mPos, it->mRot, it->mHalf, job.mModelview, job.mProjection,
                          job.mEye, hzb, width, height, strip_y0, strip_y1, true);
        }
    }

    if (job.mTileCountdown.fetch_sub(1) == 1)
    {   // last tile done: reduce the mip chain, then dispatch the tests
        LLTimer mips_timer;
        buildMips();
        job.mHiZMS.store(mips_timer.getElapsedTimeF32() * 1000.f, std::memory_order_relaxed);

        const S32 chunks = job.mTileCount;
        job.mTestCountdown.store(chunks);
        LL::WorkQueue* queue = mThreadPool ? &mThreadPool->getQueue() : nullptr;
        if (!queue)
        {
            for (S32 chunk = 0; chunk < chunks; ++chunk)
            {
                runTestChunk(chunk);
            }
            return;
        }
        const U32 generation = job.mGeneration;
        S32 posted = 0;
        for (S32 chunk = 0; chunk < chunks; ++chunk)
        {
            if (queue->post([this, generation, chunk]()
                {
                    if (mJob.mGeneration != generation || !mJobInFlight.load(std::memory_order_acquire)) return;
                    runTestChunk(chunk);
                }))
            {
                ++posted;
            }
            else
            {
                break; // queue closed: finish the remaining chunks inline
            }
        }
        for (S32 chunk = posted; chunk < chunks; ++chunk)
        {
            runTestChunk(chunk);
        }
    }
}

void SSGhillie::buildMips()
{
    Job& job = mJob;
    job.mMips.clear();
    U32 width = job.mHzbWidth;
    U32 height = job.mHzbHeight;
    const std::vector<U32>* src = &job.mHzb;
    while (width > 1 && height > 1)
    {
        const U32 nw = (width + 1) >> 1;
        const U32 nh = (height + 1) >> 1;
        std::vector<U32> dst((size_t)nw * nh);
        for (U32 y = 0; y < nh; ++y)
        {
            const U32 sy0 = llmin(y * 2, height - 1);
            const U32 sy1 = llmin(y * 2 + 1, height - 1);
            for (U32 x = 0; x < nw; ++x)
            {
                const U32 sx0 = llmin(x * 2, width - 1);
                const U32 sx1 = llmin(x * 2 + 1, width - 1);
                U32 m = (*src)[(size_t)sy0 * width + sx0];
                m = llmax(m, (*src)[(size_t)sy0 * width + sx1]);
                m = llmax(m, (*src)[(size_t)sy1 * width + sx0]);
                m = llmax(m, (*src)[(size_t)sy1 * width + sx1]);
                dst[(size_t)y * nw + x] = m;
            }
        }
        job.mMips.push_back(std::move(dst));
        src = &job.mMips.back();
        width = nw;
        height = nh;
    }
}

void SSGhillie::runTestChunk(S32 chunk)
{
    Job& job = mJob;
    Result& out = mResults[mBackBufferIdx.load(std::memory_order_relaxed)];
    const S32 total = (S32)job.mOccludees.size();
    const S32 per_chunk = (total + job.mTileCount - 1) / job.mTileCount;
    const S32 first = chunk * per_chunk;
    const S32 last = llmin(total, first + per_chunk);

    if (!job.mAbandon.load(std::memory_order_acquire))
    {
        for (S32 i = first; i < last; ++i)
        {
            out.mVerdicts[i].mGroup = job.mOccludees[i].mGroup;
            out.mVerdicts[i].mOccluded = testOccludee(job.mOccludees[i]);
        }
    }

    if (job.mTestCountdown.fetch_sub(1) == 1)
    {
        publish();
    }
}

bool SSGhillie::testOccludee(const Occludee& occludee)
{
    const Job& job = mJob;

    // stock eye-in-node early fail (fattened), parity with the query path
    const LLVector3 to_center = job.mEye - occludee.mCenter;
    if (fabsf(to_center.mV[0]) <= occludee.mHalfSize.mV[0] + EYE_FUDGE
        && fabsf(to_center.mV[1]) <= occludee.mHalfSize.mV[1] + EYE_FUDGE
        && fabsf(to_center.mV[2]) <= occludee.mHalfSize.mV[2] + EYE_FUDGE)
    {
        return false;
    }
    if (occludee.mHalfSize.magVecSquared() > MAX_OCCLUDEE_RADIUS * MAX_OCCLUDEE_RADIUS)
    {
        return false;
    }

    F32 minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    U32 nearest = DEPTH_INIT;
    for (S32 i = 0; i < 8; ++i)
    {
        const LLVector3 corner(occludee.mCenter.mV[0] + occludee.mHalfSize.mV[0] * (F32)CUBE_CORNERS[i][0],
                               occludee.mCenter.mV[1] + occludee.mHalfSize.mV[1] * (F32)CUBE_CORNERS[i][1],
                               occludee.mCenter.mV[2] + occludee.mHalfSize.mV[2] * (F32)CUBE_CORNERS[i][2]);
        const LLVector4 world(corner.mV[0], corner.mV[1], corner.mV[2], 1.f);
        const LLVector4 clip = (world * job.mModelview) * job.mProjection;
        if (clip.mV[3] < CLIP_W_EPSILON)
        {
            return false; // straddles or behind the near plane: treat as visible
        }
        const F32 iw = 1.f / clip.mV[3];
        const F32 sx = (clip.mV[0] * iw * 0.5f + 0.5f) * (F32)job.mHzbWidth;
        const F32 sy = (clip.mV[1] * iw * 0.5f + 0.5f) * (F32)job.mHzbHeight;
        const U32 u = depthFromNdc(clip.mV[2] * iw);
        nearest = llmin(nearest, u);
        minx = llmin(minx, sx);
        maxx = llmax(maxx, sx);
        miny = llmin(miny, sy);
        maxy = llmax(maxy, sy);
    }

    // pick the mip level where the rect spans about two texels
    const F32 span = llmax(maxx - minx, maxy - miny);
    S32 level = 0;
    const S32 top_level = (S32)job.mMips.size();
    while (level < top_level && span / (F32)(1 << (level + 1)) > 2.f) ++level;

    const U32* level_buffer = (level == 0) ? job.mHzb.data() : job.mMips[level - 1].data();
    const U32 level_width = (job.mHzbWidth + (1 << level) - 1) >> level;
    const U32 level_height = (job.mHzbHeight + (1 << level) - 1) >> level;
    S32 x0 = llclamp((S32)floorf(minx / (F32)(1 << level)) - 1, 0, (S32)level_width - 1);
    S32 x1 = llclamp((S32)floorf(maxx / (F32)(1 << level)) + 1, 0, (S32)level_width - 1);
    S32 y0 = llclamp((S32)floorf(miny / (F32)(1 << level)) - 1, 0, (S32)level_height - 1);
    S32 y1 = llclamp((S32)floorf(maxy / (F32)(1 << level)) + 1, 0, (S32)level_height - 1);

    U32 hzb_depth = 0;
    bool node_above_water = false;
    if (job.mWaterEnabled)
    {
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3 corner(occludee.mCenter.mV[0] + occludee.mHalfSize.mV[0] * (F32)CUBE_CORNERS[i][0],
                                   occludee.mCenter.mV[1] + occludee.mHalfSize.mV[1] * (F32)CUBE_CORNERS[i][1],
                                   occludee.mCenter.mV[2] + occludee.mHalfSize.mV[2] * (F32)CUBE_CORNERS[i][2]);
            if (corner.mV[2] > job.mWaterZ + EYE_FUDGE)
            {
                node_above_water = true;
                break;
            }
        }
    }
    U32 solid_depth = 0;
    U32 water_depth = 0;
    for (S32 y = y0; y <= y1; ++y)
    {
        for (S32 x = x0; x <= x1; ++x)
        {
            const U32 v = level_buffer[(U32)y * level_width + (U32)x];
            const U32 d = v & DEPTH_MASK;
            if (v & WATER_MARK)
            {
                water_depth = llmax(water_depth, d);
            }
            else
            {
                solid_depth = llmax(solid_depth, d);
            }
        }
    }
    hzb_depth = solid_depth;
    if (!node_above_water)
    {
        // The water plane may occlude this node (it stays below the line).
        hzb_depth = llmax(hzb_depth, water_depth);
    }

    return nearest > hzb_depth + DEPTH_BIAS;
}

void SSGhillie::publish()
{
    Job& job = mJob;
    Result& out = mResults[mBackBufferIdx.load(std::memory_order_relaxed)];

    if (job.mAbandon.load(std::memory_order_acquire))
    {   // drained a watchdog-abandoned job: null result, keep the old
        // published generation so the main thread never consumes it
        out.mVerdicts.clear();
        out.mGeneration = NO_GENERATION;
        out.mOccluderCount = 0;
        out.mOccludedCount = 0;
        out.mHiZMS = 0.f;
        out.mTotalMS = 0.f;
        out.mGhillieBoxes = 0;
        mJobInFlight.store(false, std::memory_order_release);
        return;
    }

    U64 occluded = 0;
    for (std::vector<Verdict>::const_iterator it = out.mVerdicts.begin(); it != out.mVerdicts.end(); ++it)
    {
        if (it->mOccluded) ++occluded;
    }

    out.mOccluderCount = job.mOccluders.size();
    out.mOccludedCount = occluded;
    out.mHiZMS = job.mHiZMS.load(std::memory_order_relaxed);
    out.mTotalMS = job.mTimer.getElapsedTimeF32() * 1000.f;
    out.mGeneration = job.mGeneration;

    mPublishedIdx.store(mBackBufferIdx.load(std::memory_order_relaxed), std::memory_order_release);
    mPublishedGen.store(job.mGeneration, std::memory_order_release);
    mJobInFlight.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Debug overlay + on-world occluder/occludee render
// ---------------------------------------------------------------------------

void SSGhillie::drawDebug()
{
    static LLCachedControl<bool> debug_setting(gSavedSettings, "SSGhillieDebug", false);
    if (!debug_setting || !sActive) return;

    const Result& result = mResults[mPublishedIdx.load(std::memory_order_relaxed)];
    const std::string text = llformat(
        "GHILLIE %s | threads %d | occluders %d | occluded %d | tracked nodes %d | HZB %.2fms total %.2fms | shed %d | mode %d",
        mJobInFlight.load() ? "busy" : "idle",
        mThreadPool ? (S32)mThreadPool->getWidth() : 0,
        (S32)result.mOccluderCount,
        (S32)result.mOccludedCount,
        (S32)mNodeStates.size(),
        result.mHiZMS,
        result.mTotalMS,
        (S32)mSkipFrames,
        (S32)mCameraState.mActivity);

    const LLFontGL* font = LLFontGL::getFontMonospace();
    const S32 x = 16;
    const S32 y = gViewerWindow->getWorldViewRectScaled().getHeight() - 32;
    gGL.pushMatrix();
    font->renderUTF8(text, 0, (F32)x, (F32)y, LLColor4(0.6f, 1.f, 0.6f, 0.9f),
                     LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
    gGL.popMatrix();
}

void SSGhillie::drawDebugWorld()
{
    static LLCachedControl<bool> world_setting(gSavedSettings, "SSGhillieDebugWorld", false);
    if (!world_setting || !sActive || !activeForCurrentPass()) return;

    // On-world debug: wireframe boxes around the occluders Ghillie is
    // actually using (red, mesh slabs thick) and around every node it
    // currently has occluded (green), so you can see what it thinks is
    // blocking what and whether the peek/static gates behave.
    LLGLDisable depth(GL_DEPTH_TEST);
    LLGLEnable blend(GL_BLEND);

    gGL.setLineWidth(1.5f);

    // Occluders from the current job (workers only read: safe on main).
    gGL.color4f(1.f, 0.2f, 0.2f, 0.55f);
    for (std::vector<OccluderSolid>::const_iterator it = mJob.mOccluders.begin(); it != mJob.mOccluders.end(); ++it)
    {
        drawBoxLines(it->mPos, it->mRot, it->mHalf, it->mKind == OC_STATIC_SLAB);
    }

    // Occluded nodes from the last published verdicts.
    const Result& result = mResults[mPublishedIdx.load(std::memory_order_relaxed)];
    gGL.color4f(0.f, 1.f, 0.f, 0.5f);
    for (std::vector<Verdict>::const_iterator it = result.mVerdicts.begin(); it != result.mVerdicts.end(); ++it)
    {
        if (it->mGroup && it->mOccluded)
        {
            const LLVector4a* bounds = it->mGroup->getBounds();
            if (bounds)
            {
                const LLVector3 c(bounds[0][0], bounds[0][1], bounds[0][2]);
                const LLVector3 h(bounds[1][0], bounds[1][1], bounds[1][2]);
                drawBoxLines(c, LLQuaternion::identity, h, false);
            }
        }
    }

    gGL.setLineWidth(1.f);
    gGL.flush();
}

void SSGhillie::drawBoxLines(const LLVector3& pos, const LLQuaternion& rot,
                             const LLVector3& half, bool thick)
{
    const LLMatrix3 rot3(rot);
    LLVector3 corners[8];
    for (S32 i = 0; i < 8; ++i)
    {
        const LLVector3 local(half.mV[0] * (F32)CUBE_CORNERS[i][0],
                              half.mV[1] * (F32)CUBE_CORNERS[i][1],
                              half.mV[2] * (F32)CUBE_CORNERS[i][2]);
        corners[i] = pos + local * rot3;
    }
    static const S32 EDGES[12][2] =
    {
        { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
        { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    if (thick) gGL.setLineWidth(2.5f);
    gGL.begin(LLRender::LINES);
    for (S32 e = 0; e < 12; ++e)
    {
        gGL.vertex3fv(corners[EDGES[e][0]].mV);
        gGL.vertex3fv(corners[EDGES[e][1]].mV);
    }
    gGL.end();
    if (thick) gGL.setLineWidth(1.5f);
}
// </SS:Nexii>