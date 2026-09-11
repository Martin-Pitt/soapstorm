/**
 * @file ssworldfield.h
 * @brief Atmo Magic: the shared world field.
 *
 *        The world field has no capture and no store of its own. The census
 *        navmesh (SSNavMesh) already rasterises every region it covers into
 *        16 m columns split into height bands, and every band build already
 *        reads its rcHeightfield out as a SpanSheet - a 64 x 64 grid of
 *        0.25 m cells, each holding that band's solid spans. Those sheets stay
 *        on the navmesh's bands. The world field is the classification over
 *        them: per region it snapshots the band sheets (shared_ptr copies, no
 *        geometry copied), and one worker job materialises the region's cell
 *        grid, walks the air, and bakes the acoustic probes. The finished grid
 *        is swapped in whole, under a serial gate, and is immutable from then
 *        until the next one replaces it.
 *
 *        What a cell answers: the solid spans it holds (ray occlusion - see
 *        traceSolid), and per air gap between them an outdoors/sheltered/
 *        interior label with the covered distance behind it. A query point
 *        with no air cell at its own height - inside a body, or clipped into
 *        geometry - walks DOWN to the first air gap within
 *        SSWorldFieldGroundReach metres; nothing within reach is open air.
 *
 *        The outdoors rule is geometric rather than topological: an opening
 *        hands its covered neighbours a reach budget scaled by the gap height
 *        at the opening (gap_height * cot(SSWorldFieldOpenAngle)),
 *        re-evaluated from the LOCAL gap height at every step, so a gap that
 *        pinches under a low beam stops carrying outdoors while ground under a
 *        sky platform far overhead keeps it for as far as the platform runs.
 *
 *        See doc/atmo_magic_worldfield.md - the handoff section there is the
 *        contract the wind flow and acoustics workstreams build against.
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

#include "llsingleton.h"
#include "ssacousticcore.h"
#include "ssrainshadow.h"
#include "ssnavmesh.h"
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
    // What a consumer wants out of the field. Grids are classified for every
    // region in reach regardless; a claim is what turns the optional tiers on
    // (today: ACOUSTIC gates the stochastic reverb bake).
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

    // <SS:Nexii> The edit fan-out, now only a pass-through to the census: the field itself has nothing to dirty, because its geometry is the navmesh's sheets and the navmesh rebuilds the bands an edit touched on its own schedule. The field notices through the sheet-set stamp on its next settle. [interaction: SSWorldFieldShapes::markDirty, SSNavMesh::collectSheets]
    static void markDirty(const LLVector3& pos_agent, F32 radius);

    void clear();

    // Teardown hook kept for the display shutdown path; the field holds no GL resources any more, so this is clear().
    void shutdownGL();

    void update();

    // The store's cell under a point: grid state, every span, the gap labels, and the point's own air verdict. [interaction: SSFloaterNavMesh mark]
    void dumpColumn(const LLVector3& pos_agent, std::vector<std::string>& out) const;

    // The landing-surface view - SSRainShadowMap::buildSurfaceGrid's exact
    // contract (region-anchored n x n grid, first thing a falling drop meets,
    // water and heightmap fallbacks included, geometry serial for the retrace
    // gate). Consumers switch sources without changing anything else.
    bool buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out);

    void validTiles(std::vector<std::pair<U64, U32> >& out) const;

    // The topmost surface at a point, absolute Z. False if the cell is
    // unmapped (no grid for the region, or the column is fully sky).
    bool surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const;

    // Whether the point has structure above it (sheltered), and if so how far
    // up to the column's sky-open top - the burial measure the soundscape
    // currently derives from the wind tile's column top.
    bool coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const;

    // The richer form the soundscape's probe cycle wants: whether anything
    // stands over the point at all, the nearest such surface's altitude (the
    // space's ceiling), and the column's sky-open top. Burial (the build
    // stacked between ceiling and sky) is their difference, floor-count aware
    // where the single column-top subtraction never was. False when no grid
    // covers the point; the caller keeps its raycasts for that.
    bool coverageDetail(const LLVector3& pos_agent, bool& covered,
                        F32& ceiling_z, F32& column_top_z) const;

    // <SS:Nexii> Air connectivity over the cell grid: per cell, the air gaps between its solid spans, each labelled by whether outdoors air can reach it and how far that reach had to carry. The classification is three-way - OUTDOORS where the cell is open to the sky above OR sits within one gap-height cot(theta) reach of air that is, SHELTERED where an opening still carries to it but further than that, INTERIOR once the reach budget is spent or the walk never got in. The budget an opening hands inward is gap_height * cot(SSWorldFieldOpenAngle) * SS_WF_SHELTER_MULT, re-evaluated from the LOCAL gap height at every cell step and spent one cell width per step: ground under a 200 m sky platform keeps a 200 m budget and reads outdoors across the whole footprint, while a 2.4 m room's door hands about 2.4 m of outdoors and roughly 10 m of shelter past that. Run on the General work queue over a snapshot of the navmesh's band sheets, published whole against the grid serial so a stale answer is never served and a half-built grid is never visible.
    enum EAirLabel : U8
    {
        AIR_SOLID = 0,      // no air here - the point is inside a body
        AIR_OUTDOORS,       // open to the sky, or within one gap-height reach of air that is
        AIR_SHELTERED,      // covered, but an opening's budget still carries to it
        AIR_INTERIOR,       // beyond every opening's reach, or sealed
        AIR_UNKNOWN         // no grid for the region, or the grid is stale
    };
    U8 airLabelAt(const LLVector3& pos_agent) const;

    // <SS:Nexii> The covered distance behind airLabelAt: METRES of covered travel from the opening whose budget reached the point (0 when the point is open to the sky in its own column), or AIR_DEPTH_UNREACHED when the cell is interior, off-grid, or the grid is stale. Metres, not graph hops: the pre-navmesh store counted cell steps and the figure moved whenever SSWorldFieldCell did. [interaction: SSAcoustic::Probe::mTravelM]
    static constexpr U32 AIR_DEPTH_UNREACHED = 0xFFFFu;
    U32 airDepthAt(const LLVector3& pos_agent) const;

    // <SS:Nexii> The enclosure spectrum: 0 outdoors, 1 interior, and between
    // them d / (d + tau) on the covered distance d, where tau is the LOCAL gap
    // height's own cot(theta) reach (floored at SS_WF_ENCLOSURE_TAU_M). The
    // same geometry that sets the reach budget sets the saturation length, so
    // a low ceiling encloses within a couple of metres while a hall or a sky
    // platform's underside stays near 0 for as far as it runs. Returns -1
    // whenever there is no current answer - no grid, stale after a rebuild, or
    // off-grid - and the caller keeps its own probe answer for that. This is
    // the scalar the soundscape blends its ambiences on.
    F32 enclosureAt(const LLVector3& pos_agent) const;

    // The bulk form for callers that walk one region's cells (the surface
    // field's window stitch): region and grid resolved once, the same answer
    // per point.
    F32 enclosureAtRegion(U64 region_handle, const LLVector3& pos_agent) const;

    // <SS:Nexii> The ACOUSTIC channel's wall profile: the nearest probe of the
    // listener's lattice cell (its 8 neighbours back it up when the cell is
    // probe-less), its 8-direction profile's four cardinals out in the
    // side-probe contract (metres, saturated at the reach cap). False when the
    // grid or lattice is not current and the caller keeps its raycasts.
    bool acousticAt(const LLVector3& pos_agent, F32 wall[4]) const;

    // <SS:Nexii> The occlusion trace (the doc's Part 3, the brief's realtime ask):
    // a 2D DDA over the cells the segment a->b crosses; per cell, the segment's
    // z-interval is intersected against the cell's span intervals; solid metres
    // and body crossings accumulate. Exact against the grid, no scene raycast,
    // main thread, ~4 cells per metre at the 0.25 m cell. False when no current
    // grid covers the segment (either endpoint off-grid counts - a partial count
    // would read as confidently open air, the optimistic direction for audio);
    // the caller keeps its heuristic for that.
    bool traceSolid(const LLVector3& a, const LLVector3& b, F32& solid_m, S32& crossings) const;

    // <SS:Nexii> The listener blend's raw material (the doc's Part 3): the probes of
    // the listener's own gap in its lattice cell plus graph-adjacent probes,
    // inverse-distance weighted, at most four. NEVER a raw trilinear tap over the
    // lattice - the nearest probe through a wall is exactly the one that must not
    // contribute. False when the grid's probes are stale (rebuild pending) or the
    // cell carries no probes; the caller keeps its raycast classification.
    struct ProbeSample
    {
        LLVector3 mPos;             // agent-space probe position
        F32 mWeight = 0.f;          // 1/(d^2+1), unnormalised; blend then normalise
        F32 mRT60 = 0.f;
        F32 mSkyOpen = 0.f;
        F32 mVolume = 0.f;
        F32 mTravelM = -1.f;        // travel-to-outdoors, metres
        F32 mWall[8];
        S32 mSpaceClass = 0;        // 0 outdoor / 1 sheltered / 2 small / 3 medium / 4 big
        S32 mSizeClass = 0;         // 0 tight / 1 medium / 2 open
        ProbeSample() { for (S32 i = 0; i < 8; ++i) mWall[i] = 64.f; }
    };
    bool probesAt(const LLVector3& pos_agent, ProbeSample out[4], S32& count) const;

    // <SS:Nexii> Per-source propagation (the doc's Part 3): snap the source and the
    // listener to their gaps' probes in their lattice cells, run Dijkstra over the
    // baked probe graph (edge cost = length / aperture + the fixed portal loss), and
    // read the figures back. Reads only the immutable baked graph, so it is safe on
    // the main thread between rebuilds; the serial gate keeps a stale graph from ever
    // answering. Thunder's travel time, muffle and arrival direction come from here;
    // occlusionGain's diffracted floor does too.
    struct Propagation
    {
        F32 mDirectM = 0.f;         // straight-line metres source -> listener
        F32 mPathM = 0.f;           // geometric path metres along the graph
        F32 mCostM = 0.f;           // the Dijkstra cost (aperture + portal weighted)
        S32 mPortals = 0;           // OUTDOORS-boundary crossings on the path
        F32 mMuffle = 0.f;          // 0..1, from portals and the path-vs-direct ratio
        bool mHaveArrival = false;  // mArrivalDir valid: the direction from the
                                    // listener's probe to its Dijkstra parent - sound
                                    // entering through a doorway renders from it
        LLVector3 mArrivalDir;
        std::vector<LLVector3> mPath;   // node positions source -> listener (capped)
    };
    bool propagationQuery(const LLVector3& source, const LLVector3& listener, Propagation& out) const;

    // <SS:Nexii> The debug export the V10 info view reads: the grid's probes and
    // links within range of a centre point, portal flags resolved per probe, and the
    // listener's own blend set. Read-only over the baked channel.
    struct AcousticDebug
    {
        struct Probe
        {
            LLVector3 mPos;
            F32 mGapBottom = 0.f, mGapTop = 0.f;
            U8 mLabel = 4;
            F32 mRT60 = 0.f;
            F32 mSkyOpen = 0.f;
            F32 mVolume = 0.f;
            F32 mTravelM = -1.f;
            S32 mSpaceClass = 0;
            S32 mSizeClass = 0;
            bool mPortal = false;
            bool mHaveBundle = false;
        };
        struct Link
        {
            S32 mA = -1, mB = -1;
            bool mPortal = false;
        };
        bool mValid = false;
        S32 mLatRes = 0;
        F32 mLatCell = 0.f;
        S32 mProbeCount = 0;        // the grid's full probe count, drawn set aside
        S32 mBundleCount = 0;
        S32 mLinkCount = 0;
        S32 mPortalCount = 0;
        std::vector<Probe> mProbes;
        std::vector<Link> mLinks;
        std::vector<S32> mListenerProbes;   // indices into mProbes of the blend set
    };
    bool acousticDebug(U64 region_handle, const LLVector3& centre_agent, F32 range_m,
                       AcousticDebug& out) const;

    // The share of a region's air cells the classification actually labelled -
    // 1.0 once a grid is published and current, 0 before the first one.
    F32 airCoverage(U64 region_handle) const;

    // <SS:Nexii> Drainage topology over one landing-surface grid - the DRAINAGE_NETWORK channel core, materialised synchronously over whatever SurfaceGrid the caller already holds (the surface field's geometry, at its own n). A Barnes priority flood fills every depression to its spill elevation; a cell below that level is a pool member (standing water, the figure that retires the surface field's local dips check); flow directions are D8 down the *filled* surface, so a pool's water drains toward its spill outlet rather than into its own floor - except across an eave, the discontinuity a raw drop steeper than a roof pitch and at least 0.75 m tall marks: water arriving there leaves into the air, so the drop is not a descent and the cell ends the surface. Accumulation then routes contributing area in square metres down the D8 in descending spill order; a cell whose outlet chain ends keeps its catchment, which is the figure the runoff shed reads at the lips. Nothing is cached here: the grid carries the geometry serial and the caller already gates retraces on it.
    struct Drainage
    {
        std::vector<F32> mSpill;      // fill elevation per cell, NODATA where unmapped
        std::vector<U8> mPool;        // 1 = cell sits under its spill level (depression member)
        std::vector<U8> mD8;          // outflow cell on the filled surface, 3x3-indexed
                                      // ((dy+1)*3 + (dx+1)); 4 = no outflow (sink or drain edge)
        std::vector<F32> mCatch;      // contributing area in m2 routed through each cell,
                                      // retained where the outlet chain ends
    };
    static bool buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out);

    bool tileValid(U64 region_handle) const;
    U32 geometrySerial(U64 region_handle) const;
    // A rebuild is pending: the navmesh's sheet set moved since the published grid was classified.
    bool gridStale(U64 region_handle) const;

    // Stats
    S32 tileCount() const { return (S32)mTiles.size(); }
    U32 gridBuilds() const { return mGridBuilds; }
    F32 lastBuildMS() const { return mLastBuildMS; }
    S32 resolution() const;
    F32 cellSize() const;
    F32 ceilingAt(const LLVector3& pos_agent) const;
    S32 sheetsAt(const LLVector3& pos_agent) const;
    F64 tileAge(const LLVector3& pos_agent) const;

    // <SS:Nexii> The world field's own overlay: what the cell grid holds, what the air classification decided, and what the drainage pass reads - view picked by SSWorldFieldDebugView, distance-thinned like the wind flowmap's overlay.
    void renderDebug();

private:
    static constexpr U32 MAX_TILES = 4;
    // <SS:Nexii> How long a region's sheet set must hold still before it is
    // reclassified. A region under edit republishes bands in a trickle and
    // SSNavMesh::regionSettled goes true between them; without this the field
    // would run a region-wide walk for every one of them.
    static constexpr F64 SETTLE_DEBOUNCE = 0.5;

    // <SS:Nexii> One region's published cell grid. Every array below is written
    // once by a worker job and swapped in whole on the main thread - nothing
    // mutates a live grid in place, which is what lets traceSolid and the
    // acoustic queries read it on the main thread with no lock and no torn
    // state. The geometry is the navmesh's band sheets flattened into one
    // region-anchored grid of mRes x mRes cells at mCell metres; every cell
    // holds up to SS_WF_MAX_SPANS solid spans, and the gaps between, below and
    // above them are the air the classification labels.
    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;
        F32 mCell = 0.f;
        F32 mCeiling = 0.f;         // the grid's top: above it a column is open sky
        S32 mSheets = 0;            // band sheets the published grid was built from

        // The cell grid, col-major by span slot ([k * res * res + col]) so a
        // slot is one contiguous plane - the layout SSAcoustic::Snap walks.
        std::vector<F32> mSpanBottom;
        std::vector<F32> mSpanTop;
        std::vector<U8> mSpanFlags;

        // <SS:Nexii> The classification, one label/distance per air gap: gap k
        // of a cell with n spans sits beneath span k (gap 0 below everything,
        // gap n above the highest), col-major
        // (col * (SS_WF_MAX_SPANS + 1) + k) so a cell's gaps are contiguous.
        // AIR_SOLID marks "no such gap". mGapDepth is DECIMETRES of covered
        // travel from the opening that reached the gap, AIR_DEPTH_UNREACHED
        // where nothing did; airDepthAt rounds it to metres.
        std::vector<U8> mGapLabel;
        std::vector<U16> mGapDepth;

        // <SS:Nexii> The ACOUSTIC channel's bake, built by the same worker job from
        // the same sheet snapshot (doc/atmo_magic_acoustics.md Parts 1-4). Gap-anchored
        // probes (ear at floor + 1.2 m, ceiling under a roof, intermediates every 4 m
        // for tall gaps), the probe graph (vertical links by construction, horizontal
        // links validated by cell-grid rays, aperture factors, portal flags), and the
        // tier A statistic bake per probe (8-direction wall profile, sky openness,
        // room volume/area, Sabine RT60, space/size classes, travel-to-outdoors).
        // Tier B's stochastic bundles fill the bundle fields in later batch jobs,
        // each store-back serial-gated individually.
        struct Acoustic
        {
            S32 mLatRes = 0;            // lattice cells per axis
            F32 mLatCell = 0.f;         // lattice cell size, metres
            F32 mCeiling = 0.f;         // the grid ceiling the probes span

            std::vector<SSAcoustic::Probe> mProbes;      // CSR per lattice cell:
            std::vector<S32> mCellStart;                 // [cell] first probe index,
                                                         // size latRes^2 + 1
            std::vector<SSAcoustic::Link> mLinks;        // each once, a/b node indices
            std::vector<S32> mAdjStart;                  // CSR: [probe] first adjacency
            std::vector<S32> mAdjNode;                   // neighbour probe per slot
            std::vector<F32> mAdjCost;                   // Dijkstra cost per slot
        };
        Acoustic mAcoustic;

        // <SS:Nexii> One stamp and two serials, which is the whole staleness
        // story. mWantStamp is the sheet-set signature the navmesh last
        // reported for a settled region; when it moves, mGeomSerial moves with
        // it and the region owes a classification. mGridSerial is the serial
        // the published grid carries, so mGridSerial != mGeomSerial means
        // exactly "a rebuild is owed" - that is what current() tests. A worker
        // result is only stored when its serial still matches AND the
        // generation counter has not moved (clear/evict both move it, so a
        // walk in flight for a dropped region can never land on a grid the
        // same region handle re-created and restarted at serial 0).
        U64 mWantStamp = 0;
        U32 mGeomSerial = 0;
        U32 mGridSerial = 0;

        F64 mBuiltAt = 0.0;
        F64 mLastTouched = 0.0;
        F64 mSettledAt = 0.0;       // when mWantStamp last moved: the rebuild debounce
        bool mValid = false;        // a grid is published and safe to read
    };

    const Tile* tileAt(const LLVector3& pos_agent) const;

    // Whether a grid's labels, depths and acoustic bake describe the current
    // sheet set. Span reads (surfaceTop, coverage, traceSolid) are served from
    // a stale-but-consistent grid; label reads are not.
    static bool current(const Tile& tile) { return tile.mValid && tile.mGridSerial == tile.mGeomSerial; }

    // Which air gap of a cell contains z: 0 below the lowest span, n above the
    // highest, or -1 when z falls inside a body. Bounds of the air found come
    // back for callers that want them.
    S32 gapAt(const Tile& tile, size_t col, F32 z, F32& g0, F32& g1) const;

    // <SS:Nexii> The point query of decision 2: the gap a 3D point belongs to, or
    // - for a point with no air at its own height, which is only ever a point
    // inside a body - the first gap found walking DOWN within
    // SSWorldFieldGroundReach metres. Nothing within reach is open air, which
    // the caller reads as outdoors. Returns -1 only when the walk found
    // nothing inside the grid at all. [interaction: airLabelAt, enclosureInRegion]
    S32 resolveGap(const Tile& tile, size_t col, F32 z, F32& g0, F32& g1) const;

    // The enclosure spectrum's shared body - gap resolution, label lookup and
    // the gap-height-scaled depth ramp - against a caller-resolved region/grid.
    F32 enclosureInRegion(const LLViewerRegion* regionp, const Tile& tile,
                          const LLVector3& pos_agent) const;

    // The listener blend's probe set (own-gap probes of the lattice cell plus their
    // graph-adjacent probes, deduped) as node indices - the shared body of probesAt
    // and acousticDebug. Returns the count written, 0 when the channel has no
    // current answer for the cell.
    S32 listenerProbeSet(const Tile& tile, const LLVector3& pos_agent, S32* out, S32 max_out) const;

    // Follow the navmesh: create a grid slot for every region in reach, drop
    // the ones that left, and move a region's geometry serial once the navmesh
    // has nothing queued for it and its sheet set actually changed.
    void navSettle();
    bool regionNear(const LLViewerRegion* regionp, const LLVector3& cam) const;

    // Post one region's classification to the General work queue: snapshot the
    // navmesh's band sheets (shared_ptr copies), materialise the cell grid,
    // walk the air, bake the acoustics, swap the result in under the serial
    // and generation gates.
    void scheduleGrid(Tile& tile);

    void evict();

    // Registered when the surface-field source switch is on; the field reads
    // the setting rather than holding a handle, so the switch is one settings
    // entry and the wet field's plumbing stays untouched. The interest
    // refcounts live in file statics so a handle's deleter stays valid for
    // the process's life regardless of singleton teardown order.
    bool surfaceTopDemanded() const;

    std::map<U64, Tile> mTiles;

    // One classification in flight at a time; a region that settles while one
    // runs simply gets its turn on a later update. A generation counter, not a
    // pointer, decides whether a finished job still applies - clear() and
    // eviction both move it.
    bool mBuildBusy = false;
    bool mGateWasOff = false;          // Atmo resolved no environment last frame: grids kept, rebuilds paused
    bool mWarnedNoNavMesh = false;     // the "on, but no geometry source" warning is said once per edge
    U32 mGridGeneration = 0;

    F64 mNow = 0.0;
    F32 mLastBuildMS = 0.f;
    U32 mGridBuilds = 0;
};

#endif
