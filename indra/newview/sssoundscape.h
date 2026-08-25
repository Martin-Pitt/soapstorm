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

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <string>
#include <vector>

class LLAudioSource;

class SSSoundscape : public LLSingleton<SSSoundscape>
{
    LLSINGLETON_EMPTY_CTOR(SSSoundscape);

public:
    // Per-frame driver, called from the manager's idle after parameters
    // refresh; owns probe scheduling and loop gain fades
    void idle();

    // An impact landed near the camera (already distance-gated by the
    // impact queue); feeds the local-loudness metric so sheltered spots
    // sound quieter than open ground even at the same weather parameters
    void notifyImpact(F32 strength);

    // Fade out and release every loop (system disabled, teleport, etc.)
    void stopAll();

    // The footstep voice for a foot at this position, from the surface
    // underfoot - dry, wet, puddle, indoors - or null when the soundscape
    // has nothing to say: slot unconfigured, or Atmo Magic off. Null tells
    // the avatar to fall back to the stock per-avatar step sound, so
    // turning the system off returns the viewer to its vanilla footsteps
    // rather than silence. action is an SSStepAction; on_land whether the
    // foot is on terrain rather than an object.
    LLUUID footstepSound(const LLVector3& foot_pos_agent, bool on_land, S32 action);

    // What the last footstep lookup decided, for the info overlay. Every
    // stage of the walk is recorded, including the ones that returned
    // nothing, because "no sound played" has half a dozen causes that are
    // indistinguishable from a chair: system off, wrong surface picked, slot
    // empty, list unparseable.
    struct StepDebug
    {
        F64 mWhen = -1.0;           // sharedTime of the lookup, -1 for never
        S32 mSurface = -1;          // SSStepSurface, or -1 if not reached
        S32 mAction = -1;
        bool mIndoors = false;
        bool mFieldValid = false;   // the surface field had an answer
        F32 mWet = 0.f;
        F32 mPuddle = 0.f;
        bool mGlobal = false;       // read from settings rather than the preset
        std::string mSource;        // setting name or preset slot key
        S32 mListSize = 0;
        LLUUID mPicked;
        const char* mWhyNot = "";   // empty when a sound was returned
    };
    const StepDebug& lastStep() const { return mStepDebug; }

    enum ESpace
    {
        SPACE_OUTDOOR = 0,
        SPACE_SHELTERED,    // roof overhead but open sides: porch, awning
        SPACE_SMALL,
        SPACE_MEDIUM,
        SPACE_BIG
    };

    // With nothing overhead, height stops mattering and the width of the
    // surroundings takes over: an alley between two buildings is a different
    // outdoor space from an open plain.
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

    // Rough occlusion from the bubble: a source further away than the
    // surface in its direction is on the far side of that surface, so it is
    // being heard through something. Costs no extra rays - it reuses the
    // width samples the cover probe already took.
    F32 occlusionGain(const LLVector3& source_pos) const;
    F32 wallDistanceToward(const LLVector3& dir_horizontal) const;

    // Schedules a delayed, quieter copy of a one-shot bounced off the nearest
    // wall. Positioned at the reflecting surface, so the ear that hears it
    // first falls out of normal 3D panning rather than a channel delay.
    F32 impactRate() const;   // landings per second near the camera
    F32 coverBlend() const { return mCoverSmooth; }
    S32 wallCount() const { return mWallCount; }
    F32 wallDistance() const { return mWallAvg; }
    F32 roofDistance() const { return mRoofDist; }

    // How much build stands between the ceiling overhead and the open sky, in
    // metres. The up ray finds the ceiling of the room you are in; the wind
    // flowmap's overhead capture knows the top of the whole column. The gap
    // between the two is everything stacked above you - the storeys of a
    // building, or the ground over a basement - and it is what separates
    // standing in a ground floor room from standing in a cellar under it.
    F32 burialDepth() const { return mBuriedSmooth; }

    // 0 at the ceiling, approaching 1 deep under the build. What the rain bed
    // is attenuated by.
    F32 burialOcclusion() const;

    S32 activeLoops() const;
    F64 lastProbeAge() const;

private:
    StepDebug mStepDebug;

    // Ambient loop slots; each maps to one developer-configured sound UUID
    // Outdoor beds are a light/medium/heavy set where only medium is
    // required; the roof beds are one per indoor space size. Wind is not
    // precipitation specific, so it stays global rather than per preset.
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

    // One slot holds a comma-separated *sequence* of sounds: a single entry
    // loops seamlessly at the engine level; multiple entries play unlooped
    // in order, each next one starting when the previous asset (whatever
    // its duration) has finished. The audio engine reaps finished
    // non-looping sources, so sources are tracked by ID and re-created to
    // advance the sequence, never by cached pointer.
    struct Loop
    {
        std::vector<LLUUID> mSounds;
        std::string mConfigured;    // raw setting string, for change detection
        U32 mIndex = 0;
        LLUUID mSourceID;
        F32 mGain = 0.f;
        F32 mTarget = 0.f;
    };


    void updateProbes(F64 now);
    bool castUpProbe(S32 index, F32& hit_dist);
    F32 castSideProbe(S32 index);
    void updateLoops(F64 now, F32 dt);
    void applyLoop(Loop& loop, const std::string& configured, F32 master, F32 dt);
    void releaseLoop(Loop& loop);

    Loop mLoops[LOOP_COUNT];

    // Probe state: one cycle is 3 wiggled upward rays (all must hit to
    // count as covered) plus 4 cardinal rays sizing the space, all cast in
    // a single idle so the classification never lags the camera; cycles are
    // triggered by movement or staleness and rate limited between each other
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

    // Build standing above the ceiling, measured at the last probe cycle and
    // eased the same way cover is, so walking down a stair ramps the rain out
    // rather than stepping it
    F32 mBuriedDepth = 0.f;
    F32 mBuriedSmooth = 0.f;

    // Exponential moving rate of impact strength near the camera
    F32 mImpactRate = 0.f;
    F64 mLastIdle = 0.0;
};

// </SS:Nexii>

#endif // SS_SOUNDSCAPE_H
