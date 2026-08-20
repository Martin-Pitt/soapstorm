/**
 * @file ssrainshadow.h
 * @brief Atmo Magic rain shadow maps: per-region depth captures along the
 *        precipitation fall direction, cached and lazily refreshed, so drops
 *        land on roofs and bridges and sheltered spots stay dry.
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

#ifndef SS_RAINSHADOW_H
#define SS_RAINSHADOW_H

// <SS:Nexii> Atmo Magic rain shadow maps

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3math.h"
#include "v3dmath.h"

#include <map>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

class SSRainShadowMap : public LLSingleton<SSRainShadowMap>
{
    LLSINGLETON_EMPTY_CTOR(SSRainShadowMap);

public:
    // Called from the display loop right after generateSunShadow, where the
    // pipeline is in a known state. Captures at most one tile per call,
    // throttled, picking the stalest/dirtiest tile near the camera.
    void capture();

    // Marks the covering tile for a lazy recapture. Driven from SSAtmoMagic's
    // settle queue, so what arrives here has already been filtered down to
    // geometry that is actually part of the build.
    void markDirty(const LLVector3& pos_agent, F32 radius);
    void clearCache();

    // How many regions currently have a map cached, for the simulation floater
    S32 tileCount() const { return (S32)mTiles.size(); }
    U32 resolution() const;

    // Capture accounting, for the info overlay. A map that is not shadowing
    // what it should looks identical to one that is simply out of date, and
    // these are what tell the two apart: how many captures have run, how many
    // of those were forced by geometry changing rather than by the age or band
    // checks, how long since the last one, and how long it took.
    U32 captureCount() const { return mCaptureCount; }
    U32 dirtyCaptureCount() const { return mDirtyCaptures; }
    U32 dirtyTileCount() const;
    F32 lastCaptureMS() const { return mLastCaptureMS; }
    F64 lastCaptureAge() const;

    // Render Metadata > Rain Shadow: casts the map onto the region as an
    // actual shadow, draped over the ground where the rain does not reach.
    void renderDebug();

    // A captured tile resampled down to a coarse world-space surface: for a
    // regular grid of columns, the point precipitation lands on. This is the
    // same answer resolveColumn gives, taken for the whole region in one pass
    // so a consumer that needs the surface as a connected field - rather than
    // one column at a time - can have it without a quarter of a million
    // separate lookups. Region-local so it survives the agent-origin shift on
    // a region crossing, the way the tiles themselves do.
    enum
    {
        SURF_MAPPED   = 0x01,   // the capture saw a real surface here
        SURF_WATER    = 0x02,   // the cell belongs to the water plane
        SURF_FALLBACK = 0x04    // nothing captured; terrain heightmap guess
    };

    // The grid is anchored to the region, not to the capture: cell centres are
    // ((x + 0.5) * mCell, (y + 0.5) * mCell) in region-local XY, and mZ holds
    // the height of the surface found in each. That matters because the tile
    // itself is camera-relative - its band and footprint both move with the
    // camera - so a grid laid out in the tile's own space would put its cells
    // somewhere new every recapture, and anything derived from it would crawl
    // around as the camera moved.
    struct SurfaceGrid
    {
        U64 mRegionHandle = 0;
        S32 mN = 0;                     // cells per axis
        F32 mCell = 0.f;                // region width / mN, metres
        std::vector<F32> mZ;            // agent-space surface height per cell
        std::vector<U8> mFlags;         // SURF_* above; 0 means no surface at all
        U32 mGeomSerial = 0;            // geometry revision this was built from

        // Region-local centre of a cell
        F32 axis(S32 i) const { return ((F32)i + 0.5f) * mCell; }
    };

    // Resample the cached tile for a region into the grid above. False when
    // that region has no valid tile yet.
    bool buildSurfaceGrid(U64 region_handle, S32 n, SurfaceGrid& out);

    // Walk the captured depth outward from a coarse edge at the map's own full
    // resolution, and return the last point still on the upper surface. A grid
    // cell is metres across and the map is centimetres; this is what lets an
    // eave sit on the actual lip of the geometry rather than in the middle of
    // whichever cell happened to straddle it.
    bool refineEdge(U64 region_handle, const LLVector3& from_agent, const LLVector3& out_dir,
                    F32 max_dist, F32 tolerance, LLVector3& refined_agent) const;

    // Regions holding a valid tile, paired with the geometry revision that tile
    // was captured from. The revision only moves when something in the region
    // actually changed shape - not when the tile is recaptured because the
    // camera climbed or the wind turned - so a consumer can tell the difference
    // between "there is a new capture" and "there is new geometry".
    void validTiles(std::vector<std::pair<U64, U32> >& out) const;

    // Find where the precipitation column through pos_agent first hits
    // something, walking along the current fall direction. Falls back to
    // terrain/water height when no map covers the point; the result is
    // always usable. Returns whether map data was involved. When requested,
    // hit_normal receives the surface normal at the hit, derived from the
    // depth map's gradients (terrain/water normal on fallback).
    bool resolveColumn(const LLVector3& pos_agent, LLVector3& hit_pos_agent, bool& on_water,
                       LLVector3* hit_normal = nullptr);

private:
    struct Tile
    {
        U64 mRegionHandle = 0;
        U32 mRes = 0;
        std::vector<F32> mDepth;        // window-space depth, linear over [mNear, mFar]

        // Ortho basis in region-local coordinates so tiles survive the
        // agent-origin shift on region crossings
        // Centre of the near plane, region-local. Backed off upwind of the
        // band by enough that a tilted fall direction cannot leave part of the
        // region in front of it; depth is measured along mDir from here.
        LLVector3 mEyeRegion;
        LLVector3 mDir, mRight, mUp;
        F32 mHalfW = 0.f, mHalfH = 0.f;
        F32 mNear = 0.f, mFar = 0.f;
        F32 mBandTop = 0.f, mBandBottom = 0.f;

        F64 mCaptureTime = 0.0;
        bool mDirty = false;
        bool mValid = false;
        F64 mLastTouched = 0.0;         // for LRU eviction

        // Bumped when geometry inside the band changes, and copied across at
        // capture time. Recaptures that are only chasing the camera or the
        // wind leave both alone.
        U32 mGeomSerial = 1;
        U32 mCapturedSerial = 0;
    };

    // Debug shadow, draped over the region. A regular grid in region-local XY
    // holding the receiving surface and how much rain reaches it; region-local
    // so it survives the agent-origin shift on a region crossing, and cached
    // so the per-sample column resolve happens on capture rather than per
    // frame.
    struct ShadowMesh
    {
        S32 mN = 0;                     // samples per axis
        std::vector<LLVector3> mPos;    // region-local receiver points
        std::vector<F32> mShade;        // 0 heightmap guess, 1 surface the capture saw

        // What it was baked against, so it can tell when it has gone stale
        F64 mBuiltFrom = -1.0;
        LLVector3 mBuiltDir;
        F32 mBuiltStep = 0.f;
        F32 mBuiltFloor = 0.f;
        bool mBuiltSky = false;
    };

    void buildShadowMesh(const Tile& tile, ShadowMesh& mesh);

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    bool needsCapture(const Tile& tile) const;
    bool captureTile(Tile& tile);
    void evict();

    std::map<U64, Tile> mTiles;
    std::map<U64, ShadowMesh> mDebugMesh;
    LLRenderTarget mTarget;
    F64 mLastCapture = 0.0;

    U32 mCaptureCount = 0;      // captures run since login
    U32 mDirtyCaptures = 0;     // of those, ones a geometry change forced
    F32 mLastCaptureMS = 0.f;
};

// </SS:Nexii>

#endif // SS_RAINSHADOW_H
