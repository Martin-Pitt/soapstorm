/**
 * @file ssworldfieldtiles.cpp
 * @brief See ssworldfieldtiles.h.
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

#include "ssworldfieldtiles.h"

#include "ssworldfieldshapes.h"

#include "llfasttimer.h"
#include "llframetimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

// Raster budget and span policy: a couple of tiles per frame keeps a rebuild
// under a second of worker-free main-thread time; the slab merge matches the
// legacy store's gap rule (0.5 m) so spans read the same to consumers.
static constexpr S32 SS_TILES_PER_FRAME = 2;
static constexpr S32 SS_TILES_SLAB_CELLS = 2;
static constexpr F32 SS_TILES_CELL = 0.25f;

static LLTrace::BlockTimerStatHandle FTM_SS_TILES_RASTER("SS Tiles Raster");

static constexpr S32 SS_TILES_N = SSWorldFieldTiles::TILE_CELLS;
static constexpr S32 SS_TILES_VOXELS = SS_TILES_N * SS_TILES_N * SS_TILES_N;

static U64 ss_tile_key(S32 tx, S32 ty, S32 tz)
{
    return ((U64)(U32)(tx + (1 << 20)) << 42)
         | ((U64)(U32)(ty + (1 << 20)) << 21)
         | (U64)(U32)(tz + (1 << 20));
}

static void ss_tile_unpack(U64 key, S32& tx, S32& ty, S32& tz)
{
    tx = (S32)((key >> 42) & 0x1FFFFF) - (1 << 20);
    ty = (S32)((key >> 21) & 0x1FFFFF) - (1 << 20);
    tz = (S32)(key & 0x1FFFFF) - (1 << 20);
}

// Per-frame maintenance: follow the census rebuilds, rasterize a budget.
void SSWorldFieldTiles::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldFieldTiles", false);
    if (!enabled)
    {
        if (!mTiles.empty() || !mWorklist.empty())
        {
            mTiles.clear();
            mWorklist.clear();
        }
        return;
    }

    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    const U64 stamp = shapes->censusStamp();
    if (stamp != mCensusStamp)
    {
        mCensusStamp = stamp;
        scheduleTiles();
    }

    S32 budget = SS_TILES_PER_FRAME;
    while (budget > 0 && !mWorklist.empty())
    {
        const U64 key = mWorklist.back();
        mWorklist.pop_back();
        S32 tx, ty, tz;
        ss_tile_unpack(key, tx, ty, tz);
        if (rasterTile(tx, ty, tz))
        {
            --budget;
        }
    }
}

// Geometry-bearing tile keys off the census: a record's AABB nominates its
// tiles, so empty space - including all the air between structures - costs
// nothing. DYNAMIC records are skipped here: movers are query-time only.
void SSWorldFieldTiles::scheduleTiles()
{
    mWorklist.clear();
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    if (!shapes->censusCurrent()) return;

    static LLCachedControl<F32> range(gSavedSettings, "SSWorldFieldShapesRange", 192.f);
    const F32 env = llclamp((F32)range, 32.f, 1024.f);
    const LLVector3 anchor = shapes->censusAnchor();
    const LLVector3 bmin = anchor - LLVector3(env, env, env);
    const LLVector3 bmax = anchor + LLVector3(env, env, env);

    std::set<U64> keys;
    const F32 inv = 1.f / TILE_M;
    shapes->forEachRecord(bmin, bmax, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (rec.mDynamic) return;
        S32 x0 = (S32)floorf(rec.mBMin.mV[VX] * inv);
        S32 y0 = (S32)floorf(rec.mBMin.mV[VY] * inv);
        S32 z0 = (S32)floorf(rec.mBMin.mV[VZ] * inv);
        S32 x1 = (S32)floorf(rec.mBMax.mV[VX] * inv);
        S32 y1 = (S32)floorf(rec.mBMax.mV[VY] * inv);
        S32 z1 = (S32)floorf(rec.mBMax.mV[VZ] * inv);
        if (x1 - x0 > 512) x1 = x0 + 512;
        if (y1 - y0 > 512) y1 = y0 + 512;
        if (z1 - z0 > 512) z1 = z0 + 512;
        for (S32 z = z0; z <= z1; ++z)
        {
            for (S32 y = y0; y <= y1; ++y)
            {
                for (S32 x = x0; x <= x1; ++x)
                {
                    keys.insert(ss_tile_key(x, y, z));
                }
            }
        }
    });

    mWorklist.assign(keys.begin(), keys.end());
    // Ground-up raster order: the first spans published are the ones the
    // surface field and flood consume first.
    std::sort(mWorklist.begin(), mWorklist.end(), [](U64 a, U64 b)
    {
        S32 ax, ay, az, bx, by, bz;
        ss_tile_unpack(a, ax, ay, az);
        ss_tile_unpack(b, bx, by, bz);
        if (az != bz) return az < bz;
        if (ay != by) return ay < by;
        return ax < bx;
    });
}

// One tile in: clear the scratch, rasterize every at-rest record that meets
// it, run-length the columns into the published CSR, free nothing but time.
bool SSWorldFieldTiles::rasterTile(S32 tx, S32 ty, S32 tz)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_TILES_RASTER);
    LLTimer raster_timer;

    const U64 key = ss_tile_key(tx, ty, tz);
    auto it = mTiles.find(key);
    if (it != mTiles.end() && it->second.mStamp == mCensusStamp) return false;

    const LLVector3 tmin((F32)tx * TILE_M, (F32)ty * TILE_M, (F32)tz * TILE_M);
    const LLVector3 tmax = tmin + LLVector3(TILE_M, TILE_M, TILE_M);

    if ((S32)mVoxels.size() != SS_TILES_VOXELS)
    {
        mVoxels.assign(SS_TILES_VOXELS, 0);
    }
    else
    {
        memset(mVoxels.data(), 0, mVoxels.size());
    }

    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    shapes->forEachRecord(tmin, tmax, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (rec.mDynamic) return;

        // Column range this record's AABB covers inside the tile.
        S32 x0 = (S32)floorf((llmax(rec.mBMin.mV[VX], tmin.mV[VX]) - tmin.mV[VX]) / SS_TILES_CELL);
        S32 y0 = (S32)floorf((llmax(rec.mBMin.mV[VY], tmin.mV[VY]) - tmin.mV[VY]) / SS_TILES_CELL);
        S32 x1 = (S32)floorf((llmin(rec.mBMax.mV[VX], tmax.mV[VX]) - tmin.mV[VX]) / SS_TILES_CELL);
        S32 y1 = (S32)floorf((llmin(rec.mBMax.mV[VY], tmax.mV[VY]) - tmin.mV[VY]) / SS_TILES_CELL);
        x0 = llclamp(x0, 0, SS_TILES_N - 1);
        y0 = llclamp(y0, 0, SS_TILES_N - 1);
        x1 = llclamp(x1, 0, SS_TILES_N - 1);
        y1 = llclamp(y1, 0, SS_TILES_N - 1);

        auto markColumn = [&](S32 cx, S32 cy, F32 wz0, F32 wz1)
        {
            S32 cz0 = (S32)floorf((wz0 - tmin.mV[VZ]) / SS_TILES_CELL);
            S32 cz1 = (S32)ceilf((wz1 - tmin.mV[VZ]) / SS_TILES_CELL);
            cz0 = llclamp(cz0, 0, SS_TILES_N);
            cz1 = llclamp(cz1, 0, SS_TILES_N);
            if (cz1 <= cz0) return;
            const S32 base = (cy * SS_TILES_N + cx) * SS_TILES_N;
            memset(mVoxels.data() + base + cz0, 1, cz1 - cz0);
        };

        switch (rec.mClass)
        {
            case SSWorldFieldShapes::Record::CLASS_BOX:
            {
                const LLVector3& c = rec.mCenter;
                for (S32 y = y0; y <= y1; ++y)
                {
                    const F32 py = tmin.mV[VY] + ((F32)y + 0.5f) * SS_TILES_CELL;
                    for (S32 x = x0; x <= x1; ++x)
                    {
                        const F32 px = tmin.mV[VX] + ((F32)x + 0.5f) * SS_TILES_CELL;
                        // Vertical slab interval through the oriented box.
                        F32 t0 = 0.f, t1 = 1.f;
                        bool valid = true;
                        S32 hits = 0;
                        for (U32 i = 0; i < 3; ++i)
                        {
                            const F32 o = (px - c.mV[VX]) * rec.mAxes[i].mV[VX]
                                        + (py - c.mV[VY]) * rec.mAxes[i].mV[VY]
                                        + (tmin.mV[VZ] - c.mV[VZ]) * rec.mAxes[i].mV[VZ];
                            const F32 dz = rec.mAxes[i].mV[VZ];
                            const F32 h = rec.mHalf.mV[i];
                            if (fabsf(dz) < 1e-9f)
                            {
                                if (fabsf(o) > h) { valid = false; break; }
                                continue;
                            }
                            F32 tn = (-h - o) / dz;
                            F32 tf = (h - o) / dz;
                            if (tn > tf) { F32 tmp = tn; tn = tf; tf = tmp; }
                            if (tn > t0) { t0 = tn; ++hits; }
                            if (tf < t1) t1 = tf;
                            if (t0 > t1) { valid = false; break; }
                        }
                        if (valid && hits >= 1)
                        {
                            markColumn(x, y, tmin.mV[VZ] + t0 * TILE_M, tmin.mV[VZ] + t1 * TILE_M);
                        }
                    }
                }
                break;
            }
            case SSWorldFieldShapes::Record::CLASS_SPHERE:
            {
                const LLVector3& c = rec.mCenter;
                for (S32 y = y0; y <= y1; ++y)
                {
                    const F32 py = tmin.mV[VY] + ((F32)y + 0.5f) * SS_TILES_CELL;
                    for (S32 x = x0; x <= x1; ++x)
                    {
                        const F32 px = tmin.mV[VX] + ((F32)x + 0.5f) * SS_TILES_CELL;
                        F32 o[3], d[2];
                        for (U32 i = 0; i < 3; ++i)
                        {
                            o[i] = ((px - c.mV[VX]) * rec.mAxes[i].mV[VX]
                                  + (py - c.mV[VY]) * rec.mAxes[i].mV[VY]
                                  + (tmin.mV[VZ] - c.mV[VZ]) * rec.mAxes[i].mV[VZ]) / rec.mRadii.mV[i];
                        }
                        d[0] = rec.mAxes[0].mV[VZ] / rec.mRadii.mV[0];
                        d[1] = rec.mAxes[1].mV[VZ] / rec.mRadii.mV[1];
                        const F32 dz = rec.mAxes[2].mV[VZ] / rec.mRadii.mV[2];
                        const F32 A = d[0] * d[0] + d[1] * d[1] + dz * dz;
                        if (A < 1e-12f) continue;
                        const F32 B = 2.f * (o[0] * d[0] + o[1] * d[1] + o[2] * dz);
                        const F32 C = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - 1.f;
                        const F32 disc = B * B - 4.f * A * C;
                        if (disc < 0.f) continue;
                        const F32 sq = sqrtf(disc);
                        const F32 t0 = (-B - sq) / (2.f * A);
                        const F32 t1 = (-B + sq) / (2.f * A);
                        markColumn(x, y, tmin.mV[VZ] + t0 * TILE_M, tmin.mV[VZ] + t1 * TILE_M);
                    }
                }
                break;
            }
            case SSWorldFieldShapes::Record::CLASS_CYLINDER:
            {
                // Conservative on tilt: columns within the radius get the
                // cap-span interval. Pillars and walls are upright in
                // practice; the exact tilted sweep waits for a consumer.
                const LLVector3 p0 = rec.mCenter - rec.mAxes[2] * rec.mHalfHeight;
                const LLVector3 p1 = rec.mCenter + rec.mAxes[2] * rec.mHalfHeight;
                const F32 z0 = llmin(p0.mV[VZ], p1.mV[VZ]);
                const F32 z1 = llmax(p0.mV[VZ], p1.mV[VZ]);
                const F32 reach = rec.mRadius + SS_TILES_CELL * 0.5f;
                const LLVector3 ax2d(rec.mAxes[2].mV[VX], rec.mAxes[2].mV[VY], 0.f);
                const F32 axlen2 = ax2d.magVecSquared();
                for (S32 y = y0; y <= y1; ++y)
                {
                    const F32 py = tmin.mV[VY] + ((F32)y + 0.5f) * SS_TILES_CELL;
                    for (S32 x = x0; x <= x1; ++x)
                    {
                        const F32 px = tmin.mV[VX] + ((F32)x + 0.5f) * SS_TILES_CELL;
                        bool inside = false;
                        if (axlen2 < 1e-12f)
                        {
                            inside = (LLVector3(px, py, 0.f) - LLVector3(rec.mCenter.mV[VX], rec.mCenter.mV[VY], 0.f)).magVec() <= reach;
                        }
                        else
                        {
                            // Distance from the column centre to the axis line.
                            const LLVector3 w(px - rec.mCenter.mV[VX], py - rec.mCenter.mV[VY], 0.f);
                            const F32 s = (w * ax2d) / axlen2;
                            const LLVector3 closest = LLVector3(rec.mCenter.mV[VX], rec.mCenter.mV[VY], 0.f) + ax2d * s;
                            inside = (LLVector3(px, py, 0.f) - closest).magVec() <= reach;
                        }
                        if (inside)
                        {
                            markColumn(x, y, z0, z1);
                        }
                    }
                }
                break;
            }
            case SSWorldFieldShapes::Record::CLASS_TRI:
            {
                const size_t nt = rec.mTri.size() / 3;
                for (size_t k = 0; k < nt; ++k)
                {
                    const LLVector3& v0 = rec.mTri[k * 3];
                    const LLVector3& v1 = rec.mTri[k * 3 + 1];
                    const LLVector3& v2 = rec.mTri[k * 3 + 2];
                    const F32 minx = llmin(v0.mV[VX], llmin(v1.mV[VX], v2.mV[VX]));
                    const F32 maxx = llmax(v0.mV[VX], llmax(v1.mV[VX], v2.mV[VX]));
                    const F32 miny = llmin(v0.mV[VY], llmin(v1.mV[VY], v2.mV[VY]));
                    const F32 maxy = llmax(v0.mV[VY], llmax(v1.mV[VY], v2.mV[VY]));
                    S32 cx0 = llclamp((S32)floorf((minx - tmin.mV[VX]) / SS_TILES_CELL), 0, SS_TILES_N - 1);
                    S32 cy0 = llclamp((S32)floorf((miny - tmin.mV[VY]) / SS_TILES_CELL), 0, SS_TILES_N - 1);
                    S32 cx1 = llclamp((S32)floorf((maxx - tmin.mV[VX]) / SS_TILES_CELL), 0, SS_TILES_N - 1);
                    S32 cy1 = llclamp((S32)floorf((maxy - tmin.mV[VY]) / SS_TILES_CELL), 0, SS_TILES_N - 1);

                    const LLVector3 e1 = v1 - v0;
                    const LLVector3 e2 = v2 - v0;
                    for (S32 y = cy0; y <= cy1; ++y)
                    {
                        const F32 py = tmin.mV[VY] + ((F32)y + 0.5f) * SS_TILES_CELL;
                        for (S32 x = cx0; x <= cx1; ++x)
                        {
                            const F32 px = tmin.mV[VX] + ((F32)x + 0.5f) * SS_TILES_CELL;
                            // Barycentric point-in-triangle at the column centre.
                            const F32 d = e1.mV[VX] * e2.mV[VY] - e2.mV[VX] * e1.mV[VY];
                            if (fabsf(d) < 1e-9f) continue;
                            const F32 wx = px - v0.mV[VX];
                            const F32 wy = py - v0.mV[VY];
                            const F32 b1 = (wx * e2.mV[VY] - e2.mV[VX] * wy) / d;
                            const F32 b2 = (e1.mV[VX] * wy - wx * e1.mV[VY]) / d;
                            if (b1 < -1e-6f || b2 < -1e-6f || b1 + b2 > 1.000001f) continue;
                            const F32 z = v0.mV[VZ] + b1 * e1.mV[VZ] + b2 * e2.mV[VZ];
                            markColumn(x, y, z - SS_TILES_CELL * 0.5f, z + SS_TILES_CELL * 0.5f);
                        }
                    }
                }
                break;
            }
        }
    });

    TileData& tile = mTiles[key];
    tile.mStamp = mCensusStamp;
    extractSpans(tile);

    mLastRasterMS = raster_timer.getElapsedTimeF32() * 1000.f;
    ++mRasterCount;
    return true;
}

// Run-length the scratch into per-column spans: sub-slab gaps merge, the
// span budget collapses the thinnest gap - the store reads like the legacy one.
void SSWorldFieldTiles::extractSpans(TileData& out) const
{
    out.mColStart.assign(SS_TILES_N * SS_TILES_N + 1, 0);
    out.mSpanBottom.clear();
    out.mSpanTop.clear();
    out.mSpanFlags.clear();

    S32 runs[SSWorldFieldTiles::MAX_SPANS * 2];
    U32 packed_count = 0;
    for (S32 y = 0; y < SS_TILES_N; ++y)
    {
        for (S32 x = 0; x < SS_TILES_N; ++x)
        {
            const S32 base = (y * SS_TILES_N + x) * SS_TILES_N;
            S32 nruns = 0;
            S32 k = 0;
            while (k < SS_TILES_N)
            {
                if (!mVoxels[base + k]) { ++k; continue; }
                const S32 start = k;
                while (k < SS_TILES_N && mVoxels[base + k]) ++k;
                if (nruns > 0 && start - runs[(nruns - 1) * 2 + 1] < SS_TILES_SLAB_CELLS)
                {
                    // Sub-slab gap: extend the previous run over it.
                    runs[(nruns - 1) * 2 + 1] = k;
                }
                else if (nruns < MAX_SPANS)
                {
                    runs[nruns * 2] = start;
                    runs[nruns * 2 + 1] = k;
                    ++nruns;
                }
                else
                {
                    // Span budget full: collapse the thinnest gap by merging
                    // its two neighbours, then extend the last run.
                    S32 thin = 0;
                    S32 thin_gap = SS_TILES_N;
                    for (S32 r = 0; r < nruns - 1; ++r)
                    {
                        const S32 gap = runs[(r + 1) * 2] - runs[r * 2 + 1];
                        if (gap < thin_gap) { thin_gap = gap; thin = r; }
                    }
                    runs[thin * 2 + 1] = runs[(thin + 1) * 2 + 1];
                    for (S32 r = thin + 1; r < nruns - 1; ++r)
                    {
                        runs[r * 2] = runs[(r + 1) * 2];
                        runs[r * 2 + 1] = runs[(r + 1) * 2 + 1];
                    }
                    --nruns;
                    if (start - runs[(nruns - 1) * 2 + 1] < SS_TILES_SLAB_CELLS)
                    {
                        runs[(nruns - 1) * 2 + 1] = k;
                    }
                    else
                    {
                        runs[nruns * 2] = start;
                        runs[nruns * 2 + 1] = k;
                        ++nruns;
                    }
                }
            }
            for (S32 r = 0; r < nruns; ++r)
            {
                out.mSpanBottom.push_back((U16)runs[r * 2]);
                out.mSpanTop.push_back((U16)runs[r * 2 + 1]);
                out.mSpanFlags.push_back(1);
                ++packed_count;
            }
            out.mColStart[(y * SS_TILES_N + x) + 1] = packed_count;
        }
    }
}

// The bulk read: solid spans of one world column, in world metres.
S32 SSWorldFieldTiles::spansAt(const LLVector3& pos_agent, Span out[MAX_SPANS]) const
{
    const F32 inv = 1.f / TILE_M;
    const S32 tx = (S32)floorf(pos_agent.mV[VX] * inv);
    const S32 ty = (S32)floorf(pos_agent.mV[VY] * inv);
    const S32 tz = (S32)floorf(pos_agent.mV[VZ] * inv);
    auto it = mTiles.find(ss_tile_key(tx, ty, tz));
    if (it == mTiles.end()) return 0;

    const F32 invc = 1.f / SS_TILES_CELL;
    const S32 cx = llclamp((S32)floorf((pos_agent.mV[VX] - (F32)tx * TILE_M) * invc), 0, SS_TILES_N - 1);
    const S32 cy = llclamp((S32)floorf((pos_agent.mV[VY] - (F32)ty * TILE_M) * invc), 0, SS_TILES_N - 1);
    const S32 col = cy * SS_TILES_N + cx;
    const U32 s0 = it->second.mColStart[col];
    const U32 s1 = it->second.mColStart[col + 1];
    const F32 zbase = (F32)tz * TILE_M;

    S32 count = 0;
    for (U32 s = s0; s < s1 && count < MAX_SPANS; ++s)
    {
        out[count].mBottom = zbase + (F32)it->second.mSpanBottom[s] * SS_TILES_CELL;
        out[count].mTop = zbase + (F32)it->second.mSpanTop[s] * SS_TILES_CELL;
        out[count].mFlags = it->second.mSpanFlags[s];
        ++count;
    }
    return count;
}
