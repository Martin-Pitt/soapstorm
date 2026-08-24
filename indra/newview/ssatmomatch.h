/**
 * @file ssatmomatch.h
 * @brief Atmo Magic: fit a track's sky parameters to a reference photograph
 *        by rendering and comparing, one candidate per frame.
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

#ifndef SS_ATMOMATCH_H
#define SS_ATMOMATCH_H

// <SS:Nexii> Atmo Magic: match the sky to a photograph

#include "llsingleton.h"
#include "v3color.h"
#include "v3math.h"

#include <string>
#include <vector>

// Fits the authored sky to a reference image by RENDERING IT.
//
// There is no CPU model of EEP's sky to fit against - the atmosphere lives
// in the shaders, and the only CPU one left (lllegacyatmospherics) computes
// the pre-EEP sky, so matching against it would fit parameters to a sky
// nobody draws. So this renders the real one: set a candidate, draw the sky
// into a narrow offscreen strip, read the strip, score it against the photo,
// keep or discard.
//
// Even offscreen an iteration costs part of a frame, which bounds the whole
// design: a few hundred probes, not the millions a numerical optimiser would
// like. Coordinate descent with a shrinking step suits that budget, converges
// visibly (the sky walks toward the photo while you watch), and can be
// stopped at any point with whatever it has.
class SSAtmoMatch : public LLSingleton<SSAtmoMatch>
{
    LLSINGLETON_EMPTY_CTOR(SSAtmoMatch);

public:
    // One sample up the reference image's line: how high it sits, and what
    // colour the photo is there.
    struct Sample
    {
        F32 mElevationDeg = 0.f;
        LLColor3 mColor;
    };

    // Begin fitting `samples` (bottom-first) on the given track. Replaces
    // any run in progress. The track's current values are the starting
    // point, so a run that is stopped early leaves something sensible.
    void start(S32 track_index, const std::vector<Sample>& samples);

    // The samples the overlay should show while nothing is running - set
    // whenever the sample line moves, cleared when the floater closes.
    // Ignored during a run, which has its own.
    void setPreview(const std::vector<Sample>& samples);
    void clearPreview();

    // Called once per frame from the applier's own tick, AFTER the world
    // has been drawn - the score comes out of the frame just rendered.
    void idle();

    // Put the authored values back exactly as they were when start() was
    // called. Every candidate has been written to the live track, so an
    // abandoned run must undo itself.
    void cancel();

    // Keep what has been found and stop.
    void accept();

    bool isRunning() const { return mRunning; }
    F32 progress() const;             // 0..1, for a bar
    F32 lastError() const { return mBestError; }
    S32 passesDone() const { return mPass; }

    // What the fit is currently showing, for the floater's own readout -
    // the same samples the score is built from.
    const std::vector<LLColor3>& renderedColors() const { return mRendered; }

    // Why the fit is not advancing, if it is not. Empty while it runs
    // normally - the floater shows this so a stalled run says so instead of
    // looking broken.
    const std::string& waitReason() const { return mWaitReason; }

    // Draws the sample heights in the world - a marker per height, filled
    // with the colour the PHOTO has there, so the reference can be held up
    // against the sky it is about to be fitted to.
    //
    // Shown only while NOT matching. It is an aiming and sanity aid for
    // setting a line up ("is this really the sky I mean?"), and during a run
    // it would be a dozen coloured crosses sitting over the very sky being
    // judged - clutter at best, and at worst it reads as though the fit
    // were sampling them.
    void renderDebug() const;

private:
    // The parameters this fits, and the order it walks them in. Deliberately
    // a short list: these are the ones that shape a horizon gradient, and
    // every extra dimension costs frames without changing the picture much.
    enum Param
    {
        P_BLUE_HORIZON_R, P_BLUE_HORIZON_G, P_BLUE_HORIZON_B,
        P_BLUE_DENSITY_R, P_BLUE_DENSITY_G, P_BLUE_DENSITY_B,
        P_HAZE_HORIZON,
        P_HAZE_DENSITY,
        P_DENSITY_MULT,
        P_DISTANCE_MULT,

        // The cloud deck, because it is not separable from the horizon it
        // sits on: coverage alone moves the whole lower sky, and a deck
        // whose colour is wrong drags every sample it touches with it.
        P_CLOUD_COLOR_R, P_CLOUD_COLOR_G, P_CLOUD_COLOR_B,
        P_CLOUD_COVERAGE,

        P_COUNT
    };

    F32 readParam(Param p) const;
    void writeParam(Param p, F32 value);
    static F32 paramMin(Param p);
    static F32 paramMax(Param p);

    // Renders the candidate sky into a narrow offscreen strip and reads the
    // sample colours out of it.
    //
    // Offscreen, rather than reading the frame the user is looking at. The
    // screen-reading version worked but had to mutate the live sky to be
    // measured, so the whole view flashed through every candidate, and it
    // could only see elevations the camera happened to be pointing at - so
    // it stalled whenever the aim drifted. A strip a few pixels wide costs
    // a fraction of a frame, looks wherever it likes, and can be run
    // several times per frame.
    bool sampleProbeSky(std::vector<LLColor3>& out);

    // Which way the probe looks: wherever the camera is facing, so turning
    // chooses the slice of sky being fitted. The in-world markers use this
    // too, which is what makes the aim visible.
    //
    // Held still for the length of a run, though. Coordinate descent only
    // means anything if every candidate is judged against the same sky, and
    // a camera nudged halfway through would otherwise have the fit
    // comparing north against east and calling the difference an
    // improvement.
    LLVector3 probeHeading() const;

    // How many candidates to try per frame. The probe is cheap, so the
    // limit is how much of a frame is reasonable to spend rather than how
    // fast the sky can be drawn.
    static const S32 PROBES_PER_FRAME = 4;

    static F32 scoreAgainst(const std::vector<LLColor3>& rendered,
                            const std::vector<Sample>& target);

    // One candidate: probe, score, and step the walk. Returns false when
    // the probe could not run at all.
    bool stepOnce();

    bool mRunning = false;
    S32 mTrackIndex = 0;

    std::vector<Sample> mTarget;
    std::vector<LLColor3> mRendered;

    // Where the walk is: which parameter, which direction, how big a step.
    S32 mParam = 0;
    S32 mPhase = 0;               // 0 measure baseline, 1 try up, 2 try down
    S32 mPass = 0;
    F32 mStep = 0.25f;            // fraction of each parameter's own range
    F32 mBestError = 1.0e9f;
    F32 mParamBackup = 0.f;

    // The whole track as it was, for cancel().
    std::vector<F32> mOriginal;
    std::string mWaitReason;

    // The heading the current run is fitting against, taken at start().
    LLVector3 mRunHeading;

    // The strip the candidate sky is drawn into. Narrow because only the
    // vertical direction carries information here - every column of it is
    // the same sky at a different azimuth by a degree or two.
    class LLRenderTarget* mProbe = nullptr;
};

// </SS:Nexii>

#endif // SS_ATMOMATCH_H
