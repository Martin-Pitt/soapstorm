/**
 * @file sssoundmeta.cpp
 * @brief Atmo Magic sound metadata - see sssoundmeta.h.
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

#include "sssoundmeta.h"

#include "ssatmomagic.h"
#include "ssprecippreset.h"

#include "llaudioengine.h"
#include "llviewercontrol.h"

// FMOD stays on the main thread; workers only ever see plain PCM copies. The lock/memcpy/unlock is a millisecond for a 30s clip - the arithmetic is what goes off-main.

void SSSoundMeta::cleanupSingleton()
{
    {
        std::lock_guard<std::mutex> lock(mJobMutex);
        mStop = true;
    }
    mJobSignal.notify_all();
    for (std::thread& t : mWorkers)
    {
        if (t.joinable()) t.join();
    }
    mWorkers.clear();
}

void SSSoundMeta::startWorkers()
{
    if (mStarted) return;
    mStarted = true;

    const S32 count = llclamp((S32)std::thread::hardware_concurrency() - 1, 1, 4);
    for (S32 i = 0; i < count; ++i)
    {
        mWorkers.emplace_back([this]()
        {
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mJobMutex);
                    mJobSignal.wait(lock, [this]() { return mStop || !mJobs.empty(); });
                    if (mStop) return;
                    job = std::move(mJobs.front());
                    mJobs.pop_front();
                }

                Meta meta = analyze(job.mPCM, job.mChannels, job.mRate);
                meta.mLengthMS = job.mLengthMS;

                std::lock_guard<std::mutex> lock(mResultMutex);
                mResults.emplace_back(job.mID, meta);
            }
        });
    }
}

// Pure arithmetic on a copy - the whole reason this can run anywhere.
SSSoundMeta::Meta SSSoundMeta::analyze(const std::vector<S16>& pcm, S32 channels, F32 rate)
{
    Meta meta;
    if (pcm.empty() || channels < 1 || rate <= 0.f) return meta;

    const U32 frames = (U32)(pcm.size() / (size_t)channels);
    const U32 window = llmax((U32)(rate * 0.010f), 1u);
    const U32 count = frames / window;
    if (count < 2) return meta;

    std::vector<F32> envelope(count, 0.f);
    F32 peak = 0.f;
    U32 peak_at = 0;
    for (U32 w = 0; w < count; ++w)
    {
        F64 sum = 0.0;
        const S16* p = pcm.data() + (size_t)w * window * channels;
        for (U32 i = 0; i < window * (U32)channels; ++i)
        {
            const F64 v = (F64)p[i] / 32768.0;
            sum += v * v;
        }
        const F32 rms = (F32)sqrt(sum / (F64)(window * channels));
        envelope[w] = rms;
        if (rms > peak) { peak = rms; peak_at = w; }
    }
    if (peak <= 0.0001f) return meta;

    // Onset: walk back from the loudest window to the rise past 20% of peak - the moment a listener says it happened, not the moment of most energy.
    U32 onset = peak_at;
    while (onset > 0 && envelope[onset - 1] >= peak * 0.2f) --onset;
    meta.mOnsetMS = (U32)((F32)(onset * window) * 1000.f / rate);

    // Tail: where content effectively ends. Chains join here, not at the file length - trailing silence in an export must not become a hole in a rolled rumble.
    U32 tail = count - 1;
    while (tail > 0 && envelope[tail] < peak * 0.05f) --tail;
    meta.mTailMS = (U32)((F32)((tail + 1) * window) * 1000.f / rate);

    // Loudest second, for levelling.
    {
        const S32 half = llmax((S32)(0.5f * rate / (F32)window), 1);
        const S32 lo = llmax((S32)peak_at - half, 0);
        const S32 hi = llmin((S32)peak_at + half, (S32)count - 1);
        F32 sum_env = 0.f;
        for (S32 w = lo; w <= hi; ++w) sum_env += envelope[(size_t)w];
        meta.mPeakLevel = sum_env / (F32)(hi - lo + 1);
    }

    // Impact rate: discrete envelope onsets per second, with a 120ms refractory so one splat is one impact. This is the number that lets a rain bed carry its own density - light drips and a
    // hammering downpour are the same file format but very different figures here.
    {
        S32 impacts = 0;
        const U32 refractory = llmax((U32)(0.120f * rate / (F32)window), 1u);
        U32 last = 0;
        bool armed = true;
        for (U32 w = 1; w < count; ++w)
        {
            if (armed && envelope[w] > peak * 0.35f && envelope[w] > envelope[w - 1])
            {
                ++impacts;
                last = w;
                armed = false;
            }
            else if (!armed && w - last > refractory && envelope[w] < peak * 0.2f)
            {
                armed = true;
            }
        }
        const F32 seconds = (F32)frames / rate;
        meta.mImpactRate = (seconds > 0.1f) ? (F32)impacts / seconds : 0.f;
    }

    // Crackiness: zero-crossing rate through the loudest second, normalised so ~4kHz-bright reads 1. Separates a sharp crack from a low roll without an FFT - enough to auto-sort a mixed pack
    // into near and far material.
    {
        const U32 lo_f = peak_at * window;
        const U32 hi_f = llmin(lo_f + (U32)rate, frames - 1);
        S32 crossings = 0;
        S16 prev = pcm[(size_t)lo_f * channels];
        for (U32 f = lo_f + 1; f <= hi_f; ++f)
        {
            const S16 cur = pcm[(size_t)f * channels];
            if ((prev < 0) != (cur < 0)) ++crossings;
            prev = cur;
        }
        const F32 span_s = (F32)(hi_f - lo_f) / rate;
        meta.mCrackiness = (span_s > 0.01f)
            ? llclamp(((F32)crossings / span_s) / 8000.f, 0.f, 1.f) : 0.f;
    }

    return meta;
}

const SSSoundMeta::Meta* SSSoundMeta::get(const LLUUID& id)
{
    auto it = mEntries.find(id);
    return (it != mEntries.end() && it->second.mState == READY) ? &it->second.mMeta : nullptr;
}

S32 SSSoundMeta::readyCount()
{
    S32 n = 0;
    for (const auto& e : mEntries) if (e.second.mState == READY) ++n;
    return n;
}

S32 SSSoundMeta::pendingCount()
{
    S32 n = 0;
    for (const auto& e : mEntries) if (e.second.mState == PENDING || e.second.mState == ANALYZING) ++n;
    return n;
}

void SSSoundMeta::addList(const std::string& csv)
{
    std::vector<std::string> tokens;
    LLStringUtil::getTokens(csv, tokens, ",+");
    for (const std::string& tok : tokens)
    {
        LLUUID id(tok);
        if (id.notNull()) mEntries.emplace(id, Entry());
    }
}

void SSSoundMeta::gather()
{
    // Everything the soundscape is configured to play, from every home a sound can live in. Re-gathered every few seconds so edits in the floaters trickle in without any notification plumbing.
    for (const char* setting : { "SSAtmoThunderCrack", "SSAtmoThunderRumble", "SSAtmoLightningCharge",
                                 "SSAtmoLoopWindLight", "SSAtmoLoopWindStrong" })
    {
        addList(gSavedSettings.getString(setting));
    }

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            if (SSFootstepSounds::surfaceIsGlobal((SSStepSurface)sf))
            {
                addList(gSavedSettings.getString(SSFootstepSounds::globalSettingName((SSStepSurface)sf, (SSStepAction)ac)));
            }
        }
    }

    const SSPrecipPreset& preset = SSPrecipPresetManager::instance().active();
    for (const std::string* csv : { &preset.mSounds.mAmbientLight, &preset.mSounds.mAmbientMedium,
                                    &preset.mSounds.mAmbientHeavy, &preset.mSounds.mRoofOpen,
                                    &preset.mSounds.mRoofSmall, &preset.mSounds.mRoofMedium,
                                    &preset.mSounds.mRoofBig })
    {
        addList(*csv);
    }
    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            addList(preset.mFootsteps.mSounds[sf][ac]);
        }
    }
}

void SSSoundMeta::pump()
{
    if (!gAudiop) return;

    // A couple of decodes in flight at a time: enough to stream through a library in under a minute, few enough to never contend with the sounds the moment actually needs.
    S32 in_flight = 0;
    for (auto& pair : mEntries)
    {
        if (pair.second.mState != PENDING) continue;
        if (in_flight >= 2) break;

        LLAudioData* data = gAudiop->getAudioData(pair.first);
        if (!data) { pair.second.mState = FAILED; continue; }

        if (!data->hasDecodedData())
        {
            gAudiop->preloadSound(pair.first);
            ++in_flight;
            continue;
        }

        LLAudioBuffer* buffer = data->getBuffer();
        if (!buffer)
        {
            gAudiop->updateBufferForData(data, pair.first);
            buffer = data->getBuffer();
        }
        if (!buffer) { ++in_flight; continue; }

        Job job;
        job.mID = pair.first;
        job.mLengthMS = buffer->getLengthMS();
        if (!buffer->getPCMCopy(job.mPCM, job.mChannels, job.mRate))
        {
            pair.second.mState = FAILED;
            continue;
        }

        pair.second.mState = ANALYZING;
        {
            std::lock_guard<std::mutex> lock(mJobMutex);
            mJobs.push_back(std::move(job));
        }
        mJobSignal.notify_one();
        ++in_flight;
    }
}

void SSSoundMeta::idle()
{
    if (!SSAtmoMagic::getInstance()->isSwitchedOn()) return;

    startWorkers();

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    if (mLastGather < 0.0 || now - mLastGather > 5.0)
    {
        mLastGather = now;
        gather();
    }

    pump();

    // Collect finished analyses.
    std::vector<std::pair<LLUUID, Meta>> done;
    {
        std::lock_guard<std::mutex> lock(mResultMutex);
        done.swap(mResults);
    }
    for (auto& pair : done)
    {
        Entry& entry = mEntries[pair.first];
        entry.mMeta = pair.second;
        entry.mState = READY;
    }
}

// </SS:Nexii>
