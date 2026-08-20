/**
 * @file ssrunoff.h
 * @brief Atmo Magic runoff: rain landing on a roof does not stay there. The
 *        rain shadow surface is traced downhill into a drainage network, so
 *        every sloped roof knows how much water reaches each of its edges,
 *        and sheds it off the eaves into the street.
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

#ifndef SS_RUNOFF_H
#define SS_RUNOFF_H

// <SS:Nexii> Atmo Magic roof runoff

#include "llsingleton.h"
#include "ssrainshadow.h"
#include "v3math.h"

#include <map>
#include <memory>
#include <vector>

// A borrowed view of one region's traced drainage, for the systems that dress
// the surface rather than shed off it. Laid out exactly like the SurfaceGrid
// it came from - cell (x, y) sits at ((x + 0.5) * mCell, (y + 0.5) * mCell) in
// region-local XY - so a consumer can walk it against its own copy of the same
// grid without resampling anything.
//
// The pointers belong to the network and are only good until the region is
// retraced, which happens on the main thread between idles. Read it, use it,
// do not keep it.
struct SSRunoffField
{
    S32 mN = 0;
    F32 mCell = 0.f;
    const F32* mZ = nullptr;        // surface height, agent-space
    const S32* mFlow = nullptr;     // downstream cell index, or -1 where water stands
    const U8*  mEdge = nullptr;     // 1 where the cell sheds over a lip
    const F32* mCatch = nullptr;    // square metres draining through the cell
    const U8*  mFlags = nullptr;    // SSRainShadowMap::SURF_*, 0 where the column found nothing

    bool valid() const { return mN > 0 && mZ && mFlow && mEdge && mCatch && mFlags; }
    size_t count() const { return (size_t)mN * mN; }

    bool solid(size_t i) const { return mFlags[i] != 0; }
    bool water(size_t i) const { return (mFlags[i] & SSRainShadowMap::SURF_WATER) != 0; }

    // Water stands here: nothing adjacent is lower, and it does not shed over
    // an edge either. This is the puddle test, and it is why mEdge is a
    // separate array rather than folded into mFlow as a negative index - the
    // lip of a flat roof has no downstream cell *and* sheds, and packing the
    // two together made that common case indistinguishable from a hollow.
    bool pools(size_t i) const { return mFlow[i] < 0 && !mEdge[i]; }
};

// A run of roof edge that sheds as one thing: the lip of a gable, the length
// of an awning, the side of a bridge deck. Water does not leave a roof at a
// few isolated points, it leaves along a line, so the cells that make up one
// edge are gathered here and drips are placed anywhere along it rather than
// at cell centres. Positions are region-local, so a region crossing leaves
// the network alone.
struct SSRunoffEave
{
    std::vector<LLVector3> mLip;    // points along the edge
    std::vector<LLVector3> mLand;   // where the drip from each comes down
    LLVector3 mOut;                 // mean horizontal direction the water leaves by
    LLVector3 mRun;                 // horizontal direction along the edge
    F32 mSpacing = 1.f;             // distance between neighbouring lip points
    F32 mCatchment = 0.f;           // square metres draining through the whole run

    // Mean share of the arriving water this run actually sheds. A gutter at
    // the bottom of a roof takes everything and sits at 1; the rake up the
    // side of a gable only sheds what the wind drives over it, and sits low.
    // Kept so the drip rate and the stream threshold both read the same
    // number rather than inferring it from the catchment.
    F32 mShed = 1.f;
    F32 mShedSum = 0.f;             // accumulator during the trace

    // Water held on the roof, in raindrops. A roof does not stop draining the
    // instant a gust lull passes over it, and this is what carries the drips
    // through one.
    F32 mStore = 0.f;
    F32 mAccum = 0.f;               // fractional drips owed
};

class SSRunoff : public LLSingleton<SSRunoff>
{
    LLSINGLETON_EMPTY_CTOR(SSRunoff);

public:
    // Per-frame driver, from SSAtmoMagic::idle after the sim has stepped.
    void idle(F32 dt);

    // A landing that actually arrived, reported from the impact queue. Used to
    // measure delivered rainfall against what the weather parameters claim, so
    // the shed rate answers to what is really falling.
    void notifyImpact(const LLVector3& pos_agent, F32 sample_radius);

    void clear();

    // The traced drainage for one region, or an invalid view when that region
    // has not been traced yet. Cheap: it hands out pointers into the network.
    SSRunoffField field(U64 region_handle) const;

    // Regions holding a traced field, paired with the geometry revision it was
    // built from - the same pairing SSRainShadowMap::validTiles gives for the
    // captures underneath, and used the same way, to tell "there is something
    // new here" from "this has been rebuilt for reasons of its own".
    void tracedRegions(std::vector<std::pair<U64, U32> >& out) const;

    // Render Metadata > Roof Runoff (Atmo Magic): the drainage network drawn
    // as flow arrows over the surface, with the eaves marked by catchment.
    void renderDebug();

    // Simulation floater / info overlay
    S32 eaveCount() const;
    S32 networkCount() const { return (S32)mNetworks.size(); }
    F32 lastBuildMS() const { return mLastBuildMS; }
    F32 dripRate() const { return mDripRate; }
    F32 delivery() const { return mDelivery; }
    U32 buildCount() const { return mBuildCount; }

private:
    struct Network
    {
        U64 mRegionHandle = 0;

        // The geometry revision this was traced from. Not the capture time:
        // the tile is recaptured whenever the camera climbs out of its band or
        // the wind swings the fall direction, and neither of those changes the
        // shape of the roof. Keying on the capture would retrace the whole
        // region every few seconds for an identical answer.
        U32 mBuiltSerial = 0xFFFFFFFFu;
        S32 mBuiltRes = 0;

        std::vector<SSRunoffEave> mEaves;   // sorted by catchment, largest first

        // The traced surface itself. The eaves are what the drips come off,
        // but they are a summary: the field below is the whole answer, and
        // everything that dresses the surface rather than falling off it -
        // wetness, snow depth, where water stands, which way it runs - reads
        // this rather than retracing its own copy.
        F32 mFieldCell = 0.f;
        S32 mFieldN = 0;
        std::vector<F32> mFieldZ;
        std::vector<S32> mFieldFlow;    // downstream cell, or -1 where water stands
        std::vector<U8>  mFieldEdge;    // 1 where the cell sheds over a lip
        std::vector<F32> mFieldCatch;
        std::vector<U8>  mFieldFlags;   // SurfaceGrid flags, carried through as-is
    };

    // One region's trace, in flight. The drainage solve is a flow accumulation
    // and a flood fill over a quarter of a million cells with a sort in the
    // middle of it, which is milliseconds of pure arithmetic on data nothing
    // else is looking at - so it runs on the general work queue rather than in
    // the frame.
    //
    // Held by shared_ptr so the worker owns it outright: nothing here points
    // into mNetworks, and a network being dropped while a trace is out for it
    // costs the trace, not the process.
    struct Trace
    {
        U64 mRegionHandle = 0;
        S32 mRes = 0;

        // Snapshot taken on the main thread, where the shadow map and the
        // region list are safe to read
        SSRainShadowMap::SurfaceGrid mGrid;
        LLVector3 mOrigin;
        F32 mCliffSlope = 2.f;

        // Horizontal wind at trace time, and how much shedding it may account
        // for on an edge the roof does not otherwise drain over. This is what
        // lets a squall run off the gable end of a building.
        LLVector3 mWindDir;
        F32 mWindPush = 0.f;

        // What the worker produced
        std::vector<SSRunoffEave> mEaves;
        std::vector<F32> mFieldZ;
        std::vector<S32> mFieldFlow;
        std::vector<U8>  mFieldEdge;
        std::vector<F32> mFieldCatch;
        std::vector<U8>  mFieldFlags;
        S32 mFieldN = 0;
        F32 mMS = 0.f;
    };

    void refreshNetworks();

    bool startTrace(U64 region_handle, S32 res);
    static void traceNetwork(Trace& job);            // worker
    void finishTrace(const std::shared_ptr<Trace>& job);

    void shed(F32 dt);

    // Running sheets for a run that is shedding faster than drips can carry.
    // Anchored at fixed points along the lip and refreshed in place, so they
    // stay put rather than crawling along the gutter frame to frame.
    void shedStreams(const SSRunoffEave& eave, const LLVector3& origin,
                     U64 region_handle, U32 eave_index, F32 drive);

    void measureDelivery(F32 dt);

    std::map<U64, Network> mNetworks;

    // Only one trace at a time. A second would be solving a region the camera
    // is further from while the near one waits, and the pool is shared with
    // texture and mesh work that wants it more.
    bool mTraceBusy = false;
    U32 mTraceGeneration = 0;   // bumped by clear(); a stale result is dropped

    F64 mLastRefresh = 0.0;
    F32 mLastBuildMS = 0.f;
    U32 mBuildCount = 0;
    F32 mDripRate = 0.f;        // drips per second currently being shed

    // Delivered-rainfall measurement. Impacts are counted over a window and
    // compared with what the weather parameters say should have landed in the
    // same disc; the ratio corrects the shed rate.
    F32 mImpactCount = 0.f;
    F32 mExpectedCount = 0.f;
    F32 mSampleRadius = 0.f;
    F32 mMeasureElapsed = 0.f;
    F32 mDelivery = 1.f;        // smoothed observed / expected
};

// </SS:Nexii>

#endif // SS_RUNOFF_H
