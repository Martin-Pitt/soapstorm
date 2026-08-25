/**
 * @file sssurfacefield.h
 * @brief Atmo Magic surface field: what the weather has left on the world.
 *        Wetness, settled snow and standing water, integrated over time on
 *        the drainage network the runoff trace already solved, so a shower
 *        darkens a street over a minute and it stays damp long after.
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

#ifndef SS_SURFACEFIELD_H
#define SS_SURFACEFIELD_H

// <SS:Nexii> Atmo Magic surface field

// Consumers: wet-shading + normal-flatten passes (must agree on puddle thresholds), footstep surface pick, avatar soak seeding, roof runoff reservoirs. Rain shadow feeds exposure IN - upgrading it moves everything downstream. doc/atmo_magic_interactions.md

#include "llrendertarget.h"
#include "llsingleton.h"
#include "ssrainshadow.h"
#include "v3math.h"

#include <map>
#include <vector>

struct SSPrecipPreset;
class LLGLSLShader;

// The state the weather has worked the surface into, one value per drainage cell. Everything here is slow: it is measured in minutes of weather, not in frames, which is the whole point of keeping it
// rather than deriving it from the rain shadow every time it is asked for. The field is laid out on - and only exists for - regions the rain shadow map has captured a surface for. What the surface
// IS (height, what kind of material, which way it slopes, whether it can hold water) is read straight off that capture and reduced to the per-cell Geometry below. There is deliberately no
// water-movement model here any more. An earlier version traced a full drainage network per region - flow accumulation, flood fill, eaves, streams running off lips - and it was far more machinery
// than the look it bought. Water on a slope is now a static consequence of the slope and of how much rain is landing on it, which is what a viewer actually shows: a wet roof streaks downhill because
// it is steep and being rained on, not because a solver routed a catchment through it. See doc/atmo_magic_environment.md.
class SSSurfaceField : public LLSingleton<SSSurfaceField>
{
    LLSINGLETON_EMPTY_CTOR(SSSurfaceField);

public:
    // From SSAtmoMagic::idle, after the rain shadow map has had its chance to capture, so a region captured this frame is dressed this frame rather than next.
    void idle(F32 dt);

    void clear();

    // Render Metadata > Surface Field (Atmo Magic): the cells drawn as a coloured wash over the surface they belong to.
    void renderDebug();

    // What the weather has left at a point. Agent space, and answered from whichever region covers it; invalid where nothing has been traced.
    struct Sample
    {
        F32 mWet = 0.f;         // 0 dry, 1 saturated
        F32 mSnow = 0.f;        // settled depth, metres
        F32 mPuddle = 0.f;      // standing water depth, metres
        F32 mSurfaceZ = 0.f;    // the height all of the above belongs to
        bool mValid = false;
    };
    Sample sample(const LLVector3& pos_agent) const;

    // The field as a shading pass sees it: one camera-centred window stitched from every region in range, rather than a layer per region. A shader then has a single texture and a single origin to
    // reason about, region borders stitch instead of stepping, and nothing in the fragment path has to search a list of regions to find out which one it is standing in. False when there is nothing
    // worth binding, in which case the caller has no work to do either.
    bool bindForShader(LLGLSLShader& shader, S32 channel);
    bool hasWindow() const { return mWindowTex != 0 && mWindowValid; }

    // A second stitched window, laid out exactly like the first, carrying the cell's downslope direction and how steep it is - which way a film of water on this cell would run, and how hard. Static:
    // it comes from the surface's own local gradient, not from any simulation of where water went. Kept apart from the main window rather than added as a third and fourth component on it: nothing
    // but the flow-motion pass ever reads this, and every consumer of the main window would otherwise be uploading and sampling two floats a tick that it never touches.
    bool bindFlowForShader(LLGLSLShader& shader, S32 channel);
    bool hasFlowWindow() const { return mWindowFlowTex != 0 && mWindowValid; }

    void releaseGL();

    // The gbuffer pass. From renderDeferredLighting, before anything has read the specular buffer, so every lighting pass downstream - sun, local lights, projectors, probes - sees one consistent set
    // of values rather than each having to be taught about the weather separately.
    void renderWetPass();

    // Simulation floater / info overlay
    S32 fieldCount() const { return (S32)mFields.size(); }
    F32 lastTickMS() const { return mLastTickMS; }
    F32 peakWet() const { return mPeakWet; }
    F32 peakSnow() const { return mPeakSnow; }
    F32 peakPuddle() const { return mPeakPuddle; }

private:
    // One region's surface, reduced from the rain shadow map's capture to exactly what dressing it needs. Rebuilt when the capture's geometry serial changes and not otherwise: none of this is
    // weather, all of it is shape.
    struct Geometry
    {
        S32 mN = 0;
        F32 mCell = 0.f;
        U32 mGeomSerial = 0xFFFFFFFFu;

        std::vector<F32> mZ;        // agent-space surface height per cell
        std::vector<U8> mFlags;     // SSRainShadowMap::SURF_*; 0 = no surface

        // Local downslope direction and steepness, from the height field's own gradient. This is the whole of the directionality model: a pitched roof knows which way is down, and how steeply, and
        // that is enough to streak water down it.
        std::vector<F32> mSlopeX;
        std::vector<F32> mSlopeY;
        std::vector<F32> mSlope;    // rise over run, 0 on the level

        // Lips: a cell on the surface with open air, or a long drop, beside it - the edge of a roof, a balcony, a bridge deck. Water that reaches one comes off it, so this is where drips and streams
        // are shed from. Found by looking at a cell's own neighbours and nothing further: no network, no catchment, no routing. mEdgeX/Y point outward, over the drop.
        std::vector<U8> mEdge;
        std::vector<F32> mEdgeX;
        std::vector<F32> mEdgeY;

        // Flat list of the cells mEdge marks, so shedding walks the lips rather than the whole grid looking for them - a region is 16k cells and a few hundred of them are edges.
        std::vector<S32> mEdgeCells;

        // Where water can stand: a cell flat enough to hold it that no neighbour drains it away. A purely local test - "is this a dip" - not a routed basin, so a puddle appears where the ground
        // actually dips rather than wherever a catchment happened to terminate.
        std::vector<U8> mPool;

        bool valid() const { return mN > 0 && !mZ.empty(); }
        bool solid(size_t i) const { return mFlags[i] != 0; }
        bool water(size_t i) const { return (mFlags[i] & SSRainShadowMap::SURF_WATER) != 0; }
    };

    // Builds/refreshes the Geometry for every region the shadow map has a surface for, and drops the ones it no longer does.
    void refreshGeometry();
    static void buildGeometry(const SSRainShadowMap::SurfaceGrid& grid, Geometry& out);

    std::map<U64, Geometry> mGeometry;

    struct Field
    {
        U64 mRegionHandle = 0;
        S32 mN = 0;
        F32 mCell = 0.f;

        // The height each cell's values were accumulated against. Geometry moving under the field is the one thing that has to reset it: a roof rezzed into a downpour has not been standing in it, so
        // it starts dry rather than inheriting whatever the ground below it had soaked up.
        std::vector<F32> mZ;

        std::vector<F32> mWet;
        std::vector<F32> mSnow;
        std::vector<F32> mPuddle;

        // What a lip cell is holding, in drips' worth of water, and the fractional drip it is owed. This is the roof's reservoir: rain fills it, it drains on its own time constant, and what drains
        // is what the eave sheds. It lives here rather than in Geometry because it is weather, not shape - a rebuild of the surface must not empty a roof that has been standing in a downpour. It is
        // also the whole of what remains of "water moves": water gathers where it lands and leaves at the nearest edge to it. Nothing carries it sideways from cell to cell.
        std::vector<F32> mStore;
        std::vector<F32> mAccum;

        F64 mLastTouched = 0.0;
    };

    // Sheds drips and streams off the lips the geometry found, at a rate set by how much rain is actually landing there and how steeply the surface behind the lip feeds it. This is what replaced the
    // drainage simulation. The old system traced a flow network per region, accumulated catchment through it and shed what arrived at each eave; this asks two local questions instead - is this an
    // edge, and is it raining on it - which is what the result actually looked like anyway. A roof sheds because it is a roof in the rain, not because a solver routed six hundred cells of catchment
    // to its gutter.
    void shedEdges(F32 dt);

    // One region's worth of the above.
    void shedRegion(U64 region_handle, const Geometry& geom, Field& fld,
                    F32 dt, F32 rate_m2, const LLVector3& camera_agent);

    Field* fieldFor(U64 region_handle, const Geometry& geom, F64 now);
    void updateWindow();
    void tick(Field& fld, const Geometry& geom, F32 dt,
              const SSPrecipPreset& preset, F32 intensity);
    void evict(F64 now);

    std::map<U64, Field> mFields;

    // Ticked well below frame rate. Nothing here moves fast enough to see the difference, and a quarter of a million cells a region is not something to walk every frame for a value that changes over
    // minutes.
    F32 mTickAccum = 0.f;

    // Where the modified specular buffer is built before it goes back over the gbuffer's own. Reading and writing one texture in a single pass is not something the driver promises anything about,
    // and a full resolution RGBA is cheap next to finding out which drivers get away with it.
    LLRenderTarget mScratch;

    // Same idea, for the flattened normal. Its own target rather than a second attachment on mScratch: the wetness shader's early returns are all proven correct as a single-output shader, and giving
    // it a second output to carry through unchanged on every one of those paths would have meant touching every one of them to add it. A second pass over a second target costs one more full-screen
    // triangle and touches none of that.
    LLRenderTarget mScratchNormal;

    // The stitched window. Held as RGBA32F - the height channel is an agent space Z that runs to a few thousand metres and is compared against fragment positions to within a few centimetres, which
    // is well past what a half float would carry.
    U32 mWindowTex = 0;
    S32 mWindowRes = 0;
    F32 mWindowCell = 0.f;
    LLVector3 mWindowOrigin;            // agent-space corner of texel (0, 0)
    std::vector<F32> mWindowData;       // staging, RGBA per texel
    bool mWindowValid = false;

    // xy: agent-space unit flow direction, downstream. z: how much of the cell's own footprint is drainage passing through rather than caught by it, 0 to 1 - a channel, not a puddle. Same lattice,
    // origin and cell as mWindowData, so one ssFieldOrigin uniform serves both.
    U32 mWindowFlowTex = 0;
    std::vector<F32> mWindowFlowData;   // staging, RGBA per texel

    // Rolling cursor over each region's lip cells, so a frame visits a slice of them rather than all of them: shedding is throttled by how many stream slots and drips a frame may start, not by how
    // many lips exist.
    std::map<U64, S32> mShedCursor;

    F32 mLastTickMS = 0.f;
    F32 mPeakWet = 0.f;
    F32 mPeakSnow = 0.f;
    F32 mPeakPuddle = 0.f;
};

// </SS:Nexii>

#endif // SS_SURFACEFIELD_H
