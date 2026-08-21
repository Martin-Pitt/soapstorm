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
#include "ssprecippreset.h"
#include "ssrainshadow.h"

#include "llfasttimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
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

// How much more than its own footprint a hollow has to drain before it stands
// full depth. A cell catching only itself gets a film; one taking a whole roof
// face gets the puddle.
static const F32 PUDDLE_CATCH_FULL = 12.f;  // multiples of the cell area

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

SSSurfaceField::Field* SSSurfaceField::fieldFor(U64 region_handle, const SSRunoffField& src, F64 now)
{
    Field& fld = mFields[region_handle];
    fld.mRegionHandle = region_handle;
    fld.mLastTouched = now;

    // A resolution change is a different grid, not a moved one, and there is
    // nothing sensible to carry across it
    if (fld.mN != src.mN)
    {
        fld.mN = src.mN;
        fld.mCell = src.mCell;
        fld.mZ.assign(src.count(), 0.f);
        fld.mWet.assign(src.count(), 0.f);
        fld.mSnow.assign(src.count(), 0.f);
        fld.mPuddle.assign(src.count(), 0.f);

        // Nothing has been standing in the weather yet, but the surface has to
        // start out agreeing with the trace or the first tick would read every
        // cell as freshly rebuilt and reset what it just cleared
        std::copy(src.mZ, src.mZ + src.count(), fld.mZ.begin());
    }

    fld.mCell = src.mCell;
    return &fld;
}

void SSSurfaceField::tick(Field& fld, const SSRunoffField& src, F32 dt,
                          const SSPrecipPreset& preset, F32 intensity)
{
    const S32 n = src.mN;
    const F32 cell = src.mCell;
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

    F32 peak_wet = 0.f, peak_snow = 0.f, peak_puddle = 0.f;

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;

            // Open air. Nothing to dress, and leaving stale values in the cell
            // would have them reappear if geometry came back under it.
            if (!src.solid(i))
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mZ[i] = src.mZ[i];
                continue;
            }

            // The surface moved: someone built here. Start it clean.
            if (fabsf(src.mZ[i] - fld.mZ[i]) > REBUILD_DZ)
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mZ[i] = src.mZ[i];
            }

            // Open water is not a surface the weather marks. It has its own
            // shading and its own response to rain, and a wet, snowed-over
            // lake would be neither.
            if (src.water(i))
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
                const F32 z0 = src.mZ[i];
                const F32 limit = cell * SLOPE_STEP_MAX;
                auto slopeAlong = [&](S32 lo_i, S32 hi_i, bool have_lo, bool have_hi)
                {
                    F32 d_lo = 0.f, d_hi = 0.f;
                    bool ok_lo = false, ok_hi = false;
                    if (have_lo && src.solid(lo_i) && fabsf(src.mZ[lo_i] - z0) < limit)
                    {
                        d_lo = z0 - src.mZ[lo_i];
                        ok_lo = true;
                    }
                    if (have_hi && src.solid(hi_i) && fabsf(src.mZ[hi_i] - z0) < limit)
                    {
                        d_hi = src.mZ[hi_i] - z0;
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

            // Settled snow, up to what the slope will hold. What will not stay
            // goes where the water off this cell would have gone, so a steep
            // roof feeds the drift at the foot of it rather than simply
            // refusing to take any - which is the drainage network answering a
            // question it was not traced for, and getting it right anyway.
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
                    // Downstream may already have been visited this pass, in
                    // which case it holds a little more than it should until
                    // the next tick. At four a second against depths that take
                    // minutes to build, that is not a difference anyone sees,
                    // and an ordered second pass to avoid it would cost more
                    // than the error does.
                    const S32 down = src.mFlow[i];
                    if (down >= 0) fld.mSnow[down] += snow_gain;
                }
            }
            else if (fld.mSnow[i] > 0.f)
            {
                // Nothing is settling, so what is here is going: to a thaw, to
                // the sun, or to the rain now falling on it
                fld.mSnow[i] = llmax(0.f, fld.mSnow[i] - snow_loss);
            }

            // Standing water, only where the trace says water stands. Depth is
            // shared out by catchment: the hollow that drains a whole roof
            // face stands full, the one that catches nothing but itself gets a
            // film, and that is what puts puddles where a build would have
            // them rather than evenly over every flat surface.
            if (pooling && src.pools(i))
            {
                const F32 share = llclamp(src.mCatch[i] / (cell_area * PUDDLE_CATCH_FULL), 0.f, 1.f);
                fld.mPuddle[i] = llmin(preset.mPuddleDepth * share, fld.mPuddle[i] + puddle_gain);
            }
            else if (fld.mPuddle[i] > 0.f)
            {
                // Either the weather has stopped delivering, or the trace has
                // changed its mind about this cell being a hollow at all
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

    std::vector<std::pair<U64, U32> > traced;
    SSRunoff::getInstance()->tracedRegions(traced);

    for (const auto& entry : traced)
    {
        const SSRunoffField src = SSRunoff::getInstance()->field(entry.first);
        if (!src.valid()) continue;

        Field* fld = fieldFor(entry.first, src, now);
        if (!fld) continue;

        tick(*fld, src, step, preset, intensity);
    }

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

        for (S32 wy = y0; wy < y1; ++wy)
        {
            const S32 fy = wy - off_y;
            for (S32 wx = x0; wx < x1; ++wx)
            {
                const size_t fi = (size_t)fy * fld.mN + (wx - off_x);
                const size_t wi = ((size_t)wy * WINDOW_RES + wx) * 4;
                mWindowData[wi]     = fld.mZ[fi];
                mWindowData[wi + 1] = fld.mWet[fi];
                mWindowData[wi + 2] = fld.mSnow[fi];
                mWindowData[wi + 3] = fld.mPuddle[fi];
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

void SSSurfaceField::releaseGL()
{
    if (mWindowTex)
    {
        glDeleteTextures(1, &mWindowTex);
        mWindowTex = 0;
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

        static LLStaticHashedString norm_inv_view("ssFieldInvView");
        static LLStaticHashedString norm_wet_str("ssWetStrength");
        static LLStaticHashedString norm_wet_debug("ssWetDebugForce");
        static LLStaticHashedString norm_wet_skip_exposure("ssWetSkipExposure");
        static LLStaticHashedString norm_flatten("ssWetNormalFlatten");
        static LLStaticHashedString norm_cos_full("ssWetFlattenCosFull");
        static LLStaticHashedString norm_cos_zero("ssWetFlattenCosZero");

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

        {
            LLGLDepthTest depth(GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable scissor(GL_SCISSOR_TEST);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        gGL.getTexUnit(normal_field_channel)->unbind(LLTexUnit::TT_TEXTURE);
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
