/**
 * @file sscombatanalysis.cpp
 * @brief Stage 0 derived data for the Combat Log. Everything here is the simplest honest version of a
 *        section of doc/combat_log_analysis.md; the real solvers replace it in Stage 2.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatanalysis.h"

#include "llformat.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>

using namespace SSCombat;

namespace
{
    // Two events belong to the same engagement when they are this close in space and in time.
    constexpr F32 SS_ENGAGE_RADIUS  = 40.f;
    constexpr F64 SS_ENGAGE_GAP     = 30.0;
    // The active-combatant test's expiry, verbatim from the owner's own tracker board.
    constexpr F64 SS_COMBATANT_HOLD = 120.0;
    // Nothing here is meant to run on a deathmatch of hundreds; above this the team solve gives up.
    constexpr size_t SS_TEAM_MAX_NODES = 256;

    static const std::vector<Life> sNoLives;

    // One fixed palette, so a side keeps its colour between the overlay, the timeline and the lists.
    const LLColor4 SS_TEAM_PALETTE[] =
    {
        LLColor4(0.35f, 0.62f, 1.00f, 1.f),   // Side A
        LLColor4(1.00f, 0.45f, 0.35f, 1.f),   // Side B
        LLColor4(0.55f, 0.85f, 0.45f, 1.f),   // Side C
        LLColor4(0.95f, 0.80f, 0.35f, 1.f),   // Side D
        LLColor4(0.75f, 0.55f, 0.95f, 1.f)    // Side E
    };
    constexpr S32 SS_TEAM_PALETTE_SIZE = (S32)(sizeof(SS_TEAM_PALETTE) / sizeof(SS_TEAM_PALETTE[0]));

    // Horizontal distance; engagements are ground clusters and a sniper's tower must not split one.
    F32 ss_dist2d(const LLVector3& a, const LLVector3& b)
    {
        const F32 dx = a.mV[VX] - b.mV[VX];
        const F32 dy = a.mV[VY] - b.mV[VY];
        return (F32)sqrt(dx * dx + dy * dy);
    }

    // True for the kinds that carry combat weight.
    bool ss_is_combat_kind(U8 kind)
    {
        return kind == EVENT_DAMAGE || kind == EVENT_DEATH || kind == EVENT_OBJECT_DEATH;
    }
}

// Binds the analysis to its store.
SSCombatAnalysis::SSCombatAnalysis(SSCombatLog& log)
:   mLog(log)
{
}

// Nothing owned outside the caches.
SSCombatAnalysis::~SSCombatAnalysis()
{
}

// Drops every cache; the next question rebuilds what it needs.
void SSCombatAnalysis::invalidate()
{
    mEngagementsValid = false;
    mLivesValid = false;
    mTeamsValid = false;
    mCombatantsValid = false;
}

// The synthetic generator's known sides, used as FIRM seeds.
void SSCombatAnalysis::setTeamHint(const LLUUID& combatant, S8 team)
{
    mTeamHints[combatant] = team;
    mTeamsValid = false;
}

// Forgets the seeded sides.
void SSCombatAnalysis::clearTeamHints()
{
    mTeamHints.clear();
    mTeamsValid = false;
}

// Where the hit landed: the death's own position when it has one, else the target's track at that moment.
bool SSCombatAnalysis::eventPosition(const Event& ev, LLVector3& out) const
{
    if (ev.mHasPositions)
    {
        out = ev.mTargetPos;
        return true;
    }
    Sample s;
    if (!ev.mTarget.isNull() && mLog.sampleAt(ev.mTarget, ev.mTime, s))
    {
        out = s.mPos;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Engagements
// ---------------------------------------------------------------------------

// Greedy spatiotemporal clustering of every event that has a position, then a merge pass over overlaps.
void SSCombatAnalysis::buildEngagements()
{
    mEngagements.clear();
    mEngagementsValid = true;

    const std::vector<Event>& events = mLog.events();
    std::vector<LLVector3> sums;   // running position sums, parallel to mEngagements
    std::vector<F32>       counts;

    for (const Event& ev : events)
    {
        if (!ss_is_combat_kind(ev.mKind))
        {
            continue;
        }
        LLVector3 pos;
        if (!eventPosition(ev, pos))
        {
            continue; // no position, no place; it stays in the unattributed count
        }

        S32 best = -1;
        F32 best_dist = SS_ENGAGE_RADIUS;
        for (size_t i = 0; i < mEngagements.size(); ++i)
        {
            if (ev.mTime - mEngagements[i].mEnd > SS_ENGAGE_GAP)
            {
                continue;
            }
            const F32 d = ss_dist2d(pos, mEngagements[i].mCentre);
            if (d <= best_dist)
            {
                best_dist = d;
                best = (S32)i;
            }
        }
        if (best < 0)
        {
            Engagement e;
            e.mStart = ev.mTime;
            e.mEnd = ev.mTime;
            e.mCentre = pos;
            mEngagements.push_back(e);
            sums.push_back(pos);
            counts.push_back(1.f);
            best = (S32)mEngagements.size() - 1;
        }
        else
        {
            const size_t k = (size_t)best;
            sums[k] += pos;
            counts[k] += 1.f;
            mEngagements[k].mCentre = sums[k] / counts[k];
            mEngagements[k].mEnd = llmax(mEngagements[k].mEnd, ev.mTime);
        }
        mEngagements[(size_t)best].mEvents.push_back(ev.mId);
    }

    // Merge clusters that ended up overlapping in both time and space.
    bool merged = true;
    while (merged && mEngagements.size() > 1)
    {
        merged = false;
        for (size_t i = 0; i < mEngagements.size() && !merged; ++i)
        {
            for (size_t j = i + 1; j < mEngagements.size(); ++j)
            {
                const bool overlap = mEngagements[i].mStart <= mEngagements[j].mEnd
                                  && mEngagements[j].mStart <= mEngagements[i].mEnd;
                if (!overlap || ss_dist2d(mEngagements[i].mCentre, mEngagements[j].mCentre) > SS_ENGAGE_RADIUS)
                {
                    continue;
                }
                mEngagements[i].mStart = llmin(mEngagements[i].mStart, mEngagements[j].mStart);
                mEngagements[i].mEnd   = llmax(mEngagements[i].mEnd,   mEngagements[j].mEnd);
                mEngagements[i].mEvents.insert(mEngagements[i].mEvents.end(),
                                               mEngagements[j].mEvents.begin(), mEngagements[j].mEvents.end());
                sums[i] += sums[j];
                counts[i] += counts[j];
                mEngagements[i].mCentre = sums[i] / counts[i];
                mEngagements.erase(mEngagements.begin() + (std::ptrdiff_t)j);
                sums.erase(sums.begin() + (std::ptrdiff_t)j);
                counts.erase(counts.begin() + (std::ptrdiff_t)j);
                merged = true;
                break;
            }
        }
    }

    // Radius, combatant list and the flat label the design insists on (never a coined name).
    for (size_t i = 0; i < mEngagements.size(); ++i)
    {
        Engagement& e = mEngagements[i];
        e.mId = (U32)i + 1;
        e.mLabel = llformat("Engagement %u", e.mId);
        std::sort(e.mEvents.begin(), e.mEvents.end());
        for (U32 id : e.mEvents)
        {
            const Event* ev = mLog.event(id);
            if (!ev)
            {
                continue;
            }
            LLVector3 pos;
            if (eventPosition(*ev, pos))
            {
                e.mRadius = llmax(e.mRadius, ss_dist2d(pos, e.mCentre));
            }
            if (!ev->mOwner.isNull()
                && std::find(e.mCombatants.begin(), e.mCombatants.end(), ev->mOwner) == e.mCombatants.end())
            {
                e.mCombatants.push_back(ev->mOwner);
            }
            if (ev->mTargetIsAgent && !ev->mTarget.isNull()
                && std::find(e.mCombatants.begin(), e.mCombatants.end(), ev->mTarget) == e.mCombatants.end())
            {
                e.mCombatants.push_back(ev->mTarget);
            }
        }
    }
}

// The session's engagements, rebuilt on demand.
const std::vector<Engagement>& SSCombatAnalysis::engagements()
{
    if (!mEngagementsValid)
    {
        buildEngagements();
    }
    return mEngagements;
}

// ---------------------------------------------------------------------------
// Lives
// ---------------------------------------------------------------------------

// One life per combatant per death, opened at first sight and closed at the next DEATH.
void SSCombatAnalysis::buildLives()
{
    mLives.clear();
    mLivesValid = true;

    const std::vector<Event>& events = mLog.events();
    const std::vector<LLUUID> people = combatants();
    const F64 session_start = mLog.sessionStart();
    const F64 session_end = mLog.sessionEnd();

    for (const LLUUID& id : people)
    {
        // First sight: the earliest of a track sample and an event that names them.
        F64 first_seen = session_end;
        if (const Track* track = mLog.track(id))
        {
            if (!track->mSamples.empty())
            {
                first_seen = llmin(first_seen, session_start + (F64)track->mSamples.front().mTime);
            }
        }
        std::vector<const Event*> deaths;
        for (const Event& ev : events)
        {
            if (ev.mOwner == id || ev.mTarget == id)
            {
                first_seen = llmin(first_seen, ev.mTime);
            }
            if (ev.mKind == EVENT_DEATH && ev.mTarget == id)
            {
                deaths.push_back(&ev);
            }
        }
        if (deaths.empty() && first_seen >= session_end)
        {
            continue;
        }

        std::vector<Life>& lives_out = mLives[id];
        F64 open = first_seen;
        for (const Event* death : deaths)
        {
            Life life;
            life.mCombatant = id;
            life.mStart = open;
            life.mEnd = death->mTime;
            life.mDeathEvent = death->mId;
            // The teleport that usually follows a death is its own signal (analysis 5.7): look, never assume.
            if (const Track* track = mLog.track(id))
            {
                for (const Sample& s : track->mSamples)
                {
                    const F64 t = session_start + (F64)s.mTime;
                    if (t < death->mTime)
                    {
                        continue;
                    }
                    if (t > death->mTime + 6.0)
                    {
                        break;
                    }
                    if (s.mFlags & FLAG_TELEPORT)
                    {
                        life.mTeleportSeen = true;
                        break;
                    }
                }
            }
            lives_out.push_back(life);
            open = death->mTime;
        }
        Life last;
        last.mCombatant = id;
        last.mStart = open;
        last.mEnd = session_end;
        lives_out.push_back(last);
    }
}

// Every life this combatant lived; an unknown combatant has none.
const std::vector<Life>& SSCombatAnalysis::lives(const LLUUID& combatant)
{
    if (!mLivesValid)
    {
        buildLives();
    }
    auto it = mLives.find(combatant);
    return it == mLives.end() ? sNoLives : it->second;
}

// ---------------------------------------------------------------------------
// Attribution
// ---------------------------------------------------------------------------

// Damage on the victim inside the window, grouped by (owner, rezzer), with the killing blow and its ambiguity.
Attribution SSCombatAnalysis::attribution(U32 deathEvent)
{
    Attribution out;
    out.mDeath = deathEvent;

    const Event* death = mLog.event(deathEvent);
    if (!death || (death->mKind != EVENT_DEATH && death->mKind != EVENT_OBJECT_DEATH))
    {
        return out;
    }
    static LLCachedControl<F32> window(gSavedSettings, "SSCombatLogDeathWindowSeconds", 20.f);
    out.mWindow = llmax(1.f, (F32)window);

    const F64 from = death->mTime - (F64)out.mWindow;
    const std::vector<Event>& events = mLog.events();

    const Event* blow = nullptr;
    const Event* blow_alt = nullptr;
    F32 total = 0.f;

    size_t i = mLog.eventIndexAt(from);
    for (; i < events.size(); ++i)
    {
        const Event& ev = events[i];
        if (ev.mTime > death->mTime)
        {
            break;
        }
        if (ev.mKind != EVENT_DAMAGE || ev.mTarget != death->mTarget)
        {
            continue;
        }
        if (ev.mTrust == TRUST_FOREIGN)
        {
            continue; // untrusted lines stay visible but carry no weight (analysis 5.4)
        }

        VolleyShare* share = nullptr;
        for (VolleyShare& s : out.mShares)
        {
            if (s.mAttacker == ev.mOwner && s.mRezzer == ev.mRezzer)
            {
                share = &s;
                break;
            }
        }
        if (!share)
        {
            VolleyShare fresh;
            fresh.mAttacker = ev.mOwner;
            fresh.mRezzer = ev.mRezzer;
            fresh.mFirstEvent = ev.mId;
            out.mShares.push_back(fresh);
            share = &out.mShares.back();
        }
        share->mDamage += ev.mDamage;
        share->mLastEvent = ev.mId;
        total += ev.mDamage;

        blow_alt = blow;
        blow = &ev;
    }

    if (total > 0.f)
    {
        for (VolleyShare& s : out.mShares)
        {
            s.mCredit = s.mDamage / total;
        }
        std::sort(out.mShares.begin(), out.mShares.end(),
                  [](const VolleyShare& a, const VolleyShare& b) { return a.mDamage > b.mDamage; });
    }

    if (blow)
    {
        out.mBlow = blow->mId;
        out.mBlowState = BLOW_UNAMBIGUOUS;
        if (blow_alt && blow_alt->mOwner != blow->mOwner)
        {
            // No ordering claim is made between two stamps whose uncertainty intervals overlap (analysis 5.1).
            const F64 margin = blow->mTime - blow_alt->mTime;
            if (margin <= (F64)(blow->mUncertainty + blow_alt->mUncertainty))
            {
                out.mBlowAlt = blow_alt->mId;
                out.mBlowState = BLOW_AMBIGUOUS_PAIR;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Teams
// ---------------------------------------------------------------------------

// Seeds from the synthetic hints where they exist, otherwise propagates labels over the hostility graph.
void SSCombatAnalysis::buildTeams()
{
    mTeams.clear();
    mTeamNames.clear();
    mTeamCount = 0;
    mTeamsValid = true;

    const std::vector<LLUUID> people = combatants();
    const size_t n = people.size();
    if (n == 0)
    {
        return;
    }
    if (n > SS_TEAM_MAX_NODES)
    {
        LL_WARNS("CombatLog") << "team solve skipped: " << n << " combatants is past the Stage 0 ceiling" << LL_ENDL;
        return;
    }

    std::map<LLUUID, size_t> index;
    for (size_t i = 0; i < n; ++i)
    {
        index[people[i]] = i;
    }

    // Symmetric hostility graph: who damaged whom, and how much.
    std::vector<F32> w(n * n, 0.f);
    for (const Event& ev : mLog.events())
    {
        if (ev.mKind != EVENT_DAMAGE || ev.mTrust == TRUST_FOREIGN || !ev.mTargetIsAgent)
        {
            continue;
        }
        auto a = index.find(ev.mOwner);
        auto b = index.find(ev.mTarget);
        if (a == index.end() || b == index.end() || a->second == b->second)
        {
            continue;
        }
        const F32 weight = llmax(1.f, ev.mDamage);
        w[a->second * n + b->second] += weight;
        w[b->second * n + a->second] += weight;
    }

    std::vector<S8> assign(n, (S8)-1);
    std::vector<bool> pinned(n, false);
    S32 team_count = 0;

    // The generator knows the sides it wrote; a hint is a hard seed.
    for (size_t i = 0; i < n; ++i)
    {
        auto hint = mTeamHints.find(people[i]);
        if (hint != mTeamHints.end() && hint->second >= 0)
        {
            assign[i] = hint->second;
            pinned[i] = true;
            team_count = llmax(team_count, (S32)hint->second + 1);
        }
    }

    // The groups registry would seed here, but Stage 0 has no avatar-to-group data (no active tag, no
    // attachment group), so the hostility graph carries the solve on its own.
    if (team_count == 0)
    {
        F32 best = 0.f;
        size_t bi = 0, bj = 0;
        for (size_t i = 0; i < n; ++i)
        {
            for (size_t j = i + 1; j < n; ++j)
            {
                if (w[i * n + j] > best)
                {
                    best = w[i * n + j];
                    bi = i;
                    bj = j;
                }
            }
        }
        if (best <= 0.f)
        {
            return; // nobody damaged anybody; there are no sides to find
        }
        assign[bi] = 0;
        assign[bj] = 1;
        team_count = 2;
    }
    if (team_count < 2)
    {
        team_count = 2; // one seeded side still implies an opposing one
    }

    // Two-way label propagation: a node joins the side it fights least.
    for (S32 pass = 0; pass < 16; ++pass)
    {
        bool changed = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (pinned[i])
            {
                continue;
            }
            std::vector<F32> score((size_t)team_count, 0.f);
            F32 edges = 0.f;
            for (size_t j = 0; j < n; ++j)
            {
                const F32 weight = w[i * n + j];
                if (weight <= 0.f || assign[j] < 0 || assign[j] >= (S8)team_count)
                {
                    continue;
                }
                score[(size_t)assign[j]] += weight;
                edges += weight;
            }
            if (edges <= 0.f)
            {
                continue; // nothing to go on yet
            }
            S8 pick = 0;
            for (S8 k = 1; k < (S8)team_count; ++k)
            {
                if (score[(size_t)k] < score[(size_t)pick])
                {
                    pick = k;
                }
            }
            if (assign[i] != pick)
            {
                assign[i] = pick;
                changed = true;
            }
        }
        if (!changed)
        {
            break;
        }
    }

    // Confidence: FIRM when seeded, LIKELY when the node clearly fights one side, WEAK otherwise.
    for (size_t i = 0; i < n; ++i)
    {
        TeamAssignment ta;
        ta.mTeam = assign[i];
        if (pinned[i])
        {
            ta.mConfidence = CONF_FIRM;
        }
        else if (assign[i] >= 0)
        {
            std::vector<F32> score((size_t)team_count, 0.f);
            for (size_t j = 0; j < n; ++j)
            {
                if (w[i * n + j] > 0.f && assign[j] >= 0 && assign[j] < (S8)team_count)
                {
                    score[(size_t)assign[j]] += w[i * n + j];
                }
            }
            F32 mine = score[(size_t)assign[i]];
            F32 other = 0.f;
            for (S8 k = 0; k < (S8)team_count; ++k)
            {
                if (k != assign[i])
                {
                    other = llmax(other, score[(size_t)k]);
                }
            }
            ta.mConfidence = (other > mine * 2.f + 1.f) ? CONF_LIKELY : CONF_WEAK;
        }
        else
        {
            ta.mConfidence = CONF_UNDETERMINED;
        }
        mTeams[people[i]] = ta;
        if (ta.mTeam >= 0)
        {
            mTeamCount = llmax(mTeamCount, (S32)ta.mTeam + 1);
        }
    }
    buildTeamNames();
}

// Side A / B / C, unless the army registry names the group this side's equipment is set to.
void SSCombatAnalysis::buildTeamNames()
{
    mTeamNames.assign((size_t)llmax(0, mTeamCount), std::string());
    const std::vector<GroupInfo>& registry = mLog.groups();
    for (S32 side = 0; side < mTeamCount; ++side)
    {
        std::string name = llformat("Side %c", (char)('A' + (side % 26)));
        if (!registry.empty())
        {
            // Tally the groups of gear owned by this side's members; the modal one names the army.
            std::map<LLUUID, S32> tally;
            for (const auto& eq : mLog.equipment())
            {
                if (eq.second.mGroup.isNull() || eq.second.mKind == EQUIP_PROJECTILE)
                {
                    continue; // a bullet is not a loadout
                }
                auto member = mTeams.find(eq.second.mOwner);
                if (member != mTeams.end() && member->second.mTeam == (S8)side)
                {
                    ++tally[eq.second.mGroup];
                }
            }
            LLUUID modal;
            S32 best = 0;
            for (const auto& t : tally)
            {
                if (t.second > best)
                {
                    best = t.second;
                    modal = t.first;
                }
            }
            if (!modal.isNull())
            {
                for (const GroupInfo& g : registry)
                {
                    if (modal == g.mGroup || modal == g.mMilitia || modal == g.mCivilians || modal == g.mLand)
                    {
                        name = g.mName.empty() ? g.mShort : g.mName;
                        break;
                    }
                }
            }
        }
        mTeamNames[(size_t)side] = name;
    }
}

// Which side this combatant is on.
TeamAssignment SSCombatAnalysis::team(const LLUUID& combatant)
{
    if (!mTeamsValid)
    {
        buildTeams();
    }
    auto it = mTeams.find(combatant);
    return it == mTeams.end() ? TeamAssignment() : it->second;
}

// How many sides the solve found.
S32 SSCombatAnalysis::teamCount()
{
    if (!mTeamsValid)
    {
        buildTeams();
    }
    return mTeamCount;
}

// The label for a side, from the cache the solve built.
std::string SSCombatAnalysis::teamName(S8 team_id)
{
    if (team_id < 0)
    {
        return "Unassigned";
    }
    if (!mTeamsValid)
    {
        buildTeams();
    }
    if ((size_t)team_id < mTeamNames.size())
    {
        return mTeamNames[(size_t)team_id];
    }
    return llformat("Side %c", (char)('A' + (team_id % 26)));
}

// The colour for a side; unassigned is grey so it never reads as a third army.
LLColor4 SSCombatAnalysis::teamColor(S8 team_id)
{
    if (team_id < 0)
    {
        return LLColor4(0.65f, 0.65f, 0.65f, 1.f);
    }
    return SS_TEAM_PALETTE[team_id % SS_TEAM_PALETTE_SIZE];
}

// ---------------------------------------------------------------------------
// Combatants
// ---------------------------------------------------------------------------

// Everyone who owned a combat event or was hit by one, plus anyone the generator named.
void SSCombatAnalysis::buildCombatants()
{
    mCombatants.clear();
    mCombatantsValid = true;

    for (const Event& ev : mLog.events())
    {
        if (!ss_is_combat_kind(ev.mKind))
        {
            continue;
        }
        if (!ev.mOwner.isNull())
        {
            mCombatants.push_back(ev.mOwner);
        }
        if (ev.mTargetIsAgent && !ev.mTarget.isNull())
        {
            mCombatants.push_back(ev.mTarget);
        }
    }
    for (const auto& hint : mTeamHints)
    {
        mCombatants.push_back(hint.first);
    }
    std::sort(mCombatants.begin(), mCombatants.end());
    mCombatants.erase(std::unique(mCombatants.begin(), mCombatants.end()), mCombatants.end());
}

// Everyone who fought.
std::vector<LLUUID> SSCombatAnalysis::combatants()
{
    if (!mCombatantsValid)
    {
        buildCombatants();
    }
    return mCombatants;
}

// The owner's own test: recent damage activity, or mouselook / seated at that moment (glossary 1.1).
bool SSCombatAnalysis::isCombatantAt(const LLUUID& id, F64 t)
{
    if (id.isNull())
    {
        return false;
    }
    const std::vector<Event>& events = mLog.events();
    size_t i = mLog.eventIndexAt(t - SS_COMBATANT_HOLD);
    for (; i < events.size(); ++i)
    {
        const Event& ev = events[i];
        if (ev.mTime > t)
        {
            break;
        }
        if (ss_is_combat_kind(ev.mKind) && ev.mOwner == id)
        {
            return true;
        }
    }
    Sample s;
    if (mLog.sampleAt(id, t, s))
    {
        // The parcel damage flag is the other half of the test; Stage 0 has no parcel data and ignores it.
        if (s.mFlags & (FLAG_MOUSELOOK | FLAG_ON_OBJECT))
        {
            return true;
        }
    }
    return false;
}
