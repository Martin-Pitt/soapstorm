/**
 * @file ssworldfield.h
 * @brief Atmo Magic: the shared world field.
 *
 *        One region-anchored capture of the world's solid structure, shared by
 *        every system that currently captures its own. A tile is captured as a
 *        stack of horizontal Z bands; each band is one top-down ortho depth
 *        pass whose frustum clips everything above the band, so per column and
 *        per band it yields the highest surface inside that band - the
 *        band-sliced form of a depth peel, produced with the exact machinery
 *        the rain shadow and wind captures already use.
 *
 *        The store is spans-shaped: per column, per band, a surface altitude
 *        and surface flags. Everything downstream is a materialised view over
 *        it. The first view is SURFACE_TOP - the landing-surface grid the
 *        surface field, runoff and snow read - produced with the same shape
 *        and serial semantics as SSRainShadowMap::SurfaceGrid so consumers
 *        migrate by swapping their source. COVERAGE (indoor vs outdoor,
 *        burial depth) reads the band stack directly.
 *
 *        Captures are staged across frames like the wind flowmap's build, one
 *        band per step, and a prim edit re-peels only the dirty rectangle.
 *        Tiles are built only where a channel is claimed; nothing runs for a
 *        region nobody asked about.
 *
 *        See doc/atmo_magic_worldfield.md.
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

#ifndef SS_WORLDFIELD_H
#define SS_WORLDFIELD_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "ssrainshadow.h"
#include "v3math.h"
#include "v3dmath.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

class SSWorldField : public LLSingleton<SSWorldField>
{
    LLSINGLETON_EMPTY_CTOR(SSWorldField);

public:
    // What a consumer wants out of the store. The capture's band depth and
    // later its probe passes are the union of every currently claimed
    // channel's needs; today that is SURFACE_TOP alone.
    enum class EChannel
    {
        SURFACE_TOP = 0,
        SOLID_VOLUME_3D,
        COVERAGE,
        DRAINAGE_NETWORK,
        WALKABLE,
        ACOUSTIC
    };

    // A consumer's stake in a channel. Ref-counted per (region, channel);
    // dropping the last handle stops the region paying for the channel.
    // Deliberately a handle rather than a bool: a prototype that wants a
    // channel for a minute of testing drops it and the field notices.
    class Interest
    {
    public:
        Interest() = default;

        explicit operator bool() const { return mHold != nullptr; }

    private:
        friend class SSWorldField;
        explicit Interest(std::shared_ptr<void> hold) : mHold(std::move(hold)) {}

        std::shared_ptr<void> mHold;
    };

    Interest claim(U64 region_handle, EChannel channel);

    // The one edit fan-out. Settled prim edits land here; the field marks the
    // tile's dirty rectangle and the re-peel is scissored to it.
    static void markDirty(const LLVector3& pos_agent, F32 radius);

    void clear();

    void update();

    // The landing-surface view - SSRainShadowMap::buildSurfaceGrid's exact
    // contract (region-anchored n x n grid, first thing a falling drop meets,
    // water and heightmap fallbacks included, geometry serial for the retrace
    // gate). Consumers switch sources without changing anything else.
    bool buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out);

    void validTiles(std::vector<std::pair<U64, U32> >& out) const;

    // The topmost surface at a point, absolute Z. False if the column is
    // unmapped (tile absent, not yet built, or fully sky).
    bool surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const;

    // Whether the point has structure above it (sheltered), and if so how far
    // up to the column's sky-open top - the burial measure the soundscape
    // currently derives from the wind tile's column top.
    bool coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const;

    // The richer form the soundscape's probe cycle wants: whether anything
    // stands over the point at all, the nearest such surface's altitude (the
    // space's ceiling, to band precision), and the column's sky-open top.
    // Burial (the build stacked between ceiling and sky) is their difference,
    // floor-count aware where the single column-top subtraction never was.
    // False when no valid tile covers the point; the caller keeps its
    // raycasts for that.
    bool coverageDetail(const LLVector3& pos_agent, bool& covered,
                        F32& ceiling_z, F32& column_top_z) const;

    // <SS:Nexii> Air connectivity, the flood-fill pass of the worldfield design, run over the store's air spans: each band-cell holds up to two air spans - the lower under the band's topmost body, the upper above its top - and every span is labelled by whether it can actually be reached from the sky or the tile's horizontal borders, then by how enclosed it is. The touching classification splits the reachable spans three ways - OUTDOORS where the column is open to the sky above, SHELTERED where cover stands over it but an opening still connects it to outdoors air, INTERIOR once every opening's reach is spent. The reach is a budget handed across each touching between outdoors and sheltered spans: the opening's porch (the outdoors spans touching it, clustered so one opening is one aperture) seeds sqrt(aperture spans) - the opening's linear width, not its area - and every span step inward spends one. A cave mouth hands its interior tens of spans of shelter while a window's budget dies a span or two past the glass; a sealed room was never reached and is interior by construction. Solved on the general worker queue from a snapshot of the band stack, so a build committing on the main thread never races the walk; stored against the tile's geometry serial so a stale answer is never served after an edit. The same job carries the occlusion depth: each outdoors- or sheltered-connected span's graph distance to the nearest OUTDOORS span, for the "how enclosed is this point" consumers (the sparse-air-solve and acoustic occlusion figures).
    enum EAirLabel : U8
    {
        AIR_SOLID = 0,      // no air span here - inside a body, or the band-cell is body to its top
        AIR_OUTDOORS,       // open to the sky above - the elements land here
        AIR_SHELTERED,      // covered, but an opening still reaches it
        AIR_INTERIOR,       // beyond every opening's reach, or sealed
        AIR_UNKNOWN         // no tile, no labels yet, or stale
    };
    U8 airLabelAt(const LLVector3& pos_agent) const;

    // The occlusion depth behind airLabelAt: graph distance in cells from the
    // point's air band-cell to the nearest AIR_OUTDOORS cell (0 when the point
    // itself is outdoors air), or AIR_DEPTH_UNREACHED when the cell
    // is interior, off-tile, or the labels are not current. Band and
    // horizontal steps count one cell each, so the figure is a graph hop
    // count over the store's own grid, not metres.
    static constexpr U32 AIR_DEPTH_UNREACHED = 0xFFFFu;
    U32 airDepthAt(const LLVector3& pos_agent) const;

    // <SS:Nexii> The enclosure spectrum behind the touching classification:
    // 0 outdoors, rising through sheltered air on the occlusion depth - the
    // air-graph distance back to open sky, in metres, saturated on a fixed
    // tau - and 1 interior, where every opening budget is spent or the walk
    // never got in. The camera's own band-cell usually reads SOLID (the
    // ground or a roof shares it), so the lookup resolves sub-band air: a
    // point above its band's stored surface inherits the nearest air label
    // above it in the column; a point inside the implied solid body has no
    // verdict. Returns -1 whenever there is no current answer - no labels
    // yet, stale after an edit, off-tile, or sub-band-solid - and the caller
    // keeps its own probe answer for that. This is the scalar the soundscape
    // blends its beds on.
    F32 enclosureAt(const LLVector3& pos_agent) const;

    // The bulk form for callers that walk one region's cells (the surface
    // field's window stitch): region and tile resolved once, the same answer
    // per point.
    F32 enclosureAtRegion(U64 region_handle, const LLVector3& pos_agent) const;

    // <SS:Nexii> The ACOUSTIC channel's first slice: a coarse probe lattice
    // over the tile's air, built by the same worker job as the flood - one
    // precomputed wall distance per cardinal per (band, lattice cell), the
    // room-size and occlusion questions the soundscape otherwise spends four
    // live raycasts on per probe cycle. Copies the side-probe contract:
    // metres from the probe to the nearest solid walking horizontally in the
    // point's band, saturated at the reach cap so an open direction reads as
    // "no wall". False when the tile or lattice is not current - stale after
    // an edit until the next flood - and the caller keeps its raycasts.
    bool acousticAt(const LLVector3& pos_agent, F32 wall[4]) const;

    // The share of a tile's air cells the flood actually labelled - 1.0 once
    // the labels are current, less before the first flood or after an edit.
    F32 airCoverage(U64 region_handle) const;

    // <SS:Nexii> Drainage topology over one landing-surface grid - the DRAINAGE_NETWORK channel core, materialised synchronously over whatever SurfaceGrid the caller already holds (the surface field's geometry, at its own n). A Barnes priority flood fills every depression to its spill elevation; a cell below that level is a pool member (standing water, the figure that retires the surface field's local dips check); flow directions are D8 down the *filled* surface, so a pool's water drains toward its spill outlet rather than into its own floor. Nothing is cached here: the grid carries the geometry serial and the caller already gates retraces on it. Per-span levels wait on the multi-peel store; this is the landing-surface level the design ships first.
    struct Drainage
    {
        std::vector<F32> mSpill;      // fill elevation per cell, NODATA where unmapped
        std::vector<U8> mPool;        // 1 = cell sits under its spill level (depression member)
        std::vector<U8> mD8;          // outflow cell on the filled surface, 3x3-indexed
                                      // ((dy+1)*3 + (dx+1)); 4 = no outflow (sink or drain edge)
    };
    bool buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out) const;

    bool tileValid(U64 region_handle) const;
    U32 geometrySerial(U64 region_handle) const;

    // Stats
    S32 tileCount() const { return (S32)mTiles.size(); }
    U32 captureCount() const { return mCaptureCount; }
    U32 dirtyCaptureCount() const { return mDirtyCaptures; }
    F32 lastCaptureMS() const { return mLastCaptureMS; }
    F32 bandHeight() const;
    S32 bandCount() const;
    S32 resolution() const;
    F64 tileAge(const LLVector3& pos_agent) const;
    S32 effectiveBands(const LLVector3& pos_agent) const;

    // <SS:Nexii> The world field's own overlay: what the capture saw, what the air flood decided, and what the drainage pass reads - view picked by SSWorldFieldDebugView, distance-thinned like the wind flowmap's overlay.
    void renderDebug();

private:
    static constexpr S32 MAX_BANDS = 24;
    static constexpr U32 MAX_TILES = 4;
    static constexpr F64 DIRTY_MIN_INTERVAL = 2.0;
    static constexpr F64 BAND_MIN_INTERVAL = 0.05;
    // This many consecutive empty bands end a full build: bands are swept
    // bottom-up, so empty runs only occur above all content the ceiling
    // setting covers. Skyboxes above the resulting ceiling are invisible to
    // the field until SSWorldFieldCeiling is raised - the same practical
    // shape as the wind map's band.
    static constexpr S32 EMPTY_BANDS_TO_STOP = 3;

    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;
        F32 mCell = 0.f;

        S32 mBandCount = 0;        // effective bands; bands [0, mBandCount) are live
        F32 mBandHeight = 4.f;

        // <SS:Nexii> The capture scratch: per band, per column, the two
        // boundaries the two ortho passes resolved - mBandTop (front faces,
        // the highest up-facing surface in the band) and mBandUnder (the
        // upward shot keeping the farthest front face, the topmost body's
        // underside). This is the capture's working data, not the store:
        // computeSpans folds it into the column span store at commit.
        // NO_SURFACE where a pass found nothing. Flat
        // [band][y * res + x], allocated lazily to mAllocBands bands by
        // ensureBands as a build sweeps upward and released at commit (the
        // store is the spans; only the next build needs the scratch back).
        // A dense 0.25m column tile pinning all MAX_BANDS layers up front
        // would hold ~126MB per tile
        // before capturing anything, and real builds usually stop a few
        // bands up.
        std::vector<F32> mBandTop;
        std::vector<F32> mBandUnder;
        std::vector<U8> mBandFlags;
        S32 mAllocBands = 0;

        // <SS:Nexii> The store: per column, up to SS_WF_MAX_SPANS
        // (ssworldfield.cpp) solid spans as [bottom, top] pairs, col-major
        // (col * SS_WF_MAX_SPANS + slot), NO_SURFACE top where the slot is
        // empty. Air is everything between spans, plus the gap below the
        // lowest span and above the highest one; the conversion guarantees
        // every stored gap is at least the slab threshold tall (shorter gaps
        // merge into the surrounding body), so a wall standing on a floor
        // never reads as a hollow shell and a room is one air interval
        // whatever band its floor and ceiling landed in.
        std::vector<F32> mSpanBottom;
        std::vector<F32> mSpanTop;
        std::vector<U8> mSpanFlags;

        // Dirty rectangle in cells; empty = whole tile. The re-peel renders
        // only this sub-frustum and splices only these columns.
        S32 mDirtyX0 = 0, mDirtyY0 = 0, mDirtyX1 = 0, mDirtyY1 = 0;

        // <SS:Nexii> The flood's output, one label/depth per air gap: gap k of
        // a column with n spans sits beneath span k (gap 0 below everything,
        // gap n above the highest span), col-major
        // (col * (SS_WF_MAX_SPANS + 1) + k) so a column's gaps are contiguous.
        // AIR_SOLID marks "no such gap". Valid only while mAirSerial matches
        // mGeomSerial - an edit invalidates them until the flood re-runs on
        // the next commit.
        std::vector<U8> mGapLabel;
        std::vector<U16> mGapDepth;
        U32 mAirSerial = 0;

        // <SS:Nexii> The base sweep's band count for this tile - scratch
        // slots at or above this index belong to Z bisection, and a rect
        // re-peel clears their columns before re-capturing so stale sub-band
        // bodies never survive an edit.
        S32 mBaseBands = 0;

        // <SS:Nexii> The precomputed acoustic lattice, built by the flood's
        // worker job from the span store. Per vertical ring (one every few
        // metres of height up to the capture ceiling, mWall holds
        // rings * mLatRes² entries), per lattice cell, per cardinal
        // (+X, -X, +Y, -Y): the horizontal wall distance in metres. Valid
        // while mSerial matches mGeomSerial - the same staleness gate the
        // labels ride.
        struct Acoustic
        {
            S32 mLatRes = 0;        // lattice cells per axis
            F32 mLatCell = 0.f;     // lattice cell size, metres
            F32 mCeiling = 0.f;     // the capture ceiling the rings span
            std::vector<F32> mWall; // flat [ring][y * mLatRes + x][4]
            U32 mSerial = 0;
        };
        Acoustic mAcoustic;

        U32 mGeomSerial = 1;

        S32 mBandTarget = 0;       // bands the next/active build runs

        F64 mCaptureTime = 0.0;
        F64 mLastTouched = 0.0;
        bool mDirty = false;
        bool mValid = false;
    };

    // <SS:Nexii> One capture node on the build worklist: a scratch slot and
    // the Z interval it captures. The base sweep's nodes are the uniform
    // bands; Z bisection enqueues finer intervals beneath captured bodies
    // (a column's bodies are vertically disjoint, so topmost-in-interval
    // queries partition them - every body down to the minimum interval is
    // found without any peeling).
    struct CaptureNode
    {
        S32 mSlot = 0;       // scratch band slot the results splice into
        F32 mZ0 = 0.f;       // interval floor
        F32 mZ1 = 0.f;       // interval ceiling
        bool mBisect = false;// a bisection node (never triggers the empty-run stop)
        bool mHung = false;  // set by applyBand: unexplored space beneath the body
    };

    struct Build
    {
        bool mActive = false;
        U64 mRegionHandle = 0;
        // <SS:Nexii> The capture worklist: nodes to capture, in order. Stage 1
        // fills it with the uniform band sweep; Z bisection appends finer
        // nodes beneath captured bodies (breadth-first, so each level
        // finishes before the next starts). The XY quadtree is the next
        // generalisation: nodes become (XY rect, Z interval, texel size).
        std::vector<CaptureNode> mWorklist;
        size_t mCursor = 0;        // next worklist entry
        S32 mPass = 0;             // capture pass within the node: 0 down, 1 up
        bool mRectOnly = false;    // re-peeling the dirty rectangle only
        S32 mRectX0 = 0, mRectY0 = 0, mRectX1 = 0, mRectY1 = 0;
        S32 mRectRes = 0;          // square capture resolution covering the rect
        F32 mRectHalf = 0.f;       // world half-extent of the rect frustum
        LLVector3 mRectCentre;     // agent-space centre of the rect
        std::vector<F32> mDepth[2];// the node's two depth readbacks (down, up)
        bool mNodeHung = false;    // set by applyBand: the node's body hangs - unexplored space beneath
        S32 mEmptyRun = 0;         // consecutive empty base nodes seen by the live build
        bool mChanged = false;     // any spliced column differed from what was stored
        bool mJustCaptured = false;// a pass was rendered and its readback landed; apply it next step
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    const Tile* tileAt(const LLVector3& pos_agent) const;

    bool needsBuild(const Tile& tile) const;
    Tile* pickBuildTarget();

    bool advanceBuild();

    bool capturePass(Tile& tile, const CaptureNode& node, S32 pass);
    void applyBand(Tile& tile, const CaptureNode& node);
    void commitBuild(Tile& tile);

    // Grow the tile's flat band arrays to cover at least `bands` layers,
    // filling the new region with open-sky (NO_SURFACE) cells. Readers index
    // below mBandCount, which only grows after applyBand ensured the band it
    // spliced; the flood snapshots mBandCount bands, so the invariant covers
    // it too.
    void ensureBands(Tile& tile, S32 bands);

    // Fold the capture scratch into the column span store for a rectangle of
    // columns: per column, the per-band bodies sort bottom-up, merge across
    // band planes and any gap thinner than the assumed slab, and the first
    // span extends to the world floor when the gap beneath it is thinner
    // still. Runs at commit, before the flood is scheduled.
    void computeSpans(Tile& tile, S32 x0, S32 y0, S32 x1, S32 y1);

    // Which air gap of a column contains z: 0 none (inside a body), otherwise
    // 1 + the gap index (gap 0 below the lowest span, gap n above the
    // highest). Bounds of the air found come back for callers that want them.
    S32 gapAt(const Tile& tile, size_t col, F32 z, F32& g0, F32& g1) const;

    // The enclosure spectrum's shared body - label lookup, sub-band
    // resolution and the depth ramp - against a caller-resolved region/tile.
    F32 enclosureInRegion(const LLViewerRegion* regionp, const Tile& tile,
                          const LLVector3& pos_agent) const;

    // Post the connectivity flood for a committed tile to the general worker
    // queue. Snapshot in, labels out; the completion stores them only if the
    // tile's geometry serial has not moved underneath the walk.
    void scheduleFlood(Tile& tile);

    void evict();

    std::map<U64, Tile> mTiles;
    Build mBuild;

    // Registered when the surface-field source switch is on; the field reads
    // the setting rather than holding a handle, so the switch is one settings
    // entry and the wet field's plumbing stays untouched. The interest
    // refcounts live in file statics so a handle's deleter stays valid for
    // the process's life regardless of singleton teardown order.
    bool surfaceTopDemanded() const;

    LLRenderTarget mTarget;

    // <SS:Nexii> One band readback in flight, served by SSGLReadback. mTarget must not be re-rendered (or torn down) until the outstanding read lands: advanceBuild()/capture gate on mReadbackPending, and a clear requested mid-read defers to the read's completion.
    bool mReadbackPending = false;
    bool mClearPending = false;

    // One flood in flight at a time; a tile committing while one runs simply
    // schedules again on its own commit. A generation counter, not a pointer,
    // decides whether a finished walk still applies - clear() and eviction
    // both move it.
    bool mFloodBusy = false;
    U32 mFloodGeneration = 0;

    F64 mNow = 0.0;
    F64 mLastBandAt = 0.0;
    F32 mLastCaptureMS = 0.f;
    U32 mCaptureCount = 0;
    U32 mDirtyCaptures = 0;
};

#endif
