/**
 * @file sssoundscape.h
 * @brief The Atmo Magic soundscape: everything the weather system makes
 *        audible. Layered ambient loops for rain and wind, driven by the
 *        live impact rate around the camera and by raycast probes that
 *        detect roof cover and how large the indoor space is, crossfading
 *        between outdoor beds and rain-on-roof beds per space size; and
 *        the surface-aware footstep voices, selected here so their gating
 *        lives with the rest of the audio rather than in the avatar.
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

#ifndef SS_SOUNDSCAPE_H
#define SS_SOUNDSCAPE_H

// <SS:Nexii> Atmo Magic soundscape

// Gate map: beds + in-flight thunder on isEnabled, footsteps on isSwitchedOn (dry ground plays exactly when no weather runs), all on SSAtmoSounds. Reads windflow (indoor/burial/stereo), surface field (footstep surface), temperature (sound speed). doc/atmo_magic_interactions.md

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <string>
#include <vector>

class LLAudioSource;
class LLHUDText;

class SSSoundscape : public LLSingleton<SSSoundscape>
{
    LLSINGLETON_EMPTY_CTOR(SSSoundscape);

public:
    // Per-frame driver, called from the manager's idle after parameters refresh; owns probe scheduling and loop gain fades
    void idle();

    // An impact landed near the camera (already distance-gated by the impact queue); feeds the local-loudness metric so sheltered spots sound quieter than open ground even at the same weather
    // parameters
    void notifyImpact(F32 strength);

    // Fade out and release every loop (system disabled, teleport, etc.)
    void stopAll();

    // The footstep voice for a foot at this position, from the surface underfoot - dry, wet, puddle, indoors - or null when the soundscape has nothing to say: slot unconfigured, or Atmo Magic off.
    // Null tells the avatar to fall back to the stock per-avatar step sound, so turning the system off returns the viewer to its vanilla footsteps rather than silence. action is an SSStepAction;
    // on_land whether the foot is on terrain rather than an object.
    LLUUID footstepSound(const LLUUID& avatar_id, const LLVector3& foot_pos_agent,
                         bool on_land, S32 action, bool is_self);

    // The footstep model: walk and run are LOOPS attached to the avatar, started/stopped/switched by state (surface x gait), following the avatar every frame. Jump is a one-shot at the avatar;
    // Land is the one detached one-shot, left at the spot it happened. Per-footfall triggering of loop-format recordings was the previous model, and it spammed a fresh copy of a long loop on
    // every footfall - overlapping, never stopping, and pinned where each step happened.
    //
    // Called every frame per nearby avatar. locomotion: 0 stopped, STEP_WALK or STEP_RUN. Surface changes (walking from terrain into a doorway) switch the loop mid-stride.
    void updateFootstepLoop(const LLUUID& avatar_id, const LLVector3& pos_agent,
                            bool on_land, S32 locomotion, bool is_self);

    // Jump (attached, at the avatar) and Land (detached, at the spot) one-shots.
    void footstepEvent(const LLUUID& avatar_id, const LLVector3& pos_agent,
                       bool on_land, S32 action, bool is_self);

    // A footfall happened (the support-foot swap). Consumed only when the avatar's current step state is in SEGMENT mode - a recording whose gap floor says its steps cut apart cleanly - in which
    // case one windowed step plays at the landing foot: seek to just before a random detected onset, stop in the gap after it. Loop-mode states ignore this entirely; the loop is already playing.
    void footstepImpact(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self);

    // What the last footstep lookup decided, for the info overlay. Every stage of the walk is recorded, including the ones that returned nothing, because "no sound played" has half a dozen causes
    // that are indistinguishable from a chair: system off, wrong surface picked, slot empty, list unparseable.
    struct StepDebug
    {
        F64 mWhen = -1.0;           // sharedTime of the lookup, -1 for never
        S32 mSurface = -1;          // SSStepSurface, or -1 if not reached
        S32 mAction = -1;
        bool mIndoors = false;
        char mIndoorsFrom = '-';    // 'f' flowmap column, 'p' camera cover probe fallback, '-' nothing answered (treated as outdoors)
        char mMode = '-';           // 'S' per-impact segments, 'L' attached loop, '-' no state yet - the overlay's answer to "which playback model is my avatar on right now"
        bool mFieldValid = false;   // the surface field had an answer
        F32 mWet = 0.f;
        F32 mPuddle = 0.f;
        bool mGlobal = false;       // read from settings rather than the preset
        std::string mSource;        // setting name or preset slot key
        S32 mListSize = 0;
        LLUUID mPicked;
        const char* mWhyNot = "";   // empty when a sound was returned
    };
    // Kept separately for your own avatar and for everyone else's. Footsteps fire for every avatar in earshot, so a single "last lookup" was overwritten by whoever stepped most recently - which in a
    // crowd is never you, and you are the one being debugged.
    const StepDebug& lastStep(bool self) const { return self ? mStepSelf : mStepOther; }

    // A strike happened at pos, this far away, this fierce. The clap is held and played when the sound would actually have arrived - the caller says WHEN IT STRUCK, not when it should be heard,
    // because only this side knows how long sound takes to cover the distance or how far into the chosen recording its own bang sits. muffle 0..1 is how buried the discharge is behind cloud
    // (the caller measures it against the actual puff field): a fully-buried intra-cloud strike loses its crack, plays quieter and shorter, and wears extra lowpass - the muffled rumble real
    // in-cloud lightning is, instead of a full-price bang for a flash you barely saw.
    void scheduleThunder(const LLVector3& pos_agent, F32 distance_m, F32 intensity,
                         F64 fire_at, F32 muffle = 0.f);

    // The crackle that gathers before a strike, for the anticipation effect. Played immediately - it is meant to be heard building, so there is nothing to delay.
    void playCharge(const LLVector3& pos_agent, F32 intensity);

    // Wind carry [interaction: wind -> audio]: downwind refraction bends sound to the ground (carries further), upwind away (the classic saw-the-flash-heard-nothing). Grows over km. General offer to everything the soundscape plays; thunder is the first consumer.
    F32 windCarryGain(const LLVector3& source_pos_agent) const;

    // How buried from the OPEN SKY the listener is, 0..1 - the occlusion every sky-borne sound (thunder, wind, distant weather) should wear as a lowpass. Owned here because occlusion is a
    // property of the listening situation, not of any one sound: one probe answer, every consumer agrees.
    F32 skyOcclusion() const;

    // How many claps are still on their way. For the info overlay: a sky that has gone quiet with four pending is a storm you are about to hear, not a bug.
    S32 pendingThunder() const { return (S32)mThunder.size(); }

    enum ESpace
    {
        SPACE_OUTDOOR = 0,
        SPACE_SHELTERED,    // roof overhead but open sides: porch, awning
        SPACE_SMALL,
        SPACE_MEDIUM,
        SPACE_BIG
    };

    // With nothing overhead, height stops mattering and the width of the surroundings takes over: an alley between two buildings is a different outdoor space from an open plain.
    enum ESize
    {
        SIZE_SMALL = 0,     // surfaces within 10m
        SIZE_MEDIUM,        // 10 - 30m
        SIZE_LARGE          // 30m and beyond
    };
    bool isCovered() const { return mCovered; }
    F32 coverage() const { return mCoverage; }   // fraction of roof samples that hit
    ESpace space() const { return mSpace; }

    // Readouts for the Atmo Magic info overlay
    static const char* spaceName(ESpace space);
    static const char* sizeName(ESize size);
    ESize outdoorSize() const { return mOutdoorSize; }

    // Rough occlusion from the bubble: a source further away than the surface in its direction is on the far side of that surface, so it is being heard through something. Costs no extra rays - it
    // reuses the width samples the cover probe already took.
    F32 occlusionGain(const LLVector3& source_pos) const;
    F32 wallDistanceToward(const LLVector3& dir_horizontal) const;

    // Schedules a delayed, quieter copy of a one-shot bounced off the nearest wall. Positioned at the reflecting surface, so the ear that hears it first falls out of normal 3D panning rather than a
    // channel delay.
    F32 impactRate() const;   // landings per second near the camera
    F32 coverBlend() const { return mCoverSmooth; }
    S32 wallCount() const { return mWallCount; }
    F32 wallDistance() const { return mWallAvg; }
    F32 roofDistance() const { return mRoofDist; }

    // How much build stands between the ceiling overhead and the open sky, in metres. The up ray finds the ceiling of the room you are in; the wind flowmap's overhead capture knows the top of the
    // whole column. The gap between the two is everything stacked above you - the storeys of a building, or the ground over a basement - and it is what separates standing in a ground floor room from
    // standing in a cellar under it.
    F32 burialDepth() const { return mBuriedSmooth; }

    // 0 at the ceiling, approaching 1 deep under the build. What the rain bed is attenuated by.
    F32 burialOcclusion() const;

    S32 activeLoops() const;
    F64 lastProbeAge() const;

private:
    StepDebug mStepSelf;
    StepDebug mStepOther;

    // A clap that has been fired but not yet heard.
    struct PendingThunder
    {
        F64 mHeardAt = 0.0;     // when the bang itself should land
        F64 mPlayAt = 0.0;      // when to start the file, so that it does
        LLVector3 mPos;
        F32 mGain = 1.f;
        F32 mDistanceM = 0.f;
        F32 mMuffle = 0.f;      // cloud burial at schedule time - worn as extra lowpass at play
        LLUUID mSound;
        bool mAligned = false;  // the onset has been measured, or given up on
    };
    std::vector<PendingThunder> mThunder;

    void queueThunder(const LLUUID& sound, const LLVector3& pos_agent,
                      F32 distance_m, F32 gain, F64 heard_at, F32 muffle);

    // Cached roof-over-head verdicts for OTHER avatars, one cheap up-ray each, refreshed only when the avatar has moved a couple of metres AND its distance-scaled interval has passed. The flowmap
    // answers first when it can; this covers regions it has no tile for. Keyed by avatar so a crowd costs a handful of rays a second at worst, not per footstep.
    struct AvatarCover
    {
        bool mIndoors = false;
        LLVector3 mPos;
        F64 mWhen = -1.0;

        // The underfoot verdict, cached on its own faster clock - surfaces change at street-kerb scale where roofs change at building scale.
        bool mOnObject = false;
        LLVector3 mFootPos;
        F64 mFootWhen = -1.0;
    };
    std::map<LLUUID, AvatarCover> mAvatarCover;
    bool roofOver(const LLUUID& avatar_id, const LLVector3& pos_agent, bool is_self);

    // Whether this avatar is standing on an OBJECT rather than on terrain: one short down-ray at the feet, cached per avatar like the roof ray. Exists because the stock resolver footsteps used to
    // trust (LLWorld::resolveStepHeightGlobal) initialises its object out-param to NULL and never assigns it - mStepOnLand has been permanently true for years, which made every outdoor step
    // "terrain" no matter what was actually underfoot.
    bool onObject(const LLUUID& avatar_id, const LLVector3& foot_pos_agent, bool is_self);

    // One managed looping source per moving avatar - see updateFootstepLoop.
    struct StepLoop
    {
        LLUUID mSourceID;
        LLUUID mSound;
        S32 mSurface = -1;
        S32 mAction = -1;
        F64 mLastSeen = 0.0;
        F64 mStartedAt = 0.0;   // when the loop began, for deriving playback position against the analysed onsets - and the churn guard: a source the engine reaped (asset still fetching) is not recreated more than once a second
        U32 mOffsetMS = 0;      // where inside the recording the loop STARTED - a random between-steps gap, so every walk begins on a fresh step; position math must add it
        bool mSegmented = false;    // this state plays per-impact windowed steps instead of a loop - see footstepImpact
        F64 mLastImpactAt = 0.0;    // cadence refractory: the swap detector chatters when ankle heights run near-equal, and humans cannot step faster than a gait allows
        LLUUID mSegSourceID;        // the avatar's currently sounding step, so the next footfall CUTS it rather than stacking - a recording with long inter-step gaps played at running cadence could otherwise sound three steps at once
        F64 mStopAt = 0.0;      // > 0: finishing its current step, dies at this time - see the gap-stop in updateFootstepLoop
    };
    std::map<LLUUID, StepLoop> mStepLoops;
    void releaseStepLoop(StepLoop& loop);
    void reapStepLoops(F64 now);

    // Long one-shots (thunder) ride a fixed WORLD bearing but a listener-relative distance: repositioned every frame at cam + bearing*12m with the listener's own velocity, so there is no relative
    // motion for the engine's doppler to chew on and no rolloff cliff however far the strike really was.
    struct OneShotFollower
    {
        LLUUID mSourceID;
        LLVector3 mDir;
        LLVector3 mLastPos;
        F64 mExpires = 0.0;
    };
    std::vector<OneShotFollower> mFollowers;

    // ~10s-averaged wetness, kept for anything that wants the storm's mass rather than the moment. The ladder deliberately does NOT use it any more - immediacy is wanted there, and the voices'
    // own crossfades do the smoothing.
    F32 mWetSlow = 0.f;

    // One persistent looping voice per ladder rung - see updateBedVoices. Voices crossfade by GAIN as the ladder position moves; a rung crossing a pair boundary keeps its voice (and therefore its
    // playback position) untouched, and a rung that fades out entirely remembers where it stopped and resumes there next time. This is what replaced overriding the bed SLOTS, which restarted both
    // recordings from their authored fade-ins on every boundary crossing.
    struct BedVoice
    {
        LLUUID mSourceID;
        F64 mStartedAt = 0.0;
        U32 mOffsetMS = 0;
        F32 mGain = 0.f;
        F32 mTarget = 0.f;
    };
    std::map<LLUUID, BedVoice> mBedVoices;
    std::map<LLUUID, U32> mBedResume;                       // last playback position per recording, ms

    // Debug markers over live footstep audio sources (SSAtmoDebugFootstepMarkers): a big bright tag the instant a source starts, decaying to a small dim one that rides the source until it stops -
    // so start moments, follow behaviour and stop timing are all visible at a glance.
    struct StepMark
    {
        LLUUID mSourceID;
        LLHUDText* mText = nullptr;    // owned loosely like the strike countdown: markDead + drop
        F64 mStart = 0.0;
    };
    std::vector<StepMark> mStepMarks;
    void markStepSource(const LLUUID& source_id);
    void updateStepMarks(F64 now);

    // Windowed one-shots awaiting their scheduled cut (segment-mode footfalls).
    std::vector<std::pair<LLUUID, F64>> mSegmentStops;
    void updateSegmentStops(F64 now);

    // Click-free hard stops: gain to zero NOW (FMOD ramps volume changes internally, ~one mix buffer), destruction a few frames later. Every abrupt stop in the footstep system goes through this;
    // an unramped cut lands at an arbitrary sample amplitude and pops.
    std::vector<std::pair<LLUUID, F64>> mDying;
    void fadeKill(const LLUUID& source_id);
    void updateDying(F64 now);
    std::vector<std::pair<LLUUID, F32>> mLadderTargets;     // this tick's rung gains, empty when the ladder is inactive
    void updateBedVoices(F64 now, F32 dt, F32 master_mul);
    void registerFollower(const LLUUID& source_id, const LLVector3& dir_world, F64 now);
    void updateFollowers(F64 now, F32 dt);
    void updateThunder(F64 now);

    // Ambient loop slots; each maps to one developer-configured sound UUID Outdoor beds are a light/medium/heavy set where only medium is required; the roof beds are one per indoor space size. Wind
    // is not precipitation specific, so it stays global rather than per preset.
    enum ESlot
    {
        LOOP_AMBIENT_LIGHT = 0,
        LOOP_AMBIENT_MEDIUM,
        LOOP_AMBIENT_HEAVY,
        LOOP_ROOF_OPEN,     // roof overhead, outside: porch, awning
        LOOP_ROOF_SMALL,
        LOOP_ROOF_MEDIUM,
        LOOP_ROOF_BIG,
        LOOP_WIND_LIGHT,
        LOOP_WIND_STRONG,
        LOOP_COUNT
    };

    // One slot holds a comma-separated *sequence* of sounds: a single entry loops seamlessly at the engine level; multiple entries play unlooped in order, each next one starting when the previous
    // asset (whatever its duration) has finished. The audio engine reaps finished non-looping sources, so sources are tracked by ID and re-created to advance the sequence, never by cached pointer.
    struct Loop
    {
        std::vector<LLUUID> mSounds;
        std::string mConfigured;    // raw setting string, for change detection
        U32 mIndex = 0;
        LLUUID mSourceID;
        F32 mGain = 0.f;
        F32 mTarget = 0.f;

        // Where the source sits relative to the listener's head. Zero for the beds, which surround you; the wind loops sit a few metres UPWIND, so the engine's own 3D panning puts the wind in the
        // correct ear and it audibly swings as the local flow bends around buildings. Following the flowmap back, in the literal sense.
        LLVector3 mOffset;
    };


    void updateProbes(F64 now);
    bool castUpProbe(S32 index, F32& hit_dist);
    F32 castSideProbe(S32 index);
    void updateLoops(F64 now, F32 dt);
    void applyLoop(Loop& loop, const std::string& configured, F32 master, F32 dt);
    void releaseLoop(Loop& loop);

    Loop mLoops[LOOP_COUNT];

    // Probe state: one cycle is 3 wiggled upward rays (all must hit to count as covered) plus 4 cardinal rays sizing the space, all cast in a single idle so the classification never lags the camera;
    // cycles are triggered by movement or staleness and rate limited between each other
    LLVector3 mProbeAnchor;
    F64 mLastCycleDone = -1000.0;
    F32 mSideDist[4] = { 0.f, 0.f, 0.f, 0.f };
    F32 mRoofDist = 0.f;
    S32 mWallCount = 0;
    F32 mWallAvg = 0.f;
    F32 mCoverage = 0.f;
    bool mCovered = false;
    ESpace mSpace = SPACE_OUTDOOR;
    ESize mOutdoorSize = SIZE_LARGE;
    LLVector3 mProbeOrigin;             // where mSideDist was measured from
    F32 mCoverSmooth = 0.f;

    // Build standing above the ceiling, measured at the last probe cycle and eased the same way cover is, so walking down a stair ramps the rain out rather than stepping it
    F32 mBuriedDepth = 0.f;
    F32 mBuriedSmooth = 0.f;

    // Exponential moving rate of impact strength near the camera
    F32 mImpactRate = 0.f;
    F64 mLastIdle = 0.0;
};

// </SS:Nexii>

#endif // SS_SOUNDSCAPE_H
