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
    struct Meta
    {
        U32 mLengthMS = 0;      // whole file
        U32 mOnsetMS = 0;       // where the main event begins (envelope rise to 20% of peak)
        U32 mTailMS = 0;        // where the content effectively ENDS (last envelope window above 5% of peak) - the join point for chained parts, not the file length
        F32 mPeakLevel = 0.f;   // loudest-second RMS, 0..1 - the levelling proxy
        F32 mImpactRate = 0.f;  // discrete envelope onsets per second - rain beds and footstep loops carry their density here
        F32 mCrackiness = 0.f;  // zero-crossing rate around the peak, 0..1 - bright crack vs low rumble, without an FFT
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
    void addList(const std::string& csv);
    void pump();
    void startWorkers();
    static Meta analyze(const std::vector<S16>& pcm, S32 channels, F32 rate);

    enum EState { PENDING, ANALYZING, READY, FAILED };
    struct Entry
    {
        EState mState = PENDING;
        Meta mMeta;
    };

    struct Job
    {
        LLUUID mID;
        std::vector<S16> mPCM;
        S32 mChannels = 1;
        F32 mRate = 44100.f;
        U32 mLengthMS = 0;
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
