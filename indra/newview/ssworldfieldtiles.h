/**
 * @file ssworldfieldtiles.h
 * @brief Atmo Magic: the 3D-tiled census raster - structure spans beneath the sweep.
 *
 *        Rasterizes the declared-shape census into 32 m voxel tiles
 *        (128^3 cells at 0.25 m) under the Recast scratch rule: one reusable
 *        voxel scratch per process, compact per-column spans published into a
 *        sparse world-keyed tile map, nothing fine persisted. The flood's
 *        SHELTERED/INTERIOR structure reads these spans; the region-anchored
 *        render sweep stays the outdoors authority alone
 *        (doc/atmo_magic_worldfield_competition.md 11).
 *
 *        Store-bound rules: DYNAMIC records (roots that moved within the
 *        settle window) are excluded - their cover is resolved at query time
 *        against the live census, never history. Tiles exist only where
 *        geometry is; the bucket-proven emptiness of a tile costs nothing.
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

#ifndef SS_WORLDFIELD_TILES_H
#define SS_WORLDFIELD_TILES_H

#include "llsingleton.h"
#include "v3math.h"

#include <unordered_map>
#include <vector>

class SSWorldFieldTiles : public LLSingleton<SSWorldFieldTiles>
{
    LLSINGLETON_EMPTY_CTOR(SSWorldFieldTiles);

public:
    static constexpr F32 TILE_M = 32.f;         // tile edge in metres
    static constexpr S32 TILE_CELLS = 128;      // 0.25 m cells per axis
    static constexpr S32 MAX_SPANS = 6;         // per column, thinnest-gap collapse

    // A published span in world metres: solid over [mBottom, mTop).
    struct Span
    {
        F32 mBottom = 0.f;
        F32 mTop = 0.f;
        U8 mFlags = 0;
    };

    // Per-frame maintenance: schedule tiles off the census envelope, rasterize
    // a budgeted number per frame, re-schedule everything on census rebuild.
    void update();

    // The bulk read the merge gate and future flood re-source start from: the
    // solid spans of the column at pos_agent. 0 when no tile covers it.
    S32 spansAt(const LLVector3& pos_agent, Span out[MAX_SPANS]) const;

    // Stats
    S32 tileCount() const { return (S32)mTiles.size(); }
    S32 pendingCount() const { return (S32)mWorklist.size(); }
    U32 rasterCount() const { return mRasterCount; }
    F32 lastRasterMS() const { return mLastRasterMS; }

private:
    // Published tile: spans per column, CSR over the 128^2 column grid.
    // World z = tile_min_z + cell * 0.25; cell units are U16, bottom
    // inclusive, top exclusive.
    struct TileData
    {
        U64 mStamp = 0;                     // census build the tile was rasterized from
        std::vector<U32> mColStart;         // TILE_CELLS*TILE_CELLS+1 offsets
        std::vector<U16> mSpanBottom;       // packed span entries, col-major
        std::vector<U16> mSpanTop;
        std::vector<U8> mSpanFlags;
    };

    void scheduleTiles();
    bool rasterTile(S32 tx, S32 ty, S32 tz);
    void extractSpans(TileData& out) const;
    static U64 tileKey(S32 tx, S32 ty, S32 tz);

    std::unordered_map<U64, TileData> mTiles;
    std::vector<U64> mWorklist;                 // pending tile keys
    std::vector<U8> mVoxels;                    // the scratch: 128^3, reused, never persisted
    U64 mCensusStamp = 0;                       // census build the schedule is keyed to
    F64 mScheduleAt = 0.0;
    F32 mLastRasterMS = 0.f;
    U32 mRasterCount = 0;
};

#endif
