/**
 * @file ssstormcells.cpp
 * @brief Atmo Magic: the storm-cell scheduler shell - see ssstormcells.h.
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

#include "ssstormcells.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvweatherstate.h"
#include "ssatmomagic.h"
#include "ssdaycyclecore.h"
#include "sssquallcore.h"
#include "sswindprofilecore.h"

#include "llagent.h"
#include "llregionhandle.h" // <SS:Nexii> S2: from_region_handle - the source anchor's 256m-aligned origin when its region is not currently simulated
#include "llstring.h"
#include "llviewerregion.h"
#include "llworld.h"        // <SS:Nexii> S2: LLWorld::getRegionFromHandle - resolving the asset's noted source region

#include <algorithm>
#include <cmath>

namespace
{
    // The lattice within FIELD_M of the anchor: (2 * 12000 / 2900 + 2)^2 is under 110 cells; the cap is comfortably above.
    constexpr S32 LATTICE_CAP = 256;

    // <SS:Nexii> S6: resolveActive's output buffer size. Generous margin over the S7 density calibration's worst
    // measured concurrent count (extreme weather, mean 38.49 over 960 anchor-hours - see ssstormcellcore.h's
    // calibration comment); resolveActive silently drops any excess rather than overflow.
    constexpr S32 ACTIVE_CAP = 512;

    // <SS:Nexii> S12: the scheduler's consumer refcount (the SSWorldField::Interest idiom) - file-local since only
    // claim()/update() touch it.
    S32 sInterestCount = 0;

    // <SS:Nexii> SCHEDULER (doc/atmo_magic_storm_dynamics.md section 6 layer 3): the cube's authored kind STRING ->
    // SSSquall::ForcedOverride::mKind's small int (see sssquallcore.h's own doc comment: 0 none, 1 supercell,
    // 2 tornado, 3 waterspout-preferred, 4 anticyclonic). A fixed-vocabulary lookup, not a numeric formula - stays
    // shell-side the same way mPrecipitationOverride's kind string is read directly by its consumers.
    S32 stormOverrideKindFromString(const std::string& kind)
    {
        if (kind == "supercell") return 1;
        if (kind == "tornado") return 2;
        if (kind == "waterspout") return 3;
        if (kind == "anticyclonic") return 4;
        return 0; // "none" and anything unrecognised
    }
}

// Singleton shell.
SSStormCells::SSStormCells()
{
}

// S12: hands out a ref-counted stake in the scheduler running; dropping the last one lets update() early-out.
SSStormCells::Interest SSStormCells::claim()
{
    ++sInterestCount;
    return Interest(std::shared_ptr<void>((void*)1, [](void*) { --sInterestCount; }));
}

// Drops the frame's outputs; the memo survives (it is pure) until its bucket or track changes.
void SSStormCells::clear()
{
    mValid = false;
    mCells.clear();
    mHaveHero = false;
    mHero = Hero();
    mWhyNot = WhyNot();
    mTrack = nullptr;
}

// <SS:Nexii> FIX 1: the S2 source-region anchor rule, extracted out of update()'s own anchor block (previously
// inline there) so anchorNow() below and update() call the identical logic instead of two hand-kept copies of it -
// the SOURCE region's centre when the applied asset was noted from a parcel (SSAtmoEnvManager::sourceRegionHandle),
// else region's own centre (a personal preview: nothing broadcast this asset, so there is no sync domain to anchor
// on). HONEST LIMITATION: the source handle is the region gAgent stood in AT NOTE TIME, not a server-authoritative
// id - a neighbour region whose OWN parcel happens to reference the same asset re-notes ITS region when a client
// standing there discovers it, so that client's hero re-derives against a DIFFERENT anchor. Two clients in the SAME
// region agree; two clients in different regions sharing an asset by coincidence do not, until a server-authoritative
// anchor exists. mgr/region are the caller's own reads (never fetched here) so this stays a pure function of them.
// [interaction: SSAtmoEnvManager::sourceRegionHandle] [interaction: LLWorld::getRegionFromHandle]
SSStormCell::Vec2 SSStormCells::resolveAnchor(SSAtmoEnvManager* mgr, LLViewerRegion* region)
{
    SSStormCell::Vec2 out;
    LLViewerRegion* anchor_region = region;
    const U64 source_handle = mgr->sourceRegionHandle();
    if (source_handle != 0)
    {
        LLViewerRegion* source_region = LLWorld::instanceExists()
                                         ? LLWorld::getInstance()->getRegionFromHandle(source_handle) : nullptr;
        if (source_region)
        {
            anchor_region = source_region;
        }
        else
        {
            // Not currently simulated for this client (a neighbour, or the agent has since moved away): the
            // handle still gives its 256m-aligned origin, so approximate its centre at the legacy region width
            // rather than silently falling back to the agent's own region - a region the asset was never noted from.
            F32 ox, oy;
            from_region_handle(source_handle, &ox, &oy);
            out.x = ox + 0.5f * REGION_WIDTH_METERS;
            out.y = oy + 0.5f * REGION_WIDTH_METERS;
            anchor_region = nullptr;
        }
    }
    if (anchor_region)
    {
        const LLVector3d& origin = anchor_region->getOriginGlobal();
        const F64 half = 0.5 * (F64)anchor_region->getWidth();
        out.x = (F32)(origin.mdV[VX] + half);
        out.y = (F32)(origin.mdV[VY] + half);
    }
    return out;
}

// <SS:Nexii> FIX 1: resolveAnchor() above, callable without an Interest claim - SSVortices' dust-devil block needs
// the same weather-domain anchor cells() would use, but is a parentless mini-scheduler that must run even when
// nothing has claimed this scheduler (so update() never touched mAnchor this frame, or ever). Reads gAgent/mgr
// fresh every call rather than mAnchor: never memoised, never gates mValid/mCells, so a caller mixing this with a
// live claim still sees exactly this frame's anchor either way (both routes call the identical resolveAnchor).
// SSStormCell::Vec2() (world origin) when the agent has no region or Atmo has no asset - a caller must already be
// gating on those before trusting this for placement.
SSStormCell::Vec2 SSStormCells::anchorNow() const
{
    LLViewerRegion* region = gAgent.getRegion();
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!region || !mgr->hasAsset())
    {
        return SSStormCell::Vec2();
    }
    return resolveAnchor(mgr, region);
}

// <SS:Nexii> S3 (phase-2b audit): wall clock -> the applied track's day phase. Under the editor's preview override,
// SSStormCell::previewPhaseAt with refTime = mPreviewRefTimeS, the instant the preview last turned on or changed
// phase (7d - see update()'s memo block): phase(t) = frac(preview + (t - refTime) / dayLength), so at the latch the
// sky is exactly at the previewed phase and time then flows normally; scrubbing to an authored cue phase shows the
// forced storm mature at that moment. Otherwise
// SSDayCycle::phaseAt over the day length/offset CAPTURED in update() - the same core formula SSAtmoEnvTrack::
// dayCyclePhaseAt forwards to (daycyclecore.cpp pins it bit-identical to the old track body), never mTrack itself:
// mTrack is a borrowed per-frame pointer, null outside update(), and the 7c opus check caught the public
// schedulerPhaseAt (called from the V2 view's draw path) dereferencing it. phaseAt is a pure function of the
// captured values and safe at any time. [interaction: SSDayCycle::phaseAt]
F64 SSStormCells::phaseAt(F64 wall_time) const
{
    if (mDayLengthS <= 0.0)
    {
        return 0.0;
    }
    if (mPreviewOverride)
    {
        return SSStormCell::previewPhaseAt(mPreviewPhase, mPreviewRefTimeS, wall_time, mDayLengthS);
    }
    return SSDayCycle::phaseAt(wall_time, mDayLengthS, mDayOffsetS);
}

// <SS:Nexii> 7b F3: phaseAt's own inverse - see the header. Mirrors phaseAt's preview/real switch exactly so a
// forced-storm cue phase (read through phaseAt) converts back to wall time through the identical map, never always
// the real track's map regardless of which is active.
F64 SSStormCells::wallTimeAtPhase(F64 phase, F64 nearT) const
{
    if (mDayLengthS <= 0.0)
    {
        return nearT;
    }
    if (mPreviewOverride)
    {
        return SSStormCell::previewWallTimeAt(mPreviewPhase, mPreviewRefTimeS, phase, mDayLengthS, nearT);
    }
    return SSDayCycle::wallTimeAtPhase(phase, nearT, mDayLengthS, mDayOffsetS);
}

// <SS:Nexii> The weather cube AT THE CANDIDATE'S BIRTH TIME, never at now: moisture and convection are the cube's own curves at the birth phase (the same valueAt reads SSAtmoEnvApplier::computeModulation makes for the deck base), the shear strength is the resolver's S at that phase (auto-derived or authored, SSAtmoEnvWeatherResolver::resolve), the Allow flags are the track's gated by the influence MASTER enable (S4, phase-2b audit: mWeatherInfluence.mEnabled off means no supercells or tornadoes at all, matching what the Weather Influence floater shows the author), and the anvil wind is SSWindProfile::windAt at the birth-phase profile's own anvil AGL (SSAtmoEnvApplier::windProfileAt - pure, never the live mWindProfile). [interaction: SSAtmoEnvWeatherResolver] [interaction: SSAtmoEnvApplier::windProfileAt]
const SSStormCells::BirthMemo& SSStormCells::birthMemo(const SSStormCell::Candidate& c)
{
    // 7b F5: keyed on SSStormCell::memoKey(c) (id ^ birth-quantised-to-1ms), not c.mId alone - a forced pin shares
    // its lattice twin's mId but carries a REWRITTEN birth time, so keying on mId alone let whichever variant
    // memoised first (within this 2 s memo bucket) serve the other, across every client.
    const U64 key = SSStormCell::memoKey(c);
    auto it = mMemo.find(key);
    if (it != mMemo.end())
    {
        return it->second;
    }

    const F64 phase = phaseAt(c.mBirthTime);
    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(mTrack->mWeather, phase);
    const bool influence_enabled = mTrack->mWeatherInfluence.mEnabled;

    BirthMemo memo;
    memo.mWeather.mMoisture        = llclamp(mTrack->mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    memo.mWeather.mConvection      = llclamp(mTrack->mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    memo.mWeather.mShearStrength   = llclamp(state.mShearStrength, 0.f, 1.f);
    memo.mWeather.mAllowSupercells = influence_enabled && mTrack->mWeatherInfluence.mAllowSupercells;
    memo.mWeather.mAllowTornadoes  = influence_enabled && mTrack->mWeatherInfluence.mAllowTornadoes;

    const SSWindProfile::Params profile = SSAtmoEnvApplier::windProfileAt(*mTrack, phase);
    const SSWindProfile::Vec2 wind = SSWindProfile::windAt(profile.mAnvilAglM, profile);
    memo.mAnvilWind.x = wind.x;
    memo.mAnvilWind.y = wind.y;

    return mMemo.emplace(key, memo).first->second;
}

// Global XY -> agent frame, via the agent's region origin. [interaction: gAgent]
LLVector2 SSStormCells::toAgentXY(const SSStormCell::Vec2& global_xy) const
{
    const LLVector3 a = gAgent.getPosAgentFromGlobal(LLVector3d((F64)global_xy.x, (F64)global_xy.y, 0.0));
    return LLVector2(a.mV[VX], a.mV[VY]);
}

// Agent frame -> global XY.
SSStormCell::Vec2 SSStormCells::fromAgentXY(const LLVector2& agent_xy) const
{
    const LLVector3d g = gAgent.getPosGlobalFromAgent(LLVector3(agent_xy.mV[0], agent_xy.mV[1], 0.f));
    SSStormCell::Vec2 v;
    v.x = (F32)g.mdV[VX];
    v.y = (F32)g.mdV[VY];
    return v;
}

// <SS:Nexii> Phase 3 (deck coupling): see the header - this is only the adapter, SSStormCouple::selectSlots decides
// the ranking. Global frame throughout the selection (mCells' mCentre and mAnchor are both global, so the anchor
// distances selectSlots ranks by are the same whichever frame they are read in - a uniform translation does not
// change a distance ordering); the AGENT conversion happens only for the cells actually kept, so a build with 40
// alive cells pays toAgentXY four times, not forty.
S32 SSStormCells::fillUniforms(SSStormCouple::CellUniform* out, S32 cap) const
{
    if (!out || cap <= 0)
    {
        return 0;
    }

    const S32 n = (S32)mCells.size();
    if (n <= 0)
    {
        return 0;
    }

    std::vector<SSStormCouple::SelectKey> keys((size_t)n);
    for (S32 i = 0; i < n; ++i)
    {
        const ActiveCell& c = mCells[(size_t)i];
        keys[(size_t)i].id = c.mCandidate.mId;
        keys[(size_t)i].x = c.mCentre.x;
        keys[(size_t)i].y = c.mCentre.y;
        // 7c NEW-6: an authored pin (mIsForced) is slot-preferred the same as the hero - it is guaranteed-active
        // by the same authoring intent a hero flyby is, so selectSlots must not bump it for an ordinary cell.
        keys[(size_t)i].hero = c.mIsHero || c.mIsForced;
    }

    const S32 cap_eff = llmin(cap, SSStormCouple::MAX_CELLS);
    S32 idx[SSStormCouple::MAX_CELLS];
    const S32 picked = SSStormCouple::selectSlots(keys.data(), n, mAnchor.x, mAnchor.y, idx, cap_eff);

    for (S32 s = 0; s < picked; ++s)
    {
        const ActiveCell& c = mCells[(size_t)idx[s]];
        SSStormCouple::CellUniform& u = out[s];
        u = SSStormCouple::CellUniform();

        const LLVector2 agent_xy = toAgentXY(c.mCentre);
        u.x = agent_xy.mV[0];
        u.y = agent_xy.mV[1];
        u.radius = c.mRadiusM;
        // mLifecycle's fields are already scaled by mGate.mIntensity inside lifecycle() (its `k` parameter) - not
        // multiplied again here.
        u.boost = c.mLifecycle.mTowerBoost;
        u.anvil = c.mLifecycle.mAnvil;
        u.meso = c.mLifecycle.mMeso;
        u.overshoot = c.mLifecycle.mOvershoot;
        u.mammatus = c.mLifecycle.mMammatus;
        SSStormCouple::motionDirection(c.mMotion.x, c.mMotion.y, u.dirX, u.dirY);
        u.rotSign = SSStormCouple::rotSignOf(c.mCandidate.mRotation);
    }
    return picked;
}

// <SS:Nexii> Phase 4: a plain scan of mCells for mIsHero - see the header note. Not memoised (mCells is already
// this frame's resolved set; a second walk of at most a few dozen entries costs nothing next to resolveActive).
bool SSStormCells::heroMotionAgeS(LLVector2& out_motion_ms, F32& out_age_s) const
{
    for (const ActiveCell& c : mCells)
    {
        if (c.mIsHero)
        {
            out_motion_ms = LLVector2(c.mMotion.x, c.mMotion.y);
            out_age_s = c.mAge01 * c.mCandidate.mLifetimeS;
            return true;
        }
    }
    return false;
}

// <SS:Nexii> S6/S12 (phase-2b audit): the per-frame re-derivation. Early-outs (S12) unless a consumer currently
// claims the scheduler (see Interest), before touching the anchor, the memo or the lattice at all - a consumer
// that is not claiming it should not pay this every frame. That consumer is not only a debug view: SSVolCloud's
// coupled deck build (buildDeck, phase 3) holds a claim for as long as weatherDeck() keeps building, and pays this
// cost the same as V2/V7 do. When claimed: resolve the anchor (S2), roll the memo bucket
// (S3), then SSStormCell::resolveActive runs the WHOLE enumerate/gate/sort/hero/cull pipeline - this function
// supplies only the two weather-cube hooks (birthMemo, memoised) and reads the result back; no enumerate, sort,
// cull or hero logic is respelled here. Nothing here reads a frame counter, a dt, a render setting or the eased
// SSAtmoMagic::mWind. S1 (ACCEPTED, phase-2b audit): appliedTrackIndex() above IS camera-derived - the camera's
// altitude band picks WHICH TRACK's storm world is observed, exactly as it picks the sky (SSAtmoEnvApplier); that
// is the one accepted camera input, and it is honest to name it, not to claim "nothing reads the camera" and mean
// it literally. It is never a ranking or positional input INSIDE a track's world: each track's storm world is a
// pure function of that track's cube, seed and clock, and the camera cannot select a cell, a hero or a position
// within it. Cross-client agreement of the resolved cell set is what scenario_storm_two_clients demonstrates for
// the core composition; the shell adds only the anchor and the cube reads.
void SSStormCells::update()
{
    using namespace SSStormCell;

    if (sInterestCount <= 0)
    {
        clear();
        mTrackIndex = -1;
        return;
    }

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    LLViewerRegion* region = gAgent.getRegion();

    const S32 track_index = applier->appliedTrackIndex();
    if (!atmo || !mgr->hasAsset() || track_index < 0
        || track_index >= (S32)mgr->asset().mTracks.size() || !region)
    {
        clear();
        mTrackIndex = -1;
        return;
    }

    mNow = atmo->sharedTime();
    mSeed = atmo->seed();
    mTrackIndex = track_index;
    mTrack = &mgr->asset().mTracks[(size_t)track_index];
    mDayLengthS = mTrack->mDayLengthSeconds;
    mDayOffsetS = mTrack->mDayOffsetSeconds; // 7c: captured so phaseAt/wallTimeAtPhase never touch mTrack (null outside update())
    mPreviewOverride = mgr->hasPreviewPhaseOverride();
    mPreviewPhase = mgr->previewPhaseOverride();
    mEpochNow = epochOf(mNow);

    // <SS:Nexii> S2 (phase-2b audit): the weather domain's anchor - resolveAnchor() below, extracted (FIX 1) so
    // anchorNow() can call the identical logic without a claim. See resolveAnchor's own comment for the
    // source-region rule and its honest limitation.
    mAnchor = resolveAnchor(mgr, region);

    // <SS:Nexii> S3 (phase-2b audit): the memo is pure in (track, birth time, preview phase mapping); it is dropped
    // on a wall-clock bucket (so a live cube edit shows within MEMO_BUCKET_S), on a track change (birth times then
    // map to a different cube), and now ALSO whenever the preview override or its phase changes - previously only
    // the bucket/track were checked, so flipping the override without waiting for a bucket edge could momentarily
    // keep serving WeatherAtBirth memoised under the pre-flip phase mapping.
    const S64 memo_bucket = bucket(mNow, MEMO_BUCKET_S);
    const bool preview_changed = mPreviewOverride != mMemoPreviewOverride
                                || (mPreviewOverride && mPreviewPhase != mMemoPreviewPhase);
    if (memo_bucket != mMemoBucket || mMemoTrack != mTrackIndex || preview_changed)
    {
        mMemo.clear();
        mMemoBucket = memo_bucket;
        mMemoTrack = mTrackIndex;
        mMemoPreviewOverride = mPreviewOverride;
        mMemoPreviewPhase = mPreviewPhase;
    }
    // <SS:Nexii> 7d (user report): the preview map's reference instant is LATCHED to mNow when the preview override
    // turns on or its phase changes, and held while the slider is still - no longer the memo bucket's start (the old
    // S3 rule). Re-latching every 2 s made the map's inverse (a forced cue's wall time) jump 2 s every bucket, so an
    // authored storm under preview sawtoothed a few seconds back and forth and its age never advanced. With one
    // latch, previewPhaseAt(P, ref, now) == P at the latch instant and time then flows normally; the memo is cleared
    // on the same change (above), so nothing memoised under the previous reference survives.
    if (preview_changed)
    {
        mPreviewRefTimeS = mNow;
    }

    // The two hooks resolveActive calls for every alive candidate - both route through the memoised cube read.
    auto weatherAtBirth = [this](const Candidate& c) -> WeatherAtBirth { return birthMemo(c).mWeather; };
    auto anvilWindAtBirth = [this](const Candidate& c) -> Vec2 { return birthMemo(c).mAnvilWind; };

    // <SS:Nexii> S13 (phase-2b re-audit fixup 3): the V2 "why not" readout's tallies and best-scoring-alive-candidate
    // search, collected from resolveActive's OWN enumerate/epoch/alive/gate walk via its diag hook - this used to be
    // a second, hand-rolled traversal beside this call (re-deriving candidate()/alive()/gate() for the same lattice
    // x epoch window); now there is exactly one. Captured by reference into the lambda below, read back into `why`
    // once resolveActive returns.
    bool have_best = false;
    F32 best_score = -1.f;
    Candidate best;
    WeatherAtBirth best_weather;
    S32 alive_count = 0;
    S32 spawned_count = 0;
    S32 supercell_count = 0;
    S32 tornado_count = 0;
    auto diag = [&](const Candidate& c, const WeatherAtBirth& w, const Gate& g)
    {
        ++alive_count;
        const F32 score = gateScore(c, w);
        if (!have_best || score > best_score || (score == best_score && c.mId < best.mId))
        {
            have_best = true;
            best_score = score;
            best = c;
            best_weather = w;
        }
        if (g.mSpawn) ++spawned_count;
        if (g.mSupercell) ++supercell_count;
        if (g.mTornadoEligible) ++tornado_count;
    };

    // <SS:Nexii> SCHEDULER (doc/atmo_magic_storm_dynamics.md sections 5-6): the squall-line hook resolveActive calls
    // once per epoch. "The epoch's phase" (both here and for severe below) is phaseAt(epoch start) - one weather-cube
    // read stands in for the whole epoch's line decision, the same single-sample-per-epoch discipline birthMemo
    // applies per-candidate; windAnvil is sampled at that same phase since a line's birth (inside lineEvent's own
    // hash) is not known until lineEvent runs. severe = SSWindProfile::consolidation(moisture, convection) at the
    // epoch's phase >= SSSquall::SEVERE_CONSOLIDATION_MIN (7c NEW-5: the named constant, not a literal 0.5f - the
    // V2 view's reconstruction reads the SAME constant) - the SAME consolidation figure ssvolcloud.cpp's builder
    // and SSStormCouple both read, never a second-guessed copy.
    const SSSquall::Vec2 anchorSq{ mAnchor.x, mAnchor.y };
    auto lineAtEpoch = [this, &anchorSq](S64 epoch) -> SSStormCell::LineDesc
    {
        SSStormCell::LineDesc d;

        // 7b F11: authored track state, deterministic (never epoch-hashed) - the Weather Influence master enable
        // AND the Squall Lines flag both have to be on, same influence_enabled gate birthMemo applies to the two
        // Allow flags. Off means every epoch resolves discrete cells only, matching the floater's tooltip.
        if (!(mTrack->mWeatherInfluence.mEnabled && mTrack->mWeatherInfluence.mSquallLines))
        {
            return d;
        }

        const F64 phase = phaseAt((F64)epoch * SSStormCell::EPOCH_S);
        const F32 moisture = llclamp(mTrack->mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
        const F32 convection = llclamp(mTrack->mWeather.mConvection.valueAt(phase), 0.f, 1.f);
        const bool severe = SSWindProfile::consolidation(moisture, convection) >= SSSquall::SEVERE_CONSOLIDATION_MIN;

        const SSWindProfile::Params profile = SSAtmoEnvApplier::windProfileAt(*mTrack, phase);
        const SSWindProfile::Vec2 wind = SSWindProfile::windAt(profile.mAnvilAglM, profile);
        const SSSquall::Vec2 windAnvil{ wind.x, wind.y };

        const SSSquall::LineEvent e = SSSquall::lineEvent(mSeed, epoch, anchorSq, windAnvil, severe);
        d.mIsLine = e.mIsLine;
        if (!d.mIsLine)
        {
            return d;
        }
        d.mLineId = e.mLineId;
        d.mOrigin = SSStormCell::Vec2{ e.mOrigin.x, e.mOrigin.y };
        d.mDirection = SSStormCell::Vec2{ e.mDirection.x, e.mDirection.y };
        d.mHalfLengthM = SSSquall::LINE_LENGTH_M * 0.5f;
        // 7b F6: how close a discrete lattice draw must be to the line segment before it is replaced by the line -
        // LINE_SUPPRESS_M (== LINE_SPACING_M), not the whole line's length; farther draws in the same epoch survive.
        d.mSuppressRadiusM = SSSquall::LINE_SUPPRESS_M;
        // 7b F2: the ONE anvil-wind sample this epoch's line was decided with (the same `windAnvil` fed to
        // lineEvent above) - every member's stormMotion must derive from this single sample, never a per-member
        // birth-phase read (lesson 21).
        d.mWindAnvil = SSStormCell::Vec2{ windAnvil.x, windAnvil.y };
        d.mMemberCount = llmin((S32)SSSquall::LINE_MEMBERS_MAX, SSStormCell::LINE_MEMBERS_CAP);
        for (S32 i = 0; i < d.mMemberCount; ++i)
        {
            d.mMembers[i] = SSSquall::lineMember(e, i);
        }
        return d;
    };

    // <SS:Nexii> SCHEDULER: the cube's forced-storm override keyframes (kind, cue phase, track-floor-relative XY
    // offset), all read at the CURRENT phase (never a candidate's birth phase - an authored cue is one wall-clock
    // instant, not an epoch-keyed draw). The cue phase converts to a wall-clock cueTime via THIS SHELL's own
    // wallTimeAtPhase (7b F3) - phaseAt's inverse, mirroring its preview/real switch, never always
    // SSAtmoEnvTrack::wallTimeAtPhase regardless of whether a preview override is active, so the cue converts
    // through the SAME map phaseNow below was read through. Reference "now" so every client landing on the same
    // wall clock resolves the SAME nearest occurrence. SSSquall::forcedCandidate then pins the lattice cell
    // nearest (anchor + offset); ForcedDesc::mPreferHero is set only for kind "tornado" (SSStormCell::resolveActive
    // then prefers this candidate as the hero over the ordinary ascending-id search - see its own comment).
    // 7c NEW-3: the WHOLE block is gated on mTrack->mWeatherInfluence.mEnabled - the master switch, not the two
    // Allow checkboxes: an authored cue still outranks Allow Supercells/Tornadoes (the floor above grants both
    // regardless), but with Weather Influence itself off there is no forced pin at all. Master off leaves
    // forcedDesc at its default (mPinned false), matching the kind-combo tooltip.
    SSStormCell::ForcedDesc forcedDesc;
    if (mTrack->mWeatherInfluence.mEnabled)
    {
        const F64 phaseNow = phaseAt(mNow);
        SSSquall::ForcedOverride ov;
        ov.mKind = stormOverrideKindFromString(mTrack->mWeather.mStormOverride.valueAt(phaseNow));
        if (ov.mKind != 0)
        {
            ov.mActive = true;
            const F32 cuePhase = mTrack->mWeather.mStormOverridePhase.valueAt(phaseNow);
            ov.mCueTime = wallTimeAtPhase((F64)cuePhase, mNow);
            ov.mOffsetM.x = mTrack->mWeather.mStormOverrideOffsetXM.valueAt(phaseNow);
            ov.mOffsetM.y = mTrack->mWeather.mStormOverrideOffsetYM.valueAt(phaseNow);
        }

        const SSSquall::Pinned pinned = SSSquall::forcedCandidate(mSeed, ov, anchorSq);
        forcedDesc.mPinned = pinned.mPinned;
        if (forcedDesc.mPinned)
        {
            forcedDesc.mCandidate = pinned.mCandidate;
            forcedDesc.mWeatherFloor.mMoisture = pinned.mWeatherFloor.mMoisture;
            forcedDesc.mWeatherFloor.mConvection = pinned.mWeatherFloor.mConvection;
            forcedDesc.mWeatherFloor.mShearStrength = pinned.mWeatherFloor.mShearStrength;
            forcedDesc.mWeatherFloor.mAllowSupercells = pinned.mWeatherFloor.mAllowSupercells;
            forcedDesc.mWeatherFloor.mAllowTornadoes = pinned.mWeatherFloor.mAllowTornadoes;
            forcedDesc.mPreferHero = (ov.mKind == 2); // 2 == tornado, sssquallcore.h's ForcedOverride::mKind
            forcedDesc.mKind = ov.mKind;              // 7d: reaches SSVortex::childVortex through ActiveCell::mForcedKind
            // 7b F1: composeForced (ssstormcellcore.h) reads these two off ForcedDesc directly - without them the
            // pinned candidate is placed with a zero offset and cueTime 0.0 regardless of what was authored.
            forcedDesc.mOffsetM = SSStormCell::Vec2{ ov.mOffsetM.x, ov.mOffsetM.y };
            forcedDesc.mCueTime = ov.mCueTime;
        }
    }
    auto forcedAt = [&forcedDesc]() -> SSStormCell::ForcedDesc { return forcedDesc; };

    ActiveCell resolved[ACTIVE_CAP];
    SSStormCell::Hero heroPath; // qualified: SSStormCells::Hero (this class's own nested type) would shadow it otherwise
    S32 heroIndex = -1;
    const S32 n = resolveActive(mSeed, mNow, mAnchor, FIELD_M, weatherAtBirth, anvilWindAtBirth,
                                 resolved, ACTIVE_CAP, &heroPath, &heroIndex, diag, lineAtEpoch, forcedAt);

    mCells.assign(resolved, resolved + n);
    mHaveHero = heroIndex >= 0;
    if (mHaveHero)
    {
        const ActiveCell& hc = mCells[(size_t)heroIndex];
        mHero.mId = hc.mCandidate.mId;
        mHero.mBirthTime = hc.mCandidate.mBirthTime;
        mHero.mLifetimeS = hc.mCandidate.mLifetimeS;
        mHero.mPath = heroPath;
        mHero.mDeath = centreAt(heroPath.mOrigin, heroPath.mMotion,
                                mHero.mBirthTime, mHero.mBirthTime + (F64)mHero.mLifetimeS);
    }
    else
    {
        mHero = Hero();
    }

    // <SS:Nexii> S9/S13 (phase-2b re-audit fixup 3): the V2 "why not" readout, built entirely from the diag hook's
    // tallies above - no second candidate/epoch/alive/gate walk. mLatticeCount (S9, exposed to the V7 console) still
    // needs its own enumerateLattice call since resolveActive's internal enumeration is not returned to the caller,
    // but that is lattice geometry, not a candidate traversal; mCandidates is arithmetic (lattice cells x epochs in
    // the window), since every (cell, epoch) pair is a candidate slot whether or not it turned out alive.
    WhyNot why;
    {
        S32 lx[LATTICE_CAP];
        S32 ly[LATTICE_CAP];
        mLatticeCount = llmin(enumerateLattice(mAnchor, FIELD_M, lx, ly, LATTICE_CAP), LATTICE_CAP);

        const S64 epoch_first = epochOf(mNow - (F64)LIFE_MAX_S);
        const S64 epoch_last = mEpochNow;
        why.mNextEpochInS = (F64)(epoch_last + 1) * EPOCH_S - mNow;
        why.mCandidates = mLatticeCount * (S32)(epoch_last - epoch_first + 1);
        why.mAlive = alive_count;
        why.mSpawned = spawned_count;
        why.mSupercells = supercell_count;
        why.mTornadoEligible = tornado_count;

        // The "why not" readout for the best-scoring alive candidate.
        if (have_best)
        {
            const Candidate& c = best;
            const WeatherAtBirth& w = best_weather;
            why.mHaveCandidate = true;
            why.mId = c.mId;
            why.mBirthTime = c.mBirthTime;
            why.mAge01 = age01(c, mNow);
            why.mScore = best_score;
            why.mPotential = c.mPotential;
            why.mConvection = w.mConvection;
            why.mMoisture = w.mMoisture;
            why.mShearNoise = c.mShearNoise;
            why.mShearStrength = w.mShearStrength;
            why.mRotation = c.mRotation;
            why.mRotationTerm = rotationTerm(c, w);
            why.mLifetimeS = c.mLifetimeS;
            why.mAllowSupercells = w.mAllowSupercells;
            why.mAllowTornadoes = w.mAllowTornadoes;
            why.mGate = gate(c, w);

            if (!mHaveHero)
            {
                if (!why.mGate.mSpawn)
                {
                    why.mFailing.push_back(llformat("score %.3f < %.2f (P %.2f x conv %.2f x moist %.2f x (0.5 + 0.5 x SH %.2f))",
                                                    why.mScore, SPAWN_THRESHOLD, why.mPotential, why.mConvection,
                                                    why.mMoisture, why.mShearNoise));
                }
                else if (!why.mGate.mSupercell)
                {
                    if (!w.mAllowSupercells)
                    {
                        why.mFailing.push_back("Allow Supercells off");
                    }
                    else
                    {
                        why.mFailing.push_back(llformat("|rotation| %.2f x S %.2f = %.3f < %.2f",
                                                        std::fabs(why.mRotation), why.mShearStrength,
                                                        why.mRotationTerm, SUPERCELL_ROT_MIN));
                    }
                }
                else if (!why.mGate.mTornadoEligible)
                {
                    if (!w.mAllowTornadoes)
                    {
                        why.mFailing.push_back("Allow Tornadoes off");
                    }
                    else
                    {
                        why.mFailing.push_back(llformat("lifetime %.0f s < %.0f s", why.mLifetimeS, HERO_MIN_LIFE_S));
                    }
                }
                why.mFailing.push_back(llformat("next epoch in %.1f min", why.mNextEpochInS / 60.0));
            }
        }
        else
        {
            why.mFailing.push_back("no candidate alive in the window");
            why.mFailing.push_back(llformat("next epoch in %.1f min", why.mNextEpochInS / 60.0));
        }
    }
    mWhyNot = why;

    mTrack = nullptr;
    mValid = true;
}
