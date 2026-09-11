import io

H = 'indra/newview/ssworldfield.h'
s = io.open(H, encoding='utf-8', newline='').read()


def rep(a, b, n=1):
    global s
    assert s.count(a) == n, (a[:90], s.count(a))
    s = s.replace(a, b)


# ---- A3: dead API ----------------------------------------------------------------
rep("""    // Whether the point has structure above it (sheltered), and if so how far
    // up to the column's sky-open top - the burial measure the soundscape
    // currently derives from the wind tile's column top.
    bool coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const;

""", "")
rep("""    bool tileValid(U64 region_handle) const;
    U32 geometrySerial(U64 region_handle) const;
    // A rebuild is pending: the navmesh's sheet set moved since the published grid was classified.
    bool gridStale(U64 region_handle) const;
""",
    """    // <SS:Nexii> The serial of the PUBLISHED grid - the spans and labels a query is
    // about to read, not the one the navmesh has already moved. Consumers gate their
    // retraces on it; 0 means no grid. gridStale is true while a reclassification is
    // owed, which is the build-progress signal a consumer needs and airCoverage is
    // not. [interaction: SSRainShadowMap::SurfaceGrid::mGeomSerial]
    U32 gridSerial(U64 region_handle) const;
    bool gridStale(U64 region_handle) const;
""")

# ---- B5: the bulk snapshot the wind flow workstream needs -------------------------
rep("""    // <SS:Nexii> Drainage topology over one landing-surface grid""",
    """    // <SS:Nexii> The bulk read, and the ONLY supported way to get world-field geometry
    // off the main thread. A published grid is immutable for its whole life - the
    // classification builds a whole new set of arrays and the completion swaps them in -
    // so handing out shared_ptrs to them is safe on any thread for as long as the
    // holder keeps them, even across an eviction or a reclassification. Take this on
    // the main thread, capture it by value into a worker job, and gate your result on
    // mSerial the way scheduleGrid does. Everything a consumer needs to index the grid
    // itself is here, so nobody has to reimplement the sheet-to-grid mapping (use
    // SSWorldFieldCore::buildGrid if you want your own resolution instead).
    // [interaction: SSWindFlowMap, SSSoundscape]
    struct GridView
    {
        // Spans, col-major by slot: [k * mRes * mRes + (y * mRes + x)], mMaxSpans slots,
        // NO_SURFACE top where the slot is empty.
        std::shared_ptr<const std::vector<F32> > mSpanBottom;
        std::shared_ptr<const std::vector<F32> > mSpanTop;
        std::shared_ptr<const std::vector<U8> > mSpanFlags;
        // Per gap node, [(y * mRes + x) * (mMaxSpans + 1) + k]: the air label and the
        // covered distance in DECIMETRES (AIR_DEPTH_UNREACHED where nothing reached it).
        std::shared_ptr<const std::vector<U8> > mGapLabel;
        std::shared_ptr<const std::vector<U16> > mGapDepth;
        // One byte per cell: 0 means NO band sheet covered it. Such a cell has no
        // geometry, which is not the same as empty sky - its gaps are AIR_UNKNOWN and a
        // consumer must not read its empty span list as open air.
        std::shared_ptr<const std::vector<U8> > mSurveyed;

        LLVector3 mOriginAgent;     // the grid's (0,0) cell corner in agent space
        S32 mRes = 0;               // cells per axis
        F32 mCell = 0.f;            // metres per cell
        F32 mCeiling = 0.f;         // where a cell's top gap ends; above it is open sky
        S32 mMaxSpans = 0;
        U32 mSerial = 0;            // the published grid's serial; gate results on it
        bool mStale = false;        // a reclassification is owed for this region

        explicit operator bool() const { return mRes > 0 && mSpanTop != nullptr; }
    };
    bool snapshotGrid(U64 region_handle, GridView& out) const;

    // <SS:Nexii> Drainage topology over one landing-surface grid""")

# ---- B4: the threading invariant, stated truthfully -------------------------------
rep("""    // <SS:Nexii> One region's published cell grid. Every array below is written
    // once by a worker job and swapped in whole on the main thread - nothing
    // mutates a live grid in place, which is what lets traceSolid and the
    // acoustic queries read it on the main thread with no lock and no torn
    // state.""",
    """    // <SS:Nexii> One region's published cell grid. Every array below is written
    // once by a worker job and swapped in whole on the main thread. The precise
    // invariant, because someone will move a reader off-thread on the strength of
    // it: THE ARRAYS ARE NEVER REALLOCATED OR REORDERED IN PLACE, and they are only
    // ever written from the MAIN THREAD. The one in-place write is tier B's
    // store-back, which fills per-probe statistic fields inside mAcoustic.mProbes
    // from its own main-thread completion; a reader sees either the tier A value or
    // the tier B one, never a torn probe, and mHaveBundle says which. Everything
    // else is swap-only, which is why a worker may hold the shared_ptrs handed out
    // by snapshotGrid indefinitely.""")

# ---- Tile gains the survey mask --------------------------------------------------
rep("""        // The cell grid, col-major by span slot ([k * res * res + col]) so a
        // slot is one contiguous plane - the layout SSAcoustic::Snap walks.
        std::vector<F32> mSpanBottom;
        std::vector<F32> mSpanTop;
        std::vector<U8> mSpanFlags;
""",
    """        // The cell grid, col-major by span slot ([k * res * res + col]) so a
        // slot is one contiguous plane - the layout SSAcoustic::Snap walks.
        // shared_ptr, not plain vectors, so snapshotGrid can hand a worker a
        // reference that survives the next classification and this grid's eviction.
        std::shared_ptr<std::vector<F32> > mSpanBottom;
        std::shared_ptr<std::vector<F32> > mSpanTop;
        std::shared_ptr<std::vector<U8> > mSpanFlags;

        // <SS:Nexii> One byte per cell: was this cell covered by any band sheet? The
        // census envelope is an agent-centred square while grids are kept for regions
        // within 64 m of the CAMERA, so a flycam routinely leaves part of a region
        // unsurveyed - and an unsurveyed cell has an empty span list, whose single
        // 0..ceiling gap would otherwise read as confidently OUTDOORS and suppress
        // every consumer's raycast fallback for geometry nobody has looked at.
        // [interaction: SSWorldFieldCore::classify's surveyed argument]
        std::shared_ptr<std::vector<U8> > mSurveyed;
        S32 mUnsurveyed = 0;        // cells with no sheet, for the dump and the HUD
""")
rep("""        std::vector<U8> mGapLabel;
        std::vector<U16> mGapDepth;
""",
    """        std::shared_ptr<std::vector<U8> > mGapLabel;
        std::shared_ptr<std::vector<U16> > mGapDepth;
""")

# ---- A2: the wedge watchdog -------------------------------------------------------
rep("""    bool mBuildBusy = false;""",
    """    // <SS:Nexii> One classification in flight. mBuildStartedAt is the watchdog: if the
    // mainloop queue ever drops the completion - a shutdown race, a queue closing - the
    // flag would otherwise stay true and retire the field for the process's life. After
    // BUILD_WATCHDOG_S with nothing landing, the generation moves (so a late completion
    // is refused rather than landing on a grid that has moved on) and the field tries
    // again.
    static constexpr F64 BUILD_WATCHDOG_S = 60.0;
    bool mBuildBusy = false;
    F64 mBuildStartedAt = 0.0;""")

# ---- A4: the listener probe scratch ------------------------------------------------
rep("""    S32 listenerProbeSet(const Tile& tile, const LLVector3& pos_agent, S32* out, S32 max_out) const;""",
    """    S32 listenerProbeSet(const Tile& tile, const LLVector3& pos_agent, S32* out, S32 max_out) const;

    // <SS:Nexii> listenerProbeSet's dedupe marks, kept across calls instead of being
    // allocated and zeroed per call: it ran a fresh allocate-and-memset of the full
    // probe count on every probesAt, on the main thread, at the soundscape's 50 ms
    // cycle - including on the paths that then answered false. Stamped with a
    // monotonic counter so it never needs clearing. Mutable because the query is
    // const; main thread only, like every query.
    mutable std::vector<U32> mProbeMark;
    mutable U32 mProbeStamp = 0;""")

io.open(H, 'w', encoding='utf-8', newline='').write(s)
print('ok', H)
