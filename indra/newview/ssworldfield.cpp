/**
 * @file ssworldfield.cpp
 * @brief See ssworldfield.h.
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

#include "ssworldfield.h"
#include "ssatmomagic.h"
#include "ssglreadback.h"
#include "ssworldfieldshapes.h"
#include "ssnavmesh.h"

#include "llfasttimer.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "workqueue.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <queue>

static const F32 NEIGHBOR_REACH   = 64.f;
static const F32 DEPTH_MISS       = 0.9999f;
// The upward pass clears depth to zero and keeps the farthest front-facing
// fragment, so its miss sentinel is zero rather than one.
static const F32 DEPTH_MISS_UP    = 0.0001f;
static const F32 BOUNDARY_EPSILON = 0.05f;
static const F32 NO_SURFACE       = -FLT_MAX;

// <SS:Nexii> The enclosure spectrum's saturation constant: metres of
// air-graph distance back to open sky at which the ramp reads half enclosed.
// Small structures - eaves, canopies, a doorway's threshold - sit a metre or
// two from the outdoors and stay near 0; rooms and caves whose openings are
// tens of metres of graph away climb toward 1.
static const F32 SS_WF_ENCLOSURE_TAU_M = 4.f;

// <SS:Nexii> How many solid spans a column may hold. Real content runs two
// to five; a column that resolves past the cap merges its smallest air gap
// rather than dropping a body. The span store's arrays are sized from this.
static constexpr S32 SS_WF_MAX_SPANS = 6;

// <SS:Nexii> The Z-bisection granularity: an interval is bisected only while
// its halves stay at or above this height, and a body hanging less than this
// far above its interval's floor leaves no unexplored space worth probing.
// Bodies closer together vertically than this merge into one span.
static constexpr F32 SS_WF_MIN_BAND_M = 1.f;

// <SS:Nexii> The scratch slot cap: the base sweep's uniform bands plus the
// Z-bisection children. Transient per build - commitBuild releases the
// scratch after foldSpans folds it into the span store.
static constexpr S32 SS_WF_MAX_SLOTS = 48;

// <SS:Nexii> The assumed minimum slab: air under a body's captured underside
// is trimmed by this much before it becomes a lower span, so a wall standing
// on a floor - whose underside and the floor's top face coincide - never
// reads as a hollow shell. The trim is also the body thickness the store
// assumes between a lower span's top and the body's captured top face; the
// real slab is usually thinner, and the error is sub-cell at the shipping
// resolutions.
static const F32 SS_WF_SPAN_SLAB_M = 0.25f;

static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD("Atmo Magic World Field");
static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD_GRID("Atmo Magic World Field Grid");

// Channel interest refcounts. File statics so an Interest handle's deleter
// stays valid for the process's life regardless of singleton teardown
// order - a destroyed handle must always release its count.
static std::map<std::pair<U64, S32>, S32> sInterests;

// <SS:Nexii> The debug overlay's materialised drainage view, cached per region and rebuilt only when the tile's geometry serial moves - the same drop-on-serial-change discipline the real channels will follow, so the overlay never pays a per-frame priority flood just to draw. Declared with the other file statics because evict() drops it alongside the tiles it belongs to.
struct SS_WF_DrainDebug
{
    SSRainShadowMap::SurfaceGrid mGrid;
    SSWorldField::Drainage mDrain;
    U32 mSerial = 0;
};
static std::map<U64, SS_WF_DrainDebug> sDrainDebug;

static S32 ss_wf_interest_count(U64 region_handle, S32 channel)
{
    auto it = sInterests.find(std::make_pair(region_handle, channel));
    return (it != sInterests.end()) ? it->second : 0;
}

static bool ss_wf_region_claimed(U64 region_handle)
{
    for (S32 ch = 0; ch <= (S32)SSWorldField::EChannel::ACOUSTIC; ++ch)
    {
        if (ss_wf_interest_count(region_handle, ch) > 0) return true;
    }
    return false;
}

SSWorldField::Interest SSWorldField::claim(U64 region_handle, EChannel channel)
{
    const std::pair<U64, S32> key(region_handle, (S32)channel);
    ++sInterests[key];

    return Interest(std::shared_ptr<void>((void*)1, [key](void*)
    {
        auto it = sInterests.find(key);
        if (it != sInterests.end() && --(it->second) <= 0)
        {
            sInterests.erase(it);
        }
    }));
}

// The wet field's source switch - while on, SURFACE_TOP counts as claimed
// for the camera region and its neighbours, the same reach the rain shadow
// capture serves today.
bool SSWorldField::surfaceTopDemanded() const
{
    static LLCachedControl<bool> demanded(gSavedSettings, "SSWorldFieldSurfaceTop", false);
    return demanded;
}

// One edit fan-out. Settled prim edits land here; the tile's dirty rectangle
// grows to cover the edit, and the re-peel is scissored to it.
void SSWorldField::markDirty(const LLVector3& pos_agent, F32 radius)
{
    SSWorldField* self = getInstance();
    if (!self) return;

    // <SS:Nexii> The declared-shape census rides the same fan-out, debounced on its side.
    SSWorldFieldShapes::markDirty(pos_agent, radius);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return;

    auto it = self->mTiles.find(regionp->getHandle());
    if (it == self->mTiles.end() || !it->second.mValid) return;

    Tile& tile = it->second;
    tile.mLastTouched = self->mNow;

    const F32 x = pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX];
    const F32 y = pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY];
    const F32 r = llmax(radius, tile.mCell);

    const S32 x0 = llclamp((S32)floorf((x - r) / tile.mCell), 0, tile.mRes - 1);
    const S32 y0 = llclamp((S32)floorf((y - r) / tile.mCell), 0, tile.mRes - 1);
    const S32 x1 = llclamp((S32)ceilf((x + r) / tile.mCell), x0 + 1, tile.mRes);
    const S32 y1 = llclamp((S32)ceilf((y + r) / tile.mCell), y0 + 1, tile.mRes);

    if (tile.mDirty)
    {
        tile.mDirtyX0 = llmin(tile.mDirtyX0, x0);
        tile.mDirtyY0 = llmin(tile.mDirtyY0, y0);
        tile.mDirtyX1 = llmax(tile.mDirtyX1, x1);
        tile.mDirtyY1 = llmax(tile.mDirtyY1, y1);
    }
    else
    {
        tile.mDirtyX0 = x0;
        tile.mDirtyY0 = y0;
        tile.mDirtyX1 = x1;
        tile.mDirtyY1 = y1;
        tile.mDirty = true;
    }

// Bands the edit's own altitude could touch. A removed roof drops its whole
    // column, so a rect re-peel sweeps from the band floor up to just past the
    // edit, not only the bands the edit's box overlaps.
    const S32 band = llclamp((S32)((pos_agent.mV[VZ] + r) / tile.mBandHeight), 0, SSWorldField::MAX_BANDS - 1);
    tile.mBandTarget = llmax(tile.mBandTarget, band + 1);
}

void SSWorldField::clear()
{
    // A read in flight still references mTarget's depth texture - let it land
    // first, then tear the target down from the read's completion.
    if (mReadbackPending) { mClearPending = true; return; }
    mTiles.clear();
    mBuild.mActive = false;
    ++mFloodGeneration;     // a flood in flight lands into nothing
    mTarget.release();
}

void SSWorldField::shutdownGL()
{
    mReadbackPending = false;
    mClearPending = false;
    clear();
}

bool SSWorldField::tileValid(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end() && it->second.mValid);
}

U32 SSWorldField::geometrySerial(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end()) ? it->second.mGeomSerial : 0;
}

F32 SSWorldField::bandHeight() const
{
    static LLCachedControl<F32> band(gSavedSettings, "SSWorldFieldBand", 16.f);
    return llclamp((F32)band, 4.f, 64.f);
}

S32 SSWorldField::bandCount() const
{
    static LLCachedControl<F32> ceiling(gSavedSettings, "SSWorldFieldCeiling", 256.f);
    const S32 count = (S32)ceilf(llmax((F32)ceiling, bandHeight()) / bandHeight());
    return llclamp(count, 1, MAX_BANDS);
}

S32 SSWorldField::resolution() const
{
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid) return entry.second.mRes;
    }
    return 0;
}

F64 SSWorldField::tileAge(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return -1.0;
    return mNow - tile->mCaptureTime;
}

S32 SSWorldField::effectiveBands(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    return tile ? tile->mBandCount : 0;
}

const SSWorldField::Tile* SSWorldField::tileAt(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return nullptr;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end()) return nullptr;
    return &it->second;
}

SSWorldField::Tile* SSWorldField::tileFor(LLViewerRegion* regionp, bool allow_create)
{
    auto it = mTiles.find(regionp->getHandle());
    if (it != mTiles.end()) return &it->second;
    if (!allow_create) return nullptr;

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 0.25f);
    const F32 cell = llclamp((F32)cell_setting, 0.25f, 8.f);
    const F32 width = regionp->getWidth();
    const S32 res = llclamp((S32)llround(width / cell), 32, 1024);

    Tile& tile = mTiles[regionp->getHandle()];
    tile.mRegionHandle = regionp->getHandle();
    tile.mRes = res;
    tile.mCell = width / (F32)res;
    tile.mBandHeight = bandHeight();
    tile.mAllocBands = 0;

    // The column span store and its flood output are fixed-size per tile.
    const size_t layer = (size_t)res * res;
    tile.mSpanBottom.assign((size_t)SS_WF_MAX_SPANS * layer, NO_SURFACE);
    tile.mSpanTop.assign((size_t)SS_WF_MAX_SPANS * layer, NO_SURFACE);
    tile.mSpanFlags.assign((size_t)SS_WF_MAX_SPANS * layer, 0);
    tile.mGapLabel.assign((size_t)(SS_WF_MAX_SPANS + 1) * layer, (U8)AIR_SOLID);
    tile.mGapDepth.assign((size_t)(SS_WF_MAX_SPANS + 1) * layer, (U16)AIR_DEPTH_UNREACHED);
    return &tile;
}

void SSWorldField::dumpColumn(const LLVector3& pos_agent, std::vector<std::string>& out) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    const Tile* tile = tileAt(pos_agent);
    if (!regionp || !tile) { out.push_back("world field: no tile under the point"); return; }
    out.push_back(llformat("world field tile: %s, source %s, res %d (%.2f m cells), geometry serial %u, air serial %u, acoustic serial %u, nav dirty %d",
                           tile->mValid ? "valid" : "NOT VALID", tile->mNavSourced ? "navmesh spans" : "depth capture", tile->mRes, tile->mCell,
                           tile->mGeomSerial, tile->mAirSerial, tile->mAcoustic.mSerial, (S32)tile->mNavDirty));
    const LLVector3 rp = regionp->getPosRegionFromAgent(pos_agent);
    const S32 x = llclamp((S32)(rp.mV[VX] / tile->mCell), 0, tile->mRes - 1), y = llclamp((S32)(rp.mV[VY] / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)y * tile->mRes + x, layer = (size_t)tile->mRes * tile->mRes;
    static const char* AIR_NAME[] = {"solid", "outdoors", "sheltered", "interior", "unknown"};
    S32 n = 0;
    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
    {
        const size_t si = (size_t)k * layer + col;
        if (tile->mSpanTop[si] <= NO_SURFACE * 0.5f) break;
        const U8 gl = tile->mGapLabel[col * (SS_WF_MAX_SPANS + 1) + k];
        out.push_back(llformat("  gap %d below: %s depth %u; span %d: z %.2f..%.2f flags 0x%02x%s%s", k, AIR_NAME[llclamp((S32)gl, 0, 4)],
                               (U32)tile->mGapDepth[col * (SS_WF_MAX_SPANS + 1) + k], k, tile->mSpanBottom[si], tile->mSpanTop[si], tile->mSpanFlags[si],
                               (tile->mSpanFlags[si] & SSRainShadowMap::SURF_FALLBACK) ? " terrain" : "", (tile->mSpanFlags[si] & SSRainShadowMap::SURF_WATER) ? " water" : ""));
        ++n;
    }
    const U8 top_gl = tile->mGapLabel[col * (SS_WF_MAX_SPANS + 1) + n];
    out.push_back(llformat("  gap %d above: %s depth %u (column %d,%d, %d spans)", n, AIR_NAME[llclamp((S32)top_gl, 0, 4)], (U32)tile->mGapDepth[col * (SS_WF_MAX_SPANS + 1) + n], x, y, n));
    out.push_back(llformat("  at the point: air %s, enclosure %.2f", AIR_NAME[llclamp((S32)airLabelAt(pos_agent), 0, 4)], enclosureAt(pos_agent)));
}

bool SSWorldField::navSpansWanted()
{
    static LLCachedControl<bool> from_nav(gSavedSettings, "SSWorldFieldFromNavMesh", true);
    return from_nav && SSNavMesh::instanceExists() && SSNavMesh::getInstance()->active();
}

bool SSWorldField::navSourced(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return it != mTiles.end() && it->second.mNavSourced;
}

// The same reach pickBuildTarget serves: the camera's region and any region within NEIGHBOR_REACH of the camera.
bool SSWorldField::regionNear(const LLViewerRegion* regionp, const LLVector3& cam) const
{
    const LLVector3 origin = regionp->getOriginAgent();
    const F32 width = regionp->getWidth();
    const F32 dx = llmax(origin.mV[VX] - cam.mV[VX], cam.mV[VX] - (origin.mV[VX] + width), 0.f);
    const F32 dy = llmax(origin.mV[VY] - cam.mV[VY], cam.mV[VY] - (origin.mV[VY] + width), 0.f);
    return dx * dx + dy * dy <= NEIGHBOR_REACH * NEIGHBOR_REACH;
}

// <SS:Nexii> One band's sheet into the tile: the band's z-range is cut out of every column it covers (a span straddling the range keeps its parts outside it), the sheet's spans go in through spanInsert, and a span flagged terrain then reaches the world floor, swallowing whatever was below - the capture's "a wall on unmeasured ground stays solid to it". Sheet columns are 0.25 m; a coarser tile unions every sheet column its cell covers. A region without a tile gets one only within reach, and the navmesh is asked to feed the whole region so bands published before the tile existed arrive too. [interaction: finalizeSpans]
void SSWorldField::navSpans(U64 region_handle, F32 x0_m, F32 y0_m, F32 extent_m, F32 zmin, F32 zmax, const SSNavMesh::SpanSheet* sheet)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldField", true);
    if (!enabled || !SSAtmoMagic::getInstance()->isEnabled()) return;
    // <SS:Nexii> buildBand allocates the sheet before its no-geometry and heightfield-failure early-outs, so an unfilled sheet arrives with empty vectors; indexing it below reads the null page. Unfilled means no sheet. [interaction: SSNavMesh::buildBand, extractSpanSheet]
    if (sheet && (sheet->mCount.size() != (size_t)SSNavMesh::SpanSheet::RES * SSNavMesh::SpanSheet::RES
                  || sheet->mBottom.size() != sheet->mCount.size() * SSNavMesh::SpanSheet::SPANS
                  || sheet->mTop.size() != sheet->mBottom.size() || sheet->mFlags.size() != sheet->mBottom.size())) sheet = nullptr;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return;
    Tile* tile = tileFor(regionp, false);
    if (!tile)
    {
        if (!sheet) return;
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        if (!regionNear(regionp, cam)) return;
        if (mTiles.size() >= MAX_TILES)
        {
            // Make room with a tile out of reach; none out of reach means this region waits its turn.
            auto victim = mTiles.end();
            for (auto it = mTiles.begin(); it != mTiles.end(); ++it)
            {
                LLViewerRegion* r = LLWorld::getInstance()->getRegionFromHandle(it->first);
                if (!r || !regionNear(r, cam)) { victim = it; break; }
            }
            if (victim == mTiles.end()) return;
            sDrainDebug.erase(victim->first);
            mTiles.erase(victim);
            ++mFloodGeneration;
        }
        tile = tileFor(regionp, true);
    }
    if (!tile->mNavSourced)
    {
        // Taking over from the capture (or brand new): start from an empty store and have every band land again.
        std::fill(tile->mSpanBottom.begin(), tile->mSpanBottom.end(), NO_SURFACE);
        std::fill(tile->mSpanTop.begin(), tile->mSpanTop.end(), NO_SURFACE);
        std::fill(tile->mSpanFlags.begin(), tile->mSpanFlags.end(), (U8)0);
        tile->mNavSourced = true;
        tile->mValid = false;
        tile->mDirty = false;
        tile->mBandTarget = 0;
        SSNavMesh::getInstance()->refeedRegion(region_handle);
    }
    tile->mLastTouched = mNow;
    tile->mNavLastFed = mNow;
    tile->mNavDirty = true;
    ++mNavBlocksFed;

    const S32 res = tile->mRes;
    const F32 cell = tile->mCell;
    const size_t layer = (size_t)res * res;
    const S32 cx0 = llclamp((S32)floorf(x0_m / cell + 0.5f), 0, res), cx1 = llclamp((S32)floorf((x0_m + extent_m) / cell + 0.5f), 0, res);
    const S32 cy0 = llclamp((S32)floorf(y0_m / cell + 0.5f), 0, res), cy1 = llclamp((S32)floorf((y0_m + extent_m) / cell + 0.5f), 0, res);
    const S32 R = SSNavMesh::SpanSheet::RES, K = SSNavMesh::SpanSheet::SPANS;
    const F32 sheet_cell = extent_m / (F32)R;

    F32 bottoms[SS_WF_MAX_SPANS], tops[SS_WF_MAX_SPANS];
    U8 flags[SS_WF_MAX_SPANS];
    for (S32 y = cy0; y < cy1; ++y)
    {
        for (S32 x = cx0; x < cx1; ++x)
        {
            const size_t col = (size_t)y * res + x;

            // 1. Cut the band's range out of the column.
            S32 n = 0;
            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                const F32 top = tile->mSpanTop[si];
                if (top <= NO_SURFACE * 0.5f) break;
                const F32 bot = tile->mSpanBottom[si];
                const U8 fl = tile->mSpanFlags[si];
                if (top <= zmin || bot >= zmax) { if (n < SS_WF_MAX_SPANS) { bottoms[n] = bot; tops[n] = top; flags[n] = fl; ++n; } continue; }
                if (bot < zmin && n < SS_WF_MAX_SPANS) { bottoms[n] = bot; tops[n] = zmin; flags[n] = fl; ++n; }
                if (top > zmax && n < SS_WF_MAX_SPANS) { bottoms[n] = zmax; tops[n] = top; flags[n] = fl; ++n; }
            }
            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                tile->mSpanBottom[si] = k < n ? bottoms[k] : NO_SURFACE;
                tile->mSpanTop[si] = k < n ? tops[k] : NO_SURFACE;
                tile->mSpanFlags[si] = k < n ? flags[k] : (U8)0;
            }
            if (!sheet) continue;

            // 2. Insert the sheet's spans for every sheet column this cell covers.
            const S32 sx0 = llclamp((S32)floorf(((F32)x * cell - x0_m) / sheet_cell), 0, R - 1);
            const S32 sx1 = llclamp((S32)ceilf(((F32)(x + 1) * cell - x0_m) / sheet_cell), sx0 + 1, R);
            const S32 sy0 = llclamp((S32)floorf(((F32)y * cell - y0_m) / sheet_cell), 0, R - 1);
            const S32 sy1 = llclamp((S32)ceilf(((F32)(y + 1) * cell - y0_m) / sheet_cell), sy0 + 1, R);
            bool terrain = false;
            for (S32 sy = sy0; sy < sy1; ++sy) for (S32 sx = sx0; sx < sx1; ++sx)
            {
                const size_t sc = (size_t)sy * R + sx;
                for (S32 k = 0; k < (S32)sheet->mCount[sc] && k < K; ++k)
                {
                    const U8 fl = sheet->mFlags[sc * K + k];
                    if (fl & SSRainShadowMap::SURF_FALLBACK) terrain = true;
                    spanInsert(*tile, col, sheet->mBottom[sc * K + k], sheet->mTop[sc * K + k], fl);
                }
            }
            if (!terrain) continue;

            // 3. The land reaches the world floor: everything below the terrain span folds into it.
            n = 0;
            S32 land = -1;
            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                if (tile->mSpanTop[si] <= NO_SURFACE * 0.5f) break;
                bottoms[n] = tile->mSpanBottom[si]; tops[n] = tile->mSpanTop[si]; flags[n] = tile->mSpanFlags[si];
                if (land < 0 && (flags[n] & SSRainShadowMap::SURF_FALLBACK)) land = n;
                ++n;
            }
            if (land < 0) continue;
            bottoms[land] = 0.f;
            const S32 kept = n - land;
            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                tile->mSpanBottom[si] = k < kept ? bottoms[land + k] : NO_SURFACE;
                tile->mSpanTop[si] = k < kept ? tops[land + k] : NO_SURFACE;
                tile->mSpanFlags[si] = k < kept ? flags[land + k] : (U8)0;
            }
        }
    }
}

// <SS:Nexii> Regions in reach get a tile and a refeed as they come into reach (the same reach the capture served, and the tile cap decides how many); a tile whose region the navmesh has finished with - nothing queued, nothing building, no schedule pending - moves its geometry serial, which is what sends the flood and the acoustic bake over it. A rebuild trickling in after an edit moves it again once quiet. [interaction: scheduleFlood]
void SSWorldField::navSettle()
{
    SSNavMesh* nav = SSNavMesh::getInstance();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionNear(regionp, cam)) continue;
        auto it = mTiles.find(regionp->getHandle());
        if (it != mTiles.end()) { it->second.mLastTouched = mNow; continue; }
        if (mTiles.size() >= MAX_TILES)
        {
            auto victim = mTiles.end();
            for (auto vt = mTiles.begin(); vt != mTiles.end(); ++vt)
            {
                LLViewerRegion* r = LLWorld::getInstance()->getRegionFromHandle(vt->first);
                if (!r || !regionNear(r, cam)) { victim = vt; break; }
            }
            if (victim == mTiles.end()) continue;
            sDrainDebug.erase(victim->first);
            mTiles.erase(victim);
            ++mFloodGeneration;
        }
        Tile* tile = tileFor(regionp, true);
        tile->mNavSourced = true;
        tile->mValid = false;
        tile->mLastTouched = mNow;
        nav->refeedRegion(regionp->getHandle());
    }
    for (auto& entry : mTiles)
    {
        Tile& tile = entry.second;
        if (!tile.mNavSourced || !tile.mNavDirty) continue;
        if (mNow - tile.mNavLastFed < 0.5) continue;                 // sheets still landing
        if (!nav->regionSettled(tile.mRegionHandle)) continue;
        tile.mGeomSerial = (tile.mGeomSerial == 0xFFFFFFFFu) ? 1 : tile.mGeomSerial + 1;
        tile.mValid = true;
        tile.mBandCount = bandCount();      // <SS:Nexii> The flood gate and the acoustic snapshot ceiling: a navmesh-fed store is full height, and nothing on this path set the capture-side band statistic, so it stayed 0 and scheduleFlood declined every tile silently (air serial 0 in every dump). [interaction: scheduleFlood]
        tile.mNavDirty = false;
        tile.mDirty = false;
        tile.mBandTarget = 0;
        tile.mCaptureTime = mNow;
        ++mNavSettles;
    }
}

// Whether a tile is worth (re)building: never built, edited, stale, or
// captured under a cell/band setting that has since changed.
bool SSWorldField::needsBuild(const Tile& tile) const
{
    if (!tile.mValid) return true;

    static LLCachedControl<F32> max_age(gSavedSettings, "SSWorldFieldMaxAge", 120.f);
    if (mNow - tile.mCaptureTime > (F64)llmax(5.f, (F32)max_age)) return true;

    if (tile.mDirty && mNow - tile.mCaptureTime > DIRTY_MIN_INTERVAL) return true;

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 0.25f);
    const F32 cell = llclamp((F32)cell_setting, 0.25f, 8.f);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;
    const S32 res = llclamp((S32)llround(regionp->getWidth() / cell), 32, 1024);
    if (res != tile.mRes) return true;

    if (fabsf(tile.mBandHeight - bandHeight()) > 0.01f) return true;

    return false;
}

// Most deserving tile first: the camera's region, then any region within
// neighbour reach that has a claimed channel or serves the demanded surface
// top. Nothing builds while nothing demands anything.
SSWorldField::Tile* SSWorldField::pickBuildTarget()
{
    if (!surfaceTopDemanded() && sInterests.empty()) return nullptr;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    if (cam_region)
    {
        Tile* tile = tileFor(cam_region, true);
        tile->mLastTouched = mNow;
        if (needsBuild(*tile)) return tile;
    }

    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || regionp == cam_region) continue;
        if (!ss_wf_region_claimed(regionp->getHandle()) && !surfaceTopDemanded()) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 width = regionp->getWidth();
        const F32 dx = llmax(origin.mV[VX] - cam.mV[VX], cam.mV[VX] - (origin.mV[VX] + width), 0.f);
        const F32 dy = llmax(origin.mV[VY] - cam.mV[VY], cam.mV[VY] - (origin.mV[VY] + width), 0.f);
        if (dx * dx + dy * dy > NEIGHBOR_REACH * NEIGHBOR_REACH) continue;

        Tile* tile = tileFor(regionp, true);
        tile->mLastTouched = mNow;
        if (needsBuild(*tile)) return tile;
    }

    return nullptr;
}

// Per-frame drive: evict departed regions, keep stepping the live build one
// band at a time, and begin a new build when a tile deserves one.
void SSWorldField::update()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD);

    mNow = SSAtmoMagic::getInstance()->sharedTime();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldField", true);
    // <SS:Nexii> Master switch, not hasWeather(): the wind flow map and soundscape consume the field in calm weather too, and pickBuildTarget's interest check already keeps unclaimed regions from building.
    // <SS:Nexii> The Atmo gate flickers: a death teleport or an altitude with no track resolves no environment for a while, and clearing the store on every dip threw away every tile and every consumer's overlay with it, then left the navmesh-fed tiles stale on the way back (their nav-dirty flag was down, so nothing refed them). The setting still clears; the Atmo gate only pauses, and the return edge invalidates every navmesh-fed tile and asks the navmesh for its region again. Both edges log. [interaction: navSettle, SSNavMesh::refeedRegion]
    if (!enabled)
    {
        if (!mTiles.empty() || mBuild.mActive) clear();
        return;
    }
    const bool gate_on = SSAtmoMagic::getInstance()->isEnabled();
    if (!gate_on)
    {
        if (!mGateWasOff) LL_INFOS("SSWorldField") << "world field paused: Atmo Magic resolved no environment (" << mTiles.size() << " tiles kept)" << LL_ENDL;
        mGateWasOff = true;
        mBuild.mActive = false;
        return;
    }
    if (mGateWasOff)
    {
        mGateWasOff = false;
        LL_INFOS("SSWorldField") << "world field resumed: Atmo Magic environment back, refeeding " << mTiles.size() << " tiles" << LL_ENDL;
        for (auto& entry : mTiles)
        {
            if (!entry.second.mNavSourced) continue;
            entry.second.mValid = false;
            entry.second.mNavDirty = true;
            entry.second.mNavLastFed = mNow;
            SSNavMesh::getInstance()->refeedRegion(entry.first);
        }
    }

    evict();

    const bool nav_source = navSpansWanted();
    if (nav_source) navSettle();

// Catch-up for the connectivity labels: a tile that committed while a flood
    // was in flight was skipped rather than queued; this is where it gets its
    // turn - one tile per call. Oldest-first is unnecessary at this scale
    // (four tiles).
    if (!mFloodBusy)
    {
        for (auto& entry : mTiles)
        {
            Tile& tile = entry.second;
            if (tile.mValid && tile.mAirSerial != tile.mGeomSerial && (!tile.mDirty || tile.mNavSourced))
            {
                scheduleFlood(tile);
                break;
            }
        }
    }

    if (mBuild.mActive)
    {
        if (mNow - mLastBandAt < BAND_MIN_INTERVAL) return;
        mLastBandAt = mNow;

        LLTimer band_timer;
        advanceBuild();
        mLastCaptureMS = band_timer.getElapsedTimeF32() * 1000.f;
        return;
    }

    Tile* target = nav_source ? nullptr : pickBuildTarget();     // the navmesh feeds the store; no capture
    if (!target) return;

// Begin a build. A dirty tile re-peels only its dirty rectangle's frustum,
    // but still sweeps every band from the floor to the highest band the edits
    // could touch - a removed roof drops its whole column, so band scoping below
    // the edit is not safe.
    mBuild.mActive = true;
    mBuild.mRegionHandle = target->mRegionHandle;
    target->mNavSourced = false;                                  // the capture owns the store again
    mBuild.mPass = 0;
    mBuild.mEmptyRun = 0;
    mBuild.mChanged = false;
    mBuild.mRectOnly = target->mDirty;

    if (mBuild.mRectOnly)
    {
        mBuild.mRectX0 = target->mDirtyX0;
        mBuild.mRectY0 = target->mDirtyY0;
        mBuild.mRectX1 = target->mDirtyX1;
        mBuild.mRectY1 = target->mDirtyY1;

// Rect capture resources: a square frustum covering the rect's wider axis,
        // with the short side's extra texels spilling outside the rect and skipped
        // at splice time.
        const S32 rw = mBuild.mRectX1 - mBuild.mRectX0;
        const S32 rh = mBuild.mRectY1 - mBuild.mRectY0;
        mBuild.mRectRes = llclamp(llmax(rw, rh), 4, target->mRes);

        // <SS:Nexii> The rect's columns in Z-bisection slots hold sub-band
        // bodies from the previous build. The base re-sweep overwrites the
        // base bands, but these slots would silently fold stale bodies back
        // into the store - an edited-away room would keep its floor. Clear
        // them; this build's bisection re-discovers what is still there.
        // Only slots the arrays actually cover can hold stale bodies: the
        // commit released the scratch (mAllocBands = 0) and applyBand ensures
        // before it writes, so the bound is mAllocBands, not MAX_SLOTS -
        // indexing past the allocation here crashed on the released arrays.
        const size_t layer = (size_t)target->mRes * target->mRes;
        for (S32 k = target->mBaseBands; k < target->mAllocBands; ++k)
        {
            for (S32 y = mBuild.mRectY0; y < mBuild.mRectY1; ++y)
            {
                for (S32 x = mBuild.mRectX0; x < mBuild.mRectX1; ++x)
                {
                    const size_t si = (size_t)k * layer + (size_t)y * target->mRes + x;
                    target->mBandTop[si] = NO_SURFACE;
                    target->mBandUnder[si] = NO_SURFACE;
                    target->mBandFlags[si] = 0;
                }
            }
        }
    }
    else
    {
        target->mBandTarget = bandCount();
        target->mBaseBands = target->mBandTarget;
        mBuild.mRectX0 = 0;
        mBuild.mRectY0 = 0;
        mBuild.mRectX1 = target->mRes;
        mBuild.mRectY1 = target->mRes;
        mBuild.mRectRes = target->mRes;
    }

    target->mBandTarget = llmax(llmax(target->mBandTarget, target->mBandCount), 1);
    mBuild.mEmptyRun = 0;
    mBuild.mChanged = false;
    mBuild.mSeenBand.assign((size_t)target->mRes * (size_t)target->mRes, -1);

    // The capture worklist: the band slots this build sweeps, bottom-up.
    // Stage 1 enumerated them uniformly; the bisection phase appends finer
    // nodes beneath captured bodies as the sweep exposes them.
    mBuild.mWorklist.clear();
    for (S32 b = 0; b < target->mBandTarget; ++b)
    {
        CaptureNode node;
        node.mSlot = b;
        node.mX0 = mBuild.mRectX0;
        node.mY0 = mBuild.mRectY0;
        node.mX1 = mBuild.mRectX1;
        node.mY1 = mBuild.mRectY1;
        node.mRes = llclamp(llmax(mBuild.mRectX1 - mBuild.mRectX0, mBuild.mRectY1 - mBuild.mRectY0), 4, target->mRes);
        node.mZ0 = (F32)b * target->mBandHeight;
        node.mZ1 = node.mZ0 + target->mBandHeight;
        node.mBisect = false;
        mBuild.mWorklist.push_back(node);
    }
    mBuild.mCursor = 0;
    mBuild.mNextSlot = target->mBandTarget;
    mLastBandAt = mNow;
}

// Drops tiles for departed regions, then the least recently used beyond the
// cache cap. Erasing a tile also moves the flood generation - a walk in
// flight for the departed tile must not land on a fresh tile the same region
// handle re-created (restarted at geometry serial 1, so the serial gate alone
// would not stop it) - and drops its cached debug views.
void SSWorldField::evict()
{
    bool erased = false;
    for (auto it = mTiles.begin(); it != mTiles.end();)
    {
        if (!LLWorld::getInstance()->getRegionFromHandle(it->first))
        {
            sDrainDebug.erase(it->first);
            it = mTiles.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    while ((S32)mTiles.size() > MAX_TILES)
    {
        auto oldest = mTiles.begin();
        for (auto it = mTiles.begin(); it != mTiles.end(); ++it)
        {
            if (it->second.mLastTouched < oldest->second.mLastTouched) oldest = it;
        }
        sDrainDebug.erase(oldest->first);
        mTiles.erase(oldest);
        erased = true;
    }
    if (erased) ++mFloodGeneration;
}

// One worklist step: capture the next node's two passes, apply, advance
// through the phases, and commit. Each node is captured in TWO ortho passes
// - pass 0 looks down and reads the highest up-facing surface in the node's
// interval, pass 1 looks up with a reversed depth test and reads the
// topmost body's underside - and the depth readback is async (SSGLReadback):
// the passes render and submit one at a time, and the apply runs once both
// have landed. Base nodes splice into the capture scratch and fold when the
// sweep exhausts; the refine phase's quad nodes write their bodies straight
// into the span store, so the fine work only pays for the quads that need
// it.
bool SSWorldField::advanceBuild()
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mBuild.mRegionHandle);
    Tile* tile = regionp ? tileFor(regionp, false) : nullptr;
    if (!tile)
    {
        mBuild.mActive = false;
        return false;
    }

    // A readback is still in flight: neither apply it (the texels are not
    // here yet) nor render the shared capture target into again.
    if (mReadbackPending) return true;

    // The current pass's readback landed; advance to the node's second pass
    // or apply the pair.
    if (mBuild.mJustCaptured)
    {
        mBuild.mJustCaptured = false;
        ++mBuild.mPass;

        if (mBuild.mPass < 2)
        {
            if (!capturePass(*tile, mBuild.mWorklist[mBuild.mCursor], mBuild.mPass))
            {
                // GL trouble - abandon rather than spin. The tile keeps its
                // previous contents and stays dirty, so the next update tries
                // again.
                mBuild.mActive = false;
                return false;
            }
            mBuild.mJustCaptured = true;
            return true;
        }

        const CaptureNode node = mBuild.mWorklist[mBuild.mCursor];
        if (node.mRefine)
        {
            applyRefine(*tile, node);
        }
        else
        {
            applyBand(*tile, node);
        }
        ++mBuild.mCursor;
        mBuild.mPass = 0;

// Full builds stop early once the sky has been genuinely empty for a few
        // consecutive base nodes; rect builds run their whole worklist so the
        // spliced columns stay consistent with their neighbours. Refine nodes
        // never feed the empty-run: they are targeted probes, and their
        // misses are the recursion terminating, not sky.
        if (!node.mRefine)
        {
            if (node.mHung) mBuild.mEmptyRun = 0;
            else ++mBuild.mEmptyRun;

            if (!mBuild.mRectOnly && mBuild.mEmptyRun >= EMPTY_BANDS_TO_STOP)
            {
                mBuild.mCursor = mBuild.mWorklist.size();   // the sky above is empty; jump to the phase end
            }
        }
        else if (node.mHung)
        {
            // The quad's body hangs: Z-bisect the quad itself. The children
            // run at the quad's own (quarter-cost) resolution and prune
            // independently - an open half stops immediately.
            if ((node.mZ1 - node.mZ0) >= SS_WF_MIN_BAND_M * 2.f)
            {
                const F32 mid = (node.mZ0 + node.mZ1) * 0.5f;

                for (S32 h = 0; h < 2; ++h)
                {
                    CaptureNode child = node;
                    child.mZ0 = (h == 0) ? node.mZ0 : mid;
                    child.mZ1 = (h == 0) ? mid : node.mZ1;
                    child.mBisect = true;
                    mBuild.mWorklist.push_back(child);
                }
            }
        }

        if (mBuild.mCursor >= mBuild.mWorklist.size())
        {
            // The worklist ran dry. The base phase folds its scratch into
            // the store and enqueues the refine phase's quad nodes; the
            // refine phase finalizes the store and commits.
            if (mBuild.mPhase == PHASE_BASE)
            {
                if (mBuild.mRectOnly)
                {
                    foldSpans(*tile, mBuild.mRectX0, mBuild.mRectY0, mBuild.mRectX1, mBuild.mRectY1);
                }
                else
                {
                    foldSpans(*tile, 0, 0, tile->mRes, tile->mRes);
                }

                // A base node whose body hangs at least the minimum interval
                // above its floor leaves unexplored space beneath it: that
                // quad gets refine nodes capturing the band's full interval
                // at the quadrant's own (quarter-cost) resolution. Their Z
                // bisection children peel the deeper bodies per quadrant and
                // prune independently - open quads stop immediately.
                for (S32 k = 0; k < (S32)mBuild.mCursor; ++k)
                {
                    const CaptureNode base = mBuild.mWorklist[(size_t)k];
                    if (!base.mHung) continue;

                    const S32 w = base.mX1 - base.mX0;
                    const S32 hgt = base.mY1 - base.mY0;
                    const S32 quad_res = llmax(w / 2, 4);
                    for (S32 q = 0; q < 4; ++q)
                    {
                        CaptureNode quad;
                        quad.mX0 = base.mX0 + ((q & 1) ? w / 2 : 0);
                        quad.mX1 = base.mX0 + ((q & 1) ? w : w / 2);
                        quad.mY0 = base.mY0 + ((q & 2) ? hgt / 2 : 0);
                        quad.mY1 = base.mY0 + ((q & 2) ? hgt : hgt / 2);
                        quad.mRes = quad_res;
                        quad.mZ0 = base.mZ0;
                        quad.mZ1 = base.mZ1;
                        quad.mRefine = true;
                        mBuild.mWorklist.push_back(quad);
                    }
                }

                // Consume the captured base prefix; the refine nodes are all
                // that remains.
                mBuild.mWorklist.erase(
                    mBuild.mWorklist.begin(),
                    mBuild.mWorklist.begin() + mBuild.mCursor);
                mBuild.mCursor = 0;
                mBuild.mPhase = PHASE_REFINE;
                return true;
            }

            finalizeSpans(*tile, mBuild.mRectX0, mBuild.mRectY0, mBuild.mRectX1, mBuild.mRectY1);
            commitBuild(*tile);
            return false;
        }

        return true;
    }

    if (mBuild.mCursor >= mBuild.mWorklist.size())
    {
        // The worklist was exhausted before any node applied (an empty
        // enqueue): fold or finalize and commit so the tile reaches a valid
        // state.
        if (mBuild.mPhase == PHASE_BASE)
        {
            if (mBuild.mRectOnly)
            {
                foldSpans(*tile, mBuild.mRectX0, mBuild.mRectY0, mBuild.mRectX1, mBuild.mRectY1);
            }
            else
            {
                foldSpans(*tile, 0, 0, tile->mRes, tile->mRes);
            }
            commitBuild(*tile);
        }
        else
        {
            finalizeSpans(*tile, mBuild.mRectX0, mBuild.mRectY0, mBuild.mRectX1, mBuild.mRectY1);
            commitBuild(*tile);
        }
        return false;
    }

    if (!capturePass(*tile, mBuild.mWorklist[mBuild.mCursor], mBuild.mPass))
    {
        // GL trouble - abandon rather than spin. The tile keeps its previous
        // contents and stays dirty, so the next update tries again.
        mBuild.mActive = false;
        return false;
    }

    mBuild.mJustCaptured = true;
    return true;
}
// One band pass: an ortho depth render covering exactly the band. Pass 0
// looks down with standard culling and reads the highest up-facing surface
// (the body's top). Pass 1 looks UP from the band's floor with standard
// culling - the ceiling's underside is a front face for that camera - and
// flips the depth comparison so the buffer keeps the FARTHEST front-facing
// hit: the topmost body's underside, the boundary the air beneath it lives
// under. A plain nearest-hit shot from below would return the floor slab's
// underside instead and lose the room. The frustum's far plane sits at the
// band's top exactly and depth clamping is off for the upward pass - a
// surface outside the band must never record into it, or every band would
// steal surfaces from its neighbours and eat the air between them.
bool SSWorldField::capturePass(Tile& tile, const CaptureNode& node, S32 pass)
{
    LL_PROFILE_GPU_ZONE("atmo world field band");

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    const F32 band_top = node.mZ1;
    const F32 band_bottom = node.mZ0;
    const F32 range = node.mZ1 - node.mZ0;

    // The node's own XY frustum: full tile for base nodes, the quad's rect
    // at proportional resolution for refine nodes.
    const S32 res = node.mRes;
    const F32 half = 0.5f * (F32)res * tile.mCell;
    const F32 centre_x = regionp->getOriginAgent().mV[VX] + 0.5f * (F32)(node.mX0 + node.mX1) * tile.mCell;
    const F32 centre_y = regionp->getOriginAgent().mV[VY] + 0.5f * (F32)(node.mY0 + node.mY1) * tile.mCell;

    // Pass 0 anchors at the band's ceiling looking down; pass 1 at the band's
    // floor looking up.
    const F32 eye_z = (pass == 0) ? band_top : band_bottom;
    const F32 look = (pass == 0) ? -1.f : 1.f;

    const LLVector3 eye(centre_x, centre_y, eye_z);

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

    const glm::mat4 view = glm::lookAt(
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ]),
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ] + look),
        glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::ortho(-half, half, -half, half, 0.f, range);

    set_current_modelview(view);
    set_current_projection(proj);
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_SUN_SHADOW3;

    LLCamera cam = *LLViewerCamera::getInstance();
    cam.setOrigin(eye);
    cam.setFar(range);

    LLVector3 frust[8];
    frust[0] = eye + LLVector3(-half, -half, 0.f);
    frust[1] = eye + LLVector3(half, -half, 0.f);
    frust[2] = eye + LLVector3(half, half, 0.f);
    frust[3] = eye + LLVector3(-half, half, 0.f);
    for (U32 i = 0; i < 4; i++)
    {
        frust[i + 4] = frust[i] + LLVector3(0.f, 0.f, look * range);
    }
    cam.calcAgentFrustumPlanes(frust);
    cam.mFrustumCornerDist = 0.f;

    bool ok = true;
    if (mTarget.getWidth() != (U32)res)
    {
        mTarget.release();
        ok = mTarget.allocate(res, res, 0, true);
    }
    else
    {
        ok = true;
    }

    if (ok)
    {
        mTarget.bindTarget();
        mTarget.getViewport(gGLViewport);

        // The upward pass clears depth to zero and keeps the farthest
        // front-facing fragment, so its miss sentinel is 0 rather than 1.
        const bool upward = (pass == 1);
        if (upward) glClearDepth(0.f);
        mTarget.clear();
        if (upward) glClearDepth(1.f);

        {
            static LLCullResult cull_result;

            gPipeline.pushRenderTypeMask();
            gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_AVATAR,
                                          LLPipeline::RENDER_TYPE_CONTROL_AV,
                                          LLPipeline::END_RENDER_TYPES);
            gPipeline.renderShadow(view, proj, cam, cull_result, !upward,
                                   upward ? GL_GREATER : GL_LESS);
            gPipeline.popRenderTypeMask();
        }

        mTarget.flush();

        // <SS:Nexii> The band's depth lands via the shared SSGLReadback worker: the synchronous glReadPixels that used to block becomes a glGetTexImage on a dedicated GL thread, and applyBand() runs once both passes have landed (see mJustCaptured). The worker writes only its own buffer; mDone copies into the pass's slot on the main thread, so the Build never sees a partial read.
        mBuild.mDepth[pass].assign((size_t)res * res, upward ? 0.f : 1.f);
        mReadbackPending = true;
        const U32 tres = (U32)res;
        const S32 pass_copy = pass;

        SSGLReadback::Job job;
        job.mTexture = mTarget.getDepth();
        job.mTarget = GL_TEXTURE_2D;
        job.mWidth = tres;
        job.mHeight = tres;
        job.mFormat = GL_DEPTH_COMPONENT;
        job.mType = GL_FLOAT;
        job.mDone = [this, tres, pass_copy](const U8* data, size_t bytes)
        {
            mReadbackPending = false;
            if (mClearPending)
            {
                mClearPending = false;
                clear();
                return;
            }
            const size_t n = (size_t)tres * tres;
            if (bytes >= n * sizeof(F32) && mBuild.mDepth[pass_copy].size() >= n)
            {
                memcpy(mBuild.mDepth[pass_copy].data(), data, n * sizeof(F32));
            }
        };
        if (!SSGLReadback::getInstance()->submit(job))
        {
            // Could not even stage the read - GL trouble. Abandon rather than
            // leave mReadbackPending stuck and the build spinning.
            mReadbackPending = false;
            mBuild.mJustCaptured = false;
            ok = false;
        }
    }

    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLViewerCamera::sCurCameraID = saved_camera;

    return ok;
}

// Grow the tile's flat band arrays one doubling at a time; the new region is
// open sky until a capture splices over it. Everything that indexes the arrays
// does so below mBandCount, which only grows after the band it names was
// ensured here.
void SSWorldField::ensureBands(Tile& tile, S32 bands)
{
    if (bands <= tile.mAllocBands) return;

    const S32 next = llclamp(llmax(bands, tile.mAllocBands * 2), 1, SS_WF_MAX_SLOTS);
    const size_t layer = (size_t)tile.mRes * (size_t)tile.mRes;
    const size_t old_cells = (size_t)tile.mAllocBands * layer;
    const size_t new_cells = (size_t)next * layer;

    tile.mBandTop.resize(new_cells);
    std::fill(tile.mBandTop.begin() + old_cells, tile.mBandTop.end(), NO_SURFACE);
    tile.mBandUnder.resize(new_cells);
    std::fill(tile.mBandUnder.begin() + old_cells, tile.mBandUnder.end(), NO_SURFACE);
    tile.mBandFlags.resize(new_cells);
    std::fill(tile.mBandFlags.begin() + old_cells, tile.mBandFlags.end(), 0);
    tile.mAllocBands = next;
}

// Splices the captured node into the scratch: pass zero's depth is the highest
// up-facing surface (with the water and ground fallbacks), pass one's is the
// topmost body's underside. Full builds write every column; rect builds only
// the dirty rectangle's columns. Also flags the node as hanging when any
// column's body leaves at least the minimum interval of unexplored space
// beneath its underside - the signal Z bisection uses to enqueue children.
void SSWorldField::applyBand(Tile& tile, const CaptureNode& node)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp || mBuild.mDepth[0].empty() || mBuild.mDepth[1].empty()) return;

    const S32 band = node.mSlot;
    ensureBands(tile, band + 1);

    const F32 band_top = node.mZ1;
    const F32 band_bottom = node.mZ0;
    const F32 range = node.mZ1 - node.mZ0;
    const F32 hi = band_top - BOUNDARY_EPSILON;

    const S32 x0 = node.mX0;
    const S32 y0 = node.mY0;
    const S32 x1 = node.mX1;
    const S32 y1 = node.mY1;
    const S32 cap_res = node.mRes;
    const F32 half = 0.5f * (F32)node.mRes * tile.mCell;
    const F32 centre_x = regionp->getOriginAgent().mV[VX] + 0.5f * (F32)(node.mX0 + node.mX1) * tile.mCell;
    const F32 centre_y = regionp->getOriginAgent().mV[VY] + 0.5f * (F32)(node.mY0 + node.mY1) * tile.mCell;

    const F32 texel = (2.f * half) / (F32)cap_res;
    const F32 frust_min_x = centre_x - half;
    const F32 frust_min_y = centre_y - half;

    const F32 water_z = regionp->getWaterHeight();
    const bool sky = SSAtmoMagic::getInstance()->isSkyTrack();
    const F32 sky_floor = SSAtmoMagic::getInstance()->groundZero();

    const S32 stride = tile.mRes;
    F32* top_z = &tile.mBandTop[(size_t)band * (size_t)tile.mRes * (size_t)tile.mRes];
    F32* under_z = &tile.mBandUnder[(size_t)band * (size_t)tile.mRes * (size_t)tile.mRes];
    U8* top_flags = &tile.mBandFlags[(size_t)band * tile.mRes * tile.mRes];

    U32 hits = 0;

    mBuild.mNodeHung = false;

    for (S32 cy = y0; cy < y1; ++cy)
    {
        for (S32 cx = x0; cx < x1; ++cx)
        {
            const F32 wx = regionp->getOriginAgent().mV[VX] + ((F32)cx + 0.5f) * tile.mCell;
            const F32 wy = regionp->getOriginAgent().mV[VY] + ((F32)cy + 0.5f) * tile.mCell;

            const size_t idx = (size_t)cy * stride + cx;

            F32 z = NO_SURFACE;
            F32 under = NO_SURFACE;
            U8 flags = 0;

            const F32 u = (wx - frust_min_x) / (2.f * half);
            const F32 v = (wy - frust_min_y) / (2.f * half);
            if (u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
            {
                const S32 tx = llmin((S32)(u * (F32)cap_res), cap_res - 1);
                const S32 ty = llmin((S32)(v * (F32)cap_res), cap_res - 1);

                // Pass zero: the highest up-facing surface. Clamped into the
                // band at its top (structure reaching the band's ceiling
                // belongs here and to the band above).
                const F32 d0 = mBuild.mDepth[0][(size_t)ty * cap_res + tx];
                if (d0 < DEPTH_MISS)
                {
                    z = band_top - d0 * range;
                    if (z > hi) z = hi;
                    flags = SSRainShadowMap::SURF_MAPPED;
                }

                // Pass one: the topmost body's underside - the farthest
                // front-facing hit from below, so its miss sentinel is zero
                // and its distance maps up from the band's floor. No
                // fallbacks - a miss means no body hangs over this column
                // inside the band.
                const F32 d1 = mBuild.mDepth[1][(size_t)ty * cap_res + tx];
                if (d1 > DEPTH_MISS_UP)
                {
                    under = band_bottom + d1 * range;
                    if (under > hi) under = hi;
                }
            }

            if (z > NO_SURFACE + 1.f)
            {
                // Water is the one surface the depth pass does not draw, so a
                // hit under the waterline is the seabed and the cell belongs
                // to the water above it - the rain shadow rule, at every band.
                if (!sky && z < water_z)
                {
                    z = water_z;
                    flags = SSRainShadowMap::SURF_MAPPED | SSRainShadowMap::SURF_WATER;
                }
            }
            else if (node.mZ0 <= 0.01f)
            {
                // The ground band falls back to the heightmap, exactly as the
                // rain shadow capture does for what it missed. Higher bands
                // leave unmapped cells open instead.
                if (sky)
                {
                    z = sky_floor;
                    flags = 0;
                }
                else
                {
                    const LLVector3 probe(wx, wy, water_z);
                    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(probe);
                    z = llmax(land, water_z);
                    flags = SSRainShadowMap::SURF_FALLBACK
                            | ((water_z > land) ? SSRainShadowMap::SURF_WATER : 0);
                }
            }
            else
            {
                z = NO_SURFACE;
                flags = 0;
            }

            // The Z-bisection signal: a body hanging at least the minimum
            // interval above this node's floor leaves unexplored space beneath
            // it - one probe settles it, the rest of the loop rides on the
            // flag.
            if (!mBuild.mNodeHung && under > NO_SURFACE + 1.f && under > band_bottom + SS_WF_MIN_BAND_M)
            {
                mBuild.mNodeHung = true;
            }

            // Splice, and notice when the column actually changed so a
            // no-op edit does not bump the geometry serial.
            if (fabsf(top_z[idx] - z) > 0.01f || top_flags[idx] != flags
                || fabsf(under_z[idx] - under) > 0.01f)
            {
                mBuild.mChanged = true;
            }
            top_z[idx] = z;
            under_z[idx] = under;
            top_flags[idx] = flags;

            // A real capture hit - mapped surface or captured underside -
            // marks the column resolved up to this band. Fallbacks and misses
            // never count: the fold may only trust bands the capture saw.
            // Base slots only: bisection slot indices do not map to altitudes,
            // and letting them move the seen ceiling would corrupt the rect
            // re-peel's carry-forward of spans above the swept range.
            if ((flags & SSRainShadowMap::SURF_MAPPED) || under > NO_SURFACE + 1.f)
            {
                if (band < tile.mBaseBands)
                {
                    S32& seen = mBuild.mSeenBand[idx];
                    if (band > seen) seen = band;
                }
            }

            if (z > NO_SURFACE + 1.f || under > NO_SURFACE + 1.f) ++hits;
        }
    }

    // Bands with content extend the tile's live band stack; a run of
    // genuinely empty base bands ends the base sweep early. Bisection nodes
    // never feed the empty-run: they are targeted probes, and their misses
    // are the recursion terminating, not sky.
    if (hits > 0)
    {
        if (band + 1 > tile.mBandCount) tile.mBandCount = band + 1;
        if (!node.mBisect) mBuild.mEmptyRun = 0;
    }
    else if (!node.mBisect)
    {
        ++mBuild.mEmptyRun;
    }
}

// <SS:Nexii> Inserts one solid body into a column's span list: sorted
// position, union with touching or overlapping neighbours, and over the span
// budget a collapse of the thinnest air gap (the two spans around it merge -
// the gap becomes solid). The list stays sorted and every gap in it at least
// the slab threshold tall. A member, not a file static, because it reshapes
// the private Tile's store.
void SSWorldField::spanInsert(Tile& tile, size_t col, F32 bot, F32 tp, U8 fl)
{
    const size_t layer = (size_t)tile.mRes * tile.mRes;

    S32 n = 0;
    while (n < SS_WF_MAX_SPANS && tile.mSpanTop[(size_t)n * layer + col] > NO_SURFACE * 0.5f) ++n;

    S32 at = n;
    while (at > 0 && tile.mSpanBottom[(size_t)(at - 1) * layer + col] > bot) --at;

    if (at > 0 && bot - tile.mSpanTop[(size_t)(at - 1) * layer + col] < SS_WF_SPAN_SLAB_M)
    {
        const size_t pi = (size_t)(at - 1) * layer + col;
        const bool higher = tp > tile.mSpanTop[pi];
        tile.mSpanBottom[pi] = llmin(tile.mSpanBottom[pi], bot);
        tile.mSpanTop[pi] = llmax(tile.mSpanTop[pi], tp);
        if (higher) tile.mSpanFlags[pi] = fl;
        return;
    }
    if (at < n && tp > tile.mSpanBottom[(size_t)at * layer + col] - SS_WF_SPAN_SLAB_M)
    {
        const size_t ni = (size_t)at * layer + col;
        const bool higher = bot < tile.mSpanBottom[ni];
        tile.mSpanBottom[ni] = llmin(tile.mSpanBottom[ni], bot);
        tile.mSpanTop[ni] = llmax(tile.mSpanTop[ni], tp);
        if (!higher) tile.mSpanFlags[ni] = fl;
        return;
    }
    if (n == SS_WF_MAX_SPANS)
    {
        S32 thinnest = 0;
        F32 best = FLT_MAX;
        for (S32 j = 0; j + 1 < n; ++j)
        {
            const F32 gap = tile.mSpanBottom[(size_t)(j + 1) * layer + col]
                          - tile.mSpanTop[(size_t)j * layer + col];
            if (gap < best) { best = gap; thinnest = j; }
        }
        tile.mSpanTop[(size_t)thinnest * layer + col] =
            tile.mSpanTop[(size_t)(thinnest + 1) * layer + col];
        for (S32 j = thinnest + 1; j + 1 < n; ++j)
        {
            tile.mSpanBottom[(size_t)j * layer + col] = tile.mSpanBottom[(size_t)(j + 1) * layer + col];
            tile.mSpanTop[(size_t)j * layer + col] = tile.mSpanTop[(size_t)(j + 1) * layer + col];
            tile.mSpanFlags[(size_t)j * layer + col] = tile.mSpanFlags[(size_t)(j + 1) * layer + col];
        }
        --n;
        at = n;
        while (at > 0 && tile.mSpanBottom[(size_t)(at - 1) * layer + col] > bot) --at;
    }

    for (S32 j = n; j > at; --j)
    {
        tile.mSpanBottom[(size_t)j * layer + col] = tile.mSpanBottom[(size_t)(j - 1) * layer + col];
        tile.mSpanTop[(size_t)j * layer + col] = tile.mSpanTop[(size_t)(j - 1) * layer + col];
        tile.mSpanFlags[(size_t)j * layer + col] = tile.mSpanFlags[(size_t)(j - 1) * layer + col];
    }
    tile.mSpanBottom[(size_t)at * layer + col] = bot;
    tile.mSpanTop[(size_t)at * layer + col] = tp;
    tile.mSpanFlags[(size_t)at * layer + col] = fl;
}

// <SS:Nexii> Applies a quad node: its two passes' per-column bodies insert
// straight into the span store - the topmost body of the quad's interval per
// column, at the quad's own capture resolution. A top without an underside
// reaches the quad's floor; an underside without a top crosses the quad's
// ceiling; neither is air. The node's hang flag (set when a body leaves at
// least the minimum interval of unexplored space beneath it) drives its
// Z-bisection children.
void SSWorldField::applyRefine(Tile& tile, const CaptureNode& node)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp || mBuild.mDepth[0].empty() || mBuild.mDepth[1].empty()) return;

    const F32 z0 = node.mZ0;
    const F32 z1 = node.mZ1;
    const F32 range = z1 - z0;

    const S32 x0 = node.mX0;
    const S32 y0 = node.mY0;
    const S32 x1 = node.mX1;
    const S32 y1 = node.mY1;
    const S32 cap_res = node.mRes;
    const F32 half = 0.5f * (F32)node.mRes * tile.mCell;
    const F32 centre_x = regionp->getOriginAgent().mV[VX] + 0.5f * (F32)(node.mX0 + node.mX1) * tile.mCell;
    const F32 centre_y = regionp->getOriginAgent().mV[VY] + 0.5f * (F32)(node.mY0 + node.mY1) * tile.mCell;

    const F32 frust_min_x = centre_x - half;
    const F32 frust_min_y = centre_y - half;

    const F32 water_z = regionp->getWaterHeight();
    const bool sky = SSAtmoMagic::getInstance()->isSkyTrack();

    mBuild.mNodeHung = false;

    for (S32 cy = y0; cy < y1; ++cy)
    {
        for (S32 cx = x0; cx < x1; ++cx)
        {
            const size_t col = (size_t)cy * tile.mRes + cx;

            const F32 wx = regionp->getOriginAgent().mV[VX] + ((F32)cx + 0.5f) * tile.mCell;
            const F32 wy = regionp->getOriginAgent().mV[VY] + ((F32)cy + 0.5f) * tile.mCell;

            const F32 u_norm = (wx - frust_min_x) / (2.f * half);
            const F32 v_norm = (wy - frust_min_y) / (2.f * half);
            if (u_norm < 0.f || u_norm >= 1.f || v_norm < 0.f || v_norm >= 1.f) continue;

            const S32 tx = llmin((S32)(u_norm * (F32)cap_res), cap_res - 1);
            const S32 ty = llmin((S32)(v_norm * (F32)cap_res), cap_res - 1);

            // Pass zero: the highest up-facing surface in the quad's
            // interval. Clamped at the interval's ceiling (structure
            // reaching it belongs here and to the capture above).
            F32 t = NO_SURFACE;
            const F32 d0 = mBuild.mDepth[0][(size_t)ty * cap_res + tx];
            if (d0 < DEPTH_MISS)
            {
                t = z1 - d0 * range;
                if (t > z1 - BOUNDARY_EPSILON) t = z1 - BOUNDARY_EPSILON;
            }

            // Pass one: the topmost body's underside - the farthest
            // front-facing hit from below.
            F32 u = NO_SURFACE;
            const F32 d1 = mBuild.mDepth[1][(size_t)ty * cap_res + tx];
            if (d1 > DEPTH_MISS_UP)
            {
                u = z0 + d1 * range;
                if (u > z1 - BOUNDARY_EPSILON) u = z1 - BOUNDARY_EPSILON;
            }

            // Decode the body and insert it. Both hits bound it, a top
            // without an underside reaches the quad's floor, an underside
            // without a top crosses the quad's ceiling. A mapped top under
            // the waterline is the seabed and belongs to the water above it
            // - the rain shadow rule.
            const bool hasT = t > NO_SURFACE + 1.f;
            const bool hasU = u > NO_SURFACE + 1.f;
            if (!hasT && !hasU) continue;

            const F32 bot = hasU ? u : z0;
            F32 tp = hasT ? t : z1;
            if (hasT && !sky && tp < water_z)
            {
                tp = water_z;
            }
            spanInsert(tile, col, bot, tp, SSRainShadowMap::SURF_MAPPED);

            // The hang: unexplored space between the quad's floor and the
            // body's underside.
            if (hasU && u > z0 + SS_WF_MIN_BAND_M)
            {
                mBuild.mNodeHung = true;
            }
        }
    }
}

// <SS:Nexii> Folds the capture scratch into the column span store: per
// column, the per-band bodies ([underside, top], or the band's own floor and
// ceiling where a body crosses it) sort bottom-up, merge across band planes
// and any gap thinner than the assumed slab, and the first span extends to
// the world floor when the gap beneath it is thinner still. The result is the
// store the proposal described: a column is a short list of [bottom, top]
// solid spans, air between them, every stored gap at least the slab threshold
// tall - a wall standing on unmeasured ground stays solid to the floor, and a
// room is one air interval whatever band its floor and ceiling landed in.
void SSWorldField::foldSpans(Tile& tile, S32 x0, S32 y0, S32 x1, S32 y1)
{
    const S32 res = tile.mRes;
    const size_t layer = (size_t)res * res;

    x0 = llmax(x0, 0); y0 = llmax(y0, 0);
    x1 = llmin(x1, res); y1 = llmin(y1, res);

    for (S32 y = y0; y < y1; ++y)
    {
        for (S32 x = x0; x < x1; ++x)
        {
            const size_t col = (size_t)y * res + x;

            F32 bottoms[SS_WF_MAX_SPANS];
            F32 tops[SS_WF_MAX_SPANS];
            U8 flags[SS_WF_MAX_SPANS];
            S32 n = 0;

            // The scratch release at commit leaves mBandCount standing as the
            // tile's persistent band statistic while the arrays it names are
            // gone; a build re-grows only the bands it re-splices. Fold what
            // this build actually has, not what a previous capture saw.
            const S32 band_limit = llmin(tile.mBandCount, tile.mAllocBands);
            for (S32 b = 0; b < band_limit; ++b)
            {
                const size_t bi = (size_t)b * layer + col;
                const F32 top = tile.mBandTop[bi];
                const F32 under = tile.mBandUnder[bi];
                const bool hasT = top > NO_SURFACE * 0.5f;
                const bool hasU = under > NO_SURFACE * 0.5f;
                if (!hasT && !hasU) continue;

                const F32 b0 = (F32)b * tile.mBandHeight;
                const F32 b1 = b0 + tile.mBandHeight;
                const F32 bot = hasU ? under : b0;
                const F32 tp = hasT ? top : b1;
                const U8 fl = hasT ? tile.mBandFlags[bi] : 0;

                // Z bisection's children occupy scratch slots beyond the
                // base sweep and land out of altitude order, so each body
                // inserts at its sorted position rather than appending.
                // Insertion unions it with either neighbour when they touch
                // or overlap - the parent/child duplicate, the band-plane
                // continuation - and over the span budget collapses the
                // thinnest air gap (the two spans around it merge into one;
                // the gap becomes solid) to free a slot.
                S32 at = n;
                while (at > 0 && bottoms[at - 1] > bot) --at;

                if (at > 0 && bot - tops[at - 1] < SS_WF_SPAN_SLAB_M)
                {
                    bottoms[at - 1] = llmin(bottoms[at - 1], bot);
                    tops[at - 1] = llmax(tops[at - 1], tp);
                    continue;
                }
                if (at < n && tp > bottoms[at] - SS_WF_SPAN_SLAB_M)
                {
                    bottoms[at] = llmin(bottoms[at], bot);
                    tops[at] = llmax(tops[at], tp);
                    continue;
                }
                if (n == SS_WF_MAX_SPANS)
                {
                    S32 thinnest = 0;
                    F32 best = FLT_MAX;
                    for (S32 j = 0; j + 1 < n; ++j)
                    {
                        const F32 gap = bottoms[j + 1] - tops[j];
                        if (gap < best) { best = gap; thinnest = j; }
                    }
                    tops[thinnest] = tops[thinnest + 1];
                    flags[thinnest] = flags[thinnest + 1];
                    for (S32 j = thinnest + 1; j + 1 < n; ++j)
                    {
                        bottoms[j] = bottoms[j + 1];
                        tops[j] = tops[j + 1];
                        flags[j] = flags[j + 1];
                    }
                    --n;
                    at = n;
                    while (at > 0 && bottoms[at - 1] > bot) --at;
                }

                for (S32 j = n; j > at; --j)
                {
                    bottoms[j] = bottoms[j - 1];
                    tops[j] = tops[j - 1];
                    flags[j] = flags[j - 1];
                }
                bottoms[at] = bot;
                tops[at] = tp;
                flags[at] = fl;
                ++n;
            }

            // Heal residuals: an insertion unions a body with its immediate
            // neighbours, but the union can grow a span into the next one's
            // slab zone. One pass; the spans stay sorted, and the span with
            // the higher top keeps its flags (its face is the landing
            // surface).
            for (S32 k = 0; k + 1 < n; )
            {
                if (bottoms[k + 1] - tops[k] < SS_WF_SPAN_SLAB_M)
                {
                    const bool higher = tops[k + 1] > tops[k];
                    flags[k] = higher ? flags[k + 1] : flags[k];
                    tops[k] = llmax(tops[k], tops[k + 1]);
                    bottoms[k] = llmin(bottoms[k], bottoms[k + 1]);
                    for (S32 j = k + 1; j + 1 < n; ++j)
                    {
                        bottoms[j] = bottoms[j + 1];
                        tops[j] = tops[j + 1];
                        flags[j] = flags[j + 1];
                    }
                    --n;
                }
                else
                {
                    ++k;
                }
            }

            // Rect re-peels carry forward what the capture could not re-see:
            // old spans resting entirely above the highest band this build
            // actually hit are unrebutted (their altitude was never resolved -
            // churned-out geometry, LOD cull), so they ride along instead of
            // being folded away from empty scratch. Spans whose base sits
            // below the seen ceiling were swept through and drop.
            if (mBuild.mRectOnly && col < mBuild.mSeenBand.size())
            {
                const S32 seen = mBuild.mSeenBand[col];
                const F32 seen_top = (F32)(seen + 1) * tile.mBandHeight;
                for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
                {
                    const size_t si = (size_t)k * layer + col;
                    const F32 old_top = tile.mSpanTop[si];
                    if (old_top <= NO_SURFACE * 0.5f) break;
                    const F32 old_bot = tile.mSpanBottom[si];
                    if (old_bot < seen_top - 0.01f) continue;

                    if (n > 0 && old_bot - tops[n - 1] < SS_WF_SPAN_SLAB_M)
                    {
                        tops[n - 1] = old_top;  // continues the carried stack
                        continue;
                    }

                    if (n == SS_WF_MAX_SPANS)
                    {
                        // Over budget: collapse the thinnest air gap, the
                        // same rule the band scan folds by.
                        S32 thinnest = 0;
                        F32 best = FLT_MAX;
                        for (S32 j = 0; j + 1 < n; ++j)
                        {
                            const F32 gap = bottoms[j + 1] - tops[j];
                            if (gap < best) { best = gap; thinnest = j; }
                        }
                        tops[thinnest] = tops[thinnest + 1];
                        flags[thinnest] = flags[thinnest + 1];
                        for (S32 j = thinnest + 1; j + 1 < n; ++j)
                        {
                            bottoms[j] = bottoms[j + 1];
                            tops[j] = tops[j + 1];
                            flags[j] = flags[j + 1];
                        }
                        --n;
                    }

                    bottoms[n] = old_bot;
                    tops[n] = old_top;
                    flags[n] = tile.mSpanFlags[si];
                    ++n;
                }
            }

            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                if (k < n)
                {
                    tile.mSpanBottom[si] = bottoms[k];
                    tile.mSpanTop[si] = tops[k];
                    tile.mSpanFlags[si] = flags[k];
                }
                else
                {
                    tile.mSpanBottom[si] = NO_SURFACE;
                    tile.mSpanTop[si] = NO_SURFACE;
                    tile.mSpanFlags[si] = 0;
                }
            }
        }
    }
}

// <SS:Nexii> Finalizes the column span store over a rectangle: merges gaps
// the refine phase's insertions left thinner than the slab threshold, and
// extends a first span whose below-gap is thinner to the world floor - a
// wall standing on unmeasured ground stays solid to it. Runs at commit, after
// every capture node has had its say.
void SSWorldField::finalizeSpans(Tile& tile, S32 x0, S32 y0, S32 x1, S32 y1)
{
    const S32 res = tile.mRes;
    const size_t layer = (size_t)res * res;

    x0 = llmax(x0, 0); y0 = llmax(y0, 0);
    x1 = llmin(x1, res); y1 = llmin(y1, res);

    for (S32 y = y0; y < y1; ++y)
    {
        for (S32 x = x0; x < x1; ++x)
        {
            const size_t col = (size_t)y * res + x;

            F32 bottoms[SS_WF_MAX_SPANS];
            F32 tops[SS_WF_MAX_SPANS];
            U8 flags[SS_WF_MAX_SPANS];
            S32 n = 0;

            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                const F32 top = tile.mSpanTop[si];
                if (top <= NO_SURFACE * 0.5f) break;
                bottoms[n] = tile.mSpanBottom[si];
                tops[n] = top;
                flags[n] = tile.mSpanFlags[si];
                ++n;
            }

            // The gap beneath the first span is air only when it is at least
            // a slab tall.
            if (n > 0 && bottoms[0] < SS_WF_SPAN_SLAB_M) bottoms[0] = 0.f;

            // The refine phase's insertions union with their immediate
            // neighbours, but a union can grow a span into the next one's
            // slab zone. One pass; the spans stay sorted, and the span with
            // the higher top keeps its flags (its face is the landing
            // surface).
            for (S32 k = 0; k + 1 < n; )
            {
                if (bottoms[k + 1] - tops[k] < SS_WF_SPAN_SLAB_M)
                {
                    const bool higher = tops[k + 1] > tops[k];
                    flags[k] = higher ? flags[k + 1] : flags[k];
                    tops[k] = llmax(tops[k], tops[k + 1]);
                    bottoms[k] = llmin(bottoms[k], bottoms[k + 1]);
                    for (S32 j = k + 1; j + 1 < n; ++j)
                    {
                        bottoms[j] = bottoms[j + 1];
                        tops[j] = tops[j + 1];
                        flags[j] = flags[j + 1];
                    }
                    --n;
                }
                else
                {
                    ++k;
                }
            }

            for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                if (k < n)
                {
                    tile.mSpanBottom[si] = bottoms[k];
                    tile.mSpanTop[si] = tops[k];
                    tile.mSpanFlags[si] = flags[k];
                }
                else
                {
                    tile.mSpanBottom[si] = NO_SURFACE;
                    tile.mSpanTop[si] = NO_SURFACE;
                    tile.mSpanFlags[si] = 0;
                }
            }
        }
    }
}

void SSWorldField::commitBuild(Tile& tile)
{
    if (mBuild.mChanged)
    {
        if (tile.mGeomSerial == 0xFFFFFFFFu)
        {
            tile.mGeomSerial = 1;
        }
        else
        {
            ++tile.mGeomSerial;
        }
    }

    if (!mBuild.mRectOnly)
    {
        ++mCaptureCount;
        tile.mValid = true;
    }
    else
    {
        ++mDirtyCaptures;
    }

    // The capture scratch folds into the column span store here - full builds
    // convert every column, rect re-peels only the rectangle the build
    // actually captured - so the store is current whenever the geometry serial
    // moves, and the flood reads spans, never bands.
    if (mBuild.mRectOnly)
    {
        // Fold the captured rect, never the live dirty rect: marks merged
        // mid-build name columns this build never spliced, and folding them
        // would read the regrown (all NO_SURFACE) scratch and erase spans
        // across everything that streamed in while the build ran.
        finalizeSpans(tile, mBuild.mRectX0, mBuild.mRectY0, mBuild.mRectX1, mBuild.mRectY1);

        // Consume the dirty flag only when nothing merged mid-build; the
        // grown rect stays dirty and re-peels on the next build.
        if (tile.mDirty
            && tile.mDirtyX0 == mBuild.mRectX0 && tile.mDirtyY0 == mBuild.mRectY0
            && tile.mDirtyX1 == mBuild.mRectX1 && tile.mDirtyY1 == mBuild.mRectY1)
        {
            tile.mDirtyX0 = tile.mDirtyY0 = 0;
            tile.mDirtyX1 = tile.mDirtyY1 = 0;
            tile.mDirty = false;
        }
    }
    else
    {
        finalizeSpans(tile, 0, 0, tile.mRes, tile.mRes);
    }
    // The band scope resets only with no marks pending: marks that merged
    // mid-build raised it to their own altitude, and the follow-up re-peel
    // must still sweep past them.
    if (!tile.mDirty) tile.mBandTarget = 0;
    tile.mCaptureTime = mNow;
    tile.mLastTouched = mNow;
    mBuild.mPass = 0;
    mBuild.mActive = false;

    // <SS:Nexii> The capture scratch folds into the span store above; release it.
    // At the default 0.25m cell a 1024-res tile holds ~9MB per band of band arrays
    // (~226MB at the 24-band cap) that nothing but the NEXT build reads - keeping
    // them across commits held up to ~1.2GB live at MAX_TILES and was starving the
    // heap (crash inside malloc from the cloud deck's next allocation). The
    // rect re-peel's change detection reads NO_SURFACE after this, so a no-op edit
    // now costs one extra async flood instead of ~226MB per tile of dead weight.
    // ensureBands regrows lazily on the next build's first applyBand.
    tile.mBandTop.clear();
    tile.mBandTop.shrink_to_fit();
    tile.mBandUnder.clear();
    tile.mBandUnder.shrink_to_fit();
    tile.mBandFlags.clear();
    tile.mBandFlags.shrink_to_fit();
    tile.mAllocBands = 0;

    // The connectivity labels follow every commit, not only the ones that changed
    // something: they also serve their first fill, and a commit that changed
    // nothing left mAirSerial already matching, so scheduleFlood's serial check
    // makes the walk a no-op store.
    if (tile.mAirSerial != tile.mGeomSerial)
    {
        scheduleFlood(tile);
    }
}

// Resolves the landing-surface grid for a region - SSRainShadowMap's exact
// contract, sourced from the band stack, not a private capture. The first
// thing a falling drop meets is the column's highest surface, so bands are
// scanned top-down and the first hit wins.
bool SSWorldField::buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return false;

    const Tile& tile = it->second;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    n = llclamp(n, 16, 512);
    const F32 width = regionp->getWidth();

    out.mRegionHandle = region_handle;
    out.mN = n;
    out.mCell = width / (F32)n;
    out.mGeomSerial = tile.mGeomSerial;
    out.mZ.assign((size_t)n * n, -FLT_MAX);
    out.mFlags.assign((size_t)n * n, 0);
    out.mAbove.assign((size_t)n * n, 0.f);

    const F32 water_z = regionp->getWaterHeight();
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    for (S32 gy = 0; gy < n; ++gy)
    {
        for (S32 gx = 0; gx < n; ++gx)
        {
            const S32 cx = llclamp((S32)(((F32)gx + 0.5f) * (F32)tile.mRes / (F32)n), 0, tile.mRes - 1);
            const S32 cy = llclamp((S32)(((F32)gy + 0.5f) * (F32)tile.mRes / (F32)n), 0, tile.mRes - 1);

            const size_t col = (size_t)cy * tile.mRes + cx;
            const size_t oidx = (size_t)gy * n + gx;

            F32 z = -FLT_MAX;
            U8 flags = 0;

            // The column's landing surface: the highest solid span's top.
            for (S32 k = SS_WF_MAX_SPANS - 1; k >= 0; --k)
            {
                const size_t si = (size_t)k * (size_t)tile.mRes * (size_t)tile.mRes + col;
                if (tile.mSpanTop[si] > -FLT_MAX * 0.5f)
                {
                    z = tile.mSpanTop[si];
                    flags = tile.mSpanFlags[si];
                    break;
                }
            }

            if (z > -FLT_MAX * 0.5f)
            {
                if (!sky && z < water_z)
                {
                    out.mZ[oidx] = water_z;
                    out.mFlags[oidx] = SSRainShadowMap::SURF_MAPPED | SSRainShadowMap::SURF_WATER;
                }
                else
                {
                    out.mZ[oidx] = z;
                    out.mFlags[oidx] = flags | SSRainShadowMap::SURF_MAPPED;

// Same figure the rain shadow builder derives: metres over the
                    // terrain-or-water reference (the sky track floor in a skybox),
                    // so both sources hand consumers identical ground-vs-structure
                    // data and the SSWorldFieldSurfaceTop switch stays behaviour-
                    // neutral.
                    F32 ground;
                    if (sky)
                    {
                        ground = sky_floor;
                    }
                    else
                    {
                        const LLVector3 centre(regionp->getOriginAgent().mV[VX] + out.axis(gx),
                                               regionp->getOriginAgent().mV[VY] + out.axis(gy),
                                               water_z);
                        ground = llmax(LLWorld::getInstance()->resolveLandHeightAgent(centre), water_z);
                    }
                    out.mAbove[oidx] = llmax(z - ground, 0.f);
                }
            }
            else if (sky)
            {
                out.mZ[oidx] = sky_floor;
                out.mFlags[oidx] = 0;
            }
            else
            {
                const LLVector3 centre(regionp->getOriginAgent().mV[VX] + out.axis(gx),
                                       regionp->getOriginAgent().mV[VY] + out.axis(gy),
                                       water_z);
                const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(centre);
                out.mZ[oidx] = llmax(land, water_z);
                out.mFlags[oidx] = SSRainShadowMap::SURF_FALLBACK | ((water_z > land) ? SSRainShadowMap::SURF_WATER : 0);
            }
        }
    }

    return true;
}

void SSWorldField::validTiles(std::vector<std::pair<U64, U32> >& out) const
{
    out.clear();
    out.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid)
        {
            out.emplace_back(entry.first, entry.second.mGeomSerial);
        }
    }
}

bool SSWorldField::surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);

    const size_t col = (size_t)cy * tile->mRes + cx;
    for (S32 k = SS_WF_MAX_SPANS - 1; k >= 0; --k)
    {
        const size_t si = (size_t)k * (size_t)tile->mRes * (size_t)tile->mRes + col;
        if (tile->mSpanTop[si] > -FLT_MAX * 0.5f)
        {
            z = tile->mSpanTop[si];
            flags = tile->mSpanFlags[si];
            return true;
        }
    }
    return false;
}

bool SSWorldField::coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const
{
    outdoor = true;
    buried_depth = 0.f;

    F32 top = 0.f;
    U8 flags = 0;
    if (!surfaceTop(pos_agent, top, flags)) return false;

    // Standing on the top surface counts as outdoors; anything below it is
    // under the column's sky-open top by however much.
    if (top < pos_agent.mV[VZ] - 0.01f)
    {
        return true;
    }

    outdoor = false;
    buried_depth = llmax(0.f, top - pos_agent.mV[VZ]);
    return true;
}

bool SSWorldField::coverageDetail(const LLVector3& pos_agent, bool& covered,
                                  F32& ceiling_z, F32& column_top_z) const
{
    covered = false;
    ceiling_z = 0.f;
    column_top_z = 0.f;

    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    // The half metre of grace keeps the surface being stood on from reading
    // as its own ceiling - a capture texel of the floor under the camera can
    // land a hair above the camera's own feet.
    const F32 over = pos_agent.mV[VZ] + 0.5f;

    // The column's spans, lowest first: the first span whose top clears the
    // camera is the ceiling of the space the camera stands in, and the
    // highest span's top is the column top.
    const size_t layer = (size_t)tile->mRes * tile->mRes;
    bool any = false;
    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
    {
        const size_t si = (size_t)k * layer + col;
        const F32 top = tile->mSpanTop[si];
        if (top <= -FLT_MAX * 0.5f) break;

        any = true;
        column_top_z = llmax(column_top_z, top);
        if (top > over && (!covered || top < ceiling_z))
        {
            covered = true;
            ceiling_z = top;
        }
    }

    return any;
}

// <SS:Nexii> Which air gap of a column contains z: the gaps are the intervals
// between, beneath and above the column's solid spans, every stored one at
// least the slab threshold tall. Returns the gap index (0 below the lowest
// span, n above the highest), or -1 when z falls inside a body - the store
// has no verdict for the inside of solid things.
S32 SSWorldField::gapAt(const Tile& tile, size_t col, F32 z, F32& g0, F32& g1) const
{
    const size_t layer = (size_t)tile.mRes * tile.mRes;
    // The capture ceiling, not the scratch slot high-water: bisected slots
    // inflate mBandCount, but the top gap must end where the capture ends.
    const F32 ceiling = (F32)bandCount() * tile.mBandHeight;

    F32 prev = 0.f;
    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
    {
        const size_t si = (size_t)k * layer + col;
        const F32 stop = tile.mSpanTop[si];
        if (stop <= NO_SURFACE * 0.5f)
        {
            g0 = prev; g1 = ceiling;
            return k;                                   // open above the last span
        }
        const F32 bottom = tile.mSpanBottom[si];
        if (z < bottom - 0.01f)
        {
            g0 = prev; g1 = bottom;
            return k;                                   // the gap beneath this span
        }
        if (z <= stop + 0.01f) return -1;               // inside the body
        prev = stop;
    }
    g0 = prev; g1 = ceiling;
    return SS_WF_MAX_SPANS;                             // above the last span
}

// <SS:Nexii> Air connectivity lookup: the air gap of the column that contains
// the point, read from the labels the flood stored, or AIR_UNKNOWN when
// nothing is current - after an edit, before the first flood, or off-tile. A
// point inside a body reads AIR_SOLID.
U8 SSWorldField::airLabelAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return AIR_UNKNOWN;
    if (tile->mGapLabel.empty() || tile->mAirSerial != tile->mGeomSerial) return AIR_UNKNOWN;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_UNKNOWN;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    F32 g0, g1;
    const S32 gap = gapAt(*tile, col, pos_agent.mV[VZ], g0, g1);
    if (gap < 0) return AIR_SOLID;
    const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)gap;
    return (gi < tile->mGapLabel.size()) ? tile->mGapLabel[gi] : (U8)AIR_UNKNOWN;
}

// Occlusion depth behind airLabelAt: the flood's distance walk per air gap,
// gated by the same serial check so a stale walk is never served.
U32 SSWorldField::airDepthAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return AIR_DEPTH_UNREACHED;
    if (tile->mGapDepth.empty() || tile->mAirSerial != tile->mGeomSerial) return AIR_DEPTH_UNREACHED;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_DEPTH_UNREACHED;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    F32 g0, g1;
    const S32 gap = gapAt(*tile, col, pos_agent.mV[VZ], g0, g1);
    if (gap < 0) return AIR_DEPTH_UNREACHED;
    const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)gap;
    return (gi < tile->mGapDepth.size()) ? (U32)tile->mGapDepth[gi] : AIR_DEPTH_UNREACHED;
}

// <SS:Nexii> The enclosure spectrum at a point: the air gap containing the
// point answers with its own label and occlusion depth. A point inside a body
// has no verdict (-1) - the caller keeps its own probe answer for that.
F32 SSWorldField::enclosureAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return -1.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return -1.f;

    return enclosureInRegion(regionp, *tile, pos_agent);
}

// The bulk form for callers that walk one region's cells (the surface field's
// window stitch): same answer, region and tile resolved once instead of per
// point.
F32 SSWorldField::enclosureAtRegion(U64 region_handle, const LLVector3& pos_agent) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return -1.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return -1.f;

    return enclosureInRegion(regionp, it->second, pos_agent);
}

F32 SSWorldField::enclosureInRegion(const LLViewerRegion* regionp, const Tile& tile,
                                    const LLVector3& pos_agent) const
{
    if (tile.mGapLabel.empty() || tile.mAirSerial != tile.mGeomSerial) return -1.f;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile.mCell), 0, tile.mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile.mCell), 0, tile.mRes - 1);
    const size_t layer = (size_t)tile.mRes * tile.mRes;
    const size_t col = (size_t)cy * tile.mRes + cx;
    if ((size_t)(SS_WF_MAX_SPANS + 1) * layer > tile.mGapLabel.size()) return -1.f;

    F32 g0, g1;
    const S32 gap = gapAt(tile, col, pos_agent.mV[VZ], g0, g1);
    if (gap < 0) return -1.f;   // inside a body: the store has no verdict

    const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)gap;
    switch (tile.mGapLabel[gi])
    {
        case AIR_OUTDOORS: return 0.f;
        case AIR_INTERIOR: return 1.f;
        case AIR_SHELTERED:
        {
            const U16 d = tile.mGapDepth[gi];
            if (d == AIR_DEPTH_UNREACHED) return 1.f;
            const F32 metres = (F32)d * tile.mCell;
            return metres / (metres + SS_WF_ENCLOSURE_TAU_M);
        }
        default: return -1.f;
    }
}

// <SS:Nexii> The wall profile at a point, from the gap-anchored probe bake: the
// nearest probe of the listener's lattice cell (its 8 neighbours back it up when the
// cell is probe-less - a pillar's answer is never stored, so a probe-less cell must
// reach sideways for one), its 8-direction profile's four cardinals out in the
// side-probe contract (metres, saturated at the reach cap). The ring lattice this
// once answered from sampled whatever altitude a fixed 4 m band landed on; the probe
// profile is taken at a real, ear-height z in real air.
bool SSWorldField::acousticAt(const LLVector3& pos_agent, F32 wall[4]) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    const Tile::Acoustic& ac = tile->mAcoustic;
    if (ac.mLatRes < 1 || ac.mSerial != tile->mGeomSerial
        || ac.mProbes.empty() || ac.mCellStart.size() != (size_t)ac.mLatRes * (size_t)ac.mLatRes + 1)
    {
        return false;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const F32 lx_f = (pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / ac.mLatCell;
    const F32 ly_f = (pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / ac.mLatCell;
    const S32 lx = llclamp((S32)lx_f, 0, ac.mLatRes - 1);
    const S32 ly = llclamp((S32)ly_f, 0, ac.mLatRes - 1);

    // Own cell first, then the 8-neighbour ring: nearest probe by 3D distance.
    S32 best = -1;
    F32 best_d2 = FLT_MAX;
    for (S32 ring = 0; ring <= 1 && best < 0; ++ring)
    {
        const S32 r0 = (ring == 0) ? 0 : -1;
        const S32 r1 = (ring == 0) ? 0 : 1;
        for (S32 oy = r0; oy <= r1; ++oy)
        {
            for (S32 ox = r0; ox <= r1; ++ox)
            {
                const S32 nx = lx + ox;
                const S32 ny = ly + oy;
                if (nx < 0 || ny < 0 || nx >= ac.mLatRes || ny >= ac.mLatRes) continue;
                const S32 cell = ny * ac.mLatRes + nx;
                for (S32 pi = ac.mCellStart[cell]; pi < ac.mCellStart[cell + 1]; ++pi)
                {
                    const SSAcoustic::Probe& p = ac.mProbes[(size_t)pi];
                    const F32 dx = (regionp->getOriginAgent().mV[VX] + p.mX) - pos_agent.mV[VX];
                    const F32 dy = (regionp->getOriginAgent().mV[VY] + p.mY) - pos_agent.mV[VY];
                    const F32 dz = p.mZ - pos_agent.mV[VZ];
                    const F32 d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best_d2) { best_d2 = d2; best = pi; }
                }
            }
        }
    }

    if (best < 0) return false;
    const SSAcoustic::Probe& p = ac.mProbes[(size_t)best];
    wall[0] = p.mWall[0];
    wall[1] = p.mWall[2];
    wall[2] = p.mWall[4];
    wall[3] = p.mWall[6];
    return true;
}

// <SS:Nexii> The occlusion trace over the span store - the doc's Part 3, the brief's
// realtime ask. Resolves one region's tile (both endpoints must live in it; the
// store has no verdict past its border) and hands the segment to the core's DDA.
bool SSWorldField::traceSolid(const LLVector3& a, const LLVector3& b,
                              F32& solid_m, S32& crossings) const
{
    solid_m = 0.f;
    crossings = 0;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(a);
    if (!regionp) return false;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end() || !it->second.mValid) return false;

    const Tile& tile = it->second;
    const size_t live = (size_t)SS_WF_MAX_SPANS * (size_t)tile.mRes * (size_t)tile.mRes;
    if (tile.mRes < 1 || tile.mSpanTop.size() < live || tile.mSpanBottom.size() < live) return false;

    SSAcoustic::Snap snap;
    snap.mTop = tile.mSpanTop.data();
    snap.mBottom = tile.mSpanBottom.data();
    snap.mFlags = tile.mSpanFlags.data();
    snap.mRes = tile.mRes;
    snap.mCell = tile.mCell;
    snap.mCeiling = (F32)tile.mBandCount * tile.mBandHeight;
    snap.mMaxSpans = SS_WF_MAX_SPANS;

    const LLVector3& origin = regionp->getOriginAgent();
    const F32 A[3] = { a.mV[VX] - origin.mV[VX], a.mV[VY] - origin.mV[VY], a.mV[VZ] };
    const F32 B[3] = { b.mV[VX] - origin.mV[VX], b.mV[VY] - origin.mV[VY], b.mV[VZ] };

    SSAcoustic::Trace t;
    if (!SSAcoustic::traceSolid(snap, A, B, t)) return false;

    solid_m = t.mSolidM;
    crossings = t.mCrossings;
    return true;
}

// <SS:Nexii> The listener blend's probe set, shared by probesAt and acousticDebug:
// the probes of the listener's own gap in its lattice cell (vertical overlap with
// the listener's gap, the crouch figure) plus their graph-adjacent probes, deduped,
// at most max_out. Falls back to all of the cell's probes when none overlaps (a
// listener inside a body's sliver, say), and to the nearest probe of the
// neighbourhood when the cell is probe-less.
S32 SSWorldField::listenerProbeSet(const Tile& tile, const LLVector3& pos_agent,
                                   S32* out, S32 max_out) const
{
    const Tile::Acoustic& ac = tile.mAcoustic;
    if (ac.mLatRes < 1 || ac.mProbes.empty()
        || ac.mCellStart.size() != (size_t)ac.mLatRes * (size_t)ac.mLatRes + 1
        || ac.mAdjStart.size() != ac.mProbes.size() + 1)
    {
        return 0;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return 0;

    const S32 lx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / ac.mLatCell),
                           0, ac.mLatRes - 1);
    const S32 ly = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / ac.mLatCell),
                           0, ac.mLatRes - 1);
    const S32 cell = ly * ac.mLatRes + lx;

    // The listener's own gap, from the capture column it stands in.
    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile.mCell),
                           0, tile.mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile.mCell),
                           0, tile.mRes - 1);
    const size_t col = (size_t)cy * (size_t)tile.mRes + (size_t)cx;
    F32 g0 = 0.f, g1 = 0.f;
    const S32 gap = gapAt(tile, col, pos_agent.mV[VZ], g0, g1);

    // Own-gap probes of the cell: vertical overlap with the listener's gap.
    S32 n = 0;
    for (S32 pi = ac.mCellStart[cell]; pi < ac.mCellStart[cell + 1] && n < max_out; ++pi)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)pi];
        if (gap < 0 || llmin(p.mGapTop, g1) - llmax(p.mGapBottom, g0) >= SSAcoustic::LINK_MIN_OVERLAP_M)
        {
            out[n++] = pi;
        }
    }

    // Graph-adjacent expansion (the connectivity-aware half: a probe behind a wall
    // reaches the blend only through a validated link), deduped by stamp.
    std::vector<S32> stamp(ac.mProbes.size(), -1);
    for (S32 i = 0; i < n; ++i) stamp[(size_t)out[i]] = (S32)i;
    const S32 own_n = n;
    for (S32 i = 0; i < own_n && n < max_out; ++i)
    {
        const S32 pi = out[i];
        for (S32 e = ac.mAdjStart[(size_t)pi]; e < ac.mAdjStart[(size_t)pi + 1] && n < max_out; ++e)
        {
            const S32 np = ac.mAdjNode[(size_t)e];
            if (stamp[(size_t)np] < 0)
            {
                stamp[(size_t)np] = n;
                out[n++] = np;
            }
        }
    }

    // Nothing at all in the cell: the nearest probe of the 8-neighbour ring.
    if (n == 0)
    {
        F32 best_d2 = FLT_MAX;
        S32 best = -1;
        for (S32 oy = -1; oy <= 1; ++oy)
        {
            for (S32 ox = -1; ox <= 1; ++ox)
            {
                const S32 nx = lx + ox;
                const S32 ny = ly + oy;
                if (nx < 0 || ny < 0 || nx >= ac.mLatRes || ny >= ac.mLatRes) continue;
                const S32 ncell = ny * ac.mLatRes + nx;
                for (S32 pi = ac.mCellStart[ncell]; pi < ac.mCellStart[ncell + 1]; ++pi)
                {
                    const SSAcoustic::Probe& p = ac.mProbes[(size_t)pi];
                    const F32 dx = (regionp->getOriginAgent().mV[VX] + p.mX) - pos_agent.mV[VX];
                    const F32 dy = (regionp->getOriginAgent().mV[VY] + p.mY) - pos_agent.mV[VY];
                    const F32 dz = p.mZ - pos_agent.mV[VZ];
                    const F32 d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best_d2) { best_d2 = d2; best = pi; }
                }
            }
        }
        if (best >= 0) out[n++] = best;
    }

    return n;
}

// <SS:Nexii> The listener blend (the doc's Part 3): the probes of the listener's own
// gap plus graph-adjacent probes, inverse-distance weighted. Blended figures: RT60,
// sky openness, room volume, wall profile, travel-to-outdoors, and the space/size
// classes - everything the soundscape's classification asked its raycasts for.
bool SSWorldField::probesAt(const LLVector3& pos_agent, ProbeSample out[4], S32& count) const
{
    count = 0;
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;
    if (tile->mAcoustic.mSerial != tile->mGeomSerial) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    S32 picks[16];
    const S32 n = listenerProbeSet(*tile, pos_agent, picks, 16);
    if (n <= 0) return false;

    // Top four by inverse-distance weight.
    S32 best[4] = { -1, -1, -1, -1 };
    F32 best_w[4] = { -1.f, -1.f, -1.f, -1.f };
    for (S32 i = 0; i < n; ++i)
    {
        const SSAcoustic::Probe& p = tile->mAcoustic.mProbes[(size_t)picks[i]];
        const F32 dx = (regionp->getOriginAgent().mV[VX] + p.mX) - pos_agent.mV[VX];
        const F32 dy = (regionp->getOriginAgent().mV[VY] + p.mY) - pos_agent.mV[VY];
        const F32 dz = p.mZ - pos_agent.mV[VZ];
        const F32 w = 1.f / (dx * dx + dy * dy + dz * dz + 1.f);
        for (S32 k = 0; k < 4; ++k)
        {
            if (w > best_w[k])
            {
                for (S32 j = 3; j > k; --j) { best[j] = best[j - 1]; best_w[j] = best_w[j - 1]; }
                best[k] = picks[i];
                best_w[k] = w;
                break;
            }
        }
    }

    for (S32 k = 0; k < 4; ++k)
    {
        if (best[k] < 0) break;
        const SSAcoustic::Probe& p = tile->mAcoustic.mProbes[(size_t)best[k]];
        ProbeSample& s = out[count++];
        s.mPos = regionp->getOriginAgent() + LLVector3(p.mX, p.mY, p.mZ);
        s.mWeight = best_w[k];
        s.mRT60 = p.mRT60;
        s.mSkyOpen = p.mSkyOpen;
        s.mVolume = p.mVolume;
        s.mTravelM = p.mTravelM;
        s.mSpaceClass = p.mSpaceClass;
        s.mSizeClass = p.mSizeClass;
        for (S32 i = 0; i < 8; ++i) s.mWall[i] = p.mWall[i];
    }
    return count > 0;
}

// <SS:Nexii> Per-source propagation (the doc's Part 3): Dijkstra over the baked
// probe graph with early exit at the listener's probe. Reads only the immutable
// baked graph - safe on the main thread between floods - and the serial gate keeps
// a stale graph from ever answering. The figures drive thunder's travel time
// (path metres, not euclidean), its muffle (portals and the path-vs-direct ratio),
// and its arrival direction (the listener's probe toward its Dijkstra parent: sound
// entering through a doorway is rendered from the doorway).
bool SSWorldField::propagationQuery(const LLVector3& source, const LLVector3& listener,
                                    Propagation& out) const
{
    out.mDirectM = 0.f;
    out.mPathM = 0.f;
    out.mCostM = 0.f;
    out.mPortals = 0;
    out.mMuffle = 0.f;
    out.mHaveArrival = false;
    out.mArrivalDir.setVec(0.f, 0.f, 1.f);
    out.mPath.clear();

    const LLVector3 mid = (source + listener) * 0.5f;
    const Tile* tile = tileAt(mid);
    if (!tile || !tile->mValid) return false;
    if (tile->mAcoustic.mSerial != tile->mGeomSerial) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile->mRegionHandle);
    if (!regionp) return false;

    // <SS:Nexii> Both ends have to stand in the tile's own region: the tile comes from the midpoint, and listenerProbeSet clamps a position's lattice and column indices into whatever tile it is handed, so an end across a region line used to snap silently onto the border probe and answer with a path it never walked. A false return is the caller's cue to fall back on its own heuristic, which is the honest answer here. [interaction: sssoundscape propagation]
    LLViewerRegion* src_regionp = LLWorld::getInstance()->getRegionFromPosAgent(source);
    LLViewerRegion* lis_regionp = LLWorld::getInstance()->getRegionFromPosAgent(listener);
    if (!src_regionp || src_regionp->getHandle() != tile->mRegionHandle) return false;
    if (!lis_regionp || lis_regionp->getHandle() != tile->mRegionHandle) return false;

    const Tile::Acoustic& ac = tile->mAcoustic;
    if (ac.mProbes.empty()
        || ac.mAdjStart.size() != ac.mProbes.size() + 1
        || ac.mAdjNode.size() != ac.mAdjCost.size()) return false;

    const LLVector3& origin = regionp->getOriginAgent();

    // Snap source and listener each to the nearest probe of their gap in their
    // lattice cell: the blend set's first pick is exactly that probe.
    S32 src_picks[16];
    S32 dst_picks[16];
    const S32 src_n = listenerProbeSet(*tile, source, src_picks, 16);
    const S32 dst_n = listenerProbeSet(*tile, listener, dst_picks, 16);
    if (src_n <= 0 || dst_n <= 0) return false;

    auto nearest = [&](const S32* picks, S32 n, const LLVector3& pos) -> S32
    {
        S32 best = -1;
        F32 best_d2 = FLT_MAX;
        for (S32 i = 0; i < n; ++i)
        {
            const SSAcoustic::Probe& p = ac.mProbes[(size_t)picks[i]];
            const F32 dx = (origin.mV[VX] + p.mX) - pos.mV[VX];
            const F32 dy = (origin.mV[VY] + p.mY) - pos.mV[VY];
            const F32 dz = p.mZ - pos.mV[VZ];
            const F32 d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2) { best_d2 = d2; best = picks[i]; }
        }
        return best;
    };

    const S32 src = nearest(src_picks, src_n, source);
    const S32 dst = nearest(dst_picks, dst_n, listener);
    if (src < 0 || dst < 0) return false;

    out.mDirectM = dist_vec(source, listener);

    // The solve. Cached by the caller's own move/serial discipline; a one-shot query
    // (thunder) pays its well-under-a-millisecond outright.
    const S32 n = (S32)ac.mProbes.size();
    std::vector<S32> parent((size_t)n);
    std::vector<F32> dist((size_t)n);
    if (!SSAcoustic::dijkstra(n, ac.mAdjStart.data(), ac.mAdjNode.data(), ac.mAdjCost.data(),
                              src, dst, parent.data(), dist.data()))
    {
        return false;   // no connected path: the caller keeps its guess
    }

    // Walk the chain listener <- ... <- source, then reverse.
    std::vector<S32> chain;
    {
        S32 cur = dst;
        S32 guard = 0;
        while (cur >= 0 && guard++ <= n)
        {
            chain.push_back(cur);
            if (cur == src) break;
            cur = parent[(size_t)cur];
        }
        if (chain.empty() || chain.back() != src) return false;
    }
    std::reverse(chain.begin(), chain.end());

    // Geometric path metres: the endpoint hops plus the node-to-node hops.
    const SSAcoustic::Probe& src_p = ac.mProbes[(size_t)src];
    const SSAcoustic::Probe& dst_p = ac.mProbes[(size_t)dst];
    F32 path = dist_vec(source, origin + LLVector3(src_p.mX, src_p.mY, src_p.mZ));
    for (size_t i = 1; i < chain.size(); ++i)
    {
        const SSAcoustic::Probe& a = ac.mProbes[(size_t)chain[i - 1]];
        const SSAcoustic::Probe& b = ac.mProbes[(size_t)chain[i]];
        path += sqrtf((b.mX - a.mX) * (b.mX - a.mX) + (b.mY - a.mY) * (b.mY - a.mY)
                      + (b.mZ - a.mZ) * (b.mZ - a.mZ));
    }
    path += dist_vec(origin + LLVector3(dst_p.mX, dst_p.mY, dst_p.mZ), listener);

    // Portals on the chain: consecutive nodes whose labels differ across the
    // OUTDOORS boundary - the same flag the links carry, recomputed from the pair.
    S32 portals = 0;
    for (size_t i = 1; i < chain.size(); ++i)
    {
        const U8 la = ac.mProbes[(size_t)chain[i - 1]].mLabel;
        const U8 lb = ac.mProbes[(size_t)chain[i]].mLabel;
        if ((la == 1) != (lb == 1)) ++portals;
    }

    out.mPathM = path;
    out.mCostM = dist[(size_t)dst];
    out.mPortals = portals;

    // Muffle from portals and the path-vs-direct ratio: around-buildings sound is
    // softened and darkened; a straight shot through portals less so.
    const F32 ratio = (out.mDirectM > 1.f) ? path / out.mDirectM : 1.f;
    out.mMuffle = llclamp((F32)portals * 0.25f + llmax(ratio - 1.f, 0.f) * 0.3f, 0.f, 0.9f);

    // Arrival direction: the listener's probe toward its Dijkstra parent - sound
    // entering through a doorway is rendered from the doorway.
    if (chain.size() >= 2)
    {
        const SSAcoustic::Probe& from = ac.mProbes[(size_t)chain[chain.size() - 2]];
        const LLVector3 dir((origin.mV[VX] + from.mX) - (origin.mV[VX] + dst_p.mX),
                            (origin.mV[VY] + from.mY) - (origin.mV[VY] + dst_p.mY),
                            from.mZ - dst_p.mZ);
        if (dir.magVecSquared() > 1.0e-4f)
        {
            out.mArrivalDir = dir;
            out.mArrivalDir.normVec();
            out.mHaveArrival = true;
        }
    }

    // The path for the debug layer, capped: stride when the chain runs long.
    const size_t cap = 64;
    const size_t stride = (chain.size() > cap) ? (chain.size() + cap - 1) / cap : 1;
    for (size_t i = 0; i < chain.size(); i += stride)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)chain[i]];
        out.mPath.push_back(origin + LLVector3(p.mX, p.mY, p.mZ));
    }
    if ((chain.size() - 1) % stride != 0)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)chain.back()];
        out.mPath.push_back(origin + LLVector3(p.mX, p.mY, p.mZ));
    }

    return true;
}

// <SS:Nexii> The debug export the V10 info view draws: probes and links in range,
// portal flags resolved per probe, the listener's blend set named. Read-only over
// the baked channel; a stale or unbaked channel reads mValid false and the view
// says so instead of drawing yesterday's room.
bool SSWorldField::acousticDebug(U64 region_handle, const LLVector3& centre_agent, F32 range_m,
                                 AcousticDebug& out) const
{
    out.mValid = false;
    out.mLatRes = 0;
    out.mLatCell = 0.f;
    out.mProbeCount = 0;
    out.mBundleCount = 0;
    out.mLinkCount = 0;
    out.mPortalCount = 0;
    out.mProbes.clear();
    out.mLinks.clear();
    out.mListenerProbes.clear();

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return false;

    const Tile& tile = it->second;
    const Tile::Acoustic& ac = tile.mAcoustic;
    if (ac.mLatRes < 1 || ac.mSerial != tile.mGeomSerial || ac.mProbes.empty()) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    const LLVector3& origin = regionp->getOriginAgent();
    const F32 centre_x = centre_agent.mV[VX] - origin.mV[VX];
    const F32 centre_y = centre_agent.mV[VY] - origin.mV[VY];
    const F32 r2 = range_m * range_m;

    // Range-cull probes, remembering the old->new index map.
    const S32 count = (S32)ac.mProbes.size();
    std::vector<S32> remap((size_t)count, -1);
    std::vector<U8> portal_node((size_t)count, 0);
    for (const SSAcoustic::Link& l : ac.mLinks)
    {
        if (l.mPortal && l.mA >= 0 && l.mA < count) portal_node[(size_t)l.mA] = 1;
        if (l.mPortal && l.mB >= 0 && l.mB < count) portal_node[(size_t)l.mB] = 1;
    }

    for (S32 i = 0; i < count; ++i)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)i];
        const F32 dx = p.mX - centre_x;
        const F32 dy = p.mY - centre_y;
        if (dx * dx + dy * dy > r2) continue;

        AcousticDebug::Probe o;
        o.mPos = origin + LLVector3(p.mX, p.mY, p.mZ);
        o.mGapBottom = p.mGapBottom;
        o.mGapTop = p.mGapTop;
        o.mLabel = p.mLabel;
        o.mRT60 = p.mRT60;
        o.mSkyOpen = p.mSkyOpen;
        o.mVolume = p.mVolume;
        o.mTravelM = p.mTravelM;
        o.mSpaceClass = p.mSpaceClass;
        o.mSizeClass = p.mSizeClass;
        o.mPortal = portal_node[(size_t)i] != 0;
        o.mHaveBundle = p.mHaveBundle != 0;
        remap[(size_t)i] = (S32)out.mProbes.size();
        out.mProbes.push_back(o);
    }

    for (const SSAcoustic::Link& l : ac.mLinks)
    {
        if (l.mA < 0 || l.mB < 0 || l.mA >= count || l.mB >= count) continue;
        const S32 ra = remap[(size_t)l.mA];
        const S32 rb = remap[(size_t)l.mB];
        if (ra < 0 || rb < 0) continue;
        AcousticDebug::Link o;
        o.mA = ra;
        o.mB = rb;
        o.mPortal = l.mPortal != 0;
        out.mLinks.push_back(o);
        if (o.mPortal) ++out.mPortalCount;
    }

    // The listener's own blend set, by node index.
    S32 picks[16];
    const S32 n = listenerProbeSet(tile, centre_agent, picks, 16);
    for (S32 i = 0; i < n; ++i)
    {
        const S32 r = (picks[i] >= 0 && picks[i] < count) ? remap[(size_t)picks[i]] : -1;
        if (r >= 0) out.mListenerProbes.push_back(r);
    }

    out.mValid = true;
    out.mLatRes = ac.mLatRes;
    out.mLatCell = ac.mLatCell;
    out.mProbeCount = count;
    for (const SSAcoustic::Probe& p : ac.mProbes)
    {
        if (p.mHaveBundle) ++out.mBundleCount;
    }
    out.mLinkCount = (S32)ac.mLinks.size();
    return true;
}

// Share of a tile's air gaps carrying a current label - 1.0 once the first
// flood has landed and nothing has edited since. The overlay reads this to
// tell a settled field from one still catching up.
F32 SSWorldField::airCoverage(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return 0.f;

    const Tile& tile = it->second;
    if (tile.mGapLabel.empty() || tile.mAirSerial != tile.mGeomSerial) return 0.f;
    if (tile.mRes < 1) return 0.f;

    const size_t layer = (size_t)tile.mRes * tile.mRes;
    const size_t per_col = (size_t)SS_WF_MAX_SPANS + 1;
    if (tile.mGapLabel.size() < per_col * layer) return 0.f;

    size_t labelled = 0;
    size_t total = 0;
    for (size_t col = 0; col < layer; ++col)
    {
        S32 n = 0;
        while (n < SS_WF_MAX_SPANS && tile.mSpanTop[(size_t)n * layer + col] > NO_SURFACE * 0.5f) ++n;
        for (S32 k = 0; k <= n; ++k)
        {
            const size_t gi = col * per_col + (size_t)k;
            ++total;
            if (tile.mGapLabel[gi] != AIR_UNKNOWN) ++labelled;
        }
    }
    return (total > 0) ? (F32)labelled / (F32)total : 1.f;
}

// The DRAINAGE_NETWORK core over one landing surface. Barnes' priority flood
// is the O(n log n) way to fill every depression to its spill elevation:
// drains (the grid border, water, unmapped sky) seed the heap at their own
// height, each cell pops once at the lowest spill reaching it, and a cell
// whose spill stands meaningfully above its own surface is standing water.
// Flow directions then run down the FILLED surface, so a pool's water heads
// for its outlet instead of into its own floor - with one exception the raw
// surface contributes: an EAVE. A step down steeper than a roof pitch and at
// least the eave-drop tall is a discontinuity in the capture, not a slope -
// water arriving there leaves into the air, so the drop is not a descent the
// flow may take and the cell holding it ends the surface. That is what stops
// a roof's catchment from pouring through the wall it borders and arriving
// invisibly on the street below, and it is what makes the accumulation
// terminate at the edges the shed reads. Accumulation itself is the
// hydrology-standard descending-fill pass: on the filled surface water only
// ever moves to a strictly lower cell, so one visit per cell in descending
// spill order sees every upstream contribution first, and each cell is left
// holding the area that drains through it in square metres.
bool SSWorldField::buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out)
{
    out.mSpill.clear();
    out.mPool.clear();
    out.mD8.clear();
    out.mCatch.clear();

    const S32 n = grid.mN;
    if (n < 3 || grid.mZ.size() < (size_t)n * n) return false;

    const size_t count = (size_t)n * n;
    out.mSpill.assign(count, -FLT_MAX);
    out.mPool.assign(count, 0);
    out.mD8.assign(count, 4);
    out.mCatch.assign(count, 0.f);

    // The hydrological domain: cells with any surface flag, water excluded -
    // water, unmapped sky and the grid border are drains the fill opens out at.
    // The height guard keeps a NODATA cell that somehow carried flags from
    // seeding a -FLT_MAX spill that would poison every fill elevation it
    // reached.
    auto land = [&](size_t i)
    {
        const U8 f = grid.mFlags[i];
        return (f & SSRainShadowMap::SURF_WATER) == 0 && f != 0
            && grid.mZ[i] > -FLT_MAX * 0.5f;
    };

    static const S32 DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const S32 DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

    struct Node
    {
        F32 mSpill;
        S32 mIndex;
    };
    struct NodeHeapOrder
    {
        bool operator()(const Node& a, const Node& b) const
        {
            // Min-heap on spill; index tie-break keeps the walk deterministic.
            if (a.mSpill != b.mSpill) return a.mSpill > b.mSpill;
            return a.mIndex > b.mIndex;
        }
    };
    std::priority_queue<Node, std::vector<Node>, NodeHeapOrder> heap;

    std::vector<U8> visited(count, 0);

    auto seed = [&](S32 i)
    {
        if (visited[i] || !land((size_t)i)) return;
        visited[i] = 1;
        out.mSpill[i] = grid.mZ[i];
        heap.push({ out.mSpill[i], i });
    };

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const S32 i = y * n + x;
            if (!land((size_t)i)) continue;

            if (x == 0 || y == 0 || x == n - 1 || y == n - 1)
            {
                seed(i);
                continue;
            }

            for (S32 d = 0; d < 8; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                if (!land((size_t)ny * n + nx))
                {
                    seed(i);
                    break;
                }
            }
        }
    }

    while (!heap.empty())
    {
        const S32 c = heap.top().mIndex;
        heap.pop();

        const F32 spill_c = out.mSpill[(size_t)c];
        const S32 cx = c % n;
        const S32 cy = c / n;

        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const S32 ni = ny * n + nx;
            if (visited[ni] || !land((size_t)ni)) continue;

            visited[ni] = 1;
            out.mSpill[ni] = llmax(spill_c, grid.mZ[(size_t)ni]);
            heap.push({ out.mSpill[ni], ni });
        }
    }

// Pool membership: the fill is standing water where it rises clear of the
    // surface - the depression's depth, not a sampled dip. Five centimetres sits
    // below the puddle thresholds that consume the mask, so this only gates
    // genuine standing water, never a capture texel's jitter.
    static const F32 POOL_FILL_EPS = 0.05f;

    // Flow: D8 down the filled surface, 3x3-indexed ((dy+1)*3 + (dx+1)), 4 when
    // nothing is lower - a pool floor, a sink, or a drain cell. The eave rule
    // removes the one descent a raw surface drop can offer that is not a
    // descent at all: the step off an edge.
    static const F32 EAVE_DROP_M  = 0.75f;  // at least a storey-step of fall
    static const F32 EAVE_SLOPE   = 2.0f;   // steeper than any roof pitch (~63 deg)

    const F32 cell = grid.mCell;
    const F32 eave_run = (cell > 0.f) ? EAVE_SLOPE * cell : FLT_MAX;

    for (S32 c = 0; c < (S32)count; ++c)
    {
        const size_t i = (size_t)c;
        if (!land(i)) continue;

        if (out.mSpill[i] - grid.mZ[i] > POOL_FILL_EPS)
        {
            out.mPool[i] = 1;
        }

        const S32 cx = c % n;
        const S32 cy = c / n;
        const F32 here = out.mSpill[i];
        const F32 z_here = grid.mZ[i];

        S32 best_dir = 4;
        F32 best_z = here;
        bool has_eave = false;
        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const size_t ni = (size_t)ny * n + nx;
            if (!land(ni)) continue;

            // The eave rule, tested on the RAW surface: the filled surface
            // smooths the very discontinuity that ends it. A neighbour over a
            // genuine cliff is not downhill, it is off the surface - water
            // goes into the air there rather than onto whatever is below.
            const F32 drop = z_here - grid.mZ[ni];
            if (drop >= EAVE_DROP_M && drop >= eave_run)
            {
                has_eave = true;
                continue;
            }

            // Dropping the FILLED elevation from the outlet chain windows out
            // the water-plane cases a raw-surface D8 gets wrong: a pool run
            // into its own floor.
            const F32 nz = out.mSpill[ni];
            if (nz < best_z - 0.01f)
            {
                best_z = nz;
                best_dir = (DY[d] + 1) * 3 + (DX[d] + 1);
            }
        }

        // A cell with an eave side is where the surface ends for the water
        // standing on it: it leaves there, whatever other descents the cell
        // could offer. Terminal here is what keeps a sloped gutter's lip line
        // from routing its catchment along itself and counting it twice.
        if (has_eave) best_dir = 4;

        out.mD8[i] = (U8)best_dir;
    }

    // Accumulation: contributing area in square metres, routed down the D8 in
    // descending spill order. On the filled surface water only ever moves to a
    // strictly lower cell, so one pass visits every cell after everything that
    // drains into it - no cycles, no second pass. A cell whose outlet chain
    // ends (an eave, a pool floor, the border) keeps what arrived: that is the
    // catchment the shed reads at the lips, and because an eave cell is
    // terminal, every lip of a run holds a disjoint share of the roof - the
    // run can sum its members' catchments without counting a sloped gutter
    // twice.
    std::vector<S32> order;
    order.reserve(count / 4);
    for (S32 c = 0; c < (S32)count; ++c)
    {
        if (land((size_t)c)) order.push_back(c);
    }
    std::sort(order.begin(), order.end(), [&out](S32 a, S32 b)
    {
        const F32 sa = out.mSpill[(size_t)a];
        const F32 sb = out.mSpill[(size_t)b];
        if (sa != sb) return sa > sb;
        return a > b;
    });

    const F32 area = cell * cell;
    for (const S32 c : order)
    {
        const size_t i = (size_t)c;
        out.mCatch[i] += area;

        const U8 dir = out.mD8[i];
        if (dir == 4) continue;

        const S32 dx = (dir % 3) - 1;
        const S32 dy = (dir / 3) - 1;
        const S32 nx = (c % n) + dx;
        const S32 ny = (c / n) + dy;
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

        out.mCatch[(size_t)ny * n + nx] += out.mCatch[i];
    }

    return true;
}

// The flood, over the store's air gaps. A column is a short list of solid
// [bottom, top] spans; the air is the gaps between and around them, and the
// flood's nodes are exactly those gaps - a room is ONE node whatever band its
// floor and ceiling landed in, the interior the wind solve wants to skip and
// the soundscape wants to know it is standing in. Adjacency is horizontal
// only, and strict: two columns' gaps connect when their intervals overlap by
// more than a sliver - a corner touch where a wall meets a ceiling is not a
// door. Vertical movement is free inside a node, because a gap already spans
// its full height. Evidence-only in the same spirit as the probe carve: a
// passage narrower than the store's slab threshold never becomes a gap at
// all. The touching classification then splits the reachable gaps three ways
// - OUTDOORS where nothing stands above them (the gap above every column's
// highest span starts OUTDOORS by definition), SHELTERED while an opening's
// aperture budget lasts, INTERIOR once every budget is spent - and the second
// walk measures each gap's graph distance to the nearest OUTDOORS gap, the
// "how enclosed is this air" figure the acoustic channel's travel times and
// sparse air solve both read.
static void ss_wf_flood(S32 res, S32 max_spans, F32 ceiling,
                        const std::vector<F32>& span_top,
                        const std::vector<F32>& span_bottom,
                        std::vector<U8>& gap_label, std::vector<U16>& gap_depth)
{
    const size_t layer = (size_t)res * res;
    const size_t per_col = (size_t)max_spans + 1;
    const size_t nodes = layer * per_col;
    gap_label.assign(nodes, SSWorldField::AIR_SOLID);
    gap_depth.assign(nodes, (U16)SSWorldField::AIR_DEPTH_UNREACHED);

    static const S32 DX[4] = { 1, -1, 0, 0 };
    static const S32 DY[4] = { 0, 0, 1, -1 };
    const F32 EPS = 0.05f;

    // Gap bounds per node, precomputed once: gap k of a column with n spans
    // runs from the span below (or the world floor) to the span above (or the
    // capture ceiling). Slots past a column's span count are empty.
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
    auto nodeExists = [&](size_t node) { return gb1[node] > gb0[node] + EPS; };

    // Adjacency is strict overlap: a corner touch where one column's gap ends
    // exactly where the neighbour's begins is a wall junction, not a door.
    auto touches = [&](size_t node, auto&& fn)
    {
        const size_t col = node / per_col;
        const S32 x = (S32)(col % (size_t)res);
        const S32 y = (S32)(col / (size_t)res);
        for (S32 d = 0; d < 4; ++d)
        {
            const S32 nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;

            const size_t ncol = (size_t)ny * res + nx;
            for (S32 kj = 0; kj <= max_spans; ++kj)
            {
                const size_t nnode = ncol * per_col + (size_t)kj;
                if (gb1[nnode] <= gb0[nnode] + EPS) continue;
                if (!(gb0[node] < gb1[nnode] - EPS && gb0[nnode] < gb1[node] - EPS)) continue;
                fn(nnode);
            }
        }
    };

    // Every column's top gap is open sky; every gap of a border column can
    // walk out sideways. The flood labels the rest from there.
    std::vector<S32> queue;
    queue.reserve(nodes / 8);
    for (size_t col = 0; col < layer; ++col)
    {
        const S32 x = (S32)(col % (size_t)res);
        const S32 y = (S32)(col / (size_t)res);
        const bool border = x == 0 || y == 0 || x == res - 1 || y == res - 1;
        const S32 n = span_count[col];

        for (S32 k = 0; k <= max_spans; ++k)
        {
            const size_t node = col * per_col + (size_t)k;
            if (gb1[node] <= gb0[node] + EPS) continue;
            if (k != n && !border) continue;
            gap_label[node] = SSWorldField::AIR_OUTDOORS;
            queue.push_back((S32)node);
        }
    }

    for (size_t head = 0; head < queue.size(); ++head)
    {
        touches((size_t)queue[head], [&](size_t nnode)
        {
            U8& lab = gap_label[nnode];
            if (lab != SSWorldField::AIR_INTERIOR) return;
            lab = SSWorldField::AIR_OUTDOORS;
            queue.push_back((S32)nnode);
        });
    }

    // ---- the touching classification: outdoors / sheltered / indoors ----
    // Covered gaps: outside-connected but with structure standing over them.
    // A gap below a column's top span always has that structure; the top gap
    // never does.
    std::vector<U8> covered(nodes, 0);
    for (size_t col = 0; col < layer; ++col)
    {
        const S32 n = span_count[col];
        for (S32 k = 0; k <= max_spans; ++k)
        {
            const size_t node = col * per_col + (size_t)k;
            if (gb1[node] <= gb0[node] + EPS) continue;
            if (gap_label[node] != SSWorldField::AIR_OUTDOORS) continue;
            if (k < n) covered[node] = 1;
        }
    }
    auto touchesCovered = [&](size_t node)
    {
        bool touch = false;
        touches(node, [&](size_t nnode) { if (covered[nnode]) touch = true; });
        return touch;
    };

    // Porch: the outdoors gaps touching covered air, clustered so one opening
    // is one aperture. Two windows in one wall stay separate clusters (the
    // porch gaps are wall-face neighbours only within the opening), while a
    // door and its adjacent window merge into the one opening they physically
    // are. Stored as the cluster's budget - sqrt(gaps), the opening's linear
    // width - so a metre-scale window hands the same shelter in gaps whatever
    // the column density is.
    std::vector<U16> porch(nodes, 0);
    std::vector<S32> cluster;
    for (size_t node = 0; node < nodes; ++node)
    {
        if (porch[node] || covered[node]) continue;
        if (gap_label[node] != SSWorldField::AIR_OUTDOORS) continue;
        if (!touchesCovered(node)) continue;

        cluster.clear();
        cluster.push_back((S32)node);
        porch[node] = 1;   // visited mark; the real budget lands after the walk
        for (size_t head = 0; head < cluster.size(); ++head)
        {
            touches((size_t)cluster[head], [&](size_t nnode)
            {
                if (porch[nnode] || covered[nnode]) return;
                if (gap_label[nnode] != SSWorldField::AIR_OUTDOORS) return;
                if (!touchesCovered(nnode)) return;

                porch[nnode] = 1;
                cluster.push_back((S32)nnode);
            });
        }

        // sqrt(aperture) fits a U16 by construction: a cluster cannot hold
        // more gaps than the tile carries, whose root is far under 65535.
        const U16 budget = (U16)(sqrtf((F32)cluster.size()) + 0.5f);
        for (const S32 j : cluster)
        {
            porch[j] = budget;
        }
    }

    // Budget propagation over covered gaps, widest path first: each gap's
    // remaining budget is the best (seed budget - steps) over every inward
    // path, so the strongest opening decides how deep the shelter reaches.
    // First pop is final (later entries only ever carry smaller budgets);
    // spent gaps fall back to INTERIOR.
    std::vector<U16> reach(nodes, 0);
    std::priority_queue<std::pair<U16, S32> > heap;
    for (size_t node = 0; node < nodes; ++node)
    {
        if (!covered[node]) continue;

        U16 best = 0;
        touches(node, [&](size_t nnode)
        {
            if (covered[nnode]) return;
            best = llmax(best, porch[nnode]);
        });
        if (best > 0)
        {
            reach[node] = best;
            heap.emplace(best, (S32)node);
        }
    }

    while (!heap.empty())
    {
        const U16 b = heap.top().first;
        const size_t node = (size_t)heap.top().second;
        heap.pop();
        if (b != reach[node]) continue;    // a stronger seed already passed
        if (b <= 1) continue;              // nothing left to hand inward

        touches(node, [&](size_t nnode)
        {
            if (!covered[nnode] || reach[nnode] >= b - 1) return;

            reach[nnode] = b - 1;
            heap.emplace((U16)(b - 1), (S32)nnode);
        });
    }

    for (size_t node = 0; node < nodes; ++node)
    {
        if (!covered[node]) continue;
        gap_label[node] = reach[node] > 0 ? (U8)SSWorldField::AIR_SHELTERED
                                          : (U8)SSWorldField::AIR_INTERIOR;
    }

    // Occlusion depth, one BFS from the whole outdoors set over outdoors and
    // sheltered gaps. Interior gaps are unreachable and stay marked.
    // Distances saturate at AIR_DEPTH_UNREACHED - 1: the sentinel must stay
    // exclusive to "unvisited", or a gap whose true distance hit 0xFFFF would
    // read as never visited and the walk would loop on it forever.
    {
        std::vector<S32> depth_q;
        depth_q.reserve(queue.size());
        for (size_t node = 0; node < nodes; ++node)
        {
            if (gap_label[node] == SSWorldField::AIR_OUTDOORS)
            {
                gap_depth[node] = 0;
                depth_q.push_back((S32)node);
            }
        }

        for (size_t head = 0; head < depth_q.size(); ++head)
        {
            const S32 cur = depth_q[head];
            touches((size_t)cur, [&](size_t nnode)
            {
                const U8 lab = gap_label[nnode];
                if (lab != SSWorldField::AIR_OUTDOORS && lab != SSWorldField::AIR_SHELTERED) return;
                U16& d = gap_depth[nnode];
                if (d != SSWorldField::AIR_DEPTH_UNREACHED) return;

                d = (U16)llmin((U32)gap_depth[(size_t)cur] + 1u,
                               SSWorldField::AIR_DEPTH_UNREACHED - 1u);
                depth_q.push_back((S32)nnode);
            });
        }
    }
}
// <SS:Nexii> The ACOUSTIC channel's bake (doc/atmo_magic_acoustics.md Parts 1-4),
// over the snapshot the flood just walked. Replaces the shipped ring lattice whole:
// fixed rings sampled altitudes nothing stands at - a ring inside a floor slab
// answers for nobody - while the span store knows where listeners actually stand,
// so probes anchor to air gaps instead. Per lattice cell (the same ~8 m resolution
// the rings ran at), an anchor column picked by spiralling out from the centre until
// one carries air at ear height, then per gap of that column: an ear probe at floor
// + 1.2 m, a ceiling probe under a roof, intermediates every ~4 m for tall gaps, and
// the tier A statistic bake per probe - the 8-direction wall profile at the probe's
// REAL z (the 4 side-raycasts' answer), sky openness, the bounded room flood's
// volume and area at lattice resolution, Sabine's RT60, the space/size classes the
// ambient loops already read, and the flood's own travel-to-outdoors. The probe graph
// rides the same walk: vertical links by construction, horizontal links between
// probes whose gaps overlap by at least the crouch figure, each validated by a
// span-store ray (at 8 m spacing a wall thinner than a lattice cell is invisible to
// gap overlap alone, and the ray is what keeps the graph from teleporting sound
// through it), aperture factors from the overlap clamped against the midpoint
// clearance, and portal flags across the OUTDOORS boundary. Probes are placed from
// the gap list alone, ignoring the flood's labels - a sealed room still gets probes,
// because a listener teleporting into it still deserves its reverb; labels ride
// along as data, not as placement gates.
static void ss_wf_acoustic_build(S32 res, S32 max_spans, F32 cell_m, F32 ceiling, S32 lat_res,
                                 const std::vector<F32>& span_top, const std::vector<F32>& span_bottom,
                                 const std::vector<U8>& gap_label, const std::vector<U16>& gap_depth,
                                 std::vector<SSAcoustic::Probe>& out_probes,
                                 std::vector<S32>& out_cell_start,
                                 std::vector<SSAcoustic::Link>& out_links,
                                 std::vector<S32>& out_adj_start,
                                 std::vector<S32>& out_adj_node,
                                 std::vector<F32>& out_adj_cost)
{
    out_probes.clear();
    out_cell_start.clear();
    out_links.clear();
    out_adj_start.clear();
    out_adj_node.clear();
    out_adj_cost.clear();
    if (lat_res < 1 || lat_res > res || cell_m <= 0.f || res < 1) return;

    SSAcoustic::Snap snap;
    snap.mTop = span_top.data();
    snap.mBottom = span_bottom.data();
    snap.mGapLabel = gap_label.data();
    snap.mGapDepth = gap_depth.data();
    snap.mRes = res;
    snap.mCell = cell_m;
    snap.mCeiling = ceiling;
    snap.mMaxSpans = max_spans;

    const size_t layer = (size_t)res * (size_t)res;
    const size_t per = (size_t)max_spans + 1;
    const F32 lat_cell = (F32)res * cell_m / (F32)lat_res;

    // The anchor spiral, resolved once: candidate offsets ordered by ring then angle.
    constexpr S32 SPIRAL_R = SSAcoustic::ANCHOR_SPIRAL_CELLS;
    S32 spiral_dx[(2 * SPIRAL_R + 1) * (2 * SPIRAL_R + 1)];
    S32 spiral_dy[(2 * SPIRAL_R + 1) * (2 * SPIRAL_R + 1)];
    const S32 spiral_n = SSAcoustic::anchorSpiral(spiral_dx, spiral_dy);

    SSAcoustic::LatGaps lat;
    lat.build(lat_res, max_spans);

    out_cell_start.assign((size_t)lat_res * (size_t)lat_res + 1, 0);

    for (S32 ly = 0; ly < lat_res; ++ly)
    {
        for (S32 lx = 0; lx < lat_res; ++lx)
        {
            const S32 cell = ly * lat_res + lx;
            out_cell_start[cell] = (S32)out_probes.size();

            // Anchor column: the centre, then the spiral, first column whose gap
            // list has air at ear height. None: the cell stays probe-less and its
            // neighbours' interpolation covers it.
            const S32 cx = llclamp((S32)(((F32)lx + 0.5f) * (F32)res / (F32)lat_res), 0, res - 1);
            const S32 cy = llclamp((S32)(((F32)ly + 0.5f) * (F32)res / (F32)lat_res), 0, res - 1);
            size_t anchor = ~(size_t)0;
            for (S32 i = 0; i < spiral_n; ++i)
            {
                const S32 nx = llclamp(cx + spiral_dx[i], 0, res - 1);
                const S32 ny = llclamp(cy + spiral_dy[i], 0, res - 1);
                const size_t col = (size_t)ny * (size_t)res + (size_t)nx;
                if (SSAcoustic::columnHasEarAir(snap, col))
                {
                    anchor = col;
                    break;
                }
            }
            if (anchor == ~(size_t)0) continue;

            SSAcoustic::fillLatGaps(snap, anchor, cell, lat);

            const F32 px = ((F32)(anchor % (size_t)res) + 0.5f) * cell_m;
            const F32 py = ((F32)(anchor / (size_t)res) + 0.5f) * cell_m;
            const S32 span_n = SSAcoustic::spanCount(snap, anchor);

            SSAcoustic::ProbeDef defs[16];
            for (S32 k = 0; k <= max_spans; ++k)
            {
                if (k > span_n) break;
                const F32 g0 = (k == 0) ? 0.f : span_top[(size_t)(k - 1) * layer + anchor];
                const F32 g1 = (k == span_n) ? ceiling : span_bottom[(size_t)k * layer + anchor];
                const S32 count = SSAcoustic::placeGapProbes(g0, g1, k == span_n, defs, 16);
                for (S32 i = 0; i < count; ++i)
                {
                    SSAcoustic::Probe p;
                    p.mX = px;
                    p.mY = py;
                    p.mZ = defs[i].mZ;
                    p.mGapBottom = defs[i].mGapBottom;
                    p.mGapTop = defs[i].mGapTop;
                    p.mCell = cell;
                    p.mGap = k;
                    p.mLabel = gap_label[anchor * per + (size_t)k];
                    p.mRoofed = defs[i].mRoofed ? 1 : 0;
                    const U16 dep = gap_depth[anchor * per + (size_t)k];
                    p.mGapDepth = dep;
                    p.mTravelM = (dep != 0xFFFF) ? (F32)dep * cell_m : -1.f;

                    // Tier A, all of it bake-time: the profile at the probe's real z,
                    // the column stack above, the bounded room flood, Sabine over it,
                    // and the classes the soundscape's enum already names.
                    SSAcoustic::wallProfile(snap, px, py, defs[i].mZ, p.mWall);
                    p.mSkyOpen = SSAcoustic::skyOpenness(snap, anchor, k);
                    SSAcoustic::roomEstimate(snap, lat, cell, k, p.mVolume, p.mArea);
                    p.mRT60 = SSAcoustic::sabineRT60(p.mVolume, p.mArea);
                    SSAcoustic::classify(defs[i].mRoofed, p.mLabel, p.mWall, p.mSpaceClass, p.mSizeClass);

                    out_probes.push_back(p);
                }
            }
        }
    }
    out_cell_start[(size_t)lat_res * (size_t)lat_res] = (S32)out_probes.size();

    if (out_probes.empty()) return;

    // ---- the graph ----

    // Vertical links: probes of the same gap in the same cell, consecutive by z -
    // connected by construction, cost the vertical distance.
    for (size_t i = 1; i < out_probes.size(); ++i)
    {
        SSAcoustic::Probe& a = out_probes[i - 1];
        SSAcoustic::Probe& b = out_probes[i];
        if (a.mCell != b.mCell || a.mGap != b.mGap) continue;
        SSAcoustic::Link l;
        l.mA = (S32)(i - 1);
        l.mB = (S32)i;
        l.mLen = fabsf(b.mZ - a.mZ);
        l.mAperture = 1.f;
        l.mPortal = 0;
        out_links.push_back(l);
    }

    // Horizontal links: per 4-neighbour lattice cell pair (+X and +Y only, so each
    // unordered pair validates once), candidate links between probes whose gaps
    // vertically overlap by the crouch figure, each validated by a span-store ray.
    for (S32 ly = 0; ly < lat_res; ++ly)
    {
        for (S32 lx = 0; lx < lat_res; ++lx)
        {
            const S32 cell = ly * lat_res + lx;
            static const S32 NDX[2] = { 1, 0 };
            static const S32 NDY[2] = { 0, 1 };
            for (S32 d = 0; d < 2; ++d)
            {
                const S32 nx = lx + NDX[d];
                const S32 ny = ly + NDY[d];
                if (nx >= lat_res || ny >= lat_res) continue;
                const S32 ncell = ny * lat_res + nx;

                for (S32 ai = out_cell_start[cell]; ai < out_cell_start[cell + 1]; ++ai)
                {
                    for (S32 bi = out_cell_start[ncell]; bi < out_cell_start[ncell + 1]; ++bi)
                    {
                        SSAcoustic::Probe& a = out_probes[(size_t)ai];
                        SSAcoustic::Probe& b = out_probes[(size_t)bi];
                        if (!SSAcoustic::gapsOverlap(a, b)) continue;

                        const F32 seg_a[3] = { a.mX, a.mY, a.mZ };
                        const F32 seg_b[3] = { b.mX, b.mY, b.mZ };
                        SSAcoustic::Trace tr;
                        if (!SSAcoustic::traceSolid(snap, seg_a, seg_b, tr)
                            || tr.mSolidM > SSAcoustic::LINK_BLOCK_SOLID_M)
                        {
                            continue;   // blocked: the pair may still connect via
                                        // another gap's probes or a longer path,
                                        // which is the point of a graph
                        }

                        SSAcoustic::Link l;
                        l.mA = ai;
                        l.mB = bi;
                        const F32 ddx = b.mX - a.mX;
                        const F32 ddy = b.mY - a.mY;
                        const F32 ddz = b.mZ - a.mZ;
                        l.mLen = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);

                        // Aperture: the overlap clamped against a horizontal clearance
                        // sample at the link's midpoint, the nearest wall distance
                        // perpendicular to the link.
                        const F32 mid[3] = { (a.mX + b.mX) * 0.5f, (a.mY + b.mY) * 0.5f,
                                             (a.mZ + b.mZ) * 0.5f };
                        const F32 ilen = (l.mLen > 1.0e-4f) ? 1.f / sqrtf(ddx * ddx + ddy * ddy) : 0.f;
                        const F32 perp_x = -ddy * ilen;
                        const F32 perp_y = ddx * ilen;
                        const F32 clear = llmin(
                            SSAcoustic::wallDistDir(snap, mid[0], mid[1], mid[2], perp_x, perp_y, lat_cell),
                            SSAcoustic::wallDistDir(snap, mid[0], mid[1], mid[2], -perp_x, -perp_y, lat_cell));
                        l.mAperture = SSAcoustic::apertureOf(a, b, clear);

                        // Portal: the endpoints' labels differ across the OUTDOORS
                        // boundary - the edge thunder and the muffle question care
                        // about, free from the labels.
                        l.mPortal = (U8)(((a.mLabel == 1) != (b.mLabel == 1)) ? 1 : 0);
                        out_links.push_back(l);
                    }
                }
            }
        }
    }

    // CSR adjacency: every link appears from both ends, cost precomputed once so the
    // runtime solve reads flat arrays only.
    const S32 node_count = (S32)out_probes.size();
    out_adj_start.assign((size_t)node_count + 1, 0);
    for (const SSAcoustic::Link& l : out_links)
    {
        if (l.mA >= 0 && l.mA < node_count) ++out_adj_start[(size_t)l.mA + 1];
        if (l.mB >= 0 && l.mB < node_count) ++out_adj_start[(size_t)l.mB + 1];
    }
    for (size_t i = 1; i < out_adj_start.size(); ++i)
    {
        out_adj_start[i] += out_adj_start[i - 1];
    }
    out_adj_node.resize((size_t)out_adj_start[node_count]);
    out_adj_cost.resize((size_t)out_adj_start[node_count]);
    {
        std::vector<S32> cursor(out_adj_start.begin(), out_adj_start.end() - 1);
        for (const SSAcoustic::Link& l : out_links)
        {
            const F32 cost = SSAcoustic::linkCost(l);
            if (l.mA >= 0 && l.mA < node_count)
            {
                out_adj_node[(size_t)cursor[(size_t)l.mA]] = l.mB;
                out_adj_cost[(size_t)cursor[(size_t)l.mA]++] = cost;
            }
            if (l.mB >= 0 && l.mB < node_count)
            {
                out_adj_node[(size_t)cursor[(size_t)l.mB]] = l.mA;
                out_adj_cost[(size_t)cursor[(size_t)l.mB]++] = cost;
            }
        }
    }
}

void SSWorldField::scheduleFlood(Tile& tile)
{
    if (mFloodBusy) return;                     // this commit's successor will reschedule
    if (tile.mBandCount < 1 || tile.mRes < 1) return;

    // Snapshot: the walk must never read the live span store while the next
    // build's conversion rewrites it on the main thread. Checked BEFORE the
    // busy flag - an undersized store bailing out must not wedge mFloodBusy
    // true and silently retire the flood for the rest of the session.
    const S32 res = tile.mRes;
    const size_t live = (size_t)SS_WF_MAX_SPANS * (size_t)res * res;
    if (tile.mSpanTop.size() < live || tile.mSpanBottom.size() < live) return;

    // <SS:Nexii> The acoustic bake rides the flood (same job, same snapshot, gates
    // and all); tier B is opt-in quality AND a claimed ACOUSTIC channel - nobody
    // pays for analysed reverb nobody asked for. Read on the main thread here, so
    // the worker job and the batch fan-out below never touch a setting.
    static LLCachedControl<bool> acoustics(gSavedSettings, "SSWorldFieldAcoustics", true);
    static LLCachedControl<U32> acoustic_quality(gSavedSettings, "SSWorldFieldAcousticsQuality", 0);
    const bool do_acoustic = (bool)acoustics;
    const bool tier_b = do_acoustic && llmin((U32)acoustic_quality, 1u) >= 1
                     && ss_wf_interest_count(tile.mRegionHandle, (S32)EChannel::ACOUSTIC) > 0;

    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");
    if (!general || !main) return;              // no worker: labels stay AIR_UNKNOWN, consumers cope

    mFloodBusy = true;
    const U32 generation = mFloodGeneration;
    const U64 region = tile.mRegionHandle;
    const U32 serial = tile.mGeomSerial;
    const F32 ceiling = (F32)bandCount() * tile.mBandHeight;
    auto snapshot_top = std::make_shared<std::vector<F32> >(
        tile.mSpanTop.begin(),
        tile.mSpanTop.begin() + live);
    auto snapshot_bottom = std::make_shared<std::vector<F32> >(
        tile.mSpanBottom.begin(),
        tile.mSpanBottom.begin() + live);
    auto gap_labels = std::make_shared<std::vector<U8> >();
    auto gap_depths = std::make_shared<std::vector<U16> >();
    auto probes = std::make_shared<std::vector<SSAcoustic::Probe> >();
    auto cell_start = std::make_shared<std::vector<S32> >();
    auto links = std::make_shared<std::vector<SSAcoustic::Link> >();
    auto adj_start = std::make_shared<std::vector<S32> >();
    auto adj_node = std::make_shared<std::vector<S32> >();
    auto adj_cost = std::make_shared<std::vector<F32> >();
    auto mip_top = std::make_shared<std::vector<F32> >();
    auto mip_bottom = std::make_shared<std::vector<F32> >();
    auto mip_flags = std::make_shared<std::vector<U8> >();

    // The acoustic lattice runs at a coarse multiple of the capture grid -
    // near 8m per probe, never finer than the capture itself.
    const S32 lat_res = llmin(llclamp((S32)llround((F32)res * tile.mCell / 8.f), 8, 64), res);
    const F32 cell_m = tile.mCell;

    main->postTo(
        general,
        [res, max_spans = SS_WF_MAX_SPANS, ceiling, lat_res, cell_m, do_acoustic, tier_b,
         snapshot_top, snapshot_bottom, gap_labels, gap_depths,
         probes, cell_start, links, adj_start, adj_node, adj_cost,
         mip_top, mip_bottom, mip_flags]()
        {
            ss_wf_flood(res, max_spans, ceiling, *snapshot_top, *snapshot_bottom,
                        *gap_labels, *gap_depths);
            if (do_acoustic)
            {
                ss_wf_acoustic_build(res, max_spans, cell_m, ceiling, lat_res,
                                     *snapshot_top, *snapshot_bottom, *gap_labels, *gap_depths,
                                     *probes, *cell_start, *links, *adj_start, *adj_node, *adj_cost);
                if (tier_b && !probes->empty())
                {
                    // The 4x-coarsened span mip tier B traces against (reverb
                    // statistics do not need 0.25 m walls) - built here while the
                    // snapshot is hot, handed to the batches through the shared
                    // buffers below.
                    SSAcoustic::Snap snap;
                    snap.mTop = snapshot_top->data();
                    snap.mBottom = snapshot_bottom->data();
                    snap.mRes = res;
                    snap.mCell = cell_m;
                    snap.mCeiling = ceiling;
                    snap.mMaxSpans = max_spans;
                    SSAcoustic::Snap mip = SSAcoustic::buildMip(snap, *mip_top, *mip_bottom, *mip_flags);
                    (void)mip;
                }
            }
            return true;
        },
        [this, generation, region, serial, ceiling, lat_res, cell_m, tier_b, res,
         main, general,
         gap_labels, gap_depths,
         probes, cell_start, links, adj_start, adj_node, adj_cost,
         mip_top, mip_bottom, mip_flags](bool)
        {
            mFloodBusy = false;
            if (generation != mFloodGeneration) return;

            auto it = mTiles.find(region);
            if (it == mTiles.end() || !it->second.mValid) return;
            if (it->second.mGeomSerial != serial) return;   // edited mid-walk; the next commit refloods

            it->second.mGapLabel = std::move(*gap_labels);
            it->second.mGapDepth = std::move(*gap_depths);
            it->second.mAirSerial = serial;

            Tile::Acoustic& ac = it->second.mAcoustic;
            ac.mLatRes = lat_res;
            ac.mLatCell = (lat_res > 0) ? (F32)it->second.mRes * it->second.mCell / (F32)lat_res : 0.f;
            ac.mCeiling = ceiling;
            ac.mProbes = std::move(*probes);
            ac.mCellStart = std::move(*cell_start);
            ac.mLinks = std::move(*links);
            ac.mAdjStart = std::move(*adj_start);
            ac.mAdjNode = std::move(*adj_node);
            ac.mAdjCost = std::move(*adj_cost);
            ac.mSerial = serial;

            // <SS:Nexii> Tier B behind the flood: every probe's bundle is
            // independent, so the bake fans probe BATCHES across the general queue
            // as separate jobs - the flood stays the one-at-a-time job it is, tier B
            // is many small ones behind it, each store-back serial-gated
            // individually (an edit-heavy region's re-bake drops the stale batch
            // and the next flood re-runs the whole walk). The mip the batches
            // trace against was built in the worker above; its dimensions follow
            // from buildMip's own formula, so nothing is plumbed back.
            if (tier_b && !ac.mProbes.empty() && !mip_top->empty()
                && mip_top->size() == mip_bottom->size())
            {
                const S32 mip_res = res / SSAcoustic::BUNDLE_MIP;
                const S32 mip_spans = SS_WF_MAX_SPANS + 2;
                const F32 mip_cell = cell_m * (F32)SSAcoustic::BUNDLE_MIP;
                if (mip_res >= 1 && mip_top->size() == (size_t)mip_spans * (size_t)mip_res * (size_t)mip_res)
                {
                    auto probe_snap = std::make_shared<std::vector<SSAcoustic::Probe> >(ac.mProbes);
                    constexpr S32 BATCH = 256;
                    const S32 count = (S32)ac.mProbes.size();
                    for (S32 start = 0; start < count; start += BATCH)
                    {
                        const S32 end = llmin(start + BATCH, count);
                        auto bundles = std::make_shared<std::vector<SSAcoustic::Bundle> >((size_t)(end - start));
                        main->postTo(
                            general,
                            [probe_snap, mip_top, mip_bottom, mip_flags, start, end,
                             mip_res, mip_spans, mip_cell, ceiling, bundles]()
                            {
                                SSAcoustic::Snap ms;
                                ms.mTop = mip_top->data();
                                ms.mBottom = mip_bottom->data();
                                ms.mFlags = mip_flags->data();
                                ms.mRes = mip_res;
                                ms.mCell = mip_cell;
                                ms.mCeiling = ceiling;
                                ms.mMaxSpans = mip_spans;
                                for (S32 i = start; i < end; ++i)
                                {
                                    SSAcoustic::Bundle b;
                                    SSAcoustic::traceBundle(ms, (*probe_snap)[(size_t)i],
                                                            SSAcoustic::BUNDLE_RAYS, SSAcoustic::BUNDLE_BOUNCES, b);
                                    (*bundles)[(size_t)(i - start)] = b;
                                }
                                return true;
                            },
                            [this, generation, region, serial, start, end, bundles](bool)
                            {
                                if (generation != mFloodGeneration) return;
                                auto cit = mTiles.find(region);
                                if (cit == mTiles.end() || !cit->second.mValid) return;
                                if (cit->second.mGeomSerial != serial) return;
                                if (cit->second.mAcoustic.mSerial != serial) return;
                                if (cit->second.mAcoustic.mProbes.size() < (size_t)end) return;

                                for (S32 i = start; i < end; ++i)
                                {
                                    const SSAcoustic::Bundle& b = (*bundles)[(size_t)(i - start)];
                                    SSAcoustic::Probe& p = cit->second.mAcoustic.mProbes[(size_t)i];
                                    p.mMFP = b.mMFP;
                                    p.mRT60 = b.mRT60;
                                    p.mEcho = b.mEcho;
                                    p.mFirstDelay = b.mFirstDelay;
                                    for (S32 k = 0; k < 3; ++k) p.mFirstDir[k] = b.mFirstDir[k];
                                    for (S32 k = 0; k < 8; ++k) p.mOpenness[k] = b.mOpenness[k];
                                    p.mHaveBundle = 1;
                                }
                            });
                    }
                }
            }
        });
}

// A band's surface hue for the overlay: the store's vertical resolution
// reads as colour - blue at the floor through green to ember red at the
// ceiling.
static LLColor4 ss_wf_band_hue(F32 t, F32 alpha)
{
    t = llclamp(t, 0.f, 1.f);
    const F32 c0[3] = { 0.25f, 0.5f, 1.f };
    const F32 c1[3] = { 0.3f, 1.f, 0.4f };
    const F32 c2[3] = { 1.f, 0.45f, 0.2f };
    const F32* lo = (t < 0.5f) ? c0 : c1;
    const F32* hi = (t < 0.5f) ? c1 : c2;
    const F32 u = (t < 0.5f) ? t * 2.f : (t - 0.5f) * 2.f;
    return LLColor4(lerp(lo[0], hi[0], u), lerp(lo[1], hi[1], u),
                    lerp(lo[2], hi[2], u), alpha);
}

// The world field's own overlay: what the capture resolved, what the air flood
// decided with its occlusion depth, and what the drainage pass reads - view
// picked by SSWorldFieldDebugView, distance-thinned like the wind flowmap's.
void SSWorldField::renderDebug()
{
    static LLCachedControl<U32> view(gSavedSettings, "SSWorldFieldDebugView", 1);
    const S32 which = llclamp((S32)view, 1, 7);
    // <SS:Nexii> View 7: the census navmesh - Detour polygon edges by band (doc/atmo_magic_navmesh.md). It is its own store, so it draws whether or not this field holds tiles, and it must run before the tile gate below.
    if (which == 7)
    {
        SSNavMesh::getInstance()->renderDebug(true);
        return;
    }
    if (mTiles.empty()) return;

    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);
    const F32 full = llclamp((F32)range_setting, 16.f, 4096.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto mark = [&](const LLVector3& p, const LLColor4& c, F32 size)
    {
        gGL.color4fv(c.mV);
        gGL.vertex3f(p.mV[VX] - size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX] + size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] - size, p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] + size, p.mV[VZ]);
    };

    // <SS:Nexii> Glyphs for the band-surfaces view: the shape says what the air above the surface is - a circle for outdoors, a triangle for sheltered, a square for interior, the plain cross where the flood has no label yet - so a stack of storeys reads by outline, not by hue alone. All line segments, drawn inside the LINES batch. [interaction: view 1]
    auto circle = [&](const LLVector3& p, const LLColor4& c, F32 r)
    {
        gGL.color4fv(c.mV);
        for (S32 i = 0; i < 8; ++i)
        {
            const F32 a0 = F_TWO_PI * (F32)i / 8.f, a1 = F_TWO_PI * (F32)(i + 1) / 8.f;
            gGL.vertex3f(p.mV[VX] + cosf(a0) * r, p.mV[VY] + sinf(a0) * r, p.mV[VZ]);
            gGL.vertex3f(p.mV[VX] + cosf(a1) * r, p.mV[VY] + sinf(a1) * r, p.mV[VZ]);
        }
    };
    auto triangle = [&](const LLVector3& p, const LLColor4& c, F32 r)
    {
        gGL.color4fv(c.mV);
        const LLVector3 a(p.mV[VX], p.mV[VY] + r, p.mV[VZ]), b(p.mV[VX] - r * 0.866f, p.mV[VY] - r * 0.5f, p.mV[VZ]), d(p.mV[VX] + r * 0.866f, p.mV[VY] - r * 0.5f, p.mV[VZ]);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV);
        gGL.vertex3fv(b.mV); gGL.vertex3fv(d.mV);
        gGL.vertex3fv(d.mV); gGL.vertex3fv(a.mV);
    };
    auto square = [&](const LLVector3& p, const LLColor4& c, F32 r)
    {
        gGL.color4fv(c.mV);
        const F32 x0 = p.mV[VX] - r, x1 = p.mV[VX] + r, y0 = p.mV[VY] - r, y1 = p.mV[VY] + r;
        gGL.vertex3f(x0, y0, p.mV[VZ]); gGL.vertex3f(x1, y0, p.mV[VZ]);
        gGL.vertex3f(x1, y0, p.mV[VZ]); gGL.vertex3f(x1, y1, p.mV[VZ]);
        gGL.vertex3f(x1, y1, p.mV[VZ]); gGL.vertex3f(x0, y1, p.mV[VZ]);
        gGL.vertex3f(x0, y1, p.mV[VZ]); gGL.vertex3f(x0, y0, p.mV[VZ]);
    };

    auto strideFor = [&](F32 wx, F32 wy) -> S32
    {
        const F32 away = llmax(fabsf(wx - cam.mV[VX]), fabsf(wy - cam.mV[VY]));
        return (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
    };

    gGL.begin(LLRender::LINES);

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.mValid) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 cell = tile.mCell;
        const F32 h = tile.mBandHeight;
        const size_t layer = (size_t)tile.mRes * tile.mRes;

        // The tile's stack footprint, so a region nobody has captured yet reads
        // as an empty box rather than as nothing at all.
        {
            const F32 x1 = origin.mV[VX] + (F32)tile.mRes * cell;
            const F32 y1 = origin.mV[VY] + (F32)tile.mRes * cell;
            const F32 z0 = 0.f;
            const F32 z1 = (F32)(bandCount() * tile.mBandHeight);
            gGL.color4f(0.5f, 0.55f, 0.7f, 0.35f);
            const F32 bx[4] = { origin.mV[VX], x1, x1, origin.mV[VX] };
            const F32 by[4] = { origin.mV[VY], origin.mV[VY], y1, y1 };
            for (S32 i = 0; i < 4; ++i)
            {
                const S32 j = (i + 1) % 4;
                gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[j], by[j], z0);
                gGL.vertex3f(bx[i], by[i], z1); gGL.vertex3f(bx[j], by[j], z1);
                gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[i], by[i], z1);
            }
        }

        if (which == 1)
        {
// Every solid span the store holds, standing at the altitude the store
            // holds it at, hue by altitude so stacked storeys read separately instead
            // of fusing into one roof.
            const F32 ceiling = llmax((F32)(bandCount() * tile.mBandHeight), 1.f);
            const bool labelled = !tile.mGapLabel.empty() && tile.mAirSerial == tile.mGeomSerial
                                  && tile.mGapLabel.size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer;
            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t col = (size_t)y * tile.mRes + x;
                    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
                    {
                        const F32 z = tile.mSpanTop[(size_t)k * layer + col];
                        if (z <= -FLT_MAX * 0.5f) break;

                        const LLVector3 p(wx, wy, z);
                        const LLColor4 c = ss_wf_band_hue(z / ceiling, 0.85f);
                        const U8 lab = labelled ? tile.mGapLabel[col * (SS_WF_MAX_SPANS + 1) + (size_t)k + 1] : (U8)AIR_UNKNOWN;   // the gap above span k
                        if (lab == AIR_OUTDOORS) circle(p, c, cell * 0.4f);
                        else if (lab == AIR_SHELTERED) triangle(p, c, cell * 0.45f);
                        else if (lab == AIR_INTERIOR) square(p, c, cell * 0.35f);
                        else mark(p, c, cell * 0.4f);
                    }
                }
            }
        }
        else if (which == 2)
        {
            // The touching classification, per air gap: outdoors air green,
            // sheltered air amber fading with occlusion depth (how far the
            // opening's reach still carries), interior air red. A column's
            // gaps sit between its spans, below the lowest and above the
            // highest. Current means the labels cover every live gap: a
            // no-op re-peel can shift the spans without moving the serial,
            // and the flood refills them later.
            const bool current = !tile.mGapLabel.empty() && tile.mAirSerial == tile.mGeomSerial
                                 && tile.mGapLabel.size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer
                                 && tile.mGapDepth.size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer;
            if (!current) continue;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t col = (size_t)y * tile.mRes + x;
                    F32 g0 = 0.f;
                    for (S32 k = 0; k <= SS_WF_MAX_SPANS; ++k)
                    {
                        const F32 stop = (k < SS_WF_MAX_SPANS) ? tile.mSpanTop[(size_t)k * layer + col]
                                                               : (F32)(bandCount() * tile.mBandHeight);
                        const F32 g1 = (stop > -FLT_MAX * 0.5f) ? stop : (F32)(bandCount() * tile.mBandHeight);
                        if (g1 - g0 > 0.05f)
                        {
                            const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)k;
                            const U8 lab = tile.mGapLabel[gi];
                            const F32 z = (g0 + g1) * 0.5f;
                            if (lab == AIR_OUTDOORS)
                            {
                                mark(LLVector3(wx, wy, z), LLColor4(0.3f, 1.f, 0.4f, 0.85f), cell * 0.4f);
                            }
                            else if (lab == AIR_SHELTERED)
                            {
                                const U16 d = tile.mGapDepth[gi];
                                const F32 a = llmax(0.9f / (1.f + (F32)d * 0.25f), 0.08f);
                                mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.8f, 0.2f, a), cell * 0.4f);
                            }
                            else if (lab == AIR_INTERIOR)
                            {
                                mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.25f, 0.25f, 0.85f), cell * 0.4f);
                            }
                        }
                        if (stop <= -FLT_MAX * 0.5f) break;   // past the column's spans
                        g0 = stop;
                    }
                }
            }
        }
        else if (which == 3)
        {
            // Drainage topology at the tile's own resolution: standing water
            // blue, and an arrow per cell down the filled surface's D8 - the
            // outlet chain a pool's water will actually follow, ending where
            // the eave rule ends the surface. The grid clamps its resolution,
            // so the walk and the spacing follow the grid's own answers, not
            // the tile's.
            auto& cached = sDrainDebug[tile.mRegionHandle];
            if (cached.mSerial != tile.mGeomSerial)
            {
                cached.mGrid = SSRainShadowMap::SurfaceGrid();
                cached.mDrain = Drainage();
                cached.mSerial = tile.mGeomSerial;
                if (!buildSurfaceGrid(tile.mRegionHandle, tile.mRes, cached.mGrid)
                    || !buildDrainage(cached.mGrid, cached.mDrain))
                {
                    cached.mSerial = 0;
                    continue;
                }
            }
            const SSRainShadowMap::SurfaceGrid& grid = cached.mGrid;
            const Drainage& drain = cached.mDrain;

            const S32 gn = grid.mN;
            const F32 gcell = grid.mCell;
            if (gn < 3 || gcell <= 0.f) continue;

            for (S32 y = 0; y < gn; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * gcell;
                for (S32 x = 0; x < gn; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * gcell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t i = (size_t)y * gn + x;
                    const U8 f = grid.mFlags[i];
                    if (f == 0 || (f & SSRainShadowMap::SURF_WATER)) continue;

                    const F32 z = grid.mZ[i];
                    if (drain.mPool[i])
                    {
                        mark(LLVector3(wx, wy, z), LLColor4(0.2f, 0.5f, 1.f, 0.9f), gcell * 0.45f);
                    }
                    else if (drain.mD8[i] != 4)
                    {
                        const S32 di = drain.mD8[i];
                        const F32 len = gcell * 0.7f;
                        const F32 dx = (F32)((di % 3) - 1) * len;
                        const F32 dy = (F32)((di / 3) - 1) * len;
                        gGL.color4f(0.7f, 0.85f, 1.f, 0.55f);
                        gGL.vertex3f(wx, wy, z + 0.4f);
                        gGL.vertex3f(wx + dx, wy + dy, z + 0.4f);
                    }
                }
            }
        }
        else if (which == 5)
        {
            // The column spans themselves: each solid span drawn as two flat
            // rects - its floor and its ceiling - coloured by the air state
            // standing on it (the gap above it), with a dim line joining
            // ceiling to floor through the solid. A column's stack reads as a
            // ladder of state-coloured plates on one spine; the air gaps
            // between spans stay empty, which is exactly where the flood
            // walks.
            const bool current = !tile.mGapLabel.empty() && tile.mAirSerial == tile.mGeomSerial
                                 && tile.mGapLabel.size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer
                                 && tile.mGapDepth.size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer;
            const F32 ceiling = llmax((F32)(bandCount() * tile.mBandHeight), 1.f);

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t col = (size_t)y * tile.mRes + x;
                    const F32 s = cell * 0.4f;
                    auto span_rect = [&](F32 z)
                    {
                        gGL.vertex3f(wx - s, wy - s, z); gGL.vertex3f(wx + s, wy - s, z);
                        gGL.vertex3f(wx + s, wy - s, z); gGL.vertex3f(wx + s, wy + s, z);
                        gGL.vertex3f(wx + s, wy + s, z); gGL.vertex3f(wx - s, wy + s, z);
                        gGL.vertex3f(wx - s, wy + s, z); gGL.vertex3f(wx - s, wy - s, z);
                    };
                    auto state_color = [&](U8 st)
                    {
                        switch (st)
                        {
                            case AIR_OUTDOORS:  gGL.color4f(0.3f, 1.f, 0.4f, 0.8f); break;
                            case AIR_SHELTERED: gGL.color4f(1.f, 0.8f, 0.2f, 0.8f); break;
                            case AIR_INTERIOR:  gGL.color4f(1.f, 0.25f, 0.25f, 0.85f); break;
                            default:            gGL.color4f(0.55f, 0.6f, 0.7f, 0.5f); break;
                        }
                    };

                    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
                    {
                        const size_t si = (size_t)k * layer + col;
                        const F32 z1 = tile.mSpanTop[si];
                        if (z1 <= -FLT_MAX * 0.5f) break;   // past the column's spans
                        const F32 z0 = tile.mSpanBottom[si];
                        if (z1 - z0 < 0.01f) continue;

                        // The state of the air the body holds up: the gap
                        // above it in the column.
                        U8 st = AIR_UNKNOWN;
                        if (current)
                        {
                            st = tile.mGapLabel[col * (SS_WF_MAX_SPANS + 1) + (size_t)(k + 1)];
                        }

                        state_color(st);
                        span_rect(z1);
                        span_rect(z0);

                        // The spine: ceiling to floor, through the solid.
                        gGL.color4f(0.6f, 0.65f, 0.75f, 0.35f);
                        gGL.vertex3f(wx, wy, z1);
                        gGL.vertex3f(wx, wy, z0);
                    }
                }
            }
        }
    }

    gGL.end();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    // <SS:Nexii> View 6: the declared-shape census overlay - what the exact
    // query layer holds, boxes by layer and provenance
    // (doc/atmo_magic_worldfield_competition.md 7.8).
    if (which == 6)
    {
        SSWorldFieldShapes::getInstance()->renderDebug();
    }

    // Drop debug views for regions the field no longer holds.
    for (auto it = sDrainDebug.begin(); it != sDrainDebug.end();)
    {
        it = mTiles.count(it->first) ? std::next(it) : sDrainDebug.erase(it);
    }
}

