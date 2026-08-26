/**
 * @file sssoundscape.cpp
 * @brief Atmo Magic environmental weather audio implementation.
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

#include "sssoundscape.h"
#include "ssatmomagic.h"
#include "sswindflow.h"
#include "ssprecippreset.h"
#include "sssurfacefield.h"
#include "sssoundmeta.h"

#include "llrand.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llaudioengine.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "lldrawable.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "pipeline.h"

// <SS:Nexii> Atmo Magic environmental weather audio

static const F64 PROBE_INTERVAL    = 0.05;  // min seconds between full probe cycles
static const F32 MOVE_TRIGGER     = 0.2f;  // camera movement that re-probes
static const F64 STALE_TRIGGER    = 1.0;   // full reprobe after this long regardless
static const F32 UP_RAY_LENGTH    = 100.f;
static const S32 UP_RAY_COUNT     = 3;     // roof samples; all must agree to count as covered
static const F32 UP_RAY_TILT      = 12.f;  // degrees off vertical each sample leans
static const F32 SIDE_RAY_LENGTH  = 50.f;
static const F32 SMALL_SPACE_AVG  = 10.f;  // average wall distance thresholds
static const F32 MEDIUM_SPACE_AVG = 30.f;
static const F32 IMPACT_RATE_FULL = 22.f;  // impact strength/sec that reads as full loudness
static const F32 IMPACT_RATE_TAU  = 1.5f;  // seconds, decay of the impact rate EMA
static const F32 COVER_BLEND_RATE = 8.f;   // per second, indoor/outdoor crossfade

// Burial. The depth at which build overhead has taken most of the rain bed away, and how much of it it may take at the limit. Not all of it: a cellar under a downpour is not silent, and a drain or a
// light well carries some of it down. Eased more slowly than cover because it describes moving through a building rather than through a doorway.
static const F32 BURIAL_FULL       = 12.f;  // metres of build above the ceiling
static const F32 BURIAL_MAX_DUCK   = 0.85f; // most of the rain bed it may remove
static const F32 BURIAL_BLEND_RATE = 2.5f;  // per second

static LLTrace::BlockTimerStatHandle FTM_SS_AUDIO("Atmo Magic Audio");
static LLTrace::BlockTimerStatHandle FTM_SS_AUDIO_PROBE("Cover Probes");

// Symmetric triangle weight: 0 at lo and hi, 1 at peak
static F32 tri(F32 x, F32 lo, F32 peak, F32 hi)
{
    if (x <= lo || x >= hi) return 0.f;
    return x < peak ? (x - lo) / llmax(0.01f, peak - lo)
                    : (hi - x) / llmax(0.01f, hi - peak);
}

static void parseSoundList(const std::string& value, std::vector<LLUUID>& out)
{
    out.clear();
    std::string::size_type pos = 0;
    while (pos < value.size())
    {
        std::string::size_type end = value.find(',', pos);
        if (end == std::string::npos) end = value.size();
        std::string token = value.substr(pos, end - pos);
        pos = end + 1;

        LLStringUtil::trim(token);
        if (LLUUID::validate(token))
        {
            out.push_back(LLUUID(token));
        }
    }
}

void SSSoundscape::releaseLoop(Loop& loop)
{
    if (gAudiop && loop.mSourceID.notNull())
    {
        LLAudioSource* source = gAudiop->findAudioSource(loop.mSourceID);
        if (source)
        {
            gAudiop->cleanupAudioSource(source);
        }
    }
    loop.mSourceID.setNull();
    loop.mGain = 0.f;
    loop.mTarget = 0.f;
}

void SSSoundscape::stopAll()
{
    for (Loop& loop : mLoops)
    {
        releaseLoop(loop);
    }
    mImpactRate = 0.f;
}

void SSSoundscape::notifyImpact(F32 strength)
{
    mImpactRate += strength;
}

bool SSSoundscape::castUpProbe(S32 index, F32& hit_dist)
{
    // Three rays leaning slightly off vertical, evenly spaced around the camera. Tilting them rather than firing straight up is what stops a single narrow gap or a lone beam overhead from deciding
    // the result; the caller only calls it covered when all three agree.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 azimuth = (F32)index * (F_TWO_PI / (F32)UP_RAY_COUNT) + 0.7f;
    const F32 tilt = UP_RAY_TILT * DEG_TO_RAD;
    const LLVector3 dir(cosf(azimuth) * sinf(tilt), sinf(azimuth) * sinf(tilt), cosf(tilt));

    LLVector4a start4, end4, intersect;
    start4.load3(cam.mV);
    const LLVector3 end = cam + dir * UP_RAY_LENGTH;
    end4.load3(end.mV);

    // pick_transparent: glass roofs and windows shelter from rain even though the camera ray is happy to pass through them
    if (gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true))
    {
        LLVector4a delta = intersect;
        delta.sub(start4);
        hit_dist = delta.getLength3().getF32();
        return true;
    }
    hit_dist = UP_RAY_LENGTH;
    return false;
}

F32 SSSoundscape::castSideProbe(S32 index)
{
    static const LLVector3 cardinals[4] = {
        LLVector3(1.f, 0.f, 0.f), LLVector3(-1.f, 0.f, 0.f),
        LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, -1.f, 0.f)
    };

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLVector4a start4, end4, intersect;
    start4.load3(cam.mV);
    const LLVector3 end = cam + cardinals[index] * SIDE_RAY_LENGTH;
    end4.load3(end.mV);

    if (gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true))
    {
        LLVector4a delta = intersect;
        delta.sub(start4);
        return delta.getLength3().getF32();
    }
    return SIDE_RAY_LENGTH;
}

void SSSoundscape::updateProbes(F64 now)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_AUDIO_PROBE);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    // Re-probe on movement or staleness, rate limited so standing still costs nothing and running costs at most one cycle per interval
    const bool moved = (cam - mProbeAnchor).magVec() > MOVE_TRIGGER;
    const bool stale = now - mLastCycleDone > STALE_TRIGGER;
    if (!moved && !stale) return;
    if (now - mLastCycleDone < PROBE_INTERVAL) return;

    mProbeAnchor = cam;
    mProbeOrigin = cam;
    mLastCycleDone = now;

    // Whole cycle at once: 7 short static-geometry rays resolve cover and room size in the same frame the camera moved, so the mix never trails the player walking through a doorway
    S32 up_hits = 0;
    F32 roof_dist = UP_RAY_LENGTH;
    for (S32 i = 0; i < UP_RAY_COUNT; ++i)
    {
        F32 hit_dist = UP_RAY_LENGTH;
        if (castUpProbe(i, hit_dist))
        {
            ++up_hits;
            roof_dist = llmin(roof_dist, hit_dist);
        }
    }

    // All three have to agree. Partial hits mean the rays disagree about what is overhead - a gap, a doorway, a beam - and that is treated as open sky rather than guessed at.
    mCoverage = (F32)up_hits / (F32)UP_RAY_COUNT;
    mCovered = (up_hits == UP_RAY_COUNT);
    mRoofDist = mCovered ? roof_dist : 0.f;

    // How much build is stacked above the ceiling. The up ray stops at the first thing over your head, which in a stairwell or a ground floor room is one slab; the flowmap's overhead capture knows
    // where the column actually ends against the sky. The difference is the rest of the building, or the hillside over a cellar. The up ray is also short, so a room whose ceiling is beyond its reach
    // reads as uncovered and this stays at zero - which is right, because a space that open is not muffling anything either.
    mBuriedDepth = 0.f;
    if (mCovered)
    {
        F32 column_top = 0.f;
        if (SSWindFlowMap::getInstance()->surfaceAt(cam, column_top))
        {
            // Everything above the ceiling the ray found. The ceiling itself is not burial: one roof between you and the sky is an ordinary indoor space, and mCoverSmooth already handles that.
            const F32 ceiling = cam.mV[VZ] + roof_dist;
            mBuriedDepth = llmax(0.f, column_top - ceiling);
        }
    }

    S32 walls = 0;
    F32 sum = 0.f;
    for (S32 i = 0; i < 4; ++i)
    {
        mSideDist[i] = castSideProbe(i);
        if (mSideDist[i] < SIDE_RAY_LENGTH - 0.5f) ++walls;
        sum += mSideDist[i];
    }
    const F32 avg = sum * 0.25f;
    mWallCount = walls;
    mWallAvg = avg;

    // Width is banded the same way whether it ends up describing a room or the open ground around you
    mOutdoorSize = (avg < SMALL_SPACE_AVG)  ? SIZE_SMALL
                 : (avg < MEDIUM_SPACE_AVG) ? SIZE_MEDIUM : SIZE_LARGE;

    if (!mCovered)
    {
        // Nothing overhead, so height stops counting and width takes over
        mSpace = SPACE_OUTDOOR;
    }
    else if (walls <= 1)
    {
        // Roof overhead but open sides: a porch or awning, not a room
        mSpace = SPACE_SHELTERED;
    }
    else if (walls >= 3 && avg < SMALL_SPACE_AVG)
    {
        mSpace = SPACE_SMALL;
    }
    else if (avg < MEDIUM_SPACE_AVG)
    {
        mSpace = SPACE_MEDIUM;
    }
    else
    {
        mSpace = SPACE_BIG;
    }
}

// Smooth ramp rather than a linear one: the first couple of metres of build over a ceiling should barely register, because that is an ordinary floor slab and you can still plainly hear the storm. It
// is being under several of them that takes the rain away.
F32 SSSoundscape::burialOcclusion() const
{
    const F32 t = llclamp(mBuriedSmooth / BURIAL_FULL, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// static
const char* SSSoundscape::spaceName(ESpace space)
{
    switch (space)
    {
        case SPACE_SHELTERED: return "sheltered (roof, open sides)";
        case SPACE_SMALL:     return "small room";
        case SPACE_MEDIUM:    return "medium room";
        case SPACE_BIG:       return "big hall";
        default:              return "outdoors";
    }
}

// static
const char* SSSoundscape::sizeName(ESize size)
{
    switch (size)
    {
        case SIZE_SMALL:  return "tight";
        case SIZE_MEDIUM: return "medium";
        default:          return "open";
    }
}

F32 SSSoundscape::wallDistanceToward(const LLVector3& dir_horizontal) const
{
    static const LLVector3 cardinals[4] = {
        LLVector3(1.f, 0.f, 0.f), LLVector3(-1.f, 0.f, 0.f),
        LLVector3(0.f, 1.f, 0.f), LLVector3(0.f, -1.f, 0.f)
    };

    // Only the two cardinals bracketing the direction get positive weight, so this interpolates between the samples either side of it
    F32 weighted = 0.f;
    F32 total = 0.f;
    for (S32 i = 0; i < 4; ++i)
    {
        const F32 w = llmax(0.f, dir_horizontal * cardinals[i]);
        weighted += w * mSideDist[i];
        total += w;
    }
    return (total > 0.f) ? (weighted / total) : SIDE_RAY_LENGTH;
}

F32 SSSoundscape::occlusionGain(const LLVector3& source_pos) const
{
    LLVector3 to_source = source_pos - mProbeOrigin;
    to_source.mV[VZ] = 0.f;
    const F32 dist = to_source.normVec();
    if (dist < 1.f) return 1.f;

    const F32 wall = wallDistanceToward(to_source);
    if (dist <= wall + 0.5f) return 1.f;   // same side of everything: unmuffled

    // Past a surface. Sealed in a room muffles far more than a wall standing between you and the source out in the open.
    return lerp(0.6f, 0.22f, mCoverSmooth);
}

F32 SSSoundscape::impactRate() const
{
    // mImpactRate is an exponentially decayed sum of impact strengths; its steady state is (rate * tau), so undo the tau for a real per-second figure
    return mImpactRate / IMPACT_RATE_TAU;
}

S32 SSSoundscape::activeLoops() const
{
    S32 count = 0;
    for (const Loop& loop : mLoops)
    {
        if (loop.mSourceID.notNull() && loop.mGain > 0.005f) ++count;
    }
    return count;
}

F64 SSSoundscape::lastProbeAge() const
{
    return SSAtmoMagic::getInstance()->sharedTime() - mLastCycleDone;
}

void SSSoundscape::applyLoop(Loop& loop, const std::string& configured, F32 master, F32 dt)
{
    if (configured != loop.mConfigured)
    {
        releaseLoop(loop);
        loop.mConfigured = configured;
        parseSoundList(configured, loop.mSounds);
        loop.mIndex = 0;
    }

    if (loop.mSounds.empty() || !gAudiop)
    {
        loop.mTarget = 0.f;
        return;
    }

    static LLCachedControl<F32> fade_rate(gSavedSettings, "SSAtmoSoundResponse", 3.f);
    loop.mGain = lerp(loop.mGain, loop.mTarget, llclamp(llmax(0.1f, (F32)fade_rate) * dt, 0.f, 1.f));
    const F32 gain = llclamp(loop.mGain * master, 0.f, 1.f);

    // Look the source up by ID every frame: the engine reaps finished non-looping sources itself, and a vanished source is exactly the "previous element ended" signal that advances a sequence
    LLAudioSource* source = loop.mSourceID.notNull() ? gAudiop->findAudioSource(loop.mSourceID) : nullptr;

    if (gain < 0.005f)
    {
        if (loop.mTarget < 0.005f && loop.mSourceID.notNull())
        {
            // Fully faded and not wanted: release the channel (the sequence position is kept for the next activation)
            if (source)
            {
                gAudiop->cleanupAudioSource(source);
            }
            loop.mSourceID.setNull();
        }
        else if (source)
        {
            source->setGain(0.f);
        }
        return;
    }

    if (!source)
    {
        const bool sequence = loop.mSounds.size() > 1;
        if (loop.mSourceID.notNull() && sequence)
        {
            // Previous element finished (and was reaped): next in order
            loop.mIndex = (loop.mIndex + 1) % (U32)loop.mSounds.size();
        }
        loop.mIndex %= (U32)loop.mSounds.size();

        loop.mSourceID.generate();
        source = new LLAudioSource(loop.mSourceID, gAgent.getID(), gain,
                                   LLAudioEngine::AUDIO_TYPE_AMBIENT);
        // Single entry: seamless engine-level loop. Sequence: play through once, the reap-and-respawn above chains the next element.
        source->setLoop(!sequence);
        source->setForcedPriority(true);
        gAudiop->addAudioSource(source);
        source->play(loop.mSounds[loop.mIndex]);

        if (sequence)
        {
            // Warm the decoder for the upcoming element so the handoff gap stays at a frame or two regardless of asset length
            gAudiop->preloadSound(loop.mSounds[(loop.mIndex + 1) % (U32)loop.mSounds.size()]);
        }
    }

    source->setGain(gain);
    source->setPositionGlobal(gAgent.getPosGlobalFromAgent(
        LLViewerCamera::getInstance()->getOrigin() + loop.mOffset));
}

void SSSoundscape::updateLoops(F64 now, F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    // Indoor factor eases so walking under a roof crossfades instead of cutting
    mCoverSmooth = lerp(mCoverSmooth, mCovered ? 1.f : 0.f, llclamp(COVER_BLEND_RATE * dt, 0.f, 1.f));
    mBuriedSmooth = lerp(mBuriedSmooth, mBuriedDepth, llclamp(BURIAL_BLEND_RATE * dt, 0.f, 1.f));

    // Local wetness: the parameter-side intensity blended with the actually observed impact rate around the camera, so shelter reads quieter
    const F32 env = llclamp(atmo->gustEnvelopeAt(now), 0.f, 2.5f);
    const SSPrecipPreset& preset = atmo->preset();
    const F32 param_wet = atmo->hasWeather() ? atmo->precipitation() * (0.4f + 0.3f * env) : 0.f;
    const F32 impact_wet = llclamp(mImpactRate / IMPACT_RATE_FULL, 0.f, 1.f);
    const F32 wet = llclamp(0.55f * param_wet + 0.45f * impact_wet, 0.f, 1.f);

    // Two timescales on purpose. A bed RECORDING represents the mass of rain across its whole background, so WHICH rung of the ladder plays follows a ~10s average - rung swaps track the storm,
    // not every gust. How LOUD it plays keeps the fast figure, so gusts still swell and shelter still ducks at the speed the ear expects. One number for both made rung choice flappy and loudness
    // laggy at once.
    mWetSlow = lerp(mWetSlow, wet, llclamp(dt / 10.f, 0.f, 1.f));

    // Wind rides speed, turbulence and the live gust envelope. The speed is the locally solved one where a flowmap exists, so a courtyard is quiet and a gap the wind is squeezing through is loud,
    // rather than everywhere hearing the same parameter.
    const LLVector3 cam_pos = LLViewerCamera::getInstance()->getOrigin();
    SSWindFlowMap* flow = SSWindFlowMap::getInstance();
    const F32 local_speed = flow->isValid() ? flow->sample(cam_pos).magVec()
                                            : atmo->windSpeed();

    const F32 wind = llclamp(local_speed / 14.f, 0.f, 1.f)
                   * (0.55f + 0.45f * llclamp(env, 0.f, 2.f) * 0.5f)
                   * (0.6f + 0.4f * atmo->turbulence());

    // An open shelter (roof, no walls) barely muffles the surrounding rain and wind; enclosed rooms duck them hard
    const bool sheltered = (mSpace == SPACE_SHELTERED);

    // And on top of that, everything stacked between the ceiling and the sky. Cover alone cannot tell a ground floor room from the cellar under it - both have a ceiling a couple of metres up - but
    // the flowmap's height capture can, and rain you have four storeys of building between you and should not sound like rain on the roof directly overhead.
    const F32 buried = burialOcclusion();
    const F32 outdoor = (1.f - (sheltered ? 0.4f : 0.85f) * mCoverSmooth)
                      * (1.f - BURIAL_MAX_DUCK * buried);

    // Outdoor bed: only the medium variant is required. When light or heavy are not configured their share of the blend folds back into medium, so a single-sound pack still fades in and out with
    // intensity instead of dropping to silence at the ends of the range.
    F32 w_light = tri(wet, 0.01f, 0.18f, 0.55f);
    F32 w_med   = tri(wet, 0.15f, 0.5f, 0.9f);
    F32 w_heavy = llclamp((wet - 0.55f) / 0.3f, 0.f, 1.f);
    if (preset.mSounds.mAmbientLight.empty()) { w_med += w_light; w_light = 0.f; }
    if (preset.mSounds.mAmbientHeavy.empty()) { w_med += w_heavy; w_heavy = 0.f; }
    w_med = llmin(w_med, 1.f);

    F32 targets[LOOP_COUNT] = { 0.f };
    targets[LOOP_AMBIENT_LIGHT]  = w_light * outdoor;
    targets[LOOP_AMBIENT_MEDIUM] = w_med * outdoor;
    targets[LOOP_AMBIENT_HEAVY]  = w_heavy * outdoor;

    // Bucket-free beds: every ambient recording is a rung on one ladder sorted by MEASURED density, and the FAST wetness walks it - immediacy is deliberate, and all the smoothing lives in the
    // per-rung voice crossfades (equal-power, sub-second, "studio fades"), not in slowing the signal down. Rungs are persistent voices: a boundary crossing keeps the continuing recording's voice
    // and position untouched, and a rung that fades away resumes from where it stopped next time - see updateBedVoices. Fallback to the authored tri-blend whenever the ladder cannot do better:
    // auto off, fewer than three analysed rungs, or densities too similar to sort.
    mLadderTargets.clear();
    {
        static LLCachedControl<bool> auto_beds(gSavedSettings, "SSAtmoAmbientAutoSort", true);
        if (auto_beds)
        {
            std::vector<std::pair<F32, LLUUID>> rungs;
            for (const std::string* csv : { &preset.mSounds.mAmbientLight, &preset.mSounds.mAmbientMedium, &preset.mSounds.mAmbientHeavy })
            {
                std::vector<std::string> tokens;
                LLStringUtil::getTokens(*csv, tokens, ",");
                for (const std::string& tok : tokens)
                {
                    LLUUID id(tok);
                    if (id.isNull()) continue;
                    if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id))
                    {
                        rungs.emplace_back(meta->mDensity, id);
                    }
                }
            }
            if (rungs.size() >= 3)
            {
                std::sort(rungs.begin(), rungs.end());
                if (rungs.back().first - rungs.front().first >= 0.08f)
                {
                    const F32 pos = llclamp(wet, 0.f, 1.f) * (F32)(rungs.size() - 1);
                    const size_t lo = (size_t)llmin((F32)(rungs.size() - 1), pos);
                    const size_t hi = llmin(lo + 1, rungs.size() - 1);
                    const F32 frac = llclamp(pos - (F32)lo, 0.f, 1.f);

                    // Equal-power split: constant perceived loudness across the crossfade, which is what a linear split audibly dips in the middle of.
                    const F32 amb = llclamp(wet / 0.3f, 0.f, 1.f) * outdoor;
                    mLadderTargets.emplace_back(rungs[lo].second, amb * cosf(frac * F_PI_BY_TWO));
                    if (hi != lo) mLadderTargets.emplace_back(rungs[hi].second, amb * sinf(frac * F_PI_BY_TWO));

                    // The slot beds stand down while the voices carry the rain.
                    targets[LOOP_AMBIENT_LIGHT]  = 0.f;
                    targets[LOOP_AMBIENT_MEDIUM] = 0.f;
                    targets[LOOP_AMBIENT_HEAVY]  = 0.f;
                }
            }
        }
    }

    // Rain-on-roof bed for the current situation; needs both cover and actual precipitation coming down outside. Burial takes this one too: what you hear drumming on a roof is the roof over your
    // head, and in a basement that surface is storeys away with a building damping it.
    const F32 roof = mCoverSmooth * wet * (1.f - BURIAL_MAX_DUCK * buried);
    targets[LOOP_ROOF_OPEN]   = sheltered ? roof : 0.f;
    targets[LOOP_ROOF_SMALL]  = (mSpace == SPACE_SMALL) ? roof : 0.f;
    targets[LOOP_ROOF_MEDIUM] = (mSpace == SPACE_MEDIUM) ? roof : 0.f;
    targets[LOOP_ROOF_BIG]    = (mSpace == SPACE_BIG && mCovered) ? roof : 0.f;

    // A tight outdoor space - an alley, a ravine - carries less wind than open ground even with nothing overhead. The flowmap answers this properly where it exists: it is continuous rather than a
    // three-way step, and it separates an alley lined up with the wind (which is louder than open ground) from one across it. The probe classification stays as the fallback for hardware without
    // compute.
    const F32 probe_openness = (mOutdoorSize == SIZE_SMALL)  ? 0.55f
                             : (mOutdoorSize == SIZE_MEDIUM) ? 0.8f : 1.f;
    const F32 outdoor_openness = flow->isValid()
        ? llclamp(flow->exposure(cam_pos), 0.f, 1.5f)
        : probe_openness;
    const F32 wind_indoor = (1.f - (sheltered ? 0.3f : 0.75f) * mCoverSmooth)
                          * lerp(outdoor_openness, 1.f, mCoverSmooth);
    targets[LOOP_WIND_LIGHT]  = tri(wind, 0.02f, 0.3f, 0.75f) * wind_indoor;
    targets[LOOP_WIND_STRONG] = llclamp((wind - 0.45f) / 0.4f, 0.f, 1.f) * wind_indoor;

    // Login warmup: every input above starts at its naive default - the cover probe says open sky before it has cast a ray, the flowmap has no tile so wind reads the raw parameter - and the beds
    // opened at full outdoor gain for a second before the probes discovered the roof and faded them back down. Nothing speaks until the first probe cycle has actually answered.
    if (mLastCycleDone <= 0.0)
    {
        for (S32 i = 0; i < LOOP_COUNT; ++i) targets[i] = 0.f;
        mLadderTargets.clear();
    }

    static LLCachedControl<F32> master_setting(gSavedSettings, "SSAtmoVolumeMaster", 0.8f);
    static LLCachedControl<F32> ambient_setting(gSavedSettings, "SSAtmoVolumeAmbient", 1.f);
    static LLCachedControl<F32> wind_setting(gSavedSettings, "SSAtmoVolumeWind", 1.f);
    const F32 master = llclamp((F32)master_setting, 0.f, 1.f);
    const F32 ambient_vol = llclamp((F32)ambient_setting, 0.f, 1.f);
    const F32 wind_vol = llclamp((F32)wind_setting, 0.f, 1.f);

    // Per-category trim under the master: the precipitation beds and the wind loops are mixed against each other, not just faded together
    const F32 category[LOOP_COUNT] = {
        ambient_vol, ambient_vol, ambient_vol,          // outdoor beds
        ambient_vol, ambient_vol, ambient_vol, ambient_vol, // roof beds
        wind_vol, wind_vol,
    };

    // Precipitation beds come from the preset pack; when the ladder is active their targets are zero and the rung voices carry the rain instead.
    const std::string sources[LOOP_COUNT] = {
        preset.mSounds.mAmbientLight,
        preset.mSounds.mAmbientMedium,
        preset.mSounds.mAmbientHeavy,
        preset.mSounds.mRoofOpen,
        preset.mSounds.mRoofSmall,
        preset.mSounds.mRoofMedium,
        preset.mSounds.mRoofBig,
        gSavedSettings.getString("SSAtmoLoopWindLight"),
        gSavedSettings.getString("SSAtmoLoopWindStrong"),
    };

    // The wind loops sit upwind of the head - see Loop::mOffset. Local flow, not the global vector: standing in an alley the wind you hear should come down the alley, which is exactly what the
    // flowmap knows.
    {
        LLVector3 local = flow->isValid() ? flow->sample(cam_pos)
                                          : atmo->windXY();
        const F32 speed = local.normalize();
        const LLVector3 upwind = (speed > 0.4f) ? local * -6.f : LLVector3::zero;
        mLoops[LOOP_WIND_LIGHT].mOffset = upwind;
        mLoops[LOOP_WIND_STRONG].mOffset = upwind;
    }

    for (S32 i = 0; i < LOOP_COUNT; ++i)
    {
        mLoops[i].mTarget = llclamp(targets[i], 0.f, 1.f);
        applyLoop(mLoops[i], sources[i], master * category[i], dt);
    }

    updateBedVoices(now, dt, master * ambient_vol);
}

void SSSoundscape::updateBedVoices(F64 now, F32 dt, F32 master_mul)
{
    if (!gAudiop) return;

    // Targets for every live voice: the ladder's picks get theirs, everyone else fades to zero. A voice is only ever CREATED by the ladder; it dies by fading out.
    for (auto& pair : mBedVoices) pair.second.mTarget = 0.f;
    for (const auto& want : mLadderTargets)
    {
        mBedVoices[want.first].mTarget = llclamp(want.second, 0.f, 1.f);
    }

    // The studio fade: fast enough to feel the weather react, slow enough to be a mix move rather than a cut. ~0.7s full-swing.
    const F32 fade = llclamp(dt * 1.5f, 0.f, 1.f);

    for (auto it = mBedVoices.begin(); it != mBedVoices.end(); )
    {
        BedVoice& voice = it->second;
        voice.mGain = lerp(voice.mGain, voice.mTarget, fade);

        LLAudioSource* source = voice.mSourceID.notNull() ? gAudiop->findAudioSource(voice.mSourceID) : nullptr;

        if (voice.mTarget <= 0.001f && voice.mGain <= 0.005f)
        {
            // Faded away. Remember where the recording stopped so its next appearance RESUMES rather than replaying the authored fade-in from byte zero - the resets that were audible on every
            // ladder move under the old slot-override design.
            if (source)
            {
                U32 len = 0;
                if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(it->first)) len = meta->mLengthMS;
                if (len > 0)
                {
                    mBedResume[it->first] = (U32)((voice.mOffsetMS + (U64)((now - voice.mStartedAt) * 1000.0)) % len);
                }
                gAudiop->cleanupAudioSource(source);
            }
            it = mBedVoices.erase(it);
            continue;
        }

        if (!source && voice.mTarget > 0.001f)
        {
            voice.mSourceID.generate();
            voice.mStartedAt = now;
            auto resume = mBedResume.find(it->first);
            voice.mOffsetMS = (resume != mBedResume.end()) ? resume->second : 0;

            source = new LLAudioSource(voice.mSourceID, gAgent.getID(), 0.f, LLAudioEngine::AUDIO_TYPE_AMBIENT);
            source->setStartOffsetMS(voice.mOffsetMS);
            source->setLoop(true);
            source->setForcedPriority(true);
            source->setPositionGlobal(gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()));
            gAudiop->addAudioSource(source);
            source->play(it->first);
        }

        if (source)
        {
            source->setGain(llclamp(voice.mGain * master_mul, 0.f, 1.f));
            source->setPositionGlobal(gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()));
        }
        ++it;
    }
}

//-----------------------------------------------------------------------------
// Thunder
//-----------------------------------------------------------------------------
namespace
{
    // Speed of sound in the air the environment authored. Temperature moves it by about 0.6 m/s per degree - across the -30C blizzard to +40C desert an authorable sky spans, that is a tenth of the
    // figure, which over ten kilometres of thunder delay is a couple of counted seconds.
    F32 speed_of_sound_ms()
    {
        return 331.3f + 0.606f * SSAtmoMagic::getInstance()->temperatureC();
    }

    // Beyond this a clap is all rumble, below it all crack. Not a hard switch: the two packs cross-fade across the range, because a strike at the boundary is genuinely both.
    const F32 THUNDER_CRACK_M = 1500.f;
    const F32 THUNDER_RUMBLE_M = 6000.f;

    // Pick one at random from a comma separated setting.
    LLUUID pick_from_setting(const std::string& setting, SSRandStream& rng)
    {
        const std::string csv = gSavedSettings.getString(setting);
        if (csv.empty()) return LLUUID::null;

        std::vector<std::string> tokens;
        LLStringUtil::getTokens(csv, tokens, ",");

        std::vector<LLUUID> ids;
        for (const std::string& tok : tokens)
        {
            LLUUID id(tok);
            if (id.notNull()) ids.push_back(id);
        }
        if (ids.empty()) return LLUUID::null;
        return ids[rng.rand((S32)ids.size())];
    }

    // Where the bang sits inside the recording (leading air differs per asset, so the error was per-asset too). Engine analyses decoded PCM once (getOnsetMS); needs the buffer, which is why packs preload at schedule time.
    U32 sound_onset_ms(const LLUUID& id)
    {
        // The pre-analysis table first: for anything the pipeline has already walked, the answer is a lookup with no decode race at all. The buffer path below stays as the fallback for a sound
        // configured seconds ago.
        if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id))
        {
            return meta->mOnsetMS;
        }

        if (id.isNull() || !gAudiop) return 0;

        LLAudioData* data = gAudiop->getAudioData(id);
        if (!data || !data->hasDecodedData()) return 0;

        LLAudioBuffer* buffer = data->getBuffer();
        if (!buffer)
        {
            gAudiop->updateBufferForData(data, id);
            buffer = data->getBuffer();
        }
        return buffer ? buffer->getOnsetMS() : 0;
    }
}

F32 SSSoundscape::skyOcclusion() const
{
    // Cover is the roof over the camera; burial is how much MORE build stands between that ceiling and the open sky. Together: a porch muffles thunder a little, a basement almost entirely.
    return llclamp(mCoverSmooth * 0.55f + burialOcclusion() * 0.45f, 0.f, 1.f);
}

// A positioned one-shot with occlusion - what gAudiop->triggerSound cannot express, since it offers no handle to set anything on the source it creates. The engine reaps the source when the
// sound finishes, same as the sequence loops rely on.
static LLUUID ss_play_oneshot(const LLUUID& sound, const LLVector3d& pos_global, F32 gain, F32 occlusion)
{
    if (!gAudiop || sound.isNull()) return LLUUID::null;

    const LLUUID id = LLUUID::generateNewID();
    LLAudioSource* source = new LLAudioSource(id, gAgent.getID(),
                                              llclamp(gain, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_AMBIENT);
    source->setPositionGlobal(pos_global);
    source->setOcclusion(occlusion);
    gAudiop->addAudioSource(source);
    source->play(sound);
    return id;
}

void SSSoundscape::registerFollower(const LLUUID& source_id, const LLVector3& dir_world, F64 now)
{
    if (source_id.isNull()) return;
    OneShotFollower f;
    f.mSourceID = source_id;
    f.mDir = dir_world;
    f.mLastPos = LLViewerCamera::getInstance()->getOrigin() + dir_world * 12.f;
    f.mExpires = now + 120.0;
    mFollowers.push_back(f);
}

void SSSoundscape::updateFollowers(F64 now, F32 dt)
{
    if (!gAudiop) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    for (size_t i = 0; i < mFollowers.size(); )
    {
        OneShotFollower& f = mFollowers[i];
        LLAudioSource* source = gAudiop->findAudioSource(f.mSourceID);
        if (!source || now > f.mExpires)
        {
            mFollowers.erase(mFollowers.begin() + i);
            continue;
        }

        const LLVector3 pos = cam + f.mDir * 12.f;
        source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos));
        source->setVelocity((dt > 0.001f) ? (pos - f.mLastPos) / dt : LLVector3::zero);
        f.mLastPos = pos;
        ++i;
    }
}

void SSSoundscape::playCharge(const LLVector3& pos_agent, F32 intensity)
{
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!sounds || !gAudiop) return;

    SSRandStream rng((U32)(SSAtmoMagic::getInstance()->sharedTime() * 8171.0));
    const LLUUID sound = pick_from_setting("SSAtmoLightningCharge", rng);
    if (sound.isNull()) return;

    // Placed a few metres from the listener ALONG the direction of the strike, not at the strike: the engine's 3D rolloff silences anything hundreds of metres out, and distance is already
    // carried by our own gain model. Direction is what the placement is for - the ear still learns where it came from.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 dir = pos_agent - cam;
    const F32 d = dir.normalize();
    const LLVector3 near_pos = cam + dir * llmin(d, 12.f);
    registerFollower(ss_play_oneshot(sound, gAgent.getPosGlobalFromAgent(near_pos),
                                     llclamp(intensity, 0.f, 1.f), skyOcclusion()),
                     dir, SSAtmoMagic::getInstance()->sharedTime());
}

F32 SSSoundscape::windCarryGain(const LLVector3& source_pos_agent) const
{
    const LLVector3 listener = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 to_listener = listener - source_pos_agent;
    const F32 dist = to_listener.normalize();
    if (dist < 50.f) return 1.f;    // too close for refraction to matter

    LLVector3 wind = SSAtmoMagic::getInstance()->wind();
    const F32 speed = wind.normalize();
    if (speed < 0.5f) return 1.f;

    // +1 with the wind blowing from the source toward the listener.
    const F32 along = wind * to_listener;

    // Refraction accumulates over km and saturates with wind speed; floored well above zero - upwind thunder is muffled and shortened, never erased.
    const F32 range = llmin(dist / 3000.f, 1.f);
    const F32 strength = llmin(speed / 12.f, 1.f);
    return llclamp(1.f + along * range * strength * 0.8f, 0.25f, 1.6f);
}

// Crack or rumble by MEASURED brightness rather than by which pack a sound was filed in. Both packs pool; analysed candidates sort by crackiness, near picks draw from the bright half, far from
// the dark - a mixed dump of recordings sorts itself. Every honest failure falls back to the labelled packs: auto-sort off, fewer than four analysed, or material whose crackiness barely varies
// (one storm's takes, often - nothing real to sort on). Verify against your own uploads via the SSSoundMeta log lines: cracks should score high, long rolls low.
static LLUUID pick_thunder(bool want_rumble, SSRandStream& rng)
{
    static LLCachedControl<bool> auto_sort(gSavedSettings, "SSAtmoThunderAutoSort", true);
    const char* home = want_rumble ? "SSAtmoThunderRumble" : "SSAtmoThunderCrack";
    if (!auto_sort) return pick_from_setting(home, rng);

    std::vector<std::pair<F32, LLUUID>> rated;
    for (const char* setting : { "SSAtmoThunderCrack", "SSAtmoThunderRumble" })
    {
        std::vector<std::string> tokens;
        LLStringUtil::getTokens(gSavedSettings.getString(setting), tokens, ",");
        for (const std::string& tok : tokens)
        {
            LLUUID id(tok);
            if (id.isNull()) continue;
            if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(id))
            {
                rated.emplace_back(meta->mCrackiness, id);
            }
        }
    }

    if (rated.size() < 4) return pick_from_setting(home, rng);

    std::sort(rated.begin(), rated.end());
    if (rated.back().first - rated.front().first < 0.08f) return pick_from_setting(home, rng);

    const size_t half = rated.size() / 2;
    const size_t lo = want_rumble ? 0 : rated.size() - half;
    return rated[lo + (size_t)rng.rand((S32)half)].second;
}

void SSSoundscape::scheduleThunder(const LLVector3& pos_agent, F32 distance_m,
                                   F32 intensity, F64 fire_at, F32 muffle)
{
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!sounds || !gAudiop) return;
    muffle = llclamp(muffle, 0.f, 1.f);

    SSRandStream rng((U32)(fire_at * 6151.0) ^ (U32)distance_m);

    // The delay everyone knows: light is instant, sound is not. Roughly three seconds per kilometre, which is the rule of thumb people count out loud, arrived at here from the actual speed rather
    // than from the rule. fire_at is usually in the future - a strike is prepared before it happens - which is what lets a near clap start its own run-up before the flash rather than being clipped
    // into.
    const F64 travel = (F64)(distance_m / speed_of_sound_ms());
    const F64 heard_at = fire_at + travel;

    // Crack and rumble LAYERED per strike, not near/far variants: thunder comes from the whole km-long channel at once - the near end is the crack, everything behind keeps arriving as the roll, and air absorption (~f^2) kills the crack over distance. doc/atmo_magic_lightning.md#thunder-acoustics.
    // Cloud burial kills the crack the same way distance does - the high frequencies are exactly what a few hundred metres of droplets absorb - which is why intra-cloud lightning is a rumble
    // however close it is.
    const F32 crack_gain = (1.f - llclamp(
        (distance_m - THUNDER_CRACK_M) / (THUNDER_RUMBLE_M - THUNDER_CRACK_M), 0.f, 1.f))
        * (1.f - muffle);

    // Distance attenuation on top of the 3D falloff the audio engine already applies, since the source is placed at the strike and a strike can be ten kilometres off - well past anything the
    // engine's rolloff was ever tuned for. A buried strike is quieter overall as well as duller.
    const F32 fade = 1.f / (1.f + (distance_m / 3000.f));
    const F32 gain = llclamp(intensity * fade * windCarryGain(pos_agent), 0.f, 1.f)
                   * (1.f - 0.45f * muffle);

    if (crack_gain > 0.02f)
    {
        queueThunder(pick_thunder(false, rng),
                     pos_agent, distance_m, gain * crack_gain, heard_at, muffle);
    }

    // The roll comes in behind the crack, by however long the channel takes to finish arriving. A rough stand-in for the channel's own depth: several kilometres of it, so several seconds, and more
    // of it for a fiercer strike. This one number is what makes near thunder a crack with a tail and distant thunder a roll on its own. Burial SHORTENS it: the far reaches of a buried channel are
    // behind even more cloud than its near point, so their share of the roll never arrives - short muffled rumble, not a long peal.
    const F64 spread = (F64)(rng.frand(2000.f, 5000.f) * (0.6f + intensity * 0.7f)
                             / speed_of_sound_ms()) * (F64)(1.f - 0.45f * muffle);

    queueThunder(pick_thunder(true, rng),
                 pos_agent, distance_m, gain * (0.5f + 0.5f * (1.f - crack_gain)),
                 heard_at + spread * (F64)(1.f - crack_gain), muffle);
}

void SSSoundscape::queueThunder(const LLUUID& sound, const LLVector3& pos_agent,
                                F32 distance_m, F32 gain, F64 heard_at, F32 muffle)
{
    if (sound.isNull() || gain <= 0.f) return;

    // Fetched the moment it is queued rather than when it plays. Measuring where the bang sits inside it needs a decoded buffer, and the whole reason a strike is prepared ahead of time is to give
    // that fetch room.
    gAudiop->preloadSound(sound);

    PendingThunder pending;
    pending.mPos = pos_agent;
    pending.mDistanceM = distance_m;
    pending.mMuffle = muffle;
    pending.mSound = sound;
    pending.mGain = gain;
    pending.mHeardAt = heard_at;
    pending.mPlayAt = heard_at;      // moved earlier once the onset is known

    mThunder.push_back(pending);
}

void SSSoundscape::updateThunder(F64 now)
{
    for (size_t i = 0; i < mThunder.size(); )
    {
        PendingThunder& p = mThunder[i];

        // The onset is resolved as late as possible, because the asset may still have been fetching when the strike happened. Once known it moves the start time EARLIER by that much, so the bang
        // itself lands where the physics put it rather than the file's first sample landing there.
        if (!p.mAligned)
        {
            // Resolved as late as it can be, because the asset may still have been fetching when the strike was prepared - and as early as it must be, because the answer moves the start time earlier
            // and a start time already passed cannot be honoured.
            const U32 onset = sound_onset_ms(p.mSound);
            if (onset > 0)
            {
                p.mPlayAt = p.mHeardAt - (F64)onset / 1000.0;
                p.mAligned = true;
            }
            else if (now >= p.mHeardAt - 0.05)
            {
                // Out of time to find out. The asset never decoded, or has no clear onset; play it as it is rather than hold it.
                p.mAligned = true;
            }
        }

        if (now < p.mPlayAt) { ++i; continue; }

        // Level the pack: assets uploaded from different sources sit many dB apart, and the crack/rumble balance assumes comparable mastering. Scaled toward a reference by the loudest-second RMS
        // the onset analysis already measured, clamped so a quiet recording's noise floor is never dragged up. When the buffer never decoded the level reads 0 and the gain passes through untouched.
        F32 gain = p.mGain;
        {
            const F32 REF_LEVEL = 0.22f;
            F32 level = 0.f;
            if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(p.mSound))
            {
                level = meta->mPeakLevel;
            }
            else if (gAudiop)
            {
                if (LLAudioData* data = gAudiop->getAudioData(p.mSound))
                {
                    if (LLAudioBuffer* buffer = data->getBuffer()) level = buffer->getPeakLevel();
                }
            }
            if (level > 0.001f) gain *= llclamp(REF_LEVEL / level, 0.5f, 2.f);
        }

        // Thunder's own trim, deliberately ABOVE 1 by default: the distance fade and the leveller both only ever pull the gain down, so a typical clap was landing at a fraction of full scale
        // and thunder read as polite. The boost spends that headroom; ss_play_oneshot still clamps the final figure to 1, so a point-blank strike cannot overdrive.
        static LLCachedControl<F32> thunder_vol(gSavedSettings, "SSAtmoVolumeThunder", 2.5f);
        gain *= llclamp((F32)thunder_vol, 0.f, 4.f);

        // Thunder wears the listener's sky occlusion as a lowpass: through a roof the crack dulls to a rumble - the gain barely moves, the timbre does. And like the charge, the source sits a
        // few metres away ALONG the strike's bearing rather than AT the strike [interaction: engine 3D rolloff]: distance lives in our gain model, the placement only carries direction.
        {
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
            LLVector3 dir = p.mPos - cam;
            const F32 d = dir.normalize();
            const LLVector3 near_pos = cam + dir * llmin(d, 12.f);

            // Cloud burial stacks onto the listener's own roof burial as lowpass - both are stuff between the discharge and the ear, and the occlusion channel is the one lever the engine dulls by.
            const F32 occ = llclamp(skyOcclusion() + p.mMuffle * 0.55f, 0.f, 1.f);
            registerFollower(ss_play_oneshot(p.mSound, gAgent.getPosGlobalFromAgent(near_pos), gain, occ),
                             dir, now);
        }

        mThunder.erase(mThunder.begin() + i);
    }
}

//-----------------------------------------------------------------------------
// Footsteps
//-----------------------------------------------------------------------------
// One tilted up-ray over an avatar, cached - see AvatarCover. Tilted like the camera probe's rays and for the same reason: a lone beam or a narrow gap directly overhead should not decide it.
bool SSSoundscape::roofOver(const LLUUID& avatar_id, const LLVector3& pos_agent, bool is_self)
{
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    AvatarCover& cover = mAvatarCover[avatar_id];

    // Refresh only when they have actually gone somewhere AND their share of ray budget has come round - near avatars get answers within a second, far ones can wait several.
    const F32 moved = (pos_agent - cover.mPos).magVec();
    // Your own avatar always gets the fastest cadence: camming away from yourself makes cam_dist large, and your own footsteps going stale-indoors for five seconds is exactly the case this
    // exists to fix.
    const F32 cam_dist = (pos_agent - LLViewerCamera::getInstance()->getOrigin()).magVec();
    const F64 interval = is_self ? 1.0 : 1.0 + (F64)llclamp(cam_dist / 24.f, 0.f, 4.f);

    if (cover.mWhen < 0.0 || (moved > 1.5f && now - cover.mWhen > interval))
    {
        cover.mPos = pos_agent;
        cover.mWhen = now;

        const LLVector3 start = pos_agent + LLVector3(0.f, 0.f, 2.2f);
        const LLVector3 dir(0.12f, 0.09f, 0.99f);
        LLVector4a start4, end4, intersect;
        start4.load3(start.mV);
        const LLVector3 end = start + dir * 50.f;
        end4.load3(end.mV);
        cover.mIndoors = gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true);

        // A crowd that left the region should not be remembered forever.
        if (mAvatarCover.size() > 64)
        {
            for (auto it = mAvatarCover.begin(); it != mAvatarCover.end(); )
            {
                it = (now - it->second.mWhen > 60.0) ? mAvatarCover.erase(it) : ++it;
            }
        }
    }

    return cover.mIndoors;
}

bool SSSoundscape::onObject(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self)
{
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    AvatarCover& cover = mAvatarCover[avatar_id];

    // Kerb-scale cadence, faster than the roof ray's: a street edge is crossed in a stride. Distance still buys other avatars slack.
    const F32 moved = (foot_pos_agent - cover.mFootPos).magVec();
    const F32 cam_dist = (foot_pos_agent - LLViewerCamera::getInstance()->getOrigin()).magVec();
    const F64 interval = is_self ? 0.3 : 0.3 + (F64)llclamp(cam_dist / 24.f, 0.f, 3.f);

    if (cover.mFootWhen < 0.0 || (moved > 0.75f && now - cover.mFootWhen > interval))
    {
        cover.mFootPos = foot_pos_agent;
        cover.mFootWhen = now;

        // Down through the feet: start above the ankles, end far enough below to survive a bouncy walk animation. The intersect helper tests terrain too and RETURNS what it hit, so terrain
        // underfoot is recognised as terrain rather than read as a miss - the distinction the dead stock resolver was supposed to make. pick_transparent on: invisible prims as walkway surfaces
        // are a decades-old SL building tradition.
        const LLVector3 start = foot_pos_agent + LLVector3(0.f, 0.f, 0.5f);
        const LLVector3 end = foot_pos_agent - LLVector3(0.f, 0.f, 1.2f);
        LLVector4a start4, end4, intersect;
        start4.load3(start.mV);
        end4.load3(end.mV);

        LLDrawable* hit = gPipeline.lineSegmentIntersectWorldGeometry(start4, end4, &intersect, true, true);
        LLViewerObject* vo = hit ? hit->getVObj().get() : nullptr;
        cover.mOnObject = vo && vo->getPCode() != LLViewerObject::LL_VO_SURFACE_PATCH;
    }

    return cover.mOnObject;
}

void SSSoundscape::fadeKill(const LLUUID& source_id)
{
    if (!gAudiop || source_id.isNull()) return;
    if (LLAudioSource* source = gAudiop->findAudioSource(source_id))
    {
        source->setGain(0.f);
        mDying.emplace_back(source_id, SSAtmoMagic::getInstance()->sharedTime() + 0.06);
    }
}

void SSSoundscape::updateDying(F64 now)
{
    if (!gAudiop) return;
    for (size_t i = 0; i < mDying.size(); )
    {
        if (now >= mDying[i].second)
        {
            if (LLAudioSource* source = gAudiop->findAudioSource(mDying[i].first))
            {
                gAudiop->cleanupAudioSource(source);
            }
            mDying.erase(mDying.begin() + i);
        }
        else ++i;
    }
}

void SSSoundscape::releaseStepLoop(StepLoop& loop)
{
    if (gAudiop && loop.mSourceID.notNull())
    {
        fadeKill(loop.mSourceID);
    }
    loop.mSourceID.setNull();
    loop.mSound.setNull();
    loop.mSurface = -1;
    loop.mAction = -1;
}

void SSSoundscape::reapStepLoops(F64 now)
{
    for (auto it = mStepLoops.begin(); it != mStepLoops.end(); )
    {
        // An avatar that stopped reporting (left range, logged off, or simply stood still long enough that its updates stopped mattering) takes its loop with it.
        if (now - it->second.mLastSeen > 0.5)
        {
            releaseStepLoop(it->second);
            it = mStepLoops.erase(it);
        }
        else ++it;
    }
}

void SSSoundscape::updateFootstepLoop(const LLUUID& avatar_id, const LLVector3& pos_agent,
                                      bool on_land, S32 locomotion, bool is_self)
{
    if (!gAudiop) return;

    const F64 now = SSAtmoMagic::getInstance()->sharedTime();

    if (locomotion != STEP_WALK && STEP_RUN != locomotion)
    {
        // Stopped (or airborne, or sitting). Not cut dead mid-splat though: the analysis knows where each step lands inside the recording, so the loop is allowed to finish the step it is in and
        // dies at the gap after it - the midpoint before the NEXT onset - capped at 400ms so a stop never audibly lags the avatar. No analysis yet, or a sparse recording: immediate stop, exactly
        // the old behaviour.
        auto it = mStepLoops.find(avatar_id);
        if (it == mStepLoops.end()) return;
        StepLoop& loop = it->second;
        loop.mLastSeen = now;

        if (loop.mStopAt <= 0.0)
        {
            F64 wait = 0.0;
            const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(loop.mSound);
            if (meta && meta->mOnsets.size() >= 2 && meta->mLengthMS > 0)
            {
                const F64 pos_ms = fmod((F64)loop.mOffsetMS + (now - loop.mStartedAt) * 1000.0, (F64)meta->mLengthMS);
                F64 cut_ms = -1.0;
                for (size_t k = 0; k + 1 < meta->mOnsets.size(); ++k)
                {
                    // 2/3 of the way to the next onset, NOT the midpoint: the inter-step gap is asymmetric - the front of it is still the previous step's reverb tail decaying, and the true
                    // quiet sits late, just before the next step. A midpoint cut lands inside the tail.
                    const F64 gap = (F64)meta->mOnsets[k] + ((F64)meta->mOnsets[k + 1] - (F64)meta->mOnsets[k]) * 0.66;
                    if (gap > pos_ms) { cut_ms = gap; break; }
                }
                if (cut_ms < 0.0) cut_ms = (F64)meta->mTailMS;    // past the last onset: die at content end
                wait = llclamp((cut_ms - pos_ms) / 1000.0, 0.0, 0.4);
            }
            loop.mStopAt = now + wait;
        }

        if (now >= loop.mStopAt)
        {
            releaseStepLoop(loop);
            mStepLoops.erase(it);
        }
        else if (gAudiop)
        {
            // Still finishing its step - and still on the avatar's shoulder while it does.
            if (LLAudioSource* source = gAudiop->findAudioSource(loop.mSourceID))
            {
                source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
            }
        }
        return;
    }

    StepLoop& loop = mStepLoops[avatar_id];
    loop.mLastSeen = now;
    loop.mStopAt = 0.0;    // moving again cancels a pending gap-stop - same state resumes the same loop mid-word

    // Surface is re-resolved every frame (doorways, wet ground switch the state mid-stride), but the SOUND pick is only re-rolled when the state actually changes. footstepSound rolls randomly
    // from the slot each call, and using every frame's roll made a mixed slot flip between segment and loop mode at frame rate - brief loop bursts stomping over impact steps, which is what
    // "per-impact is messed up" turned out to be.
    LLUUID rolled;
    S32 surface = -1;
    {
        rolled = footstepSound(avatar_id, pos_agent, on_land, locomotion, is_self);
        const StepDebug& dbg = is_self ? mStepSelf : mStepOther;
        surface = dbg.mSurface;
    }

    const bool state_changed = (surface != loop.mSurface) || (locomotion != loop.mAction);
    const LLUUID sound = (state_changed || loop.mSound.isNull()) ? rolled : loop.mSound;

    // Segment mode: the analysis says this recording's steps cut apart cleanly (real silence between onsets), so footfalls drive windowed single steps and NO loop plays. The gate is the gap
    // floor - wash or reverb between steps keeps the whole-loop model, which is the honest one for such material.
    if (!sound.isNull())
    {
        if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(sound))
        {
            // Three-part verdict: enough steps to pick from, quiet gaps (the cuts are clean), and REGULAR cadence (the map is actually steps - a splashy recording whose detector confused puddle
            // noise with footfalls has ragged intervals and stays honestly in loop mode). Rate bounds catch maps counting things no gait produces.
            const bool segmentable = meta->mOnsets.size() >= 4
                && meta->mGapFloor < 0.12f
                && meta->mCadenceCV < 0.4f
                && meta->mImpactRate > 0.8f && meta->mImpactRate < 4.5f;
            if (segmentable)
            {
                if (LLAudioSource* old_src = loop.mSourceID.notNull() ? gAudiop->findAudioSource(loop.mSourceID) : nullptr)
                {
                    fadeKill(loop.mSourceID);
                    loop.mSourceID.setNull();
                }
                loop.mSurface = surface;
                loop.mAction = locomotion;
                loop.mSound = sound;
                loop.mSegmented = true;
                loop.mStopAt = 0.0;
                (is_self ? mStepSelf : mStepOther).mMode = 'S';
                return;
            }
        }
    }
    loop.mSegmented = false;
    (is_self ? mStepSelf : mStepOther).mMode = 'L';

    LLAudioSource* source = loop.mSourceID.notNull() ? gAudiop->findAudioSource(loop.mSourceID) : nullptr;

    // Churn guard: a slot whose asset has not decoded yet gets its source reaped by the engine, and recreating per frame is a 60Hz allocation treadmill. Once a second is plenty to catch the decode.
    if (!state_changed && !source && now - loop.mStartedAt < 1.0) return;

    if (state_changed || !source)
    {
        // New state, new loop - the sound picked above is this state's voice until the state changes again. An empty slot means this state is deliberately silent.
        releaseStepLoop(loop);
        loop.mSurface = surface;
        loop.mAction = locomotion;
        loop.mLastSeen = now;

        if (sound.isNull()) return;

        static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);
        loop.mSourceID.generate();
        loop.mSound = sound;
        loop.mStartedAt = now;

        // Start at a RANDOM between-steps gap, not the top of the file: with the step map analysed, every walk begins on a fresh step - the loop format's answer to the variety a one-shot pack
        // gets by picking. Enter and exit both happen in the silence between steps now, never mid-splat.
        loop.mOffsetMS = 0;
        if (const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(sound))
        {
            if (meta->mOnsets.size() >= 2)
            {
                // Same 2/3-of-gap point the stops use: entering at the midpoint dropped the listener into the previous step's reverb tail mid-decay, which reads as a blip before the first real step.
                const size_t k = (size_t)ll_rand((S32)meta->mOnsets.size() - 1);
                loop.mOffsetMS = meta->mOnsets[k] + (U32)((meta->mOnsets[k + 1] - meta->mOnsets[k]) * 2 / 3);
            }
        }

        source = new LLAudioSource(loop.mSourceID, avatar_id,
                                   llclamp((F32)vol, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_AMBIENT);
        source->setStartOffsetMS(loop.mOffsetMS);
        source->setLoop(true);
        source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
        source->setOcclusion(0.f);
        gAudiop->addAudioSource(source);
        source->play(sound);
        gAudiop->preloadSound(sound);
        markStepSource(loop.mSourceID);
    }

    // Attached: the loop is wherever the avatar is, every frame. This is the "following" half of the model.
    if (source)
    {
        source->setPositionGlobal(gAgent.getPosGlobalFromAgent(pos_agent));
    }
}

void SSSoundscape::footstepImpact(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self)
{
    auto it = mStepLoops.find(avatar_id);
    if (it == mStepLoops.end() || !it->second.mSegmented || it->second.mSound.isNull() || !gAudiop) return;

    const SSSoundMeta::Meta* meta = SSSoundMeta::getInstance()->get(it->second.mSound);
    if (!meta || meta->mOnsets.size() < 4 || meta->mLengthMS == 0) return;

    // Cadence ceiling. The ankle-swap detector is a zero-crossing test and chatters when the two heights run near-equal - turning in place, stair lips, certain anims - firing "footfalls" no gait
    // could produce. A run peaks around 3.5 steps/s and a walk around 2.5, so anything faster than the refractory is detector noise, not feet.
    const F64 now = SSAtmoMagic::getInstance()->sharedTime();
    const F64 min_gap = (it->second.mAction == STEP_RUN) ? 0.24 : 0.34;
    if (now - it->second.mLastImpactAt < min_gap) return;
    it->second.mLastImpactAt = now;

    // A random step from the map, windowed: begin 60ms before its onset (the foot's own approach), cut in the gap before the NEXT onset. The analysis put real silence there, so the hard cut is
    // inaudible by construction.
    const size_t k = (size_t)ll_rand((S32)meta->mOnsets.size() - 1);

    // Start stays a tight 60ms pre-roll - the segment fires ON the footfall, and every ms before the onset is latency between the visual step and its sound, so the start cannot afford to chase
    // tail purity the way the CUT can. The cut moves to the 2/3 point: the step keeps its own reverb tail (the two-thirds of the gap that belongs to it) and dies in the true quiet before the
    // next recorded step.
    const U32 start = (meta->mOnsets[k] > 60) ? meta->mOnsets[k] - 60 : 0;
    const U32 cut = meta->mOnsets[k] + (meta->mOnsets[k + 1] - meta->mOnsets[k]) * 2 / 3;
    const F64 window_s = llclamp((F64)(cut - start) / 1000.0, 0.1, 0.9);

    static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);

    // One sounding step per avatar: the new footfall cuts the previous one's window short (its transient masks the cut), so consecutive impacts can never stack whole steps however long the
    // recording's own inter-step gaps are.
    fadeKill(it->second.mSegSourceID);

    const LLUUID id = LLUUID::generateNewID();
    it->second.mSegSourceID = id;
    LLAudioSource* source = new LLAudioSource(id, gAgent.getID(),
                                              llclamp((F32)vol, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_AMBIENT);
    source->setStartOffsetMS(start);
    source->setPositionGlobal(gAgent.getPosGlobalFromAgent(foot_pos_agent));
    gAudiop->addAudioSource(source);
    source->play(it->second.mSound);
    markStepSource(id);

    mSegmentStops.emplace_back(id, SSAtmoMagic::getInstance()->sharedTime() + window_s);
}

void SSSoundscape::updateSegmentStops(F64 now)
{
    if (!gAudiop) return;
    for (size_t i = 0; i < mSegmentStops.size(); )
    {
        if (now >= mSegmentStops[i].second)
        {
            fadeKill(mSegmentStops[i].first);
            mSegmentStops.erase(mSegmentStops.begin() + i);
        }
        else ++i;
    }
}

void SSSoundscape::footstepEvent(const LLUUID& avatar_id, const LLVector3& pos_agent,
                                 bool on_land, S32 action, bool is_self)
{
    const LLUUID sound = footstepSound(avatar_id, pos_agent, on_land, action, is_self);
    if (sound.isNull() || !gAudiop) return;

    static LLCachedControl<F32> vol(gSavedSettings, "SSAtmoVolumeFootsteps", 0.5f);

    // Jump travels with the avatar in spirit but is over in a moment; Land is genuinely detached - it belongs to the spot the landing happened and stays there.
    markStepSource(ss_play_oneshot(sound, gAgent.getPosGlobalFromAgent(pos_agent),
                                   llclamp((F32)vol, 0.f, 1.f), 0.f));
}

void SSSoundscape::markStepSource(const LLUUID& source_id)
{
    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugFootstepMarkers", false);
    if (!markers || source_id.isNull()) return;

    StepMark mark;
    mark.mSourceID = source_id;
    mark.mStart = SSAtmoMagic::getInstance()->sharedTime();
    mark.mText = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
    if (mark.mText)
    {
        mark.mText->setDoFade(false);
        mark.mText->setZCompare(false);    // through walls, same as the strike countdown - a marker you cannot find marks nothing
    }
    mStepMarks.push_back(mark);
}

void SSSoundscape::updateStepMarks(F64 now)
{
    for (size_t i = 0; i < mStepMarks.size(); )
    {
        StepMark& mark = mStepMarks[i];
        LLAudioSource* source = gAudiop ? gAudiop->findAudioSource(mark.mSourceID) : nullptr;
        if (!source)
        {
            // The source stopped (or was fade-killed): the tag vanishing IS the stop-timing readout.
            if (mark.mText) mark.mText->markDead();
            mStepMarks.erase(mStepMarks.begin() + i);
            continue;
        }
        if (mark.mText)
        {
            // Rides the source's own position, so an attached loop visibly follows its avatar while a detached land/segment stays planted where it played.
            const LLVector3 pos = gAgent.getPosAgentFromGlobal(source->getPositionGlobal());
            mark.mText->setPositionAgent(pos + LLVector3(0.f, 0.f, 0.3f));

            // The two phases the request named: a very short highly visible birth, then a subtle presence for as long as it keeps playing.
            const bool fresh = now - mark.mStart < 0.25;
            mark.mText->setString(fresh ? std::string(">> STEP <<") : std::string("."));
            mark.mText->setColor(fresh ? LLColor4(1.f, 0.95f, 0.1f, 1.f)
                                       : LLColor4(0.45f, 0.95f, 0.55f, 0.5f));
        }
        ++i;
    }
}

LLUUID SSSoundscape::footstepSound(const LLUUID& avatar_id, const LLVector3& foot_pos_agent,
                                   bool on_land, S32 action, bool is_self)
{
    StepDebug& dbg = is_self ? mStepSelf : mStepOther;
    dbg = StepDebug();
    dbg.mWhen = SSAtmoMagic::getInstance()->sharedTime();
    dbg.mAction = action;
    // Switched on, not running: isEnabled() is false whenever no weather track is active, and dry ground is exactly the surface you walk on when none is. Gating these on it silenced every footstep
    // in fair weather - which is all of them, most of the time.
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!SSAtmoMagic::getInstance()->isSwitchedOn() || !sounds)
    {
        dbg.mWhyNot = SSAtmoMagic::getInstance()->isSwitchedOn()
            ? "SSAtmoSounds off" : "Atmo Magic off";
        return LLUUID::null;
    }

    // Indoors: the wind flowmap already builds a topdown capture of the topmost surface in every column (roof, floor slab, or open ground) to drive the rain sound's burial depth. Reusing it here is
    // one grid lookup, no raycast, and - unlike the camera-anchored cover probe the rain sound uses - it is evaluated at each avatar's own feet, so a crowd standing half in and half out of a doorway
    // reads correctly instead of everyone sharing the camera's verdict.
    F32 column_top = 0.f;
    const bool flow_knows = SSWindFlowMap::getInstance()->surfaceAt(foot_pos_agent, column_top);
    bool indoors = flow_knows && (column_top - foot_pos_agent.mV[VZ] > 0.75f);
    dbg.mIndoorsFrom = flow_knows ? 'f' : '-';

    // The flowmap only knows regions it has solved a tile for, and a silent flowmap used to read as "outdoors" - your own avatar standing in a roofed room picked terrain_dry while the audio probe
    // two lines up in the overlay said ROOFED. For yourself the camera cover probe IS at your position, so borrow its verdict when the flowmap has none. Other avatars keep the outdoor default; the
    // camera's roof says nothing about theirs.
    if (!flow_knows)
    {
        // The avatar's OWN cached up-ray, self included: the camera cover probe was tried for self first, but camera != avatar - cammed across the street, its roof says nothing about yours. One
        // mechanism for everyone, at the avatar's actual feet.
        if (roofOver(avatar_id, foot_pos_agent, is_self))
        {
            indoors = true;
            dbg.mIndoorsFrom = 'r';
        }
    }

    dbg.mIndoors = indoors;

    SSStepSurface surface;
    if (indoors)
    {
        surface = STEP_INSIDE_DRY;
    }
    else
    {
        const SSSurfaceField::Sample wet = SSSurfaceField::instance().sample(foot_pos_agent);
        const bool puddle = wet.mValid && wet.mPuddle > 0.005f;
        const bool damp = wet.mValid && wet.mWet > 0.3f;

        dbg.mFieldValid = wet.mValid;
        dbg.mWet = wet.mWet;
        dbg.mPuddle = wet.mPuddle;

        // NOT on_land: mStepOnLand upstream comes from the stock resolver whose object out-param has never been assigned (see onObject), so it is permanently "land" and every street prim, deck
        // and skybox floor outdoors read as terrain. Our own cached foot ray answers what is actually underfoot.
        if (on_land && !onObject(avatar_id, foot_pos_agent, is_self))
        {
            surface = puddle ? STEP_TERRAIN_PUDDLE : (damp ? STEP_TERRAIN_WET : STEP_TERRAIN_DRY);
        }
        else
        {
            surface = puddle ? STEP_OUTSIDE_PUDDLE : (damp ? STEP_OUTSIDE_WET : STEP_OUTSIDE_DRY);
        }
    }

    // Dry ground comes from the global settings, everything else from the preset - see SSFootstepSounds::surfaceIsGlobal. Asked here rather than resolved earlier because this is the only place that
    // knows which surface was decided on.
    dbg.mSurface = surface;
    dbg.mGlobal = SSFootstepSounds::surfaceIsGlobal(surface);

    std::string csv;
    if (dbg.mGlobal)
    {
        dbg.mSource = SSFootstepSounds::globalSettingName(surface, (SSStepAction)action);
        csv = gSavedSettings.getString(dbg.mSource);
    }
    else
    {
        dbg.mSource = std::string(SSPrecipPresetManager::instance().active().mName)
            + "/" + SSFootstepSounds::surfaceKey(surface)
            + "_" + SSFootstepSounds::actionKey((SSStepAction)action);
        csv = SSPrecipPresetManager::instance().active().mFootsteps.at(surface, (SSStepAction)action);
    }

    if (csv.empty())
    {
        dbg.mWhyNot = "slot empty";
        return LLUUID::null;
    }

    std::vector<std::string> tokens;
    LLStringUtil::getTokens(csv, tokens, ",");
    std::vector<LLUUID> ids;
    ids.reserve(tokens.size());
    for (const std::string& tok : tokens)
    {
        LLUUID id(tok);
        if (id.notNull()) ids.push_back(id);
    }
    dbg.mListSize = (S32)ids.size();
    if (ids.empty())
    {
        dbg.mWhyNot = "no valid UUIDs";
        return LLUUID::null;
    }

    // A different drop each step, the way the ambient sequences avoid an audible repeat - but picked at random rather than walked in order, since footsteps fire far too quickly for a long sequence
    // to matter.
    dbg.mPicked = ids[ll_rand((S32)ids.size())];
    return dbg.mPicked;
}

void SSSoundscape::idle()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_AUDIO);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const F64 now = atmo->sharedTime();
    F32 dt = (mLastIdle > 0.0) ? (F32)(now - mLastIdle) : 0.f;
    dt = llclamp(dt, 0.f, 0.25f);
    mLastIdle = now;

    // The analysis pipeline: walks every configured sound through decode and the worker pool while nothing needs it, so the moment something does, the answer is already in the table.
    SSSoundMeta::getInstance()->idle();

    reapStepLoops(now);
    updateFollowers(now, dt);
    updateSegmentStops(now);
    updateStepMarks(now);
    updateDying(now);

    // Before the gate below: a clap already on its way must still arrive. The strike happened - the sky clearing in the eight seconds since does not un-happen it, and swallowing the sound is a worse
    // artefact than hearing thunder from a sky that has moved on.
    updateThunder(now);

    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!atmo->isEnabled() || !sounds || !gAudiop)
    {
        stopAll();
        return;
    }

    // Impact-rate EMA decay
    if (dt > 0.f)
    {
        mImpactRate *= expf(-dt / IMPACT_RATE_TAU);
    }

    updateProbes(now);
    updateLoops(now, dt);
}

// </SS:Nexii>
