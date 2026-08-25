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

    // Precipitation beds come from the preset pack; wind is global
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

void SSSoundscape::playCharge(const LLVector3& pos_agent, F32 intensity)
{
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!sounds || !gAudiop) return;

    SSRandStream rng((U32)(SSAtmoMagic::getInstance()->sharedTime() * 8171.0));
    const LLUUID sound = pick_from_setting("SSAtmoLightningCharge", rng);
    if (sound.isNull()) return;

    gAudiop->triggerSound(sound, gAgentID, llclamp(intensity, 0.f, 1.f),
                          LLAudioEngine::AUDIO_TYPE_AMBIENT,
                          gAgent.getPosGlobalFromAgent(pos_agent));
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

void SSSoundscape::scheduleThunder(const LLVector3& pos_agent, F32 distance_m,
                                   F32 intensity, F64 fire_at)
{
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    if (!sounds || !gAudiop) return;

    SSRandStream rng((U32)(fire_at * 6151.0) ^ (U32)distance_m);

    // The delay everyone knows: light is instant, sound is not. Roughly three seconds per kilometre, which is the rule of thumb people count out loud, arrived at here from the actual speed rather
    // than from the rule. fire_at is usually in the future - a strike is prepared before it happens - which is what lets a near clap start its own run-up before the flash rather than being clipped
    // into.
    const F64 travel = (F64)(distance_m / speed_of_sound_ms());
    const F64 heard_at = fire_at + travel;

    // Crack and rumble LAYERED per strike, not near/far variants: thunder comes from the whole km-long channel at once - the near end is the crack, everything behind keeps arriving as the roll, and air absorption (~f^2) kills the crack over distance. doc/atmo_magic_lightning.md#thunder-acoustics.
    const F32 crack_gain = 1.f - llclamp(
        (distance_m - THUNDER_CRACK_M) / (THUNDER_RUMBLE_M - THUNDER_CRACK_M), 0.f, 1.f);

    // Distance attenuation on top of the 3D falloff the audio engine already applies, since the source is placed at the strike and a strike can be ten kilometres off - well past anything the
    // engine's rolloff was ever tuned for.
    const F32 fade = 1.f / (1.f + (distance_m / 3000.f));
    const F32 gain = llclamp(intensity * fade * windCarryGain(pos_agent), 0.f, 1.f);

    if (crack_gain > 0.02f)
    {
        queueThunder(pick_from_setting("SSAtmoThunderCrack", rng),
                     pos_agent, distance_m, gain * crack_gain, heard_at);
    }

    // The roll comes in behind the crack, by however long the channel takes to finish arriving. A rough stand-in for the channel's own depth: several kilometres of it, so several seconds, and more
    // of it for a fiercer strike. This one number is what makes near thunder a crack with a tail and distant thunder a roll on its own.
    const F64 spread = (F64)(rng.frand(2000.f, 5000.f) * (0.6f + intensity * 0.7f)
                             / speed_of_sound_ms());

    queueThunder(pick_from_setting("SSAtmoThunderRumble", rng),
                 pos_agent, distance_m, gain * (0.5f + 0.5f * (1.f - crack_gain)),
                 heard_at + spread * (F64)(1.f - crack_gain));
}

void SSSoundscape::queueThunder(const LLUUID& sound, const LLVector3& pos_agent,
                                F32 distance_m, F32 gain, F64 heard_at)
{
    if (sound.isNull() || gain <= 0.f) return;

    // Fetched the moment it is queued rather than when it plays. Measuring where the bang sits inside it needs a decoded buffer, and the whole reason a strike is prepared ahead of time is to give
    // that fetch room.
    gAudiop->preloadSound(sound);

    PendingThunder pending;
    pending.mPos = pos_agent;
    pending.mDistanceM = distance_m;
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

        if (gAudiop)
        {
            gAudiop->triggerSound(p.mSound, gAgentID, gain,
                                  LLAudioEngine::AUDIO_TYPE_AMBIENT,
                                  gAgent.getPosGlobalFromAgent(p.mPos));
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

        if (on_land)
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
