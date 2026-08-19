/**
 * @file sswindflow.h
 * @brief Atmo Magic wind flowmap: a per-region volume describing how wind
 *        actually moves through the build, rather than the single uniform
 *        vector the weather parameters carry.
 *
 *        A top-down and a bottom-up ortho depth capture give the solid span of
 *        every column. Those are sliced into a handful of adaptive horizontal
 *        slabs, and a divergence-free field is solved over the result on the
 *        GPU, so air accelerates through alleys, piles up against windward
 *        faces, lifts over rooftops and goes calm in courtyards.
 *
 *        Anchored to the region and solved once, the way the rain shadow maps
 *        are: a static flowmap of the build, not a running simulation. It
 *        rebuilds when the build changes underneath it, when the ambient wind
 *        changes, or when the camera moves to a different sky track.
 *
 *        Consumed by the ambient audio mix and by precipitation advection.
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

#ifndef SS_WINDFLOW_H
#define SS_WINDFLOW_H

// <SS:Nexii> Atmo Magic wind flowmap

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3math.h"
#include "v4math.h"

#include <glm/mat4x4.hpp>

#include <map>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

// Hard ceiling on slabs; the shaders size their uniform arrays to this
// Slabs are what give the mask its vertical resolution. Too few and a
// building is smeared across a slab thick enough that the air treats it as a
// haze to push through rather than a wall to go around.
const S32 SS_WIND_MAX_SLICES = 16;
const S32 SS_WIND_MIN_SLICES = 2;

// Levels in the horizontal pressure pyramid, finest first. Jacobi converges a
// feature of wavelength L cells in roughly L^2 passes, so at a metre per cell
// an affordable pass count only ever settles the few-metre detail around a
// corner - the region-long pressure difference that drives a jet down a lane
// is still barely started when the solve stops. Solving a halved grid first
// and handing its answer up as the starting guess settles the long wavelengths
// on a grid where they are short, at a quarter of the cost per level.
// Coarsening is horizontal only: the slabs are already far apart next to a
// metre of cell, and there is nothing to gain by making them coarser still.
const S32 SS_WIND_MAX_LEVELS = 5;
const S32 SS_WIND_MIN_LEVEL_RES = 24;

// Oblique probes, one per cardinal direction. The overhead capture alone says
// everything under a rooftop is solid, which is right for a building and wrong
// for a skyway; these look in under the overhangs and carve back out whatever
// they can actually see. Evidence only ever removes solid, never adds it, so a
// probe that misses a passage leaves the conservative answer standing.
const S32 SS_WIND_PROBES = 4;

class SSWindFlowMap : public LLSingleton<SSWindFlowMap>
{
    LLSINGLETON_EMPTY_CTOR(SSWindFlowMap);

public:
    // Compute shaders are GL 4.3. Below that the flowmap simply does not run
    // and every consumer falls back to the uniform ambient wind.
    static bool isSupported();

    // Called from the display loop right after the rain shadow capture, where
    // the pipeline is in a known state. Solves at most one region per call,
    // throttled, picking the stalest or dirtiest tile near the camera.
    void update();

    // Object update hook, mirroring the rain shadow's: marks a region's tile
    // stale when geometry inside its domain moves.
    static void onObjectUpdate(LLViewerObject* objectp);

    void clear();

    // Throw away every solve and start again. The simulation floater's rebuild
    // button, and anything else that changes the world in a way the staleness
    // checks cannot see.
    void rebuildAll();

    // Whether the region the camera is in has a solved tile. Consumers use
    // this to decide whether sampling will tell them anything.
    bool isValid() const;

    // Wind velocity at a point, m/s. Falls back to the ambient wind where no
    // tile covers the point, so callers never need to branch.
    LLVector3 sample(const LLVector3& pos_agent) const;

    // How much of the ambient wind reaches this point, roughly 0..1. Below 1
    // is sheltered, above 1 is a gap the wind is being squeezed through.
    F32 exposure(const LLVector3& pos_agent) const;

    // Render Metadata > Wind Flow: one arrow per solved cell, on every slab,
    // at the density the solve actually ran at.
    void renderDebug();

    // Which region the capture debug view is showing, for the floater to say
    U64 capturedRegion() const { return mCaptureRegion; }

    // Diagnostics for the Atmo Magic overlay, describing the tile the camera
    // is standing in
    S32 sliceCount() const;
    S32 resolution() const;
    F32 extent() const;
    F32 cellSize() const;
    F32 sliceAltitude(S32 i) const;
    F32 lastSolveMS() const { return mSolveMS; }

    // Diagnostics: what the height capture found for the column at a position,
    // false when it found nothing there at all, and how much of the solved
    // volume came out solid. Both answer the same question from either side -
    // whether the solve has any geometry in it to flow around.
    bool surfaceAt(const LLVector3& pos_agent, F32& top) const;
    F32 carvedFraction() const;
    F32 solidFill() const;
    F64 age() const;
    U32 buildCount() const { return mBuildCount; }
    S32 tileCount() const { return (S32)mTiles.size(); }

private:
    // One region's flowmap. Everything horizontal is region-local, so a tile
    // survives the agent-origin shift on a region crossing without being
    // touched; altitudes are absolute, and those do not shift.
    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;                   // texels per horizontal axis
        S32 mSlices = 0;
        F32 mExtent = 0.f;              // region width plus a margin each side
        F32 mMargin = 0.f;
        LLVector3 mOriginRegion;        // region-local XY of texel (0,0); negative by the margin

        F32 mSliceZ[SS_WIND_MAX_SLICES + 1] = { 0.f };
        LLVector3 mAmbient[SS_WIND_MAX_SLICES];

        F32 mGroundRef = 0.f;           // reference ground level for the wind gradient
        F32 mBandTop = 0.f;
        F32 mBandBottom = 0.f;
        S32 mTrack = 0;                 // sky track the band was chosen for

        // Solved field, res*res*slices; xyz velocity, w exposure
        std::vector<LLVector4> mFlow;

        // The solid mask the field was solved against, one byte per cell. Kept
        // so a consumer can ask whether a point is inside geometry rather than
        // inferring it from the velocity being small, which a lee also is.
        std::vector<U8> mSolid;

        // The height field it was solved from, res*res each, kept so the
        // overlay can say what the capture actually saw over a given spot
        std::vector<F32> mSurfaceTop;

        // Fraction of probe samples that landed under an overhang. Zero means
        // the probes found nothing the overhead pass had already seen, so the
        // mask is a plain heightmap.
        F32 mCarved = 0.f;
        F32 mSolidFill = 0.f;           // fraction of the volume that is solid

        LLVector3 mBuiltWind;
        U32 mTuning = 0;                // hash of the settings it was solved under
        F64 mBuildTime = 0.0;
        bool mDirty = false;
        bool mValid = false;
        F64 mLastTouched = 0.0;         // for LRU eviction
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    const Tile* tileAt(const LLVector3& pos_agent) const;
    const Tile* cameraTile() const;

    bool needsSolve(const Tile& tile) const;
    bool buildTile(Tile& tile);
    void evict();

    bool ensureResources(S32 res, S32 slices);
    void releaseResources();
    bool ensureShaders();

    // Vertical band for a region, from the sky track the camera is in and how
    // tall the region actually builds. Camera altitude inside a track does not
    // move it, which is what keeps the tile static.
    void chooseBand(Tile& tile, LLViewerRegion* regionp);

    // Overhead ortho depth render giving the top surface of every column,
    // followed by the oblique probes that find what it hid
    bool captureHeights(Tile& tile);

    // One ortho depth render along dir. Returns the linear distance from the
    // near plane to the first surface along each ray, PROBE_MISS where the ray
    // left the world without hitting anything, and the world-to-view matrix
    // needed to look a point up in the result.
    bool captureAlong(LLRenderTarget& target, S32 res, const Tile& tile,
                      const LLVector3& dir, const LLVector3& eye,
                      F32 half, F32 range, std::vector<F32>& out, glm::mat4& view_out);

    // The four oblique probes, and the height field they refine
    bool captureProbes(Tile& tile);

    // Run the shader's visibility test on the CPU for one known-solid column
    // and log every intermediate. The carve has no way to report itself from
    // inside the solve, and a wrong matrix or a wrong footprint produces the
    // same symptom as a wrong depth map.
    void auditProbes(const Tile& tile) const;

    // 0 = the overhead heightmap, 1..4 = probe 0..3, 5 = carve candidates
    void renderDebugCapture(S32 which);

    // Flow lines traced through the solved field, rather than one arrow per
    // cell. Reads the same data; what it adds is continuity.
    void renderDebugStreamlines();

    // How opaque a slab is drawn, from how far it is above or below the one the
    // camera is standing in. Depth cueing by altitude: the volume is drawn all
    // at once and is otherwise an unreadable thicket.
    F32 slabAlpha(const Tile& tile, S32 k, F32 cam_z) const;

    // Repeat the shader's carve on the CPU and record why each cell ended up
    // solid or open, for the candidates view
    void buildCarveFlags(const Tile& tile);

    // Carry a discovered passage through the stretch of it no probe could see,
    // by opening solid cells that have carved cells on both sides. Reads the
    // mask back, works on it, and writes it out again between the init pass and
    // the solve.
    void bridgePassages(const Tile& tile);

    // Choose slab boundaries from the captured heights: bounded count, a
    // minimum separation, quantile placement where the geometry actually
    // varies, and forced boundaries at EEP track altitudes.
    void placeSlices(Tile& tile);

    bool solve(const Tile& tile);

    // Resolution of a pyramid level, and how many levels a grid supports
    static S32 levelRes(S32 res, S32 level) { return res >> level; }
    static S32 levelCount(S32 res);
    void readback(Tile& tile);

    // Flat index into a tile's mFlow
    static size_t index(const Tile& tile, S32 x, S32 y, S32 k)
    {
        return ((size_t)k * tile.mRes + y) * tile.mRes + x;
    }

    // Slab whose span contains z, and the blend toward the next one
    static void sliceAt(const Tile& tile, F32 z, S32& k, F32& frac);

    bool mShadersReady = false;
    bool mShaderFailed = false;

    std::map<U64, Tile> mTiles;

    // Scratch for the tile currently being built, res*res each
    std::vector<F32> mTop;      // topmost surface, or very negative for none

    // Oblique probe depths, res*res per probe, and the matrix that maps a world
    // point into each probe's view
    std::vector<F32> mProbeDepth[SS_WIND_PROBES];
    glm::mat4        mProbeView[SS_WIND_PROBES];
    F32              mProbeHalf[SS_WIND_PROBES] = { 0.f };   // ortho half-width per probe

    // Probes render at their own resolution, not the mask's. Their ortho box
    // has to be half as wide again as the region to cover the vertical shear,
    // and a horizontal surface seen at a shallow angle is foreshortened on top
    // of that - so at matched resolution a probe texel covers several mask
    // cells, and the ray a cell tests against is not the ray through that cell.
    S32              mProbeRes = 0;

    // Enough of each probe's camera to turn a texel back into the world point
    // it saw, which is what the capture debug view draws
    struct ProbeFrame
    {
        LLVector3 mEye;
        LLVector3 mDir;
        LLVector3 mRight;
        LLVector3 mUp;
    };
    ProbeFrame       mProbeFrame[SS_WIND_PROBES];

    // Region the scratch buffers above describe. They are overwritten by every
    // build, so the capture debug view can only ever show the last one.
    U64              mCaptureRegion = 0;
    S32              mCaptureRes = 0;
    F32              mCaptureCell = 0.f;
    LLVector3        mCaptureOrigin;
    bool             mProbeUsable[SS_WIND_PROBES] = { false };
    F32              mProbeMiss[SS_WIND_PROBES] = { 1.f };   // share of rays that hit nothing

    // Altitudes where a probe hit something the overhead pass could not see -
    // the soffit of an overhang, and the floor of the passage under it. These
    // are the altitudes a slab boundary has to land on for the passage to have
    // anywhere to exist, and they are the only place they can come from now
    // that the mask is a heightmap the probes carve rather than a pair of
    // surfaces per column.
    std::vector<F32> mHidden;

    // GPU resources, shared across tiles and sized to the largest seen
    // One set per pyramid level; level 0 is the full-resolution grid and is
    // the only one the height capture, the projection and the readback touch.
    U32 mHeightTex = 0;                             // R32F 2D
    U32 mProbeTex = 0;                              // R32F 2D array, one layer per probe
    U32 mSolidTex[SS_WIND_MAX_LEVELS] = { 0 };      // R8 3D
    U32 mVelTex[SS_WIND_MAX_LEVELS] = { 0 };        // RGBA16F 3D
    U32 mDivTex[SS_WIND_MAX_LEVELS] = { 0 };        // R32F 3D
    U32 mPressureTex[SS_WIND_MAX_LEVELS][2] = {};   // R32F 3D, ping-ponged
    S32 mTexRes = 0;
    S32 mTexSlices = 0;
    S32 mProbeTexRes = 0;
    S32 mTexLevels = 0;

    LLRenderTarget mCapture;        // overhead pass, at mask resolution
    LLRenderTarget mProbeCapture;   // oblique probes, at mProbeRes

    // Per-cell carve verdict for the last capture, for the candidates debug
    // view. Only filled while that view is selected: it costs a pass over the
    // whole volume against every probe.
    std::vector<U8> mCarveFlags;

    F64 mLastBuild = 0.0;
    F32 mSolveMS = 0.f;
    U32 mBuildCount = 0;
};

// </SS:Nexii>

#endif // SS_WINDFLOW_H
