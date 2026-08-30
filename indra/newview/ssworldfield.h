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
    // later its probe passes are the union of what every currently claimed
    // channel needs; today that is SURFACE_TOP alone.
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
    // dropping the last handle lets the region stop paying for the channel.
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

private:
    static constexpr S32 MAX_BANDS = 24;
    static constexpr U32 MAX_TILES = 4;
    static constexpr F64 DIRTY_MIN_INTERVAL = 2.0;
    static constexpr F64 BAND_MIN_INTERVAL = 0.05;
    // This many consecutive bands with nothing in them end a full build:
    // bands are swept bottom-up, so empty runs only occur above all content
    // that the ceiling setting covers. Skyboxes above the resulting ceiling
    // are invisible to the field until SSWorldFieldCeiling is raised - the
    // same practical shape as the wind map's band.
    static constexpr S32 EMPTY_BANDS_TO_STOP = 3;

    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;
        F32 mCell = 0.f;

        S32 mBandCount = 0;        // effective bands; bands [0, mBandCount) are live
        F32 mBandHeight = 16.f;

        // Per band, per column: the highest surface inside the band, absolute
        // Z, NO_SURFACE where the band is open there. Flat [band][y * res + x].
        std::vector<F32> mBandTop;
        std::vector<U8> mBandFlags;

        // Dirty rectangle in cells; empty = whole tile. The re-peel renders
        // only this sub-frustum and splices only these columns.
        S32 mDirtyX0 = 0, mDirtyY0 = 0, mDirtyX1 = 0, mDirtyY1 = 0;

        U32 mGeomSerial = 1;

        S32 mBandTarget = 0;       // bands the next/active build runs

        F64 mCaptureTime = 0.0;
        F64 mLastTouched = 0.0;
        bool mDirty = false;
        bool mValid = false;
    };

    struct Build
    {
        bool mActive = false;
        U64 mRegionHandle = 0;
        S32 mBand = 0;
        bool mRectOnly = false;    // re-peeling the dirty rectangle only
        S32 mRectX0 = 0, mRectY0 = 0, mRectX1 = 0, mRectY1 = 0;
        S32 mRectRes = 0;          // square capture resolution covering the rect
        F32 mRectHalf = 0.f;       // world half-extent of the rect frustum
        LLVector3 mRectCentre;     // agent-space centre of the rect
        std::vector<F32> mDepth;   // last band's depth readback
        S32 mEmptyRun = 0;         // consecutive empty bands seen by the live build
        bool mChanged = false;     // any spliced column differed from what was stored
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    const Tile* tileAt(const LLVector3& pos_agent) const;

    bool needsBuild(const Tile& tile) const;
    Tile* pickBuildTarget();

    bool advanceBuild();

    bool captureBand(Tile& tile);
    void applyBand(Tile& tile);
    void commitBuild(Tile& tile);

    void evict();

    static F32 bandTopZ(S32 band, F32 band_height) { return (F32)(band + 1) * band_height; }

    std::map<U64, Tile> mTiles;
    Build mBuild;

    // Registered when the surface-field source switch is on; the field reads
    // the setting rather than holding a handle for it, so the switch is one
    // settings entry and the wet field's plumbing stays untouched. The
    // interest refcounts live in file statics so a handle's deleter is valid
    // for the life of the process regardless of singleton teardown order.
    bool surfaceTopDemanded() const;

    LLRenderTarget mTarget;

    F64 mNow = 0.0;
    F64 mLastBandAt = 0.0;
    F32 mLastCaptureMS = 0.f;
    U32 mCaptureCount = 0;
    U32 mDirtyCaptures = 0;
};

#endif
