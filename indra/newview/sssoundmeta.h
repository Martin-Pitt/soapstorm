/**
 * @file sssoundmeta.h
 * @brief Atmo Magic sound metadata: every sound the soundscape is configured to play, analysed ahead of need on worker threads, so timing-critical playback (thunder onset alignment, pack
 *        levelling, future chain joins) reads a table instead of racing a decode.
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

#ifndef SS_SOUNDMETA_H
#define SS_SOUNDMETA_H

// <SS:Nexii> Atmo Magic sound metadata
// Interactions: fed from every configured soundscape pack (thunder, charge, wind, ambient, footsteps); consumed by thunder timing/levelling now, chain joins and rain-bed auto-classification later.

#include "llsingleton.h"
#include "lluuid.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

class SSSoundMeta : public LLSingleton<SSSoundMeta>
{
    LLSINGLETON_EMPTY_CTOR(SSSoundMeta);

public:
    // What a sound's ROLE actually consumes - analysis computes only these, and a sound whose role consumes nothing never enters the pipeline at all (no decode, no copy).
    enum Purpose : U32
    {
        PURPOSE_TIMING  = 1,    // onset + tail: thunder alignment, chain joins
        PURPOSE_LEVEL   = 2,    // loudest-second RMS: pack levelling
        PURPOSE_BRIGHT  = 4,    // crackiness: crack/rumble auto-sort
        PURPOSE_DENSITY = 8,    // envelope duty: the bed ladder
        PURPOSE_STEPS   = 16,   // onset list + rate + tail: gap-stops, fresh starts
    };
    struct Meta
    {
        U32 mLengthMS = 0;      // whole file
        U32 mOnsetMS = 0;       // where the main event begins (envelope rise to 20% of peak)
        U32 mTailMS = 0;        // where the content effectively ENDS (last envelope window above 5% of peak) - the join point for chained parts, not the file length
        F32 mPeakLevel = 0.f;   // loudest-second RMS, 0..1 - the levelling proxy
        F32 mImpactRate = 0.f;  // discrete envelope onsets per second - right for SPARSE material (steps, drips); honestly ~0 for continuous wash
        F32 mDensity = 0.f;     // mean envelope over peak (inverse crest), 0..1 - the density proxy for CONTINUOUS material, monotonic where onset counting is not; the bed ladder sorts on this
        F32 mCrackiness = 0.f;  // zero-crossing rate around the peak, 0..1 - bright crack vs low rumble, without an FFT

        // The individual onset times, ms from file start (capped). For a footstep loop these are the steps themselves - what lets a stopping walk loop finish its current step and cut in the gap
        // after it instead of mid-splat.
        std::vector<U32> mOnsets;

        // How quiet the recording is at its CUT POINTS - the late 45% of each inter-onset gap, 0..1 against peak. THE segmentability verdict: low means the 2/3-point cuts land in real quiet and
        // windowed per-impact playback works; high means wash bridges the steps even late in the gap and only whole-loop playback is honest. Deliberately blind to the early gap, so a clean room
        // recording with a booming reverb tail (which decays before the cut point) still qualifies - it was reading as wash when the whole gap was averaged.
        F32 mGapFloor = 1.f;

        // Coefficient of variation of the inter-onset intervals. Real footsteps are quasi-periodic - intervals cluster around the stride time, CV low - while a map polluted by splash/debris noise
        // between the true steps is ragged. The second half of the segmentability verdict: quiet gaps prove the CUTS are clean, regular cadence proves the MAP is steps.
        F32 mCadenceCV = 9.f;
        U32 mRepaired = 0;      // onsets INSERTED by cadence-grid repair: a quiet step under the detector threshold leaves a double-length interval, and an even-cadence recording lets the missing
                                // step be put back on the grid. Visible so a heavily-repaired map can be eyed with suspicion.

        U32 mPeakMS = 0;              // centre of the loudest window, for the debug floater's marker
        std::vector<F32> mEnvelope;   // 240-point downsampled RMS envelope, normalised to peak - the debug floater's waveform
    };

    // Ready metadata or null. Null means not analysed YET - callers keep their fallback path.
    const Meta* get(const LLUUID& id);

    // Main-thread pump: gathers the configured packs, walks decodes, hands PCM copies to the workers. Cheap per call; all the arithmetic happens off-main.
    void idle();

    // For the info overlay.
    S32 readyCount();
    S32 pendingCount();

private:
    void cleanupSingleton() override;

    void gather();
    void addList(const std::string& csv, const std::string& source, U32 purpose);
    void pump();
    void startWorkers();
    static Meta analyze(const std::vector<S16>& pcm, S32 channels, F32 rate, U32 purpose);

public:
    enum EState { PENDING, ANALYZING, READY, FAILED };
private:
    struct Entry
    {
        EState mState = PENDING;
        F64 mFirstTried = -1.0;    // when the pump first asked for this decode; missing assets give up after a while instead of camping the in-flight slots
        std::string mSource;       // which pack/slot configured this UUID - so a dead one names its OWN home when it fails, instead of being an anonymous stall
        U32 mPurpose = 0;          // OR of Purpose bits; a UUID serving two roles gets both
        Meta mMeta;
    };

public:
    // Read-only walk for the analysis debug floater. Main thread.
    const std::map<LLUUID, Entry>& entriesForDebug() const { return mEntries; }

private:

    struct Job
    {
        LLUUID mID;
        std::vector<S16> mPCM;
        S32 mChannels = 1;
        F32 mRate = 44100.f;
        U32 mLengthMS = 0;
        U32 mPurpose = 0;
    };

    std::map<LLUUID, Entry> mEntries;   // main thread only, except mMeta writes under mResultMutex
    F64 mLastGather = -1.0;

    // The pool. Sized modestly on purpose: the per-file arithmetic is milliseconds, decode (already on its own thread) dominates, and a handful of workers saturates the queue long before the
    // machine's cores do.
    std::vector<std::thread> mWorkers;
    std::deque<Job> mJobs;
    std::vector<std::pair<LLUUID, Meta>> mResults;
    std::mutex mJobMutex;
    std::mutex mResultMutex;
    std::condition_variable mJobSignal;
    bool mStop = false;
    bool mStarted = false;
};

// </SS:Nexii>

#endif // SS_SOUNDMETA_H
