/**
 * @file sssurfacefield.cpp
 * @brief Atmo Magic surface field: the slow integration of weather into the
 *        surface it falls on, over the drainage network.
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

#include "sssurfacefield.h"

#include "ssatmomagic.h"
#include "ssavatarwet.h"
#include "ssprecippreset.h"
#include "ssprecipitation.h"
#include "ssrainshadow.h"

#include "llappviewer.h"
#include "llenvironment.h"
#include "llfasttimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llsettingswater.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>
#include <glm/mat4x4.hpp>

// <SS:Nexii> Atmo Magic surface field

extern bool gCubeSnapshot;

static const F32 TICK_INTERVAL   = 0.25f;   // seconds between integrations
static const F32 MAX_TICK_DT     = 8.f;     // longest step honoured in one go
static const F64 FIELD_KEEP      = 20.0;    // seconds a field survives its region going quiet
static const size_t MAX_FIELDS   = 4;

// How far the surface may move under a cell before what has accumulated there
// is taken to belong to something else. A prim nudged a few centimetres is the
// same roof; one rezzed a metre up is not.
static const F32 REBUILD_DZ      = 0.35f;

// Slope is read from the height field, and across the lip of a roof that
// gradient is not a slope at all - it is a cliff with open air on the far
// side. Neighbours further than this below a cell are left out of the fit, so
// the edge of a flat roof reads flat, which is what it is.
static const F32 SLOPE_STEP_MAX  = 3.f;     // multiples of the cell size


// The stitched window handed to the shaders. 256 cells at the drainage
// resolution is 256 metres around the camera on a standard region, which is
// further than a damp pavement reads at all, and a megabyte of texture.
static const S32 WINDOW_RES = 256;
static const F32 WINDOW_NO_SURFACE = -1.0e6f;   // texel the stitch found nothing for

// Weather that says nothing about drying must not freeze the world in what the
// last lot left on it. Rain gives way to embers, or to nothing at all, and the
// street still has to recover - slowly, but it has to get there. These are the
// rates used when the running preset offers none of its own, and they are the
// reason a preset can opt into marking the surface without also having to
// state how the marks come off.
static const F32 FALLBACK_DRY   = 0.002f;       // toward dry, per second
static const F32 FALLBACK_MELT  = 0.0000045f;   // metres per second
static const F32 FALLBACK_DRAIN = 0.0001f;      // metres per second

static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE("Atmo Magic Surface Field");

void SSSurfaceField::clear()
{
    mFields.clear();
    mWindowValid = false;
    mTickAccum = 0.f;
    mPeakWet = mPeakSnow = mPeakPuddle = 0.f;
}

// The resolution the geometry is reduced to. The same figure the drainage
// trace used, kept because it is what the Field lattice and the stitched
// window are both built around - the window copies cells rather than
// resampling them, which only works while everyone agrees on the cell size.
// The gradient at which a surface is running water as hard as this system
// draws it - about 40 degrees, a steep roof pitch. Anything steeper streaks
// at full strength rather than more; there is no more to give.
static const F32 SLOPE_RUN_FULL = 0.85f;

// How far a neighbour has to sit below a cell before the cell counts as a
// lip rather than a step. A kerb is not an eave; a roof edge over a two
// storey drop is.
static const F32 GEOM_EDGE_DROP = 1.5f;

// Shedding. A lip catches the rain landing on its own footprint plus an
// allowance for the surface feeding it from behind, and that allowance is
// inferred from the local pitch rather than routed: a steep roof delivers
// what lands on it to its eaves, a flat one mostly holds it. This is the
// deliberate simplification that replaced the drainage solve - the number
// is a plausible metres-squared, not an accounting of where water went.
static const F32 SHED_FEED_FLAT  = 1.5f;    // cell footprints, on the level
static const F32 SHED_FEED_STEEP = 9.f;     // cell footprints, at SLOPE_RUN_FULL

// Drops that gather into one drip on the way down. Water arrives at an eave
// as fewer, fatter drips than the rain that fed it, which is also what keeps
// a warehouse roof from costing more particles than the rain landing on it.
static const F32 SHED_MERGE = 12.f;

// Where an edge stops shedding drops and starts running as a sheet, in drips
// per second, and how far past that it takes to become one entirely.
static const F32 SHED_STREAM_MIN  = 8.f;
static const F32 SHED_STREAM_FULL = 24.f;

// Ceilings, per lip cell and per frame. A downpour on a city block is
// thousands of lips; the look does not improve past a handful of runs near
// the camera, and the particle pool has ripples and splashes to serve too.
static const F32 SHED_MAX_RATE = 40.f;      // drips per second from one lip
static const S32 SHED_MAX_BURST = 4;        // drips one lip may start in a frame
static const S32 SHED_VISIT_PER_FRAME = 96; // lip cells examined per region

// Widest curtain a preset may ask for, so one stream cannot span a bridge.
// Carried over from the shedder this replaced, where it bounded the same
// preset field.
static const F32 STREAM_SPAN_MAX = 24.f;

static const S32 GEOM_RES = 128;

// Above this gradient a cell is a wall rather than a surface anything lies
// on. Water still runs down it (that is what the slope channel is for) but
// nothing settles or stands.
static const F32 GEOM_WALL_SLOPE = 1.2f;

// A cell holds standing water when it is flatter than this and no neighbour
// is meaningfully lower - "this is a dip", asked of the eight cells around
// it and nothing further away. Deliberately strict: a puddle is a dip in the
// ground, and the looser this gets the more a level plaza turns into one
// enormous sheet of standing water.
static const F32 GEOM_POOL_SLOPE = 0.06f;

// Height differences below this are the capture's own noise rather than
// relief. A level street reads as a field of sub-millimetre ties, and
// letting that decide which way a cell drains is how a puddle ends up
// somewhere different on every recapture.
static const F32 GEOM_FLAT_NOISE = 0.02f;

// static
void SSSurfaceField::buildGeometry(const SSRainShadowMap::SurfaceGrid& grid, Geometry& out)
{
    const S32 n = grid.mN;
    const size_t count = (size_t)n * n;

    out.mN = n;
    out.mCell = grid.mCell;
    out.mGeomSerial = grid.mGeomSerial;
    out.mZ = grid.mZ;
    out.mFlags = grid.mFlags;
    out.mSlopeX.assign(count, 0.f);
    out.mSlopeY.assign(count, 0.f);
    out.mSlope.assign(count, 0.f);
    out.mPool.assign(count, 0);
    out.mEdge.assign(count, 0);
    out.mEdgeX.assign(count, 0.f);
    out.mEdgeY.assign(count, 0.f);
    out.mEdgeCells.clear();

    const F32 cell = grid.mCell;
    if (n < 3 || cell <= 0.f) return;

    // How far apart two neighbouring cells' heights may be and still be the
    // same surface. Beyond it they are a roof and the ground below it, and a
    // gradient across that pair would describe a cliff that no water follows
    // - it would just be the lip.
    const F32 step_max = cell * 3.f;

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            if (!out.solid(i) || out.water(i)) continue;

            const F32 z0 = out.mZ[i];

            // Central differences where both neighbours are on this surface,
            // one-sided where only one is, zero at a lip. The result is the
            // direction water on this cell runs - straight down the slope,
            // which is all the directionality this system claims to model.
            auto gradAlong = [&](S32 lo, S32 hi, bool have_lo, bool have_hi) -> F32
            {
                const bool ok_lo = have_lo && out.solid((size_t)lo)
                    && fabsf(out.mZ[(size_t)lo] - z0) < step_max;
                const bool ok_hi = have_hi && out.solid((size_t)hi)
                    && fabsf(out.mZ[(size_t)hi] - z0) < step_max;

                if (ok_lo && ok_hi) return (out.mZ[(size_t)hi] - out.mZ[(size_t)lo]) * 0.5f / cell;
                if (ok_hi) return (out.mZ[(size_t)hi] - z0) / cell;
                if (ok_lo) return (z0 - out.mZ[(size_t)lo]) / cell;
                return 0.f;
            };

            const F32 gx = gradAlong((S32)i - 1, (S32)i + 1, x > 0, x < n - 1);
            const F32 gy = gradAlong((S32)i - n, (S32)i + n, y > 0, y < n - 1);

            const F32 mag = sqrtf(gx * gx + gy * gy);
            out.mSlope[i] = mag;
            if (mag > 0.0001f)
            {
                // Downhill is the negative gradient.
                out.mSlopeX[i] = -gx / mag;
                out.mSlopeY[i] = -gy / mag;
            }

            // Standing water: flat enough, and a dip rather than a shelf.
            // Both halves matter - "flat" alone would flood every rooftop
            // and pavement, which is exactly the runaway the size limit is
            // meant to prevent.
            if (mag > GEOM_POOL_SLOPE) continue;

            bool dips = true;
            for (S32 dy = -1; dy <= 1 && dips; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    if (dx == 0 && dy == 0) continue;
                    const S32 nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                    const size_t ni = (size_t)ny * n + nx;
                    if (!out.solid(ni)) continue;
                    if (fabsf(out.mZ[ni] - z0) > step_max) continue; // over a lip; not this surface

                    if (out.mZ[ni] < z0 - GEOM_FLAT_NOISE)
                    {
                        dips = false;   // it drains that way instead
                        break;
                    }
                }
            }

            out.mPool[i] = dips ? 1 : 0;
        }
    }

    // Lips, in a second pass so it can read the slopes the first one wrote.
    // A cell is a lip when a neighbour is open air or a long way below it:
    // the edge of a roof, a balcony rail, a bridge deck. Water reaching one
    // comes off it, and that is all the "drainage" this system needs to know
    // - no network, no catchment, no routing. Which way it comes off is the
    // sum of the directions the drops lie in.
    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            if (!out.solid(i) || out.water(i)) continue;

            const F32 z0 = out.mZ[i];
            F32 ox = 0.f, oy = 0.f;

            static const S32 DX[4] = { 1, -1, 0, 0 };
            static const S32 DY[4] = { 0, 0, 1, -1 };
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                const size_t ni = (size_t)ny * n + nx;
                const bool open = !out.solid(ni);
                const bool below = out.solid(ni) && (z0 - out.mZ[ni]) > GEOM_EDGE_DROP;
                if (!open && !below) continue;

                ox += (F32)DX[d];
                oy += (F32)DY[d];
            }

            const F32 len = sqrtf(ox * ox + oy * oy);
            if (len < 0.0001f) continue;    // no drop beside it; not a lip

            out.mEdge[i] = 1;
            out.mEdgeX[i] = ox / len;
            out.mEdgeY[i] = oy / len;
            out.mEdgeCells.push_back((S32)i);
        }
    }
}

void SSSurfaceField::refreshGeometry()
{
    SSRainShadowMap* shadow = SSRainShadowMap::getInstance();

    std::vector<std::pair<U64, U32> > tiles;
    shadow->validTiles(tiles);

    for (const auto& entry : tiles)
    {
        Geometry& geom = mGeometry[entry.first];
        // Keyed on the geometry revision, not the capture: the tile is
        // recaptured whenever the camera climbs out of its band or the wind
        // swings the fall direction, and neither of those changes the shape
        // of a roof. Rebuilding on every capture would re-derive an identical
        // answer several times a minute.
        if (geom.valid() && geom.mGeomSerial == entry.second) continue;

        SSRainShadowMap::SurfaceGrid grid;
        if (!shadow->buildSurfaceGrid(entry.first, GEOM_RES, grid)) continue;

        buildGeometry(grid, geom);
    }

    // Regions the shadow map has forgotten (walked out of range, or torn
    // down) take their geometry with them - the Field on top of it is evicted
    // separately, on its own idle timer.
    for (auto it = mGeometry.begin(); it != mGeometry.end(); )
    {
        bool still_there = false;
        for (const auto& entry : tiles)
        {
            if (entry.first == it->first) { still_there = true; break; }
        }
        it = still_there ? std::next(it) : mGeometry.erase(it);
    }
}

// How long a lip takes to shed what it is holding, in seconds. This is what
// keeps an eave running after the rain stops - and what keeps it running
// THROUGH a gust lull, which is the failure the reservoir exists to prevent:
// a shed rate taken straight off the instantaneous weather made every eave
// in the region stutter in time with the gusts.
static const F32 SHED_DRAIN_TAU = 6.f;

// Ceiling on the reservoir, as multiples of what one second of the current
// inflow adds. A roof holds a film, not a tank; without this a long storm
// would bank hours of water and go on shedding it into a clear evening.
static const F32 SHED_STORE_CEILING = 8.f;

void SSSurfaceField::shedEdges(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo ? atmo->sim() : nullptr;
    if (!sim || dt <= 0.f) return;

    static LLCachedControl<bool> shed_enabled(gSavedSettings, "SSAtmoRunoff", true);
    if (!shed_enabled) return;

    static LLCachedControl<F32> scale_setting(gSavedSettings, "SSAtmoRunoffScale", 1.f);
    const F32 scale = llclamp((F32)scale_setting, 0.f, 4.f);
    if (scale <= 0.f) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    // Drops per square metre per second the weather is actually delivering.
    // Sampled once at the camera rather than per lip: the field it comes from
    // drifts over hundreds of metres, so it says the same thing anywhere in
    // the shedding radius.
    const F32 rate_m2 = SSPrecipSim::dropRateAt(cam) * scale;

    for (auto& entry : mFields)
    {
        auto geom_it = mGeometry.find(entry.first);
        if (geom_it == mGeometry.end()) continue;

        const Geometry& geom = geom_it->second;
        Field& fld = entry.second;
        if (!geom.valid() || geom.mN != fld.mN) continue;

        shedRegion(entry.first, geom, fld, dt, rate_m2, cam);
    }
}

void SSSurfaceField::shedRegion(U64 region_handle, const Geometry& geom, Field& fld,
                                F32 dt, F32 rate_m2, const LLVector3& camera_agent)
{
    if (geom.mEdgeCells.empty()) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo->sim();

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return;

    const LLVector3 origin = regionp->getOriginAgent();
    const F32 cell = geom.mCell;
    const F32 cell_area = cell * cell;

    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoRunoffRadius", 48.f);
    const F32 radius = llclamp((F32)radius_setting, 8.f, 128.f);
    const F32 radius_sq = radius * radius;

    // Drips share the effects pool with ripples and splash crowns, and in
    // heavy rain the splashes alone can fill it - budgeting eaves against the
    // whole pool shut them off exactly when it was raining hardest, which is
    // precisely backwards. They are budgeted against their own share instead.
    static const S32 DRIP_BUDGET = 220;
    const S32 live = sim->dripCount();
    const F32 budget = (live >= DRIP_BUDGET) ? 0.f
                     : llmin(1.f, (F32)(DRIP_BUDGET - live) / (F32)(DRIP_BUDGET / 4));

    const F64 now = atmo->sharedTime();

    // Every lip integrates its reservoir every frame, wherever it is: a roof
    // fills and drains whether or not anyone is watching it, and gating the
    // accounting on distance would mean walking up to a building in a
    // downpour and finding its eaves dry for the first few seconds. Only the
    // shedding itself - the particles - is near-camera work, and only a slice
    // of the lips are even considered for it in any one frame.
    S32& cursor = mShedCursor[region_handle];
    const S32 lip_count = (S32)geom.mEdgeCells.size();
    if (cursor >= lip_count) cursor = 0;

    S32 visited = 0;

    for (S32 k = 0; k < lip_count; ++k)
    {
        const S32 i = geom.mEdgeCells[(size_t)k];
        const size_t ui = (size_t)i;

        // The catchment that is left: this cell's own footprint, plus an
        // allowance for the pitch behind it. Statically inferred - a steep
        // roof delivers what lands on it to its edge, a flat one mostly
        // holds it - rather than accumulated through a flow network.
        const F32 slope_norm = llclamp(geom.mSlope[ui] / SLOPE_RUN_FULL, 0.f, 1.f);
        const F32 feed = cell_area * lerp(SHED_FEED_FLAT, SHED_FEED_STEEP, slope_norm);

        const F32 inflow = feed * rate_m2;
        fld.mStore[ui] = llmin(fld.mStore[ui] + inflow * dt,
                               inflow * SHED_STORE_CEILING + 1.f);

        const F32 outflow = fld.mStore[ui] / SHED_DRAIN_TAU;
        fld.mStore[ui] = llmax(0.f, fld.mStore[ui] - outflow * dt);

        // Nothing left to shed, and nothing arriving: skip the rest, which is
        // all particle work.
        if (outflow <= 0.01f)
        {
            fld.mAccum[ui] = 0.f;
            continue;
        }

        // Only a slice of the lips gets as far as spawning anything in any
        // one frame - see SHED_VISIT_PER_FRAME. The window starts at the
        // rolling cursor and wraps, so over a few frames every lip gets its
        // turn and none is starved.
        const S32 offset = (k - cursor + lip_count) % lip_count;
        if (offset >= SHED_VISIT_PER_FRAME) continue;
        ++visited;

        const S32 x = i % geom.mN;
        const S32 y = i / geom.mN;
        const LLVector3 lip = origin
            + LLVector3(((F32)x + 0.5f) * cell, ((F32)y + 0.5f) * cell, 0.f);
        const LLVector3 lip_agent(lip.mV[VX], lip.mV[VY], geom.mZ[ui]);

        const LLVector3 delta = lip_agent - camera_agent;
        if (delta.magVecSquared() > radius_sq)
        {
            fld.mAccum[ui] = 0.f;
            continue;
        }

        const LLVector3 out_dir(geom.mEdgeX[ui], geom.mEdgeY[ui], 0.f);

        // Where what comes off this lip lands. Resolved now rather than
        // stored: only the handful of lips actually shedding this frame ask,
        // and the answer changes whenever anything is built below them.
        LLVector3 land = lip_agent;
        bool on_water = false;
        SSRainShadowMap::getInstance()->resolveColumn(
            lip_agent + out_dir * (cell * 0.5f) - LLVector3(0.f, 0.f, 0.1f), land, on_water);

        // Water gathers on the way down and arrives as fewer, fatter drips.
        const F32 raw_rate = llmin(outflow / SHED_MERGE, SHED_MAX_RATE);

        // Past a point an edge stops shedding drops at all: a gutter in a
        // downpour comes off as a continuous fall, and drawing that as more
        // and more separate drips both reads wrong and costs a particle each.
        const F32 stream_drive = llclamp(
            (raw_rate - SHED_STREAM_MIN) / SHED_STREAM_FULL, 0.f, 1.f);

        // One stream per lip cell, keyed stably on where it is, so the same
        // edge is the same stream frame to frame and client to client.
        const U32 key = SSAtmoNoise::combine(
            SSAtmoNoise::combine((U32)region_handle, (U32)(region_handle >> 32)),
            (U32)i);

        SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
            SSAtmoNoise::combine((U32)(S64)(now * 8.0), key)));
        rng.next();

        if (stream_drive > 0.f)
        {
            // The curtain spans this cell's own width by default, so a run of
            // neighbouring lip cells puts up neighbouring curtains that meet
            // end to end - a gutter, without anything having to know that the
            // run exists. A preset asking for a particular span is taken at
            // its word instead, the same way the old shedder took it.
            const SSPrecipPreset& preset = atmo->preset();
            const F32 width = (preset.mStreamSpan > 0.f)
                ? llclamp(preset.mStreamSpan, 1.f, STREAM_SPAN_MAX) : cell;

            sim->refreshStream(key, lip_agent, out_dir, land,
                               stream_drive, width, 0.f, rng);
        }

        // Drips are the spatter around a running stream, not a second helping
        // of it: once the stream is up it carries the water, and a full share
        // of drips on top reads as the roof shedding twice.
        const F32 drips_per_s = raw_rate * (1.f - 0.9f * stream_drive);
        if (budget <= 0.f) continue;

        fld.mAccum[ui] += drips_per_s * budget * dt;
        S32 shed_now = (S32)fld.mAccum[ui];
        if (shed_now <= 0) continue;

        shed_now = llmin(shed_now, SHED_MAX_BURST);
        fld.mAccum[ui] -= (F32)shed_now;

        for (S32 d = 0; d < shed_now; ++d)
        {
            // Anywhere across the cell's width rather than exactly on its
            // centre: a metre-wide lip shedding down one line reads as a
            // dotted seam, which is the one thing a roof edge never looks
            // like.
            const LLVector3 along(-out_dir.mV[VY], out_dir.mV[VX], 0.f);
            const LLVector3 jitter = along * (rng.frand(-0.5f, 0.5f) * cell);
            sim->spawnDrip(lip_agent + jitter, out_dir, land + jitter, SHED_MERGE, rng);
        }
    }

    // Advance by what was actually looked at, not by the window size: a
    // region whose lips are all out of range would otherwise spin the cursor
    // through them at a frame each for no reason.
    cursor = (cursor + llmax(1, visited)) % lip_count;
}

SSSurfaceField::Field* SSSurfaceField::fieldFor(U64 region_handle, const Geometry& geom, F64 now)
{
    Field& fld = mFields[region_handle];
    fld.mRegionHandle = region_handle;
    fld.mLastTouched = now;

    // A resolution change is a different grid, not a moved one, and there is
    // nothing sensible to carry across it
    if (fld.mN != geom.mN)
    {
        fld.mN = geom.mN;
        fld.mCell = geom.mCell;
        fld.mZ.assign(geom.mZ.size(), 0.f);
        fld.mWet.assign(geom.mZ.size(), 0.f);
        fld.mSnow.assign(geom.mZ.size(), 0.f);
        fld.mPuddle.assign(geom.mZ.size(), 0.f);
        fld.mStore.assign(geom.mZ.size(), 0.f);
        fld.mAccum.assign(geom.mZ.size(), 0.f);

        // Nothing has been standing in the weather yet, but the surface has to
        // start out agreeing with the geometry or the first tick would read
        // every cell as freshly rebuilt and reset what it just cleared
        fld.mZ = geom.mZ;
    }

    fld.mCell = geom.mCell;
    return &fld;
}

void SSSurfaceField::tick(Field& fld, const Geometry& geom, F32 dt,
                          const SSPrecipPreset& preset, F32 intensity)
{
    const S32 n = geom.mN;
    const F32 cell = geom.mCell;
    const F32 cell_area = cell * cell;
    const bool falling = intensity > 0.001f;

    // Repose as a plain angle rather than a cosine: the preset says degrees,
    // and a taper that reaches zero exactly where the preset says it should is
    // worth more here than saving an atan per cell.
    const F32 repose = llclamp(preset.mSnowRepose, 5.f, 89.f) * DEG_TO_RAD;

    // Whether what is falling is the kind of thing that does each of these,
    // rather than merely whether anything is falling. Rain over lying snow is
    // not a snowfall that happens to have a rate of zero - it is a thaw, and
    // the melt below is what has to run.
    const bool wetting  = falling && preset.mWetRate > 0.f;
    const bool snowing  = falling && preset.mSnowRate > 0.f && preset.mSnowDepth > 0.f;
    const bool pooling  = falling && preset.mPuddleRate > 0.f && preset.mPuddleDepth > 0.f;

    const F32 wet_rate    = wetting ? preset.mWetRate * intensity
                                    : (preset.mDryRate > 0.f ? preset.mDryRate : FALLBACK_DRY);
    const F32 wet_target  = wetting ? 1.f : 0.f;
    const F32 wet_blend   = 1.f - expf(-wet_rate * dt);

    const F32 snow_gain   = preset.mSnowRate * intensity * dt;
    const F32 snow_loss   = (preset.mSnowMelt > 0.f ? preset.mSnowMelt : FALLBACK_MELT) * dt;
    const F32 puddle_gain = preset.mPuddleRate * intensity * dt;
    const F32 puddle_loss = (preset.mPuddleDrain > 0.f ? preset.mPuddleDrain : FALLBACK_DRAIN) * dt;

    // A ceiling on top of whatever a preset authors, independent of it,
    // because nothing here actually models a body of standing water yet -
    // no shared level, no volume, no spilling into the next hollow over once
    // one fills. A single deep hollow with a whole roof draining into it
    // will happily be told to stand as deep as the preset's own figure lets
    // it, and until there is a real basin-fill simulation to make that mean
    // something, an unbounded depth just means an increasingly wrong-looking
    // one. This is meant to come out again once that simulation exists.
    static LLCachedControl<F32> pool_depth_max(gSavedSettings, "SSAtmoWetPoolDepthMax", 0.15f);
    const F32 puddle_depth_ceiling = llmin(preset.mPuddleDepth, llmax((F32)pool_depth_max, 0.f));

    F32 peak_wet = 0.f, peak_snow = 0.f, peak_puddle = 0.f;

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;

            // Open air. Nothing to dress, and leaving stale values in the cell
            // would have them reappear if geometry came back under it.
            if (!geom.solid(i))
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mZ[i] = geom.mZ[i];
                continue;
            }

            // The surface moved: someone built here. Start it clean.
            if (fabsf(geom.mZ[i] - fld.mZ[i]) > REBUILD_DZ)
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mZ[i] = geom.mZ[i];
            }

            // Open water is not a surface the weather marks. It has its own
            // shading and its own response to rain, and a wet, snowed-over
            // lake would be neither.
            if (geom.water(i))
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                continue;
            }

            // How readily the cell lies flat enough to hold anything, from
            // whichever neighbours are on the same surface. A cell at the lip
            // of a roof has open air on one side and the ground five metres
            // down beyond it; folding that in would call a flat roof a cliff
            // and refuse to let snow settle on it.
            //
            // Only snow asks, so only snow pays for it. This is an atan and a
            // square root per cell, which over a region at four ticks a second
            // is worth not doing on a wet night.
            auto lieHere = [&]()
            {
                const F32 z0 = geom.mZ[i];
                const F32 limit = cell * SLOPE_STEP_MAX;
                auto slopeAlong = [&](S32 lo_i, S32 hi_i, bool have_lo, bool have_hi)
                {
                    F32 d_lo = 0.f, d_hi = 0.f;
                    bool ok_lo = false, ok_hi = false;
                    if (have_lo && geom.solid(lo_i) && fabsf(geom.mZ[lo_i] - z0) < limit)
                    {
                        d_lo = z0 - geom.mZ[lo_i];
                        ok_lo = true;
                    }
                    if (have_hi && geom.solid(hi_i) && fabsf(geom.mZ[hi_i] - z0) < limit)
                    {
                        d_hi = geom.mZ[hi_i] - z0;
                        ok_hi = true;
                    }
                    if (ok_lo && ok_hi) return (d_lo + d_hi) * 0.5f / cell;
                    if (ok_lo) return d_lo / cell;
                    if (ok_hi) return d_hi / cell;
                    return 0.f;
                };

                const F32 gx = slopeAlong((S32)i - 1, (S32)i + 1, x > 0, x < n - 1);
                const F32 gy = slopeAlong((S32)i - n, (S32)i + n, y > 0, y < n - 1);
                const F32 angle = atanf(sqrtf(gx * gx + gy * gy));
                return llclamp(1.f - angle / repose, 0.f, 1.f);
            };

            // Wetness. The one value here that every surface takes, whatever
            // the weather is doing to it otherwise - snow lying on a roof
            // leaves the bare edges of it damp rather than dry.
            fld.mWet[i] += (wet_target - fld.mWet[i]) * wet_blend;

            // Settled snow, up to what the slope will hold - and no further:
            // a pitch too steep to hold snow stays bare.
            if (snowing)
            {
                // Only what is arriving slides. Snow already lying does not
                // start moving because the pitch it settled on was always that
                // steep; it got there by falling more slowly than it slid.
                const F32 room = preset.mSnowDepth * lieHere() - fld.mSnow[i];
                if (room > 0.f)
                {
                    fld.mSnow[i] += llmin(room, snow_gain);
                }
                else
                {
                    // What will not lie here simply does not settle. It used
                    // to be handed to the cell downhill, which was a water
                    // -movement model in all but name; a steep roof now just
                    // stays bare and the drift at the foot of it builds from
                    // the snow landing there, which is where it was coming
                    // from anyway.
                }
            }
            else if (fld.mSnow[i] > 0.f)
            {
                // Nothing is settling, so what is here is going: to a thaw, to
                // the sun, or to the rain now falling on it
                fld.mSnow[i] = llmax(0.f, fld.mSnow[i] - snow_loss);
            }

            // Standing water, only in a cell the geometry says is a dip (see
            // buildGeometry) and only up to the ceiling. There is no
            // catchment term any more: a puddle is fed by the rain landing in
            // it, not by water routed to it from a whole roof face, so its
            // depth is bounded by the weather rather than by how much
            // upstream a solver decided to hand it. That bound is the point -
            // an unbounded catchment share is what let one hollow stand
            // absurdly deep while the cell beside it stayed a film.
            if (pooling && geom.mPool[i])
            {
                fld.mPuddle[i] = llmin(puddle_depth_ceiling, fld.mPuddle[i] + puddle_gain);
            }
            else if (fld.mPuddle[i] > 0.f)
            {
                // Either the weather has stopped delivering, or a rebuild has
                // changed its mind about this cell being a dip at all
                fld.mPuddle[i] = llmax(0.f, fld.mPuddle[i] - puddle_loss);
            }

            peak_wet = llmax(peak_wet, fld.mWet[i]);
            peak_snow = llmax(peak_snow, fld.mSnow[i]);
            peak_puddle = llmax(peak_puddle, fld.mPuddle[i]);
        }
    }

    mPeakWet = llmax(mPeakWet, peak_wet);
    mPeakSnow = llmax(mPeakSnow, peak_snow);
    mPeakPuddle = llmax(mPeakPuddle, peak_puddle);
}

void SSSurfaceField::evict(F64 now)
{
    for (auto it = mFields.begin(); it != mFields.end();)
    {
        const bool gone = !LLWorld::getInstance()->getRegionFromHandle(it->first);
        it = (gone || now - it->second.mLastTouched > FIELD_KEEP)
                 ? mFields.erase(it) : std::next(it);
    }

    while (mFields.size() > MAX_FIELDS)
    {
        auto oldest = mFields.begin();
        for (auto it = mFields.begin(); it != mFields.end(); ++it)
        {
            if (it->second.mLastTouched < oldest->second.mLastTouched) oldest = it;
        }
        mFields.erase(oldest);
    }
}

void SSSurfaceField::idle(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    if (!atmo->isEnabled())
    {
        if (!mFields.empty()) clear();
        return;
    }

    const SSPrecipPreset& preset = atmo->preset();

    // A preset that never marks anything is not a reason to throw away what an
    // earlier one left: the weather changing from rain to embers should let the
    // street dry, not blink it dry. But with nothing to integrate and nothing
    // left to decay, there is no work to do.
    const bool marks = preset.marksSurface();
    if (!marks && mFields.empty()) return;

    mTickAccum += dt;
    if (mTickAccum < TICK_INTERVAL) return;

    const F32 step = llmin(mTickAccum, MAX_TICK_DT);
    mTickAccum = 0.f;

    LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE);
    LLTimer timer;

    const F64 now = atmo->sharedTime();
    const F32 intensity = atmo->hasWeather() ? llclamp(atmo->precipitation(), 0.f, 1.f) : 0.f;

    mPeakWet = mPeakSnow = mPeakPuddle = 0.f;

    // Shape first, then what the weather has done to it: a region captured
    // this frame is dressed this frame rather than next.
    refreshGeometry();

    for (const auto& entry : mGeometry)
    {
        const Geometry& geom = entry.second;
        if (!geom.valid()) continue;

        Field* fld = fieldFor(entry.first, geom, now);
        if (!fld) continue;

        tick(*fld, geom, step, preset, intensity);
    }

    // Eaves: what the roofs are holding, and what comes off them. After the
    // tick, so a lip sheds against this step's weather rather than last
    // step's.
    shedEdges(step);

    evict(now);
    updateWindow();

    mLastTickMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
}

SSSurfaceField::Sample SSSurfaceField::sample(const LLVector3& pos_agent) const
{
    Sample out;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return out;

    auto it = mFields.find(regionp->getHandle());
    if (it == mFields.end()) return out;

    const Field& fld = it->second;
    if (fld.mN < 1 || fld.mCell <= 0.f) return out;

    const LLVector3 local = pos_agent - regionp->getOriginAgent();
    const S32 x = (S32)(local.mV[VX] / fld.mCell);
    const S32 y = (S32)(local.mV[VY] / fld.mCell);
    if (x < 0 || y < 0 || x >= fld.mN || y >= fld.mN) return out;

    const size_t i = (size_t)y * fld.mN + x;
    out.mWet = fld.mWet[i];
    out.mSnow = fld.mSnow[i];
    out.mPuddle = fld.mPuddle[i];
    out.mSurfaceZ = fld.mZ[i];
    out.mValid = true;
    return out;
}

// Stitch every field in range into one camera-centred grid and hand it to the
// GPU. Rebuilt whole on each tick rather than scrolled: at a megabyte four
// times a second that is a few megabytes a second of upload, against the
// bookkeeping of tracking which rows moved and which regions changed under
// them. If that ever matters, the window is snapped to whole cells precisely
// so it can be scrolled instead.
void SSSurfaceField::updateWindow()
{
    if (mFields.empty())
    {
        mWindowValid = false;
        return;
    }

    // Cell size comes from whichever field the camera is standing in, so the
    // window and the drainage agree texel for cell and nothing is resampled
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    F32 cell = 0.f;
    {
        LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
        auto it = cam_region ? mFields.find(cam_region->getHandle()) : mFields.end();
        if (it != mFields.end() && it->second.mCell > 0.f) cell = it->second.mCell;
        else cell = mFields.begin()->second.mCell;
    }
    if (cell <= 0.f)
    {
        mWindowValid = false;
        return;
    }

    // Snapped to whole cells: a window that slid by fractions of a texel would
    // put every cell somewhere new each tick, and the shading over a still
    // scene would crawl
    const F32 span = cell * (F32)WINDOW_RES;
    LLVector3 origin(floorf((cam.mV[VX] - span * 0.5f) / cell) * cell,
                     floorf((cam.mV[VY] - span * 0.5f) / cell) * cell,
                     0.f);

    mWindowData.assign((size_t)WINDOW_RES * WINDOW_RES * 4, 0.f);
    for (size_t t = 0; t < (size_t)WINDOW_RES * WINDOW_RES; ++t)
    {
        mWindowData[t * 4] = WINDOW_NO_SURFACE;
    }
    mWindowFlowData.assign((size_t)WINDOW_RES * WINDOW_RES * 4, 0.f);

    bool any = false;
    for (const auto& entry : mFields)
    {
        const Field& fld = entry.second;
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp || fld.mN < 1 || fabsf(fld.mCell - cell) > 0.001f) continue;

        // The overlap between this field and the window, as whole cells in
        // each. Both are on the same lattice, so this is a copy rather than a
        // resample - which is the reason for snapping the window to the cell
        // size the fields are already using.
        const LLVector3 forigin = regionp->getOriginAgent();
        const S32 off_x = (S32)llround((forigin.mV[VX] - origin.mV[VX]) / cell);
        const S32 off_y = (S32)llround((forigin.mV[VY] - origin.mV[VY]) / cell);

        const S32 x0 = llmax(0, off_x), x1 = llmin(WINDOW_RES, off_x + fld.mN);
        const S32 y0 = llmax(0, off_y), y1 = llmin(WINDOW_RES, off_y + fld.mN);
        if (x0 >= x1 || y0 >= y1) continue;

        // The surface's own shape, read fresh rather than cached on the
        // Field: it is geometry, already owned by mGeometry, so copying the
        // slope into the integrator alongside wet/snow/puddle would only be a
        // second copy of data that already exists and could go stale between
        // the two.
        auto geom_it = mGeometry.find(entry.first);
        const Geometry* geom = (geom_it != mGeometry.end()) ? &geom_it->second : nullptr;
        const bool have_slope = geom && geom->valid() && geom->mN == fld.mN;

        for (S32 wy = y0; wy < y1; ++wy)
        {
            const S32 fy = wy - off_y;
            for (S32 wx = x0; wx < x1; ++wx)
            {
                const S32 fx = wx - off_x;
                const size_t fi = (size_t)fy * fld.mN + fx;
                const size_t wi = ((size_t)wy * WINDOW_RES + wx) * 4;
                mWindowData[wi]     = fld.mZ[fi];
                mWindowData[wi + 1] = fld.mWet[fi];
                mWindowData[wi + 2] = fld.mSnow[fi];
                mWindowData[wi + 3] = fld.mPuddle[fi];

                if (have_slope && geom->solid(fi) && !geom->water(fi))
                {
                    // Which way water on this cell runs, and how hard it is
                    // being pushed to run: the surface's own downslope
                    // direction, and how steep it is against how much rain is
                    // actually landing here. That product is the whole
                    // directionality model - a steep roof in a downpour
                    // streaks hard, the same roof in a drizzle barely does,
                    // and a level floor never does however wet it gets.
                    mWindowFlowData[wi]     = geom->mSlopeX[fi];
                    mWindowFlowData[wi + 1] = geom->mSlopeY[fi];
                    mWindowFlowData[wi + 2] =
                        llclamp(geom->mSlope[fi] / SLOPE_RUN_FULL, 0.f, 1.f) * fld.mWet[fi];
                }
            }
        }
        any = true;
    }

    if (!any)
    {
        mWindowValid = false;
        return;
    }

    if (mWindowTex == 0 || mWindowRes != WINDOW_RES)
    {
        releaseGL();

        // Immutable storage is GL 4.2. Everything else here is older than that,
        // so this is the one call that can be missing on a driver the rest of
        // the system is perfectly happy on.
        if (glTexStorage2D == nullptr)
        {
            LL_WARNS_ONCE("AtmoMagic") << "No glTexStorage2D; the surface field"
                                          " cannot be uploaded and nothing will"
                                          " shade wet" << LL_ENDL;
            mWindowValid = false;
            return;
        }

        glGenTextures(1, &mWindowTex);
        glBindTexture(GL_TEXTURE_2D, mWindowTex);

        // Linear, so the wetness under a fragment varies smoothly rather than
        // in metre squares. It softens the height channel across a roof edge
        // too, over the one cell the edge falls in, and that reads as the lip
        // of the shelter being soft - which is nearer the truth than a step.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, WINDOW_RES, WINDOW_RES);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenTextures(1, &mWindowFlowTex);
        glBindTexture(GL_TEXTURE_2D, mWindowFlowTex);

        // Linear, like the main window. This used to be nearest, because a
        // routed flow field genuinely could not be interpolated: two channels
        // meeting head-on in a roof valley blend to a shrunken vector that
        // points somewhere between two real answers and means neither. A
        // gradient field has no such discontinuities - neighbouring cells on
        // one surface slope almost the same way, by construction - so it
        // interpolates cleanly, and the hard cell edges that idiom cost are
        // gone with it.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, WINDOW_RES, WINDOW_RES);
        glBindTexture(GL_TEXTURE_2D, 0);

        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            LL_WARNS("AtmoMagic") << "Surface field texture failed to allocate,"
                                     " GL error 0x" << std::hex << (U32)err << std::dec
                                  << "; nothing will shade wet" << LL_ENDL;
            releaseGL();
            mWindowValid = false;
            return;
        }

        mWindowRes = WINDOW_RES;
        LL_INFOS("AtmoMagic") << "Surface field window allocated, " << WINDOW_RES
                              << " cells square" << LL_ENDL;
    }

    glBindTexture(GL_TEXTURE_2D, mWindowTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WINDOW_RES, WINDOW_RES,
                    GL_RGBA, GL_FLOAT, mWindowData.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindTexture(GL_TEXTURE_2D, mWindowFlowTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WINDOW_RES, WINDOW_RES,
                    GL_RGBA, GL_FLOAT, mWindowFlowData.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    mWindowCell = cell;
    mWindowOrigin = origin;
    mWindowValid = true;
}

bool SSSurfaceField::bindForShader(LLGLSLShader& shader, S32 channel)
{
    if (!hasWindow() || channel < 0) return false;

    static LLStaticHashedString field_map("ssFieldMap");
    static LLStaticHashedString field_origin("ssFieldOrigin");

    gGL.getTexUnit(channel)->activate();
    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWindowTex);
    shader.uniform1i(field_map, channel);

    // xy the agent-space corner of texel zero, z the metres a cell spans,
    // w the resolution - between them everything a lookup needs
    shader.uniform4f(field_origin, mWindowOrigin.mV[VX], mWindowOrigin.mV[VY],
                     mWindowCell, (F32)mWindowRes);
    return true;
}

bool SSSurfaceField::bindFlowForShader(LLGLSLShader& shader, S32 channel)
{
    if (!hasFlowWindow() || channel < 0) return false;

    static LLStaticHashedString field_flow_map("ssFieldFlowMap");

    gGL.getTexUnit(channel)->activate();
    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWindowFlowTex);
    shader.uniform1i(field_flow_map, channel);

    // Same lattice as the main window - ssFieldOrigin, already bound by
    // bindForShader, is all either texture needs to be sampled.
    return true;
}

void SSSurfaceField::releaseGL()
{
    if (mWindowTex)
    {
        glDeleteTextures(1, &mWindowTex);
        mWindowTex = 0;
    }
    if (mWindowFlowTex)
    {
        glDeleteTextures(1, &mWindowFlowTex);
        mWindowFlowTex = 0;
    }
    mWindowRes = 0;
    mWindowValid = false;
    mScratch.release();
    mScratchNormal.release();
}

void SSSurfaceField::renderWetPass()
{
    // A pass that quietly declines to run looks exactly like one that runs and
    // changes nothing, and the two want completely different fixes. Each gate
    // says so once, and says so again only when the answer changes, so a log
    // from a whole session is a handful of lines rather than a flood.
    static S32 s_state = -1;
    auto note = [](S32 state, const char* what)
    {
        if (s_state != state)
        {
            s_state = state;
            LL_INFOS("AtmoMagic") << "Surface wetness pass: " << what << LL_ENDL;
        }
    };

    if (gCubeSnapshot) return;      // probe capture, not a frame anyone sees
    if (!hasWindow()) { note(1, "idle, no field window uploaded"); return; }
    if (!gSSSurfaceWetProgram.isComplete()) { note(2, "idle, shader did not build"); return; }
    if (!gSSSurfaceCommitProgram.isComplete()) { note(6, "idle, commit shader did not build"); return; }

    static LLCachedControl<F32> strength(gSavedSettings, "SSAtmoWetStrength", 1.f);
    const F32 wet_strength = llclamp((F32)strength, 0.f, 2.f);
    if (wet_strength <= 0.f) { note(3, "idle, SSAtmoWetStrength is zero"); return; }

    LLRenderTarget* gbuffer = &gPipeline.mRT->deferredScreen;
    const U32 w = gbuffer->getWidth();
    const U32 h = gbuffer->getHeight();
    if (w == 0 || h == 0 || gbuffer->getNumTextures() < 2)
    {
        note(4, "idle, gbuffer has no specular attachment");
        return;
    }

    if (mScratch.getWidth() != w || mScratch.getHeight() != h)
    {
        mScratch.release();
        if (!mScratch.allocate(w, h, GL_RGBA, false))
        {
            note(5, "idle, could not allocate the scratch target");
            return;
        }

        // allocate() returning true is not the same as the target actually
        // holding a texture - addColorAttachment can decline quietly further
        // down. Caught here, once, rather than discovered as a getTexture()
        // warning three calls later with no context left to explain it.
        if (mScratch.getNumTextures() < 1)
        {
            LL_WARNS("AtmoMagic") << "Surface wetness scratch target has no"
                                     " colour attachment after allocate("
                                  << w << "x" << h << ")" << LL_ENDL;
            return;
        }
    }

    if (s_state != 0)
    {
        s_state = 0;
        LL_INFOS("AtmoMagic") << "Surface wetness pass running at " << w << "x" << h
                              << ", field window " << mWindowRes << " at " << mWindowCell
                              << "m" << LL_ENDL;
    }

    LL_PROFILE_GPU_ZONE("atmo surface wetness");

    mScratch.bindTarget();

    // Binds the gbuffer's own attachments as the diffuse, specular and normal
    // samplers, along with the depth. The specular one is what this pass is
    // rewriting, which is exactly why it is being rewritten into the scratch
    // target rather than in place.
    gPipeline.bindDeferredShader(gSSSurfaceWetProgram);

    // The field's own sampler goes after everything the deferred bind claimed,
    // which is what mActiveTextureChannels is counting. Bound by hand rather
    // than through enableTexture, so it has to be let go by hand too - the
    // deferred unbind below only knows about the channels it claimed itself.
    const S32 field_channel = gSSSurfaceWetProgram.mActiveTextureChannels;
    bindForShader(gSSSurfaceWetProgram, field_channel);

    // Avatar capsules, uploaded whether or not there are any: the shader
    // reads ssAvatarCount before anything else, and a stale count from a
    // previous frame would have it testing capsules that are no longer
    // there.
    SSAvatarWet::getInstance()->bindForShader(gSSSurfaceWetProgram);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const LLVector3 fall = atmo->rainDirection();

    static LLStaticHashedString inv_view("ssFieldInvView");
    static LLStaticHashedString field_fall("ssFieldFall");
    static LLStaticHashedString field_spread("ssFieldSpread");
    static LLStaticHashedString field_facing("ssFieldFacing");
    static LLStaticHashedString wet_str("ssWetStrength");
    static LLStaticHashedString wet_rough("ssWetRoughness");
    static LLStaticHashedString wet_rough_min("ssWetRoughMin");
    static LLStaticHashedString wet_gloss("ssWetGlossTarget");
    static LLStaticHashedString wet_spec("ssWetSpecular");
    static LLStaticHashedString wet_spec_matte("ssWetSpecularMatte");
    static LLStaticHashedString wet_debug("ssWetDebugForce");
    static LLStaticHashedString wet_puddle_depth("ssWetPuddleDepthFull");
    static LLStaticHashedString wet_puddle_rough("ssWetPuddleRoughness");
    static LLStaticHashedString wet_puddle_rough_min("ssWetPuddleRoughMin");
    static LLStaticHashedString wet_puddle_spec("ssWetPuddleSpecular");
    static LLStaticHashedString wet_puddle_gloss("ssWetPuddleGloss");

    const glm::mat4 inv = glm::inverse(get_current_modelview());
    gSSSurfaceWetProgram.uniformMatrix4fv(inv_view, 1, GL_FALSE, glm::value_ptr(inv));
    gSSSurfaceWetProgram.uniform3fv(field_fall, 1, fall.mV);

    // Turbulence spreads the fall direction, and that spread is what softens
    // the edge of an overhang. Tried tying this to the gust the wind system
    // tracks, on the reasoning that a squall should blur the shelter line
    // more than still air does - but a value that keeps changing frame to
    // frame makes a perfectly static edge visibly breathe, which reads as a
    // bug even though the reasoning behind it was sound. A single fixed
    // width holds still, and that matters more here than the physical
    // justification did.
    static LLCachedControl<F32> spread_setting(gSavedSettings, "SSAtmoWetSpread", 0.35f);
    const F32 spread = llclamp((F32)spread_setting, 0.f, 1.f);
    gSSSurfaceWetProgram.uniform1f(field_spread, spread);

    static LLCachedControl<F32> facing(gSavedSettings, "SSAtmoWetFacing", 0.6f);
    gSSSurfaceWetProgram.uniform1f(field_facing, llclamp((F32)facing, 0.f, 1.f));

    // Left as settings rather than constants because what reads as wet is a
    // judgement about content, not a physical figure: a region of matte legacy
    // prims and one of authored PBR want different numbers out of the same
    // rain. Winding the roughness pair to zero is also the quickest way to
    // find out whether this pass is running at all.
    static LLCachedControl<F32> rough_mul(gSavedSettings, "SSAtmoWetRoughness", 0.25f);
    static LLCachedControl<F32> rough_min(gSavedSettings, "SSAtmoWetRoughMin", 0.04f);
    static LLCachedControl<F32> gloss_target(gSavedSettings, "SSAtmoWetGloss", 0.55f);
    static LLCachedControl<F32> spec_target(gSavedSettings, "SSAtmoWetSpecular", 0.25f);

    // Terrain, and any legacy content like it, carries no baked specular at
    // all - checked in the shader rather than here, since it depends on the
    // gbuffer's own value at each fragment, not anything known on this side.
    static LLCachedControl<F32> spec_matte(gSavedSettings, "SSAtmoWetSpecularMatte", 0.1f);

    gSSSurfaceWetProgram.uniform1f(wet_str, wet_strength);
    gSSSurfaceWetProgram.uniform1f(wet_rough, llclamp((F32)rough_mul, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_rough_min, llclamp((F32)rough_min, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_gloss, llclamp((F32)gloss_target, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_spec, llclamp((F32)spec_target, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_spec_matte, llclamp((F32)spec_matte, 0.f, 1.f));

    // Standing water. The depth figure is shared verbatim with the normal
    // flatten pass below so the two agree on what counts as a full puddle;
    // everything else here is this pass's own look, tuned independently of
    // the general wetness dials above it.
    static LLCachedControl<F32> puddle_depth_full(gSavedSettings, "SSAtmoWetPuddleDepthFull", 0.02f);
    static LLCachedControl<F32> puddle_rough(gSavedSettings, "SSAtmoWetPuddleRoughness", 0.02f);
    static LLCachedControl<F32> puddle_rough_min(gSavedSettings, "SSAtmoWetPuddleRoughMin", 0.02f);
    static LLCachedControl<F32> puddle_spec(gSavedSettings, "SSAtmoWetPuddleSpecular", 0.9f);
    static LLCachedControl<F32> puddle_gloss(gSavedSettings, "SSAtmoWetPuddleGloss", 0.9f);
    const F32 puddle_depth_full_m = llmax((F32)puddle_depth_full, 0.001f);
    gSSSurfaceWetProgram.uniform1f(wet_puddle_depth, puddle_depth_full_m);
    gSSSurfaceWetProgram.uniform1f(wet_puddle_rough, llclamp((F32)puddle_rough, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_puddle_rough_min, llclamp((F32)puddle_rough_min, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_puddle_spec, llclamp((F32)puddle_spec, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_puddle_gloss, llclamp((F32)puddle_gloss, 0.f, 1.f));

    // Diagnostic override: soaks every fragment the pass reaches, whatever the
    // field says. Separates "the pass writes nothing anyone can see" from "the
    // field is not reaching the shader", which look identical from a chair.
    static LLCachedControl<F32> debug_force(gSavedSettings, "SSAtmoWetDebugForce", 0.f);
    gSSSurfaceWetProgram.uniform1f(wet_debug, llclamp((F32)debug_force, 0.f, 1.f));

    // Diagnostic: real field lookup, exposure march skipped. Isolates the
    // field texture/coordinate path from the shelter logic.
    static LLStaticHashedString wet_skip_exposure("ssWetSkipExposure");
    static LLCachedControl<F32> skip_exposure(gSavedSettings, "SSAtmoWetSkipExposure", 0.f);
    gSSSurfaceWetProgram.uniform1f(wet_skip_exposure, llclamp((F32)skip_exposure, 0.f, 1.f));

    {
        LLGLDepthTest depth(GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gGL.getTexUnit(field_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gPipeline.unbindDeferredShader(gSSSurfaceWetProgram);

    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                  << " after the wetness draw" << LL_ENDL;
        }
    }

    mScratch.flush();

    if (mScratch.getNumTextures() < 1)
    {
        LL_WARNS_ONCE("AtmoMagic") << "Surface wetness scratch target lost its"
                                      " colour attachment before the commit"
                                      " pass could read it" << LL_ENDL;
    }

    // Same field, same wet value, a second independent pass: flattens the
    // shading normal toward up on the same surfaces the pass above just
    // brightened, tapered off on anything steep enough that water on it runs
    // as a sheet rather than pooling flat. Gated on its own shader having
    // built, separately from the wetness one above - a compile failure here
    // costs the flattening, not the wetness the surfaces already have.
    static S32 s_normal_state = -1;
    auto note_normal = [](S32 state, const char* what)
    {
        if (s_normal_state != state)
        {
            s_normal_state = state;
            LL_INFOS("AtmoMagic") << "Surface normal flatten pass: " << what << LL_ENDL;
        }
    };

    const bool do_normal = gSSSurfaceNormalProgram.isComplete();
    if (!do_normal)
    {
        note_normal(1, "idle, shader did not build");
    }
    else
    {
        if (mScratchNormal.getWidth() != w || mScratchNormal.getHeight() != h)
        {
            mScratchNormal.release();
            if (!mScratchNormal.allocate(w, h, GL_RGBA, false))
            {
                note_normal(2, "idle, could not allocate the scratch target");
            }
            else if (mScratchNormal.getNumTextures() < 1)
            {
                note_normal(3, "idle, allocate reported success but left no colour attachment");
            }
        }
    }

    if (do_normal && mScratchNormal.getNumTextures() >= 1)
    {
        if (s_normal_state != 0)
        {
            s_normal_state = 0;
            LL_INFOS("AtmoMagic") << "Surface normal flatten pass running" << LL_ENDL;
        }

        LL_PROFILE_GPU_ZONE("atmo surface normal flatten");

        mScratchNormal.bindTarget();
        gPipeline.bindDeferredShader(gSSSurfaceNormalProgram);

        const S32 normal_field_channel = gSSSurfaceNormalProgram.mActiveTextureChannels;
        bindForShader(gSSSurfaceNormalProgram, normal_field_channel);
        const S32 flow_field_channel = normal_field_channel + 1;
        bindFlowForShader(gSSSurfaceNormalProgram, flow_field_channel);

        // The same tileable wave-normal texture the water plane itself uses
        // for ripples - fetched by UUID each pass rather than cached on this
        // object, the same way LLDrawPoolWater picks it up, so a region
        // changing its water settings changes what streams look like too
        // without this needing to be told. The texture manager's own cache
        // keyed on that UUID is what keeps this cheap.
        const S32 wave_channel = flow_field_channel + 1;
        LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
        LLUUID wave_id = pwater ? pwater->getNormalMapID() : LLUUID::null;
        if (wave_id.isNull()) wave_id = LLSettingsWater::GetDefaultWaterNormalAssetId();
        LLViewerFetchedTexture* wave_tex = LLViewerTextureManager::getFetchedTexture(wave_id);

        static LLStaticHashedString wave_map("ssWaveMap");
        bool have_wave = false;
        if (wave_tex)
        {
            wave_tex->addTextureStats(1024.f * 1024.f);
            gGL.getTexUnit(wave_channel)->activate();
            gGL.getTexUnit(wave_channel)->bindManual(LLTexUnit::TT_TEXTURE, wave_tex->getTexName());
            gSSSurfaceNormalProgram.uniform1i(wave_map, wave_channel);
            have_wave = true;
        }

        static LLStaticHashedString norm_inv_view("ssFieldInvView");
        static LLStaticHashedString norm_wet_str("ssWetStrength");
        static LLStaticHashedString norm_wet_debug("ssWetDebugForce");
        static LLStaticHashedString norm_wet_skip_exposure("ssWetSkipExposure");
        static LLStaticHashedString norm_flatten("ssWetNormalFlatten");
        static LLStaticHashedString norm_cos_full("ssWetFlattenCosFull");
        static LLStaticHashedString norm_cos_zero("ssWetFlattenCosZero");
        static LLStaticHashedString norm_puddle_depth("ssWetPuddleDepthFull");
        static LLStaticHashedString norm_puddle_flatten("ssWetPuddleFlatten");

        gSSSurfaceNormalProgram.uniformMatrix4fv(norm_inv_view, 1, GL_FALSE, glm::value_ptr(inv));
        gSSSurfaceNormalProgram.uniform1f(norm_wet_str, wet_strength);
        gSSSurfaceNormalProgram.uniform1f(norm_wet_debug, llclamp((F32)debug_force, 0.f, 1.f));
        gSSSurfaceNormalProgram.uniform1f(norm_wet_skip_exposure, llclamp((F32)skip_exposure, 0.f, 1.f));

        static LLCachedControl<F32> flatten_amount(gSavedSettings, "SSAtmoWetNormalFlatten", 0.6f);
        gSSSurfaceNormalProgram.uniform1f(norm_flatten, llclamp((F32)flatten_amount, 0.f, 1.f));

        // Degrees from vertical-up, converted to cosines here rather than in
        // the shader so a per-pixel inverse cosine is never needed - the
        // shader already has a cosine on hand in the surface's own normal
        // dotted with up, and comparing that against another cosine is
        // exactly the smoothstep it already wanted to do.
        static LLCachedControl<F32> flatten_angle_full(gSavedSettings, "SSAtmoWetFlattenAngleFull", 25.f);
        static LLCachedControl<F32> flatten_angle_zero(gSavedSettings, "SSAtmoWetFlattenAngleZero", 65.f);
        const F32 cos_full = cosf(llclamp((F32)flatten_angle_full, 0.f, 89.f) * DEG_TO_RAD);
        const F32 cos_zero = cosf(llclamp((F32)flatten_angle_zero, 1.f, 90.f) * DEG_TO_RAD);
        gSSSurfaceNormalProgram.uniform1f(norm_cos_full, cos_full);
        gSSSurfaceNormalProgram.uniform1f(norm_cos_zero, cos_zero);

        // Same standing-depth figure the wetness pass just used, and its own
        // flatten amount rather than a share of ssWetNormalFlatten - a puddle
        // is meant to read as fully level water at full depth regardless of
        // how far the ordinary wet-wall taper is dialled.
        gSSSurfaceNormalProgram.uniform1f(norm_puddle_depth, puddle_depth_full_m);
        static LLCachedControl<F32> puddle_flatten(gSavedSettings, "SSAtmoWetPuddleFlatten", 1.f);
        gSSSurfaceNormalProgram.uniform1f(norm_puddle_flatten, llclamp((F32)puddle_flatten, 0.f, 1.f));

        static LLStaticHashedString norm_time("ssTime");
        static LLStaticHashedString norm_flow_scale("ssWetFlowScale");
        static LLStaticHashedString norm_flow_speed("ssWetFlowSpeed");
        static LLStaticHashedString norm_flow_strength("ssWetFlowStrength");
        static LLStaticHashedString norm_flow_rot_sin("ssWetFlowRotSin");
        static LLStaticHashedString norm_flow_rot_cos("ssWetFlowRotCos");
        static LLStaticHashedString norm_flow_min_wet("ssWetFlowMinWet");
        static LLCachedControl<F32> flow_scale(gSavedSettings, "SSAtmoWetFlowScale", 4.f);
        static LLCachedControl<F32> flow_speed(gSavedSettings, "SSAtmoWetFlowSpeed", 0.6f);
        static LLCachedControl<F32> flow_strength(gSavedSettings, "SSAtmoWetFlowStrength", 0.6f);
        static LLCachedControl<F32> flow_rotate(gSavedSettings, "SSAtmoWetFlowRotate", 90.f);
        static LLCachedControl<F32> flow_min_wet(gSavedSettings, "SSAtmoWetFlowMinWet", 0.3f);

        // atmo->sharedTime() is seconds since the Unix epoch - a fine clock
        // for scheduling, but a poor one to hand a shader: at that magnitude
        // a 32-bit float's precision has already coarsened to better than a
        // minute per step, which is exactly why nothing here looked like it
        // was moving. gFrameTimeSeconds is the same continuous clock the
        // rest of the viewer already animates against, rebased to zero at
        // launch so it stays precise for hours.
        gSSSurfaceNormalProgram.uniform1f(norm_time, gFrameTimeSeconds);
        gSSSurfaceNormalProgram.uniform1f(norm_flow_scale, llmax((F32)flow_scale, 0.1f));
        gSSSurfaceNormalProgram.uniform1f(norm_flow_speed, (F32)flow_speed);
        const F32 flow_rot_rad = (F32)flow_rotate * DEG_TO_RAD;
        gSSSurfaceNormalProgram.uniform1f(norm_flow_rot_sin, sinf(flow_rot_rad));
        gSSSurfaceNormalProgram.uniform1f(norm_flow_rot_cos, cosf(flow_rot_rad));
        // No wave texture bound leaves ssWaveMap sampling whatever texture
        // unit wave_channel happened to hold last, so the strength that
        // blends its result in has to drop to zero along with it rather than
        // trusting the setting alone.
        gSSSurfaceNormalProgram.uniform1f(norm_flow_strength,
                                          have_wave ? llclamp((F32)flow_strength, 0.f, 1.f) : 0.f);
        gSSSurfaceNormalProgram.uniform1f(norm_flow_min_wet, llclamp((F32)flow_min_wet, 0.f, 0.99f));

        {
            LLGLDepthTest depth(GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable scissor(GL_SCISSOR_TEST);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        gGL.getTexUnit(normal_field_channel)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(flow_field_channel)->unbind(LLTexUnit::TT_TEXTURE);
        if (have_wave) gGL.getTexUnit(wave_channel)->unbind(LLTexUnit::TT_TEXTURE);
        gPipeline.unbindDeferredShader(gSSSurfaceNormalProgram);

        {
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                      << " after the normal flatten draw" << LL_ENDL;
            }
        }

        mScratchNormal.flush();
    }

    // The scratch now holds what the specular attachment should become. Put it
    // back by drawing into the gbuffer with every attachment but that one
    // masked off, rather than by copying into the texture.
    //
    // The copy this replaces named its destination by way of whichever texture
    // unit happened to be active, and getting that unit right meant agreeing
    // with the renderer's binding cache about state it is entitled to skip
    // work over. A draw names the destination through the framebuffer, which
    // nothing caches and nothing can quietly decline to do.
    static LLStaticHashedString commit_src("ssCommitSource");
    static LLStaticHashedString commit_paint("ssCommitDebugPaint");

    gbuffer->bindTarget();

    const GLenum bufs[4] = { GL_NONE, GL_COLOR_ATTACHMENT1, GL_NONE, GL_NONE };
    glDrawBuffers(4, bufs);

    {
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            LL_WARNS_ONCE("AtmoMagic") << "Gbuffer framebuffer incomplete for the"
                                          " commit pass, status 0x" << std::hex
                                       << (U32)status << std::dec << LL_ENDL;
        }

        static LLCachedControl<F32> commit_debug_paint_peek(gSavedSettings, "SSAtmoCommitDebugPaint", 0.f);
        if ((F32)commit_debug_paint_peek > 0.f)
        {
            const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
            GLint box[4] = { 0, 0, 0, 0 };
            glGetIntegerv(GL_SCISSOR_BOX, box);
            LL_INFOS("AtmoMagic") << "Scissor going into the commit draw: "
                                  << (scissor_was_on ? "ON" : "off") << " box ("
                                  << box[0] << "," << box[1] << "," << box[2]
                                  << "," << box[3] << ")" << LL_ENDL;
        }
    }

    gSSSurfaceCommitProgram.bind();
    gGL.getTexUnit(0)->activate();
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mScratch.getTexture(0));
    gSSSurfaceCommitProgram.uniform1i(commit_src, 0);

    // Same diagnostic idea as the debug force above, one stage further down
    // the pipe: paints diffuse magenta from the commit pass itself, so a
    // mechanism failure here shows up whether or not the wetness pass upstream
    // of it wrote anything sensible to the scratch target.
    static LLCachedControl<F32> commit_debug_paint_setting(gSavedSettings, "SSAtmoCommitDebugPaint", 0.f);
    const F32 ssCommitDebugPaint = llclamp((F32)commit_debug_paint_setting, 0.f, 1.f);
    gSSSurfaceCommitProgram.uniform1f(commit_paint, ssCommitDebugPaint);

    {
        LLGLDepthTest depth(GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                  << " after the commit draw" << LL_ENDL;
        }
    }

    // Ground truth. Everything checked so far says the draw succeeded and
    // landed nowhere visible, which is a contradiction - one of those two
    // things is not actually true. Reading the pixel back, while this FBO and
    // this draw buffer are still current, settles which: it names the exact
    // texture object the read comes from, so there is no scope left for "the
    // write worked but not into what the screen shows" to hide in.
    if (ssCommitDebugPaint > 0.f)
    {
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        GLint bound_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo);

        // Five points rather than one: if a leftover scissor rect clipped the
        // draw to a corner of the screen, sampling only the centre could land
        // inside it by chance and report a false all-clear. A pattern across
        // corners and centre cannot be faked by a single narrow rect.
        const S32 inset = 4;
        const struct { const char* name; S32 x, y; } points[5] = {
            { "centre", (S32)(w / 2), (S32)(h / 2) },
            { "top-left",     inset,            (S32)h - 1 - inset },
            { "top-right",    (S32)w - 1 - inset, (S32)h - 1 - inset },
            { "bottom-left",  inset,            inset },
            { "bottom-right", (S32)w - 1 - inset, inset },
        };

        std::ostringstream line;
        line << "Commit readback, FBO " << bound_fbo << " tex "
            << gbuffer->getTexture(0) << ":";
        for (const auto& pt : points)
        {
            U8 px[4] = { 0, 0, 0, 0 };
            glReadPixels(pt.x, pt.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            line << " " << pt.name << "=(" << (int)px[0] << "," << (int)px[1]
                << "," << (int)px[2] << "," << (int)px[3] << ")";
        }
        LL_INFOS("AtmoMagic") << line.str() << LL_ENDL;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSSurfaceCommitProgram.unbind();

    // Same shader, same draw, a different source and a different mask: the
    // commit shader only ever does "copy this texture into whichever
    // attachment glDrawBuffers points frag_data[1] at", which is exactly as
    // true for the normal buffer as it was for the specular one. Nothing
    // about this shader needed to know there would be a second caller.
    if (do_normal && mScratchNormal.getNumTextures() >= 1)
    {
        const GLenum normal_bufs[4] = { GL_NONE, GL_COLOR_ATTACHMENT2, GL_NONE, GL_NONE };
        glDrawBuffers(4, normal_bufs);

        gSSSurfaceCommitProgram.bind();
        gGL.getTexUnit(0)->activate();
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mScratchNormal.getTexture(0));
        gSSSurfaceCommitProgram.uniform1i(commit_src, 0);
        gSSSurfaceCommitProgram.uniform1f(commit_paint, 0.f); // never diagnostic-paint the normal

        {
            LLGLDepthTest depth(GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable scissor(GL_SCISSOR_TEST);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        {
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                      << " after the normal commit draw" << LL_ENDL;
            }
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gSSSurfaceCommitProgram.unbind();
    }

    // Handed back with every attachment live again, so the next thing to bind
    // this target does not inherit a mask it never asked for
    const GLenum restore[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, restore);

    gbuffer->flush();
}

void SSSurfaceField::renderDebug()
{
    if (mFields.empty()) return;

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);     // test against the world, do not write
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoSurfaceRadius", 64.f);
    const F32 reach = llclamp((F32)radius_setting, 8.f, 256.f);

    for (const auto& entry : mFields)
    {
        const Field& fld = entry.second;
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp || fld.mN < 2) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const S32 n = fld.mN;
        const F32 cell = fld.mCell;
        const F32 half = cell * 0.45f;      // gapped, so the grid stays readable

        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < (S32)fld.mZ.size(); ++i)
        {
            const F32 wet = fld.mWet[i];
            const F32 snow = fld.mSnow[i];
            const F32 puddle = fld.mPuddle[i];
            if (wet < 0.01f && snow < 0.001f && puddle < 0.001f) continue;

            const LLVector3 c(origin.mV[VX] + ((F32)(i % n) + 0.5f) * cell,
                              origin.mV[VY] + ((F32)(i / n) + 0.5f) * cell,
                              fld.mZ[i] + 0.04f);
            if ((c - cam).magVecSquared() > reach * reach) continue;

            // Blue for wet, white for snow, cyan and opaque for standing
            // water, so which of the three a cell is carrying is legible at a
            // glance rather than needing the depth read off a number
            F32 r, g, b, a;
            if (puddle > 0.001f)
            {
                const F32 t = llclamp(puddle / 0.05f, 0.f, 1.f);
                r = 0.1f; g = 0.7f; b = 0.9f; a = 0.35f + 0.5f * t;
            }
            else if (snow > 0.001f)
            {
                const F32 t = llclamp(snow / 0.1f, 0.f, 1.f);
                r = 0.85f; g = 0.9f; b = 1.f; a = 0.25f + 0.6f * t;
            }
            else
            {
                r = 0.2f; g = 0.35f; b = 0.8f; a = 0.1f + 0.45f * wet;
            }
            gGL.color4f(r, g, b, a);

            gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] + half, c.mV[VY] - half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);

            gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] - half, c.mV[VY] + half, c.mV[VZ]);
        }
        gGL.end();
    }

    gGL.flush();
}

// </SS:Nexii>
