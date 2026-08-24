/**
 * @file ssatmomatch.cpp
 * @brief Atmo Magic: fit the sky to a reference photograph. See the header.
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

#include "ssatmomatch.h"

#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "lldrawpoolwlsky.h"
#include "llenvironment.h"
#include "llrendertarget.h"
#include "pipeline.h"

#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"

#include <cmath>

// <SS:Nexii> Atmo Magic: match the sky to a photograph

namespace
{
    // Passes over the whole parameter list before giving up. Each pass
    // halves the step, so this is a bisection in every dimension at once:
    // six passes takes a quarter-range step down to about half a percent,
    // which is finer than the eye reads on a sky gradient.
    const S32 MAX_PASSES = 6;

    // How far off the top and bottom of the world view to stay when
    // sampling. Small: it only has to clear the very edge of the frame, and
    // every pixel of margin is elevation the fit cannot see.
    const F32 VIEW_MARGIN = 0.02f;
}

void SSAtmoMatch::start(S32 track_index, const std::vector<Sample>& samples)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset() || samples.empty()) return;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (track_index < 0 || track_index >= (S32)asset.mTracks.size()) return;

    mTrackIndex = track_index;
    mTarget = samples;
    mRendered.assign(samples.size(), LLColor3(0.f, 0.f, 0.f));

    // Everything gets written back on cancel, so it is all saved first -
    // the run mutates the live track deliberately, which is what makes the
    // fit visible while it happens.
    mOriginal.clear();
    for (S32 p = 0; p < P_COUNT; ++p)
    {
        mOriginal.push_back(readParam((Param)p));
    }

    mParam = 0;
    mPhase = 0;
    mPass = 0;
    mStep = 0.25f;
    mBestError = 1.0e9f;
    mRunning = true;
    mWaitReason.clear();

    // Whatever the camera was facing when Match was pressed - see
    // probeHeading().
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mRunHeading = camera ? camera->getAtAxis() : LLVector3::x_axis;

    LL_INFOS("AtmoMagicEnv") << "Sky match started on track " << track_index
                             << " against " << (S32)samples.size() << " samples" << LL_ENDL;
}

void SSAtmoMatch::cancel()
{
    if (!mRunning && mOriginal.empty()) return;

    for (S32 p = 0; p < P_COUNT && p < (S32)mOriginal.size(); ++p)
    {
        writeParam((Param)p, mOriginal[(size_t)p]);
    }
    mOriginal.clear();
    mRunning = false;
}

void SSAtmoMatch::accept()
{
    mOriginal.clear();
    mRunning = false;
}

F32 SSAtmoMatch::progress() const
{
    if (!mRunning) return 1.f;

    const F32 per_pass = 1.f / (F32)MAX_PASSES;
    const F32 within = (F32)mParam / (F32)P_COUNT;
    return llclamp(((F32)mPass + within) * per_pass, 0.f, 1.f);
}

// -----------------------------------------------------------------------
// Reading the sky back out of the frame
// -----------------------------------------------------------------------

LLVector3 SSAtmoMatch::probeHeading() const
{
    // Wherever the camera is facing. The user chooses the slice of sky by
    // turning, which is the only thing that can be right: a reference photo
    // is usually a stretch of horizon AWAY from the sun, and pointing the
    // probe at the sun regardless - as this used to - fitted the one part
    // of the sky the photo was deliberately not of.
    //
    // Flattening happens at the point of use; the elevations are measured
    // from the horizon, so only the heading matters here.
    if (mRunning) return mRunHeading;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    return camera ? camera->getAtAxis() : LLVector3::x_axis;
}

bool SSAtmoMatch::sampleProbeSky(std::vector<LLColor3>& out)
{
    out.assign(mTarget.size(), LLColor3(0.f, 0.f, 0.f));
    if (mTarget.empty()) return false;

    // Narrow and tall: elevation is the only axis carrying information, so
    // width buys nothing but fill.
    static const S32 PROBE_W = 4;
    static const S32 PROBE_H = 128;

    if (!mProbe)
    {
        mProbe = new LLRenderTarget();
    }
    if (mProbe->getWidth() != PROBE_W || mProbe->getHeight() != PROBE_H)
    {
        mProbe->release();
        if (!mProbe->allocate(PROBE_W, PROBE_H, GL_RGBA, true))
        {
            mWaitReason = "Cannot allocate the sky probe target.";
            return false;
        }
    }

    LLDrawPool* pool = gPipeline.findPool(LLDrawPool::POOL_WL_SKY, nullptr);
    LLDrawPoolWLSky* sky_pool = dynamic_cast<LLDrawPoolWLSky*>(pool);
    if (!sky_pool)
    {
        mWaitReason = "The sky draw pool is not available.";
        return false;
    }

    // The band the samples span, with a little headroom either side so the
    // topmost and bottommost are not sitting on the very edge pixel.
    const F32 lowest = mTarget.front().mElevationDeg;
    const F32 highest = mTarget.back().mElevationDeg;
    const F32 pad = llmax(1.f, (highest - lowest) * 0.05f);

    if (!sky_pool->renderSkyProbe(*mProbe, probeHeading(), lowest - pad, highest + pad))
    {
        mWaitReason = "The sky could not be rendered offscreen.";
        return false;
    }

    // Read the strip back once and pick the rows out of it, rather than a
    // readback per sample: one stall instead of a dozen.
    std::vector<U8> pixels((size_t)PROBE_W * PROBE_H * 4, 0);
    mProbe->bindTarget();
    glReadPixels(0, 0, PROBE_W, PROBE_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    mProbe->flush();

    const F32 span = (highest + pad) - (lowest - pad);
    for (size_t i = 0; i < mTarget.size(); ++i)
    {
        const F32 t = llclamp((mTarget[i].mElevationDeg - (lowest - pad)) / span, 0.f, 1.f);
        const S32 row = llclamp((S32)(t * (F32)(PROBE_H - 1)), 0, PROBE_H - 1);

        F32 r = 0.f, g = 0.f, b = 0.f;
        for (S32 x = 0; x < PROBE_W; ++x)
        {
            const size_t idx = ((size_t)row * PROBE_W + x) * 4;
            r += (F32)pixels[idx];
            g += (F32)pixels[idx + 1];
            b += (F32)pixels[idx + 2];
        }
        out[i].setVec(r / (PROBE_W * 255.f), g / (PROBE_W * 255.f), b / (PROBE_W * 255.f));
    }

    mWaitReason.clear();
    return true;
}

// static
F32 SSAtmoMatch::scoreAgainst(const std::vector<LLColor3>& rendered,
                              const std::vector<Sample>& target)
{
    if (rendered.size() != target.size() || rendered.empty()) return 1.0e9f;

    // Every sample counts now: the probe renders exactly the band the
    // samples span, so nothing can be out of frame the way it could be when
    // this read the user's own screen.
    F32 sum = 0.f;
    for (size_t i = 0; i < rendered.size(); ++i)
    {
        for (S32 c = 0; c < 3; ++c)
        {
            const F32 d = rendered[i].mV[c] - target[i].mColor.mV[c];
            sum += d * d;
        }
    }
    return sum / (F32)rendered.size();
}

void SSAtmoMatch::setPreview(const std::vector<Sample>& samples)
{
    if (mRunning) return;   // a run owns mTarget; a preview must not disturb it
    mTarget = samples;
}

void SSAtmoMatch::clearPreview()
{
    if (mRunning) return;
    mTarget.clear();
}

void SSAtmoMatch::renderDebug() const
{
    // Only while idle - see the header.
    if (mRunning || mTarget.empty()) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;

    const LLVector3 origin = camera->getOrigin();

    // These markers CANNOT contaminate the fit, and it is worth being sure
    // of that rather than careful about it: the score comes from an
    // offscreen strip that draws the haze dome and the cloud layer and
    // nothing else, so debug geometry drawn into the world never reaches
    // it. Back when the score was read off the user's own screen this
    // overlay would have been sampling itself.
    //
    // The direction the PROBE looks, so the markers stand in the same part
    // of the sky a run would score. Idle, that is the camera's own heading,
    // which is what lets the aim be set by turning.
    //
    // Flattened before use because sample elevations are measured from the
    // horizon, not from wherever the heading happens to be pointing.
    LLVector3 heading = probeHeading();
    heading.mV[VZ] = 0.f;
    if (heading.normalize() < 0.001f) heading = LLVector3::x_axis;

    static const F32 MARKER_DIST = 30.f;
    static const F32 MARKER_HALF = 0.9f;

    // Drawn as open crosses rather than filled patches, so they read as
    // instruments laid over the sky rather than as part of it.
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE);

    gGL.begin(LLRender::LINES);

    LLVector3 previous;
    bool have_previous = false;

    for (size_t i = 0; i < mTarget.size(); ++i)
    {
        const F32 elev = mTarget[i].mElevationDeg * DEG_TO_RAD;
        const LLVector3 dir = heading * cosf(elev) + LLVector3::z_axis * sinf(elev);
        const LLVector3 at = origin + dir * MARKER_DIST;

        // The photo's colour at this height, drawn as the marker itself:
        // held against the sky behind it, the two either agree or they do
        // not, and that is the whole state of the fit at a glance.
        const LLColor3& want = mTarget[i].mColor;
        gGL.color4f(want.mV[0], want.mV[1], want.mV[2], 0.95f);

        LLVector3 across = dir % LLVector3::z_axis;
        if (across.normalize() < 0.001f) across = LLVector3::y_axis;
        const LLVector3 up = across % dir;

        gGL.vertex3fv((at - across * MARKER_HALF).mV);
        gGL.vertex3fv((at + across * MARKER_HALF).mV);
        gGL.vertex3fv((at - up * MARKER_HALF).mV);
        gGL.vertex3fv((at + up * MARKER_HALF).mV);

        // A faint spine joining them, so the sample line reads as one thing
        // rather than a scatter of crosses.
        if (have_previous)
        {
            gGL.color4f(1.f, 1.f, 1.f, 0.25f);
            gGL.vertex3fv(previous.mV);
            gGL.vertex3fv(at.mV);
        }
        previous = at;
        have_previous = true;
    }

    gGL.end();
    gGL.flush();

    if (shader) shader->bind();
}

// -----------------------------------------------------------------------
// The walk
// -----------------------------------------------------------------------

void SSAtmoMatch::idle()
{
    if (!mRunning) return;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset() || mTrackIndex >= (S32)mgr->asset().mTracks.size())
    {
        mRunning = false;
        return;
    }

    // Several candidates per frame: the probe is a few thousand pixels, so
    // the fit is limited by how much of a frame to spend rather than by the
    // frame rate itself.
    for (S32 probe = 0; probe < PROBES_PER_FRAME && mRunning; ++probe)
    {
        if (!stepOnce()) break;
    }
}

bool SSAtmoMatch::stepOnce()
{
    std::vector<LLColor3> rendered;
    if (!sampleProbeSky(rendered))
    {
        return false;   // sampleProbeSky has set the reason
    }
    mRendered = rendered;

    const F32 error = scoreAgainst(rendered, mTarget);
    const Param param = (Param)mParam;
    const F32 range = paramMax(param) - paramMin(param);

    switch (mPhase)
    {
        case 0:
            // Baseline for this parameter, then try a step up.
            mBestError = error;
            mParamBackup = readParam(param);
            writeParam(param, mParamBackup + range * mStep);
            mPhase = 1;
            break;

        case 1:
            if (error < mBestError)
            {
                // Up was better - keep it and move on.
                mBestError = error;
                mParam = (mParam + 1) % P_COUNT;
                mPhase = 0;
            }
            else
            {
                // Up was worse - put it back and try down.
                writeParam(param, mParamBackup - range * mStep);
                mPhase = 2;
            }
            break;

        case 2:
            if (error < mBestError)
            {
                mBestError = error;
            }
            else
            {
                // Neither direction helped: this parameter is already where
                // it wants to be at this step size.
                writeParam(param, mParamBackup);
            }
            mParam = (mParam + 1) % P_COUNT;
            mPhase = 0;
            break;

        default:
            mPhase = 0;
            break;
    }

    // A full lap of every parameter is one pass. Halving the step each time
    // is what turns a coarse sweep into a fine one without needing to know
    // in advance how far anything has to move.
    if (mParam == 0 && mPhase == 0)
    {
        ++mPass;
        mStep *= 0.5f;

        if (mPass >= MAX_PASSES)
        {
            LL_INFOS("AtmoMagicEnv") << "Sky match settled after " << mPass
                                     << " passes, error " << mBestError << LL_ENDL;
            mRunning = false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------
// The parameters themselves
// -----------------------------------------------------------------------

F32 SSAtmoMatch::readParam(Param p) const
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)mTrackIndex];
    const SSAtmoEnvAtmosphere& atm = track.mAtmosphere;
    const SSAtmoEnvCloudDome& dome = track.mCloudDome;
    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride() : 0.0;

    switch (p)
    {
        case P_BLUE_HORIZON_R: return atm.mBlueHorizon.valueAt(phase).mV[0];
        case P_BLUE_HORIZON_G: return atm.mBlueHorizon.valueAt(phase).mV[1];
        case P_BLUE_HORIZON_B: return atm.mBlueHorizon.valueAt(phase).mV[2];
        case P_BLUE_DENSITY_R: return atm.mBlueDensity.valueAt(phase).mV[0];
        case P_BLUE_DENSITY_G: return atm.mBlueDensity.valueAt(phase).mV[1];
        case P_BLUE_DENSITY_B: return atm.mBlueDensity.valueAt(phase).mV[2];
        case P_HAZE_HORIZON:   return atm.mHazeHorizon.valueAt(phase);
        case P_HAZE_DENSITY:   return atm.mHazeDensity.valueAt(phase);
        case P_DENSITY_MULT:   return atm.mDensityMultiplier.valueAt(phase);
        case P_DISTANCE_MULT:  return atm.mDistanceMultiplier.valueAt(phase);
        case P_CLOUD_COLOR_R:  return dome.mColor.valueAt(phase).mV[0];
        case P_CLOUD_COLOR_G:  return dome.mColor.valueAt(phase).mV[1];
        case P_CLOUD_COLOR_B:  return dome.mColor.valueAt(phase).mV[2];
        case P_CLOUD_COVERAGE: return dome.mCoverage.valueAt(phase);
        default: return 0.f;
    }
}

void SSAtmoMatch::writeParam(Param p, F32 value)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoEnvTrack& track = mgr->editable().mTracks[(size_t)mTrackIndex];
    SSAtmoEnvAtmosphere& atm = track.mAtmosphere;
    SSAtmoEnvCloudDome& dome = track.mCloudDome;
    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride() : 0.0;

    value = llclamp(value, paramMin(p), paramMax(p));

    // Written AT THE PREVIEW HEAD, so a match lands as a keyframe at the
    // instant being looked at rather than flattening the whole cycle - the
    // same rule dropping an EEP sky on the floater follows.
    switch (p)
    {
        case P_BLUE_HORIZON_R:
        case P_BLUE_HORIZON_G:
        case P_BLUE_HORIZON_B:
        {
            LLColor3 c = atm.mBlueHorizon.valueAt(phase);
            c.mV[p - P_BLUE_HORIZON_R] = value;
            atm.mBlueHorizon.setValueAtHead(phase, c);
            break;
        }
        case P_BLUE_DENSITY_R:
        case P_BLUE_DENSITY_G:
        case P_BLUE_DENSITY_B:
        {
            LLColor3 c = atm.mBlueDensity.valueAt(phase);
            c.mV[p - P_BLUE_DENSITY_R] = value;
            atm.mBlueDensity.setValueAtHead(phase, c);
            break;
        }
        case P_HAZE_HORIZON:  atm.mHazeHorizon.setValueAtHead(phase, value); break;
        case P_HAZE_DENSITY:  atm.mHazeDensity.setValueAtHead(phase, value); break;
        case P_DENSITY_MULT:  atm.mDensityMultiplier.setValueAtHead(phase, value); break;
        case P_DISTANCE_MULT: atm.mDistanceMultiplier.setValueAtHead(phase, value); break;

        case P_CLOUD_COLOR_R:
        case P_CLOUD_COLOR_G:
        case P_CLOUD_COLOR_B:
        {
            LLColor3 c = dome.mColor.valueAt(phase);
            c.mV[p - P_CLOUD_COLOR_R] = value;
            dome.mColor.setValueAtHead(phase, c);
            break;
        }
        case P_CLOUD_COVERAGE: dome.mCoverage.setValueAtHead(phase, value); break;

        default: break;
    }
}

// static
F32 SSAtmoMatch::paramMin(Param p)
{
    switch (p)
    {
        case P_DENSITY_MULT:  return 0.0001f;
        case P_DISTANCE_MULT: return 0.f;
        default:              return 0.f;
    }
}

// static
F32 SSAtmoMatch::paramMax(Param p)
{
    // EEP's own editor ranges - see llpaneleditsky.cpp - so a fit can never
    // walk a parameter somewhere the sliders could not have put it.
    switch (p)
    {
        case P_BLUE_HORIZON_R:
        case P_BLUE_HORIZON_G:
        case P_BLUE_HORIZON_B:
        case P_BLUE_DENSITY_R:
        case P_BLUE_DENSITY_G:
        case P_BLUE_DENSITY_B: return 2.f;
        case P_HAZE_HORIZON:   return 1.f;
        case P_HAZE_DENSITY:   return 4.f;
        case P_DENSITY_MULT:   return 0.9f;
        case P_DISTANCE_MULT:  return 100.f;
        case P_CLOUD_COLOR_R:
        case P_CLOUD_COLOR_G:
        case P_CLOUD_COLOR_B:  return 1.f;
        case P_CLOUD_COVERAGE: return 1.f;
        default:               return 1.f;
    }
}

// </SS:Nexii>
