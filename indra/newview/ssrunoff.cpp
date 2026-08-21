/**
 * @file ssrunoff.cpp
 * @brief Atmo Magic runoff: drainage network over the rain shadow surface,
 *        and the drips it sheds off every eave it finds.
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

#include "ssrunoff.h"

#include "ssatmomagic.h"
#include "ssprecipitation.h"
#include "ssrainshadow.h"

#include "llfasttimer.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "workqueue.h"

#include <algorithm>
#include <cfloat>

// <SS:Nexii> Atmo Magic roof runoff

static const F64 REFRESH_INTERVAL = 0.5;    // at most one network traced this often
static const F32 REGION_REACH     = 320.f;  // regions further than this keep no network
static const size_t MAX_EAVES     = 128;    // runs kept per region, largest catchment first
static const size_t MAX_LIPS      = 64;     // points kept along one run
static const F32 MAX_EAVE_RATE    = 24.f;   // drips per second one run may shed
static const S32 MAX_EAVE_BURST   = 4;      // drips one run may shed in a single frame
static const S32 DRIP_BUDGET      = 900;    // live drips past which shedding backs off

// How long a roof takes to give up the water standing on it. This is what
// carries the drips through a gust lull instead of stopping them dead the
// moment the rain eases, and it is why an eave keeps going for a few seconds
// after a shower has passed.
static const F32 DRAIN_TAU        = 5.f;    // seconds
static const F32 STORE_CEILING    = 6.f;    // seconds of inflow a roof may bank

// Delivered-rainfall measurement window
static const F32 DELIVERY_WINDOW  = 4.f;    // seconds of impacts averaged
static const F32 DELIVERY_EMA     = 0.15f;  // smoothing on the ratio, per window

// How closely the roof's own fall has to line up with a drop before water is
// considered to go over it rather than past it. cos 60 degrees: at a right
// angle the water is running along the edge, which is what the rake of a
// gable is, and shedding there put a drip line up the slope of every roof.
static const F32 EDGE_ALIGN_MIN   = 0.5f;

// Below this an edge is not worth carrying at all
static const F32 MIN_SHED_FRAC    = 0.12f;

// How steeply the surface has to keep falling before it is taken to be
// carrying the water on past a drop rather than delivering it over the edge.
// Below the first figure the onward path is level - the sag along a gutter,
// the sampling noise along a lip - and everything arriving goes over; by the
// second there is a roof pitch underneath it and the alignment test has it.
// A shallow roof is about 0.2, so the window sits below where real pitches
// start and a rake keeps its water.
static const F32 LIP_FLAT_SLOPE   = 0.04f;  // ~2 degrees
static const F32 CARRY_SLOPE      = 0.18f;  // ~10 degrees

// Most of an edge's shedding the wind alone may account for. Rain driven
// across a gable end does run off it, but a side edge in a gale should not
// out-shed the gutter it drains into.
static const F32 WIND_SHED_MAX    = 0.6f;

// Runs only join while they shed the same way. Without this the flood fill
// walks round the corner where a gutter meets a rake and returns one run
// wrapping the whole roof, with a mean direction belonging to neither.
static const F32 RUN_JOIN_ALIGN   = 0.5f;

// And how far the lip of a run may wander off a straight line before the run
// is cut in two, as a gradient across the length it has wandered over. Runs
// are gathered by how their cells shed, which two rakes either side of a ridge
// agree on completely while running in opposite directions - so shape is a
// separate question from direction, and this is what answers it.
static const F32 RUN_SHAPE_TAN    = 0.176f;     // tan(10 degrees)

// Where separate drips give way to a running stream. Below the first figure an
// edge drips and nothing else; by the second it is a continuous fall and the
// drips are only the spatter around it.
//
// These are in drips per second, which is water divided by the merge factor,
// and that is what made the old pair so far out. A whole roof face in heavy
// rain delivers a few hundred raindrops a second to its gutter, and at a merge
// of 12 that is a raw rate in the twenties before merging - but the gutter
// itself was only asked to start running at six and did not reach a full stream
// until twenty. Every eave in a downpour sat at a fraction of running, which
// is why a whole town of running gutters came out as a couple of dozen
// streams. A roof shedding a couple of drips a second per run is already
// running rather than dripping.
static const F32 STREAM_RATE_MIN  = 2.f;    // drips/s at which streams start
static const F32 STREAM_RATE_FULL = 9.f;    // further drips/s to reach a full stream

// A run sheds along the whole of its length, so it is cut into slots and each
// one gets a stream spanning it: the streams stand side by side and a gutter
// in a downpour comes off as one fall the length of the roof, rather than as a
// few strands hanging off a thirty metre edge.
//
// How wide a slot should be is not a number to invent here - it is the width
// the art was drawn for, which the preset already states as the sheet tier's
// quad. On rain that is 18 m, so a 36 m eave is two streams and looks
// like the two quads of water it is. Cutting it into a fixed couple of metres
// instead broke the same run into fifteen little ribbons, every one of them
// showing a sliver of a texture meant to span ten times as much.
static const S32 MAX_RUN_STREAMS  = 12;
static const F32 STREAM_SPAN_MIN  = 4.f;    // narrowest slot the art's own width may give
static const F32 STREAM_SPAN_MAX  = 24.f;   // widest, so one stream cannot span a bridge
static const F32 STREAM_SPAN_ALT  = 8.f;    // when the preset has no sheet tier to read

static LLTrace::BlockTimerStatHandle FTM_SS_RUNOFF("Atmo Magic Runoff");
static LLTrace::BlockTimerStatHandle FTM_SS_RUNOFF_BUILD("Trace Drainage");

void SSRunoff::clear()
{
    // A trace already out is for a world that has gone; its result is dropped
    // when it reports back rather than installed over the top of the new one
    ++mTraceGeneration;

    mNetworks.clear();
    mLastRefresh = 0.0;
    mDripRate = 0.f;
    mImpactCount = 0.f;
    mExpectedCount = 0.f;
    mSampleRadius = 0.f;
    mDelivery = 1.f;
}

S32 SSRunoff::eaveCount() const
{
    S32 total = 0;
    for (const auto& entry : mNetworks) total += (S32)entry.second.mEaves.size();
    return total;
}

SSRunoffField SSRunoff::field(U64 region_handle) const
{
    SSRunoffField out;

    auto it = mNetworks.find(region_handle);
    if (it == mNetworks.end()) return out;

    const Network& net = it->second;
    if (net.mFieldN <= 0 || net.mFieldZ.empty()) return out;

    out.mN = net.mFieldN;
    out.mCell = net.mFieldCell;
    out.mZ = net.mFieldZ.data();
    out.mFlow = net.mFieldFlow.data();
    out.mEdge = net.mFieldEdge.data();
    out.mCatch = net.mFieldCatch.data();
    out.mFlags = net.mFieldFlags.data();
    return out;
}

void SSRunoff::tracedRegions(std::vector<std::pair<U64, U32> >& out) const
{
    out.clear();
    for (const auto& entry : mNetworks)
    {
        if (entry.second.mFieldN > 0 && !entry.second.mFieldZ.empty())
        {
            out.push_back(std::make_pair(entry.first, entry.second.mBuiltSerial));
        }
    }
}

// Whether runoff means anything for the weather currently running. Water runs
// off a roof; snow settles on it and embers rise off it, and neither wants a
// drip line under the eaves.
static bool runoffApplies()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoRunoff", true);
    if (!enabled) return false;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->isEnabled() || !atmo->hasWeather()) return false;

    const SSPrecipPreset& preset = atmo->preset();
    return preset.mArchetype == SSPrecipArchetype::LIQUID && preset.makesImpacts();
}

void SSRunoff::notifyImpact(const LLVector3& pos_agent, F32 sample_radius)
{
    // Counted against a disc of exactly this radius around the camera, so the
    // measurement stays self-consistent whatever the caller has already culled
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 dx = pos_agent.mV[VX] - cam.mV[VX];
    const F32 dy = pos_agent.mV[VY] - cam.mV[VY];
    if (dx * dx + dy * dy > sample_radius * sample_radius) return;

    mImpactCount += 1.f;
    mSampleRadius = sample_radius;
}

void SSRunoff::idle(F32 dt)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_RUNOFF);

    if (!runoffApplies())
    {
        if (!mNetworks.empty()) clear();
        return;
    }

    refreshNetworks();
    measureDelivery(dt);
    shed(dt);
}

void SSRunoff::measureDelivery(F32 dt)
{
    // What the weather claims should be landing inside the disc the impact
    // queue reports on, against what turned up. Every column lands on
    // something, so in open country and in a dense build alike this should come
    // out near one; where it does not, the difference is the throttling and
    // capping between the weather parameters and the screen, which is exactly
    // the correction wanted. Shelter does not bias it - a drop landing on a
    // roof is still a drop that landed.
    if (mSampleRadius <= 0.f) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 area = F_PI * mSampleRadius * mSampleRadius;
    mExpectedCount += SSPrecipSim::dropRateAt(cam) * area * dt;

    mMeasureElapsed += dt;
    if (mMeasureElapsed < DELIVERY_WINDOW) return;

    if (mExpectedCount > 1.f)
    {
        // Bounded hard. This is a correction on a rate that is already
        // physically derived, not a replacement for it: a bad window - the
        // camera cutting inside a wall, a region handover - must not be able to
        // turn the eaves off or open them into a flood.
        const F32 ratio = llclamp(mImpactCount / mExpectedCount, 0.35f, 2.f);
        mDelivery = lerp(mDelivery, ratio, DELIVERY_EMA);
    }

    mMeasureElapsed = 0.f;
    mImpactCount = 0.f;
    mExpectedCount = 0.f;
}

void SSRunoff::refreshNetworks()
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const F64 now = atmo->sharedTime();

    static LLCachedControl<U32> res_setting(gSavedSettings, "SSAtmoRunoffRes", 256);
    const S32 res = llclamp((S32)res_setting, 32, 512);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    std::vector<std::pair<U64, U32> > tiles;
    SSRainShadowMap::getInstance()->validTiles(tiles);

    // Drop networks whose region has gone, whose map has gone, or that the
    // camera has left far enough behind that nothing they hold could be seen
    for (auto it = mNetworks.begin(); it != mNetworks.end(); )
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(it->first);
        bool keep = regionp != nullptr;
        if (keep)
        {
            const F32 half = regionp->getWidth() * 0.5f;
            const LLVector3 centre = regionp->getOriginAgent() + LLVector3(half, half, 0.f);
            const F32 dx = centre.mV[VX] - cam.mV[VX];
            const F32 dy = centre.mV[VY] - cam.mV[VY];
            keep = dx * dx + dy * dy < REGION_REACH * REGION_REACH
                && std::any_of(tiles.begin(), tiles.end(),
                       [&](const std::pair<U64, U32>& t) { return t.first == it->first; });
        }
        it = keep ? std::next(it) : mNetworks.erase(it);
    }

    if (now - mLastRefresh < REFRESH_INTERVAL) return;
    if (mTraceBusy) return;     // one region in the pool at a time

    U64 stalest = 0;
    F32 stalest_d2 = 0.f;
    for (const auto& tile : tiles)
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.first);
        if (!regionp) continue;

        const F32 half = regionp->getWidth() * 0.5f;
        const LLVector3 centre = regionp->getOriginAgent() + LLVector3(half, half, 0.f);
        const F32 dx = centre.mV[VX] - cam.mV[VX];
        const F32 dy = centre.mV[VY] - cam.mV[VY];
        const F32 d2 = dx * dx + dy * dy;
        if (d2 > REGION_REACH * REGION_REACH) continue;

        // Only a change in the region's geometry is worth retracing for. The
        // tile behind it is recaptured far more often than that - every time
        // the camera climbs out of its vertical band, and every time the wind
        // swings the fall direction more than a few degrees - and retracing for
        // those would be milliseconds a second spent arriving at the same
        // answer, with the eaves twitching each time it landed slightly
        // differently.
        auto it = mNetworks.find(tile.first);
        if (it != mNetworks.end() && it->second.mBuiltSerial == tile.second
            && it->second.mBuiltRes == res)
        {
            continue;
        }

        // Nearest stale region first: that is the one whose eaves are in view
        if (stalest == 0 || d2 < stalest_d2)
        {
            stalest = tile.first;
            stalest_d2 = d2;
        }
    }

    if (stalest == 0) return;

    mLastRefresh = now;
    startTrace(stalest, res);
}

// Main thread: snapshot everything the trace reads, then hand it to the pool.
// The shadow map's tiles and the region list are only safe to touch here.
bool SSRunoff::startTrace(U64 region_handle, S32 res)
{
    if (mTraceBusy) return false;

    auto job = std::make_shared<Trace>();
    job->mRegionHandle = region_handle;
    job->mRes = res;

    if (!SSRainShadowMap::getInstance()->buildSurfaceGrid(region_handle, res, job->mGrid))
    {
        return false;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    job->mOrigin = regionp ? regionp->getOriginAgent() : LLVector3();

    // How steep a step has to be before the water stops running down it and
    // starts falling off it. Roofs are continuous however steep they are; a
    // real eave is a discontinuity in the capture, so the step between two
    // adjacent samples is a whole storey rather than a course of tiles.
    static LLCachedControl<F32> edge_setting(gSavedSettings, "SSAtmoRunoffEdge", 2.f);
    job->mCliffSlope = llclamp((F32)edge_setting, 0.75f, 8.f);

    // Wind driving the rain sideways is the one thing that makes a side edge
    // shed properly, so it is part of what the trace is solved against. Read
    // here, where the weather is safe to touch.
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    LLVector3 wind = atmo->windXY();
    const F32 speed = wind.normVec();
    job->mWindDir = wind;
    job->mWindPush = llclamp(speed / 12.f, 0.f, 1.f) * WIND_SHED_MAX;

    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");

    if (!general || !main)
    {
        // No pool to hand it to; better in the frame than not at all
        traceNetwork(*job);
        finishTrace(job);
        return true;
    }

    mTraceBusy = true;
    const U32 generation = mTraceGeneration;

    main->postTo(
        general,
        [job]() { traceNetwork(*job); return true; },
        [this, job, generation](bool)
        {
            mTraceBusy = false;
            // clear() while it was out: the network it belongs to has gone
            if (generation == mTraceGeneration) finishTrace(job);
        });
    return true;
}

// A run comes off the trace as a staircase. The cells are square and axis
// aligned; a roof is under no obligation to be, and an eave lying at any
// angle to the grid comes back as lip points alternating half a cell either
// side of the line the edge is really on. Each one is a legitimate cell
// centre and the set of them is a zig-zag, which is then made worse by the
// refinement, since that moves every lip independently along the shed
// direction and so keeps none of its neighbours' company.
//
// Nothing downstream wants the staircase. The run has already been cut
// wherever its lip stops being one straight line, so what is left is a
// straight line measured badly: fit it and put the points back on it. The
// axis is the principal one of the lips in plan rather than the mean of the
// cells' shed directions, because those are quantised to eight compass
// points and the fit is not.
static void straightenRun(SSRunoffEave& run)
{
    const size_t count = run.mLip.size();
    if (count < 3) return;

    LLVector3 centre;
    for (const LLVector3& p : run.mLip) centre += p;
    centre *= 1.f / (F32)count;

    F32 sxx = 0.f, sxy = 0.f, syy = 0.f;
    for (const LLVector3& p : run.mLip)
    {
        const F32 dx = p.mV[VX] - centre.mV[VX];
        const F32 dy = p.mV[VY] - centre.mV[VY];
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }

    // Larger eigenvector of the 2x2 scatter. Both forms of the eigenvector
    // are kept because either can come out degenerate on its own: the first
    // vanishes on a run lying along x, the second on one lying along y.
    const F32 trace = sxx + syy;
    const F32 det = sxx * syy - sxy * sxy;
    const F32 disc = llmax(0.f, trace * trace * 0.25f - det);
    const F32 lambda = trace * 0.5f + sqrtf(disc);

    LLVector3 axis(sxy, lambda - sxx, 0.f);
    if (axis.magVec() < 1e-4f) axis.set(lambda - syy, sxy, 0.f);
    if (axis.normVec() < 1e-4f) return;

    // The renderer hangs its curtain along mRun and signs the pitch below
    // along it, so the fit may aim the axis but not turn it round.
    if (axis * run.mRun < 0.f) axis = -axis;

    std::vector<F32> along(count);
    F32 lo = F32_MAX, hi = -F32_MAX;
    for (size_t i = 0; i < count; ++i)
    {
        along[i] = (run.mLip[i] - centre) * axis;
        lo = llmin(lo, along[i]);
        hi = llmax(hi, along[i]);
    }
    if (hi - lo < 1e-3f) return;    // no extent to fit a line to

    // Height along the run, fitted the same way. A gutter comes out level and
    // a rake comes out climbing at its own pitch, which is the whole reason
    // this is a fit and not a flattening.
    F32 stt = 0.f, stz = 0.f;
    for (size_t i = 0; i < count; ++i)
    {
        stt += along[i] * along[i];
        stz += along[i] * (run.mLip[i].mV[VZ] - centre.mV[VZ]);
    }
    const F32 slope = (stt > 1e-6f) ? (stz / stt) : 0.f;

    // The staircase also scrambles the order: neighbouring cells on a
    // diagonal edge project onto the axis within rounding of each other, so
    // the sort that ordered the run is only as good as the points it sorted.
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return along[a] < along[b]; });

    std::vector<LLVector3> lip, land;
    lip.reserve(count);
    land.reserve(count);
    for (size_t i : order)
    {
        LLVector3 fitted = centre + axis * along[i];
        fitted.mV[VZ] = centre.mV[VZ] + slope * along[i];
        // The drip still comes down where its own cell said it did; only the
        // part of the move that is horizontal carries over, because the
        // ground under the eave has a shape of its own to keep.
        LLVector3 down = run.mLand[i];
        down.mV[VX] += fitted.mV[VX] - run.mLip[i].mV[VX];
        down.mV[VY] += fitted.mV[VY] - run.mLip[i].mV[VY];

        lip.push_back(fitted);
        land.push_back(down);
    }

    run.mLip.swap(lip);
    run.mLand.swap(land);
    run.mRun = axis;
    run.mOut.set(axis.mV[VY], -axis.mV[VX], 0.f);

    // Measured along the fitted line rather than assumed to be one cell. A
    // diagonal edge steps corner to corner, so its lips are a cell and a half
    // apart and a run made of them is half as long again as the grid spacing
    // says - which is the length the streams are spread over.
    run.mSpacing = (hi - lo) / (F32)(count - 1);
}

// Worker. Pure arithmetic over the snapshot: no GL, no region list, no
// singletons beyond the job it was handed.
void SSRunoff::traceNetwork(Trace& job)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_RUNOFF_BUILD);
    LLTimer timer;

    const SSRainShadowMap::SurfaceGrid& grid = job.mGrid;

    const S32 n = grid.mN;
    const F32 cell = grid.mCell;
    const F32 cell_area = cell * cell;
    const size_t count = (size_t)n * n;

    const F32 cliff_slope = job.mCliffSlope;
    const F32 min_drop = 0.75f;

    // Below this, a height difference between neighbours is the capture's
    // own noise rather than real relief - a level street reads as a field of
    // sub-millimetre ties, not a field of exact equality, and letting any of
    // that decide a downhill direction is what put a puddle in the flow
    // solve at all: the "lowest" neighbour changes with whichever way the
    // noise fell on a given trace, so the direction - and with it whether
    // the cell pools or drains - could flip from one retrace to the next
    // with the ground never having moved. See the plateau pass below for
    // what a cell that clears no neighbour by this much does instead.
    static const F32 FLAT_NOISE_EPS = 0.02f;

    // How far off level a water neighbour is still trusted to be the same
    // waterline the ground beside it was built to, rather than a lower body
    // this column merely happens to overlook. Generous next to the capture's
    // own noise, because dock and canal edges are rarely modelled perfectly
    // flush, but nowhere near generous enough to reach a genuinely lower
    // drop - that is what the cliff/edge system below is for.
    static const F32 WATER_FLUSH_TOLERANCE = 0.5f;

    std::vector<S32> flow(count, -1);       // downstream cell on the surface
    std::vector<S32> spill(count, -1);      // where it lands when it leaves
    std::vector<U8> is_edge(count, 0);
    std::vector<F32> shed_frac(count, 0.f); // how much of the arriving water goes over
    std::vector<F32> catchment(count, 0.f);

    // Wind, for the rake allowance. Sampled once on the main thread into the
    // job: a worker has no business reading the live weather.
    const LLVector3& wind_dir = job.mWindDir;
    const F32 wind_push = job.mWindPush;

    static const S32 DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const S32 DY[8] = { 0, 1, 1,  1,  0, -1, -1, -1 };

    auto solid = [&](size_t i) { return grid.mFlags[i] != 0; };
    auto water = [&](size_t i) { return (grid.mFlags[i] & SSRainShadowMap::SURF_WATER) != 0; };

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            if (!solid(i)) continue;        // open air, nothing here to catch anything

            catchment[i] = cell_area;

            // Rain landing on water is already where it was going
            if (water(i)) continue;

            const F32 pz = grid.mZ[i];

            // Two separate questions, and conflating them is what put drips
            // all the way up the rake of every gable. "Where does water on
            // this cell run to" is answered by the roof surface, which is
            // continuous however steep it is. "Where could water leave the
            // surface here" is answered by the cliffs around it. A cell part
            // way up a gable rake has both: the roof carries on down toward
            // the eave, and there is a sheer drop off the side.
            S32 gentle = -1, cliff = -1;
            F32 gentle_slope = 0.f, cliff_slope_found = 0.f;

            for (S32 d = 0; d < 8; ++d)
            {
                const S32 nx = x + DX[d];
                const S32 ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                const size_t j = (size_t)ny * n + nx;
                if (!solid(j)) continue;

                // Water is a free sink at the waterline, not at any height:
                // a dock or canal edge is ordinarily built flush with the
                // water, near enough that the two surfaces were authored to
                // already agree rather than to measurably disagree, and it
                // is that near-agreement the ordinary drop test below is too
                // strict to see through. It is not a licence to drain into
                // any water anywhere near this column - a terrace three
                // storeys above a lake is not "flush" with it just because
                // the lake happens to be the nearest thing lower down, and
                // treating it as if it were would skip the eave/shed system
                // that already exists for a genuine drop into water,
                // teleporting the water down rather than letting it fall.
                // Past this tolerance a water neighbour is judged exactly
                // like any other drop - most such drops still end up
                // classified as cliffs, and a cliff spilling onto water
                // already lands correctly without this shortcut's help.
                if (water(j) && fabsf(pz - grid.mZ[j]) <= WATER_FLUSH_TOLERANCE)
                {
                    gentle = (S32)j;
                    gentle_slope = 1.0e6f;
                    continue;
                }

                const F32 drop = pz - grid.mZ[j];
                if (drop <= FLAT_NOISE_EPS) continue;

                // The grid is axis aligned and regular, so the separation is
                // the cell size on the cardinals and its diagonal otherwise
                const F32 h = (DX[d] && DY[d]) ? cell * 1.41421f : cell;
                const F32 slope = drop / h;

                if (slope > cliff_slope && drop > min_drop)
                {
                    if (slope > cliff_slope_found)
                    {
                        cliff_slope_found = slope;
                        cliff = (S32)j;
                    }
                }
                else if (slope > gentle_slope)
                {
                    gentle_slope = slope;
                    gentle = (S32)j;
                }
            }

            // Water follows the roof while the roof still leads somewhere.
            // Accumulating along the cliff instead is what let a rake collect
            // the catchment that belongs to the eave below it.
            flow[i] = (gentle >= 0) ? gentle : cliff;

            if (cliff < 0) continue;    // no way off the surface here

            if (gentle < 0)
            {
                // Nowhere left to run: the surface genuinely ends at this cell
                // and everything arriving leaves over the edge. This is a
                // bottom eave, and it sheds in full.
                is_edge[i] = 1;
                shed_frac[i] = 1.f;
                spill[i] = cliff;
                continue;
            }

            // Both: a side edge. How much leaves over it depends on whether
            // the water was heading that way anyway. Down the rake of a gable
            // the roof runs along the edge and the drop is off to the side, so
            // almost nothing goes over. Where the roof's own fall lines up
            // with the drop, the two are the same edge and it sheds properly.
            const LLVector3 down(grid.axis(gentle % n) - grid.axis(x),
                                 grid.axis(gentle / n) - grid.axis(y), 0.f);
            const LLVector3 over(grid.axis(cliff % n) - grid.axis(x),
                                 grid.axis(cliff / n) - grid.axis(y), 0.f);

            LLVector3 down_n = down, over_n = over;
            if (down_n.normVec() < 0.01f || over_n.normVec() < 0.01f) continue;

            const F32 align = down_n * over_n;

            // Nothing below a right angle: that is water running along the
            // edge, not over it
            F32 frac = llclamp((align - EDGE_ALIGN_MIN) / (1.f - EDGE_ALIGN_MIN), 0.f, 1.f);
            frac *= frac * (3.f - 2.f * frac);

            // Alignment alone is not enough, because along a gutter there is
            // nothing to align with. The lip of an eave is level to within the
            // noise of the capture, so every cell on it finds a neighbour a
            // centimetre lower *along* the line, at right angles to the drop -
            // and the whole run is then read as water flowing sideways past
            // the edge rather than over it. One cell, wherever the noise
            // happens to bottom out, ends up the only way off a fifty metre
            // eave, which is what put two drip lines on a roof that should
            // have a curtain of them.
            //
            // What actually decides it is whether the surface is still
            // carrying the water anywhere. A rake has a whole roof pitch
            // pulling water down it and past the drop; a gutter has a
            // millimetre of sag. So the flatter the onward path, the more of
            // the water simply goes over the side.
            const F32 carry = llclamp((gentle_slope - LIP_FLAT_SLOPE) /
                                      (CARRY_SLOPE - LIP_FLAT_SLOPE), 0.f, 1.f);
            const F32 flat = 1.f - carry * carry * (3.f - 2.f * carry);
            frac = llmax(frac, flat);

            // A rake still drips when the wind is driving the rain across it,
            // which is what makes a squall visible on the gable end of a
            // building rather than only along its gutters
            if (wind_push > 0.f)
            {
                const F32 blown = llclamp(over_n * wind_dir, 0.f, 1.f) * wind_push;
                frac = llmax(frac, blown);
            }

            if (frac < MIN_SHED_FRAC) continue;

            is_edge[i] = 1;
            shed_frac[i] = frac;
            spill[i] = cliff;
        }
    }

    // The plateau pass. Every cell above found no neighbour more than
    // FLAT_NOISE_EPS lower than itself is still sitting at flow[i] == -1 -
    // not because it has nowhere to drain, but because the loop above only
    // ever asks its own eight neighbours, and on level ground the real
    // answer is "the same way everything else on this plateau goes", which
    // is not a question one cell's own neighbourhood can answer. Water on a
    // real street does not stay put for want of a local gradient either; it
    // piles up until the surface tilts enough to move it toward wherever it
    // can actually leave, which in practice is the nearest edge of the flat
    // stretch it is sitting on.
    //
    // Grown as a breadth-first flood from every cell that DID resolve a real
    // direction, out across same-height neighbours one ring at a time. That
    // gives three things a purely local re-guess at the tie could not: the
    // direction found is always toward the nearest real exit rather than a
    // noise-driven guess, it is the same direction every time the trace
    // reruns over unchanged geometry because the flood order depends only on
    // the (fixed) grid and never on which way noise happened to break a tie,
    // and it chains a landing between two ramps on to the second ramp
    // exactly the way it chains a street on to the drain at the end of it -
    // both are just a flat cell one ring out from a cell that already knows
    // where to go.
    std::vector<S32> plateau_queue;
    plateau_queue.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (flow[i] >= 0) plateau_queue.push_back((S32)i);
    }

    std::vector<S32> plateau_chain;   // discovery order, oldest (nearest a real exit) first
    for (size_t qhead = 0; qhead < plateau_queue.size(); ++qhead)
    {
        const S32 i = plateau_queue[qhead];
        const S32 ix = i % n, iy = i / n;

        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = ix + DX[d];
            const S32 ny = iy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const size_t j = (size_t)ny * n + nx;
            if (!solid(j) || water(j) || flow[j] >= 0) continue;
            if (fabsf(grid.mZ[j] - grid.mZ[i]) > FLAT_NOISE_EPS) continue;

            flow[j] = i;
            plateau_queue.push_back((S32)j);
            plateau_chain.push_back((S32)j);
        }
    }

    // Carried leaf-to-root: the reverse of the order the flood discovered
    // them in, so a cell's catchment has already reached the neighbour it
    // flows into before that neighbour's own turn to pass anything on. The
    // height-sorted pass below gets this ordering for free from strictly
    // decreasing height; two plateau cells at the same height cannot, which
    // is exactly why they are settled here first and left out of that pass
    // entirely rather than trusted to fall in the right place in it.
    for (auto it = plateau_chain.rbegin(); it != plateau_chain.rend(); ++it)
    {
        const S32 i = *it;
        const F32 carried = catchment[i];
        const S32 down = flow[i];
        if (down >= 0 && carried > 0.f) catchment[down] += carried;
    }

    std::vector<U8> is_plateau(count, 0);
    for (S32 i : plateau_chain) is_plateau[i] = 1;

    // Flow accumulation. Water only ever moves to a strictly lower cell, so
    // walking the grid from the top down visits every cell after everything
    // that drains into it: no cycles to guard against, and one pass to do it.
    // Plateau cells are excluded - the flood above already settled them, in
    // the order that same-height ties actually need and this sort cannot
    // give them.
    std::vector<S32> order;
    order.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (solid(i) && !is_plateau[i]) order.push_back((S32)i);
    }
    std::sort(order.begin(), order.end(), [&](S32 a, S32 b) { return grid.mZ[a] > grid.mZ[b]; });

    // A partial edge passes on whatever it did not shed. The rake of a gable
    // keeps most of its water on the roof and hands it down toward the gutter,
    // which is where that water is supposed to arrive; only the share the edge
    // actually sheds is taken out of the flow.
    std::vector<F32> shed_here(count, 0.f);

    for (S32 i : order)
    {
        const F32 arriving = catchment[i];
        const F32 frac = is_edge[i] ? shed_frac[i] : 0.f;

        if (frac > 0.f) shed_here[i] = arriving * frac;

        const F32 carried = arriving - shed_here[i];
        const S32 down = flow[i];
        if (down < 0 || carried <= 0.f) continue;   // pooled, or nothing left
        catchment[down] += carried;
    }

    // Gather the edge cells into runs. Water does not leave a roof at a few
    // isolated points, it leaves along a line: a gable lip is thirty edge cells
    // in a row, and shedding those independently is what makes the drips arrive
    // in a dotted line with dry gaps between them. Cells join the same run when
    // they touch, sit at much the same height, and shed the same way.
    const F32 min_catch = llmax(2.f, cell_area * 2.f);
    const F32 join_dz = llmax(0.6f, cell);

    std::vector<S32> label(count, -1);
    std::vector<SSRunoffEave> runs;
    std::vector<S32> stack;
    std::vector<S32> cells;

    // One edge cell's lip point, the point the water off it comes down at, and
    // the horizontal direction it leaves by. The fill gathers cells now and the
    // runs are built from them afterwards, so these are needed in both places.
    auto lipOf = [&](S32 i)
    {
        return LLVector3(grid.axis(i % n), grid.axis(i / n), grid.mZ[i]);
    };
    auto landOf = [&](S32 i)
    {
        // Where it leaves, not where the surface carries on. On a partial edge
        // those are different cells, and it is the spill that says which way
        // the drip goes.
        const S32 d = (spill[i] >= 0) ? spill[i] : i;
        return LLVector3(grid.axis(d % n), grid.axis(d / n), grid.mZ[d]);
    };
    auto outOf = [&](S32 i)
    {
        const LLVector3 lip = lipOf(i);
        const LLVector3 land = landOf(i);
        LLVector3 o(land.mV[VX] - lip.mV[VX], land.mV[VY] - lip.mV[VY], 0.f);
        if (o.normVec() < 0.01f) o.set(1.f, 0.f, 0.f);
        return o;
    };

    // Turn one stretch of edge cells into a run. The stretch arrives roughly in
    // order along the lip, from the patch's mean axis; this works out the axis
    // the stretch itself has and puts the cells in order along that, because a
    // piece cut off the end of a patch does not have to point the way the whole
    // patch did.
    auto addRun = [&](std::vector<S32>::const_iterator first,
                      std::vector<S32>::const_iterator last)
    {
        if (first == last) return;

        SSRunoffEave run;
        run.mSpacing = cell;

        LLVector3 out_sum;
        for (auto it = first; it != last; ++it) out_sum += outOf(*it);

        run.mOut = out_sum;
        if (run.mOut.normVec() < 0.01f) run.mOut.set(1.f, 0.f, 0.f);

        // Along the edge, at right angles to the way the water leaves it. The
        // renderer hangs the curtain along this same axis and the pitch below
        // is signed along it, so the handedness of this cross product is not
        // free to change.
        run.mRun.set(-run.mOut.mV[VY], run.mOut.mV[VX], 0.f);

        // Put the lips in order along the edge. They arrive in whatever order
        // the flood fill happened to pop them off its stack, which is fine for
        // picking a random point to drip from and no use at all for anything
        // that has to treat the run as a line: the overlay drew a scatter of
        // unconnected ticks, the thinning below strided across the run rather
        // than along it, and a stream has no path to follow. Runs are cut
        // where their line turns, so a projection onto the along-edge axis is
        // enough to order one.
        std::vector<S32> ordered(first, last);
        const LLVector3 axis = run.mRun;
        std::sort(ordered.begin(), ordered.end(),
                  [&](S32 a, S32 b) { return lipOf(a) * axis < lipOf(b) * axis; });

        run.mLip.reserve(ordered.size());
        run.mLand.reserve(ordered.size());
        for (S32 i : ordered)
        {
            run.mLip.push_back(lipOf(i));
            run.mLand.push_back(landOf(i));
            run.mCatchment += shed_here[i];
            run.mShedSum += shed_frac[i];
        }

        run.mShed = run.mShedSum / (F32)ordered.size();

        // Off the grid's staircase and onto the line the edge is really on
        straightenRun(run);

        runs.push_back(std::move(run));
    };

    for (size_t seed = 0; seed < count; ++seed)
    {
        if (!is_edge[seed] || label[seed] >= 0) continue;

        cells.clear();

        // The cell the patch started from. Neighbours are held against this as
        // well as against the cell they joined through, because agreeing with
        // your neighbour is a chain and a chain has no limit: a run creeping
        // round a rotunda or a chamfered corner turns a few degrees per cell,
        // never fails a local test, and comes back as one run whose mean
        // direction belongs nowhere on it.
        const LLVector3 seed_out = outOf((S32)seed);

        stack.push_back((S32)seed);
        label[seed] = 1;

        while (!stack.empty())
        {
            const S32 i = stack.back();
            stack.pop_back();
            cells.push_back(i);

            const S32 x = i % n;
            const S32 y = i / n;
            const LLVector3 out = outOf(i);

            for (S32 d = 0; d < 8; ++d)
            {
                const S32 nx = x + DX[d];
                const S32 ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                const size_t j = (size_t)ny * n + nx;
                if (!is_edge[j] || label[j] >= 0) continue;
                if (fabsf(grid.mZ[j] - grid.mZ[i]) > join_dz) continue;

                // And it has to shed the same way. Two edges meeting at the
                // corner of a roof touch and sit at the same height, so
                // without this the fill walks straight round the corner from
                // a gutter onto the rake and returns one run wrapping the
                // whole roof - whose mean direction then belongs to neither,
                // so its drips leave along a diagonal into thin air.
                const S32 jd = spill[j];
                if (jd < 0) continue;

                LLVector3 jout(grid.axis(jd % n) - grid.axis(nx),
                               grid.axis(jd / n) - grid.axis(ny), 0.f);
                if (jout.normVec() < 0.01f) continue;
                if (jout * out < RUN_JOIN_ALIGN) continue;
                if (jout * seed_out < RUN_JOIN_ALIGN) continue;

                label[j] = 1;
                stack.push_back((S32)j);
            }
        }

        if (cells.empty()) continue;

        // Order the patch along its own mean edge axis, so what follows can
        // read it as a line. Provisional only - each piece the cut hands back
        // works out its own axis from the cells it ended up with.
        LLVector3 patch_out;
        for (S32 i : cells) patch_out += outOf(i);
        if (patch_out.normVec() < 0.01f) patch_out.set(1.f, 0.f, 0.f);

        const LLVector3 patch_axis(-patch_out.mV[VY], patch_out.mV[VX], 0.f);
        std::sort(cells.begin(), cells.end(),
                  [&](S32 a, S32 b)
                  {
                      return lipOf(a) * patch_axis < lipOf(b) * patch_axis;
                  });

        // Cut the patch wherever its lip stops being one straight line.
        //
        // Shedding the same way is not the same as running the same way, and
        // the gable is the case that shows it. The two rakes either side of a
        // ridge shed off the same gable end, in the same horizontal direction,
        // and they meet at the apex at much the same height - so every test
        // above passes and the fill hands back both arms as one run. Its lips
        // then climb to the ridge and come back down, its pitch measured end
        // to end is nothing like either arm, and the stream hung along it is a
        // curtain across a peak: level where the roof is steepest, cutting
        // through the tiles at both ends. The same is true, less dramatically,
        // of an edge that bends a few degrees along its length.
        //
        // So a line is tracked along the ordered lips and the patch is cut
        // where a lip departs from it. The tolerance grows with distance from
        // the anchor of that line, which is what makes it an angle rather than
        // a width, and it never falls below a cell: the trace works on a grid,
        // and an edge not running along one of its axes staircases about half
        // a cell either side of the line it is really on.
        size_t start = 0;
        LLVector3 anchor = lipOf(cells[0]);
        LLVector3 dir;
        bool have_dir = false;

        for (size_t k = 1; k < cells.size(); ++k)
        {
            const LLVector3 lip = lipOf(cells[k]);
            const LLVector3 v = lip - anchor;

            if (!have_dir)
            {
                // A direction taken from two neighbouring cells is a direction
                // taken from the staircase, so the line is not aimed until
                // there is a baseline long enough to aim it along.
                if (v.magVec() >= cell * 2.f)
                {
                    dir = v;
                    dir.normVec();
                    have_dir = true;
                }
                continue;
            }

            const F32 along = v * dir;
            const F32 off = (v - dir * along).magVec();

            if (off > llmax(cell, RUN_SHAPE_TAN * fabsf(along)))
            {
                // The piece ends at the lip before this one, and this one
                // anchors the next: the cells are in order along the edge, so
                // the two pieces meet rather than leaving a gap.
                addRun(cells.begin() + start, cells.begin() + k);
                start = k;
                anchor = lip;
                have_dir = false;
            }
        }

        addRun(cells.begin() + start, cells.end());
    }

    for (SSRunoffEave& run : runs)
    {
        if (run.mCatchment < min_catch || run.mLip.empty()) continue;

        // A long run is thinned rather than truncated, so the drips still cover
        // its whole length
        if (run.mLip.size() > MAX_LIPS)
        {
            const size_t stride = (run.mLip.size() + MAX_LIPS - 1) / MAX_LIPS;
            std::vector<LLVector3> lip, land;
            for (size_t i = 0; i < run.mLip.size(); i += stride)
            {
                lip.push_back(run.mLip[i]);
                land.push_back(run.mLand[i]);
            }
            run.mLip.swap(lip);
            run.mLand.swap(land);
            run.mSpacing *= (F32)stride;
        }

        // The lip refinement is left to the main thread: it reads the shadow
        // map's own tiles, which the capture is free to replace at any time.
        job.mEaves.push_back(std::move(run));
    }

    std::sort(job.mEaves.begin(), job.mEaves.end(),
              [](const SSRunoffEave& a, const SSRunoffEave& b) { return a.mCatchment > b.mCatchment; });
    if (job.mEaves.size() > MAX_EAVES) job.mEaves.resize(MAX_EAVES);

    // The solved surface is kept whole. The eaves above are the part that
    // sheds; this is the part that everything dressing the surface reads, and
    // it is the same arithmetic either way - the only cost of keeping it is
    // the memory, about a megabyte a region at the default resolution.
    job.mFieldN = n;
    job.mFieldZ = grid.mZ;
    job.mFieldFlow.assign(flow.begin(), flow.end());
    job.mFieldEdge.assign(is_edge.begin(), is_edge.end());
    job.mFieldCatch.swap(catchment);
    job.mFieldFlags = grid.mFlags;

    job.mMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
}

// Main thread: refine the lips against the full-resolution map, then install.
void SSRunoff::finishTrace(const std::shared_ptr<Trace>& job)
{
    Network& net = mNetworks[job->mRegionHandle];
    net.mRegionHandle = job->mRegionHandle;

    // Water standing on the roofs is carried across a retrace as a total, so a
    // prim edit next door does not make every eave in the region stutter
    F32 carried = 0.f;
    for (const SSRunoffEave& eave : net.mEaves) carried += eave.mStore;

    net.mBuiltSerial = job->mGrid.mGeomSerial;
    net.mBuiltRes = job->mRes;
    ++mBuildCount;

    SSRainShadowMap* shadow = SSRainShadowMap::getInstance();
    const LLVector3& origin = job->mOrigin;
    const F32 cell = job->mGrid.mCell;
    const F32 join_dz = llmax(0.6f, cell);

    net.mEaves = std::move(job->mEaves);

    // Refine each lip against the map at its own full resolution. The trace
    // runs on metre cells; the capture is centimetres, and this is what puts
    // the drip on the actual edge of the geometry rather than in the middle
    // of whichever cell happened to straddle it.
    for (SSRunoffEave& run : net.mEaves)
    {
        for (size_t i = 0; i < run.mLip.size(); ++i)
        {
            LLVector3 refined;
            if (shadow->refineEdge(job->mRegionHandle, origin + run.mLip[i], run.mOut,
                                   cell * 1.5f, join_dz, refined))
            {
                run.mLip[i] = refined - origin;
            }
        }

        // Refinement moves each lip on its own, against whatever the map holds
        // under it, so a run that arrived straight does not stay that way: a
        // lip that finds the edge a centimetre early and its neighbour that
        // finds it a centimetre late put a kink between them. The fit is the
        // same one the trace made and it keeps the part of the refinement that
        // was agreed on - where the run sits and which way it points - while
        // dropping the part each lip made up by itself.
        straightenRun(run);
    }

    // Put the water that was standing on the roofs back, shared out by
    // catchment, so a retrace is invisible rather than a pause in the drips
    F32 total_catch = 0.f;
    for (const SSRunoffEave& eave : net.mEaves) total_catch += eave.mCatchment;
    if (total_catch > 0.f && carried > 0.f)
    {
        for (SSRunoffEave& eave : net.mEaves)
        {
            eave.mStore = carried * (eave.mCatchment / total_catch);
        }
    }

    net.mFieldN = job->mFieldN;
    net.mFieldCell = job->mGrid.mCell;
    net.mFieldZ = std::move(job->mFieldZ);
    net.mFieldFlow = std::move(job->mFieldFlow);
    net.mFieldEdge = std::move(job->mFieldEdge);
    net.mFieldCatch = std::move(job->mFieldCatch);
    net.mFieldFlags = std::move(job->mFieldFlags);

    mLastBuildMS = job->mMS;
}

// Streams for one run. Placed at fixed fractions along the lip rather than at
// random points: a stream is anchored and refreshed in place, so it has to
// come out at the same spot every frame or it would crawl along the gutter.
void SSRunoff::shedStreams(const SSRunoffEave& eave, const LLVector3& origin,
                           U64 region_handle, U32 eave_index, F32 drive)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo->sim();
    if (!sim) return;

    // Length of the run, and the places along it worth running from. The slots
    // are a property of the run alone - never of how hard it is shedding - so
    // they stay put while a gutter fills and empties. Scaling the count with
    // the drive instead would slide every stream along the roof each time a
    // gust passed over it.
    const F32 run_len = eave.mSpacing * (F32)llmax<size_t>(1, eave.mLip.size() - 1);

    // As close to one sheet quad each as the run divides into. Rounding up
    // rather than down, so the slots come out at or under that width and the
    // art is never stretched past the size it was drawn for. A preset that
    // wants a different figure says so outright and is taken at its word.
    const SSPrecipPreset& preset = atmo->preset();
    const F32 sheet_w = preset.mTiers[TIER_SHEETS].mSizeX * 2.f;
    const F32 target = (preset.mStreamSpan > 0.f)
        ? llclamp(preset.mStreamSpan, 1.f, STREAM_SPAN_MAX)
        : llclamp(sheet_w > 1.f ? sheet_w : STREAM_SPAN_ALT,
                  STREAM_SPAN_MIN, STREAM_SPAN_MAX);
    const S32 slots = llclamp((S32)ceilf(run_len / target), 1, MAX_RUN_STREAMS);

    // Every slot covers its share of the run, so the streams meet end to end.
    // A run longer than the cap allows for gets wider streams rather than gaps
    // between them: a bridge deck sheds along all of itself too.
    const F32 slot_w = run_len / (F32)slots;

    // Filling out a little with the flow, but only a little: with slots this
    // wide a run is one or two of them, and narrowing them to a thread would
    // leave the ends of the gutter dry rather than reading as less water. How
    // hard it is running shows in the opacity instead.
    const F32 width = slot_w * (0.8f + 0.2f * drive);

    for (S32 k = 0; k < slots; ++k)
    {
        // Stable across frames and across clients: the same run at the same
        // place is the same stream, so two people watching one roof see the
        // water coming off it in the same places.
        const U32 key = SSAtmoNoise::combine(
            SSAtmoNoise::combine((U32)region_handle, (U32)(region_handle >> 32)),
            SSAtmoNoise::combine(eave_index, (U32)k));

        // Spread across the run, inset from the ends so a stream does not hang
        // off the corner of a roof
        const F32 t = ((F32)k + 0.5f) / (F32)slots;
        const size_t idx = llmin((size_t)(t * (F32)eave.mLip.size()),
                                 eave.mLip.size() - 1);

        // How the lip climbs over the stretch this stream covers. Plenty of
        // eaves are not level - the rake up the side of a gable, the sides of
        // a valley, a sloped awning - and a curtain hung horizontally off one
        // cuts straight through the roof shedding onto it. The lips are in
        // order along the run and a run is split where its direction turns, so
        // the two ends of the slot are enough to say what its pitch is.
        const size_t first = llmin((size_t)((F32)k / (F32)slots * (F32)eave.mLip.size()),
                                   eave.mLip.size() - 1);
        const size_t last = llmin((size_t)((F32)(k + 1) / (F32)slots * (F32)eave.mLip.size()),
                                  eave.mLip.size() - 1);

        // A slot narrower than the lip spacing lands on one point, which says
        // nothing about the pitch; the neighbouring point does.
        const size_t a = (first == last && first > 0) ? first - 1 : first;
        const size_t b = (first == last && first + 1 < eave.mLip.size()) ? first + 1 : last;

        F32 slope = 0.f;
        if (b > a)
        {
            const LLVector3 delta = eave.mLip[b] - eave.mLip[a];
            const F32 flat = sqrtf(delta.mV[VX] * delta.mV[VX] + delta.mV[VY] * delta.mV[VY]);
            if (flat > 0.01f) slope = delta.mV[VZ] / flat;
        }

        // The slot was measured across the ground, and a sloped lip is longer
        // than its own shadow. Without this a rake is covered by a curtain the
        // length of the wall below it and the top of the run stays dry.
        const F32 slot_span = width * sqrtf(1.f + slope * slope);

        SSRandStream rng(key);

        sim->refreshStream(key,
                           origin + eave.mLip[idx],
                           eave.mOut,
                           origin + eave.mLand[idx],
                           drive,
                           slot_span,
                           slope,
                           rng);
    }
}

void SSRunoff::shed(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo->sim();
    if (!sim || dt <= 0.f) { mDripRate = 0.f; return; }

    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoRunoffRadius", 48.f);
    static LLCachedControl<F32> merge_setting(gSavedSettings, "SSAtmoRunoffMerge", 12.f);
    static LLCachedControl<F32> scale_setting(gSavedSettings, "SSAtmoRunoffScale", 1.f);

    const F32 radius = llclamp((F32)radius_setting, 8.f, 128.f);
    const F32 merge = llclamp((F32)merge_setting, 1.f, 200.f);
    const F32 scale = llclamp((F32)scale_setting, 0.f, 4.f);
    if (scale <= 0.f) { mDripRate = 0.f; return; }

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    // Drops per square metre per second the weather is delivering, corrected by
    // what the impact queue says is actually arriving. Sampled once at the
    // camera rather than per eave: the area field it comes from drifts over
    // hundreds of metres, so it says the same thing anywhere in the radius.
    const F32 rate_m2 = SSPrecipSim::dropRateAt(cam) * mDelivery;

    // Only drips count against the drip budget. They share the effects pool
    // with the ripples and splash crowns, and in heavy rain the splashes alone
    // fill it - budgeting against the pool shut the eaves off exactly when it
    // was raining hardest, which is precisely backwards.
    const S32 live = sim->dripCount();
    const F32 budget = (live >= DRIP_BUDGET) ? 0.f
                     : llmin(1.f, (F32)(DRIP_BUDGET - live) / (F32)(DRIP_BUDGET / 4));

    const F64 now = atmo->sharedTime();
    F32 total_rate = 0.f;

    for (auto& entry : mNetworks)
    {
        Network& net = entry.second;
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();

        for (size_t i = 0; i < net.mEaves.size(); ++i)
        {
            SSRunoffEave& eave = net.mEaves[i];
            if (eave.mLip.empty()) continue;

            // The roof fills and drains whether or not anyone is watching, so
            // the store is integrated for every eave in the network. Gating the
            // accounting on distance instead would mean walking up to a roof in
            // a downpour and finding it dry for the first few seconds.
            //
            // The store is what carries the drips through a gust lull. Rain
            // arriving in waves does not make a roof stop draining between
            // them, and a shed rate taken straight off the instantaneous
            // weather made the eaves stutter in time with the gusts.
            const F32 inflow = eave.mCatchment * rate_m2 * scale;
            eave.mStore = llmin(eave.mStore + inflow * dt, inflow * STORE_CEILING + 1.f);

            const F32 outflow = eave.mStore / DRAIN_TAU;
            eave.mStore = llmax(0.f, eave.mStore - outflow * dt);

            const LLVector3 centre = origin + eave.mLip[0];
            const F32 dx = centre.mV[VX] - cam.mV[VX];
            const F32 dy = centre.mV[VY] - cam.mV[VY];
            const F32 dz = centre.mV[VZ] - cam.mV[VZ];
            if (dx * dx + dy * dy + dz * dz > radius * radius)
            {
                eave.mAccum = 0.f;
                continue;
            }

            // Water gathers on the way down the slope and arrives at the lip as
            // fewer, fatter drips. That is what merge stands for, and it is also
            // what keeps a warehouse roof from costing more particles than the
            // rain falling on it.
            const F32 raw_rate = outflow / merge;

            // Past a point an edge stops shedding drops at all. A gutter in a
            // downpour comes off as a continuous fall, and drawing that as
            // more and more separate drips both reads wrong and costs a
            // particle each. Over the threshold the run puts up streams
            // instead, and the drips it still emits are cut back to the
            // spatter around them.
            const F32 stream_drive = llclamp((raw_rate - STREAM_RATE_MIN)
                                             / STREAM_RATE_FULL, 0.f, 1.f);

            if (stream_drive > 0.f)
            {
                shedStreams(eave, origin, entry.first, (U32)i, stream_drive);
            }

            // Drips are the spatter around a running stream, not a second
            // helping of it. Once it is up the stream carries the water and
            // a full share of drips on top of it reads as rain falling off the
            // roof twice.
            const F32 drips_per_s = llmin(raw_rate * (1.f - 0.9f * stream_drive),
                                          MAX_EAVE_RATE);
            total_rate += drips_per_s;
            if (budget <= 0.f) continue;

            eave.mAccum += drips_per_s * budget * dt;
            S32 shed_now = (S32)eave.mAccum;
            if (shed_now <= 0) continue;

            shed_now = llmin(shed_now, MAX_EAVE_BURST);
            eave.mAccum -= (F32)shed_now;

            // One stream per run, advanced by a coarse clock tick, so two
            // clients watching the same roof see the same drips at the same
            // size rather than two unrelated dribbles
            SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
                SSAtmoNoise::combine((U32)(S64)(now * 8.0),
                SSAtmoNoise::combine((U32)(S32)(eave.mLip[0].mV[VX] * 8.f),
                                     (U32)(S32)(eave.mLip[0].mV[VY] * 8.f)))));
            rng.next();

            for (S32 k = 0; k < shed_now; ++k)
            {
                // Anywhere along the run, not only at the sampled points. Those
                // are a metre or so apart, and without the slide the drips fall
                // in a dotted line with dry gaps between them, which is the one
                // thing a roof edge never looks like.
                const S32 pick = rng.rand((S32)eave.mLip.size());
                const LLVector3 along = eave.mRun * (rng.frand(-0.5f, 0.5f) * eave.mSpacing);

                sim->spawnDrip(origin + eave.mLip[pick] + along, eave.mOut,
                               origin + eave.mLand[pick] + along, merge, rng);
            }
        }
    }

    mDripRate = total_rate;
}

void SSRunoff::renderDebug()
{
    if (!runoffApplies()) return;

    refreshNetworks();

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);     // test against the world, do not write
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoRunoffRadius", 48.f);
    const F32 reach = llclamp((F32)radius_setting, 8.f, 128.f) * 1.5f;

    for (const auto& entry : mNetworks)
    {
        const Network& net = entry.second;
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp || net.mFieldN < 2) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const S32 n = net.mFieldN;
        const F32 cell = regionp->getWidth() / (F32)n;

        auto cellPos = [&](S32 i)
        {
            return LLVector3(origin.mV[VX] + ((F32)(i % n) + 0.5f) * cell,
                             origin.mV[VY] + ((F32)(i / n) + 0.5f) * cell,
                             net.mFieldZ[i] + 0.05f);
        };

        // Flow arrows, brightening with how much drains through each cell
        gGL.begin(LLRender::LINES);
        for (S32 i = 0; i < (S32)net.mFieldZ.size(); ++i)
        {
            const S32 down = net.mFieldFlow[i];
            const bool eave = net.mFieldEdge[i] != 0;

            // Nowhere to draw to: water stands here, or it goes over a lip
            // that the trace found no landing for
            if (down < 0) continue;

            const LLVector3 p = cellPos(i);
            if ((p - cam).magVecSquared() > reach * reach) continue;

            const F32 strength = llclamp(net.mFieldCatch[i] / 60.f, 0.05f, 1.f);
            const LLVector3 q = cellPos(down);

            if (eave)
            {
                // Red the whole way down the face the water leaves by, so the
                // fall it takes is visible rather than implied
                gGL.color4f(1.f, 0.25f, 0.1f, 0.35f + 0.5f * strength);
                gGL.vertex3fv(p.mV);
                gGL.vertex3fv(q.mV);
            }
            else
            {
                gGL.color4f(0.2f, 0.6f, 1.f, 0.1f + 0.6f * strength);
                gGL.vertex3fv(p.mV);
                gGL.vertex3fv((p + (q - p) * 0.7f).mV);
            }
        }
        gGL.end();

        // The runs that actually shed, drawn as the connected lines they are
        // rather than as isolated markers, brightening with how much water each
        // is currently holding
        for (const SSRunoffEave& eave : net.mEaves)
        {
            if (eave.mLip.empty()) continue;
            if ((origin + eave.mLip[0] - cam).magVecSquared() > reach * reach) continue;

            const F32 heat = llclamp(eave.mStore / llmax(1.f, eave.mCatchment * 0.05f), 0.f, 1.f);

            // Green where the run sheds in full - a gutter along the bottom of
            // a roof - shading to amber where it only sheds what the wind
            // drives over it, which is what a gable rake does. Two runs that
            // look alike on the roof behave very differently, and this is what
            // says which is which.
            const LLVector3 lift(0.f, 0.f, 0.06f);
            gGL.color4f(0.2f + 0.8f * (1.f - eave.mShed),
                        0.7f + 0.3f * heat,
                        0.4f * eave.mShed,
                        0.9f);

            // The run itself, as the line it is. The lips are ordered along
            // the edge, so this traces the actual gutter rather than joining
            // whatever two cells happened to be adjacent in the fill.
            gGL.begin(LLRender::LINE_STRIP);
            for (size_t i = 0; i < eave.mLip.size(); ++i)
            {
                gGL.vertex3fv((origin + eave.mLip[i] + lift).mV);
            }
            gGL.end();

            // And where the water goes over, every few points so the run stays
            // readable. These are the drops the drips actually take.
            const size_t stride = llmax<size_t>(1, eave.mLip.size() / 8);
            gGL.begin(LLRender::LINES);
            for (size_t i = 0; i < eave.mLip.size(); i += stride)
            {
                gGL.color4f(1.f, 0.4f, 0.15f, 0.7f);
                gGL.vertex3fv((origin + eave.mLip[i] + lift).mV);
                gGL.color4f(1.f, 0.4f, 0.15f, 0.05f);
                gGL.vertex3fv((origin + eave.mLand[i]).mV);
            }
            gGL.end();
        }
    }

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

// </SS:Nexii>
