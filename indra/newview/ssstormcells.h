/**
 * @file ssstormcells.h
 * @brief Atmo Magic: the storm-cell scheduler shell - lattice candidates, weather-at-birth gate, hero flyby, per-frame cell set.
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

#ifndef SS_STORMCELLS_H
#define SS_STORMCELLS_H

#include "llsingleton.h"
#include "ssstormcellcore.h"
#include "ssstormcouplecore.h"
#include "v2math.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SSAtmoEnvTrack;
class SSAtmoEnvManager;
class LLViewerRegion;

// <SS:Nexii> The SHELL of doc/atmo_magic_storm_dynamics.md section 2: every frame the active storm cells are re-derived from scratch - no persisted sim state, no dt, no integrator. Inputs to every positional output, listed per the phase-1 lesson: the shared seed (SSAtmoMagic::seed), the wall clock (SSAtmoMagic::sharedTime), the weather domain's anchor (S2, phase-2b: the source region of the applied asset's from_parcel note when it has one, else the agent's own region - see update()'s anchor block for the honest limitation), the applied track's weather cube and dome-height keyframes evaluated at each candidate's BIRTH phase, and the track's two Allow flags gated by the influence master enable (S4). The camera never enters: it picks the TRACK (as it does for the sky itself, an altitude band - S1) and nothing else; each track's storm world is a pure function of that track's cube, seed and clock. Positions are GLOBAL-frame metres (region origin global + local), because the agent frame re-bases on every region crossing and a 2900m lattice hashed in it would give two clients in neighbouring regions different cells; consumers convert at the read site with toAgentXY (the same frame SSVolCloud positions puffs in). No rendering and no deck coupling here - phase 3 reads cells() from the deck builder; this phase exists so the V2/V7 debug views can show cells and a hero flyby. S6 (phase-2b): the enumerate/gate/sort/hero/cull pipeline itself lives in SSStormCell::resolveActive (ssstormcellcore.h) - this shell is an ADAPTER that builds the two weather-cube hooks and reads the result; cells() is directly the core's ActiveCell type.
class SSStormCells : public LLSingleton<SSStormCells>
{
    LLSINGLETON(SSStormCells);
    ~SSStormCells() = default;

public:
    // How far out from the anchor the lattice is enumerated (12 km, the visible field); cells whose closed-form centre
    // has drifted past this plus RADIUS_MAX_M are culled - by radius from the anchor, never by camera distance.
    static constexpr F32 FIELD_M = 12000.f;

    // <SS:Nexii> S6: directly the core's resolved-cell POD (SSStormCell::ActiveCell) - the shell adds nothing to it.
    // mCandidate.mId is the ranking key cells() is sorted ascending on; mCandidate.mOriginXY is always the LATTICE
    // draw even for the hero (whose mOrigin/mMotion are overwritten by composeHero - compare the two to see the offset).
    using ActiveCell = SSStormCell::ActiveCell;

    struct Hero
    {
        U64 mId = 0;
        F64 mBirthTime = 0.0;
        F32 mLifetimeS = 0.f;
        SSStormCell::Hero mPath;              // origin, motion, closest point/time against the anchor
        SSStormCell::Vec2 mDeath;             // centre at birth + lifetime
    };

    // <SS:Nexii> The V2 "why not" readout (doc/atmo_magic_debug_views.md): the alive candidate with the best gate score this frame, every number the gate decided on, and the terms it failed, spelled with live values. Formation debugging without reading code; when a hero exists mFailing is empty.
    struct WhyNot
    {
        bool mHaveCandidate = false;
        U64 mId = 0;
        F64 mBirthTime = 0.0;
        F32 mAge01 = 0.f;
        F32 mScore = 0.f;                     // SSStormCell::gateScore
        F32 mPotential = 0.f;
        F32 mConvection = 0.f;
        F32 mMoisture = 0.f;
        F32 mShearNoise = 0.f;
        F32 mShearStrength = 0.f;
        F32 mRotation = 0.f;
        F32 mRotationTerm = 0.f;              // SSStormCell::rotationTerm
        F32 mLifetimeS = 0.f;
        bool mAllowSupercells = false;
        bool mAllowTornadoes = false;
        SSStormCell::Gate mGate;
        std::vector<std::string> mFailing;    // human-readable failing terms, in gate order
        F64 mNextEpochInS = 0.0;              // seconds until the next epoch's candidates start being born
        S32 mCandidates = 0;                  // lattice cells x epochs in the window this frame
        S32 mAlive = 0;
        S32 mSpawned = 0;
        S32 mSupercells = 0;
        S32 mTornadoEligible = 0;
    };

    // <SS:Nexii> S12 (phase-2b audit): a consumer's stake in the scheduler running at all. Ref-counted (the
    // SSWorldField::Interest idiom); while nothing holds one, update() early-outs to an empty state before touching
    // the anchor, the memo or the lattice - a debug view that is not open should not pay this every frame. A handle
    // rather than a bool so a view that closes (or a mode that switches away) drops its claim automatically.
    class Interest
    {
    public:
        Interest() = default;
        explicit operator bool() const { return mHold != nullptr; }
    private:
        friend class SSStormCells;
        explicit Interest(std::shared_ptr<void> hold) : mHold(std::move(hold)) {}
        std::shared_ptr<void> mHold;
    };
    Interest claim();

    // Ticked from SSAtmoMagic::idle() right before SSVolCloud::update(). Reads the shared clock, never a dt. A no-op
    // (see Interest above) unless something currently claims the scheduler.
    void update();

    // False when nothing drives (Atmo off, no asset, no region): cells() is then empty and hero() null.
    bool valid() const { return mValid; }

    const std::vector<ActiveCell>& cells() const { return mCells; }
    const Hero* hero() const { return mHaveHero ? &mHero : nullptr; }
    const WhyNot& whyNot() const { return mWhyNot; }

    // <SS:Nexii> Phase 4 (doc/atmo_magic_storm_dynamics.md section 3, "Storm motion vs cloud drift"): the hero
    // ActiveCell's own mMotion (m/s, closed-form, frame-invariant under the agent frame's pure translation - no
    // toAgentXY needed for a velocity) and elapsed age in seconds (mAge01 * mCandidate.mLifetimeS, the same
    // denormalisation lifecycle() itself uses internally) - what SSDeckFrame::HeroFrame needs beyond the centre/
    // radius fillUniforms already resolves into mStormCells[0]. A lookup adapter only (no formula beyond that one
    // fraction*total unit conversion): false and both outputs untouched when no hero is currently active.
    bool heroMotionAgeS(LLVector2& out_motion_ms, F32& out_age_s) const;

    // The frame's shared inputs, for the V7 sync console.
    F64 now() const { return mNow; }
    U32 seed() const { return mSeed; }
    S32 trackIndex() const { return mTrackIndex; }
    const SSStormCell::Vec2& anchor() const { return mAnchor; }   // global metres
    S64 currentEpoch() const { return mEpochNow; }
    S32 latticeCount() const { return mLatticeCount; }

    // <SS:Nexii> FIX 1: the S2 source-region anchor logic (see the class comment and update()'s own anchor block),
    // available to a caller that holds no Interest and so never sees update() run - SSVortices' dust-devil block is
    // parentless and reads no resolved cell, but it still needs the SAME weather-domain anchor cells() would use, so
    // this exposes the identical computation (resolveAnchor, shared with update() so the two can never disagree)
    // without requiring a claim, without touching mAnchor/mValid/mCells, and without memoising anything. Global
    // metres, SSStormCell::Vec2() (world origin) when the agent has no region or Atmo has no asset - a caller must
    // already be gating on those before trusting this for placement.
    SSStormCell::Vec2 anchorNow() const;

    // <SS:Nexii> 7c NEW-2: phaseAt (private, below), exposed read-only so a caller that reconstructs the scheduler's
    // own decisions (SSAtmoInfoView::stormCellsData's squall-line rebuild) uses the IDENTICAL phase map update()
    // just resolved this frame's cells with - never mTrack->dayCyclePhaseAt unconditionally, which is silently
    // wrong under a preview-phase override (phaseAt's preview branch reads mPreviewPhase/mPreviewRefTimeS, both
    // captured by the same update() call). Forwards to phaseAt with no formula of its own.
    F64 schedulerPhaseAt(F64 wall_time) const { return phaseAt(wall_time); }

    // Global-metre XY <-> the agent frame the renderer and SSVolCloud work in. Unit conversion only.
    LLVector2 toAgentXY(const SSStormCell::Vec2& global_xy) const;
    SSStormCell::Vec2 fromAgentXY(const LLVector2& agent_xy) const;

    // <SS:Nexii> Phase 3: the deck coupling's per-frame uniform export - the SAME selection SSStormCouple::selectSlots
    // decides (the first slot-preferred cell - the hero, else the first forced pin, in input order - then ascending
    // distance of centre to ANCHOR, never camera distance), converted
    // to the AGENT frame the deck positions its puffs in (SSVolCloud::buildDeck's air-cell-centre + drift is agent
    // frame; this must land in the same one). radius/boost/anvil/meso/overshoot/mammatus come straight off the
    // resolved cell's mRadiusM/mLifecycle (already intensity-scaled by resolveActive - see ssstormcellcore.h's
    // lifecycle(), whose `k` parameter IS mGate.mIntensity - so this does not multiply by intensity again), dir is
    // SSStormCouple::motionDirection(mMotion), rotSign is SSStormCouple::rotSignOf(mCandidate.mRotation). Slots at or
    // beyond the returned count are left at CellUniform's own zero-radius default (a disabled cell to sampleAt).
    // Invariants: returns min(cells().size(), cap); the same n cells in the SAME order produce the same n CellUniforms
    // (selectSlots is order-independent except for exact ties and for which of several slot-preferred entries -
    // hero plus forced pins, 7c NEW-6 - takes slot 0; mCells is id-sorted by resolveActive, so the order is shared
    // by every client); called with cap <= 0 or an empty cell set returns 0.
    S32 fillUniforms(SSStormCouple::CellUniform* out, S32 cap) const;

private:
    // The cube at one birth instant, memoised by SSStormCell::memoKey(candidate) (7b F5: id + birth quantised to
    // 1 ms, not id alone - a forced pin shares its lattice twin's mId but carries a rewritten birth time): a pure
    // function of (track, birth time), so the memo is not state - it is dropped every MEMO_BUCKET_S of wall clock
    // (so live edits of the cube show up) and whenever the applied track changes.
    struct BirthMemo
    {
        SSStormCell::WeatherAtBirth mWeather;
        SSStormCell::Vec2 mAnvilWind;
    };
    static constexpr F64 MEMO_BUCKET_S = 2.0;

    void clear();
    // <SS:Nexii> S3 (phase-2b audit): under the editor's preview override, SSStormCell::previewPhaseAt(mPreviewPhase,
    // refTime, wall_time, mDayLengthS) with refTime = mPreviewRefTimeS, LATCHED when the preview turns on or its
    // phase changes and held while the slider is still (7d; the old bucket-start rule made a forced cue's wall time
    // jump every 2 s) - so the map is fixed between slider moves and time flows through it normally; a slider move
    // re-latches and clears the memo (see update()). Otherwise mTrack->dayCyclePhaseAt(wall_time) (S3: the same fmod SSAtmoEnvTrack itself
    // exposes, not a shell respelling of it). NOTE: this differs in kind from the sky's OWN currentDayCyclePhase(),
    // which quantises to whole seconds (time(nullptr)) - the storms and the sky agree on the FORMULA, not bit-for-bit
    // on every fractional second, and no test claims otherwise.
    F64 phaseAt(F64 wall_time) const;

    // <SS:Nexii> 7b F3: phaseAt's own INVERSE, mirroring its preview/real switch exactly - a forced-storm cue phase
    // (read through phaseAt, above) must convert back to wall time through the SAME map it was read through, real
    // or preview, never always the real track's map regardless of which is active (lesson 24). Preview:
    // SSStormCell::previewWallTimeAt(mPreviewPhase, mPreviewRefTimeS, phase, mDayLengthS, nearT) - the SAME
    // refTime phaseAt uses. Otherwise mTrack->wallTimeAtPhase(phase, nearT) (now a one-line forward to
    // SSDayCycle::wallTimeAtPhase, ssdaycyclecore.h).
    F64 wallTimeAtPhase(F64 phase, F64 nearT) const;

    const BirthMemo& birthMemo(const SSStormCell::Candidate& c);

    // <SS:Nexii> FIX 1: the S2 anchor computation itself, extracted out of update()'s own anchor block so
    // anchorNow() (public, above) and update() (below) call the identical logic rather than a hand-kept copy of it.
    static SSStormCell::Vec2 resolveAnchor(SSAtmoEnvManager* mgr, LLViewerRegion* region);

    bool mValid = false;
    F64 mNow = 0.0;
    U32 mSeed = 0;
    S32 mTrackIndex = -1;
    S64 mEpochNow = 0;
    S32 mLatticeCount = 0;
    SSStormCell::Vec2 mAnchor;

    // Phase mapping inputs, captured per frame so phaseAt/wallTimeAtPhase are pure functions of them and safe to
    // call at ANY time (the public schedulerPhaseAt runs from the V2 view's draw path, outside update(), when
    // mTrack below is null - 7c). Both maps go through the SSDayCycle core, not a shell respelling.
    F64 mDayLengthS = 0.0;
    F64 mDayOffsetS = 0.0;
    bool mPreviewOverride = false;
    F64 mPreviewPhase = 0.0;

    const SSAtmoEnvTrack* mTrack = nullptr;  // valid during update() only

    std::vector<ActiveCell> mCells;
    bool mHaveHero = false;
    Hero mHero;
    WhyNot mWhyNot;

    std::unordered_map<U64, BirthMemo> mMemo;
    S64 mMemoBucket = -1;
    S32 mMemoTrack = -1;
    F64 mPreviewRefTimeS = 0.0;       // 7d: phaseAt's preview-branch refTime - mNow at the last preview on/phase change, held while the slider is still
    bool mMemoPreviewOverride = false; // S3: memo also drops when either of these changes, not just the bucket/track
    F64 mMemoPreviewPhase = 0.0;
};

#endif
