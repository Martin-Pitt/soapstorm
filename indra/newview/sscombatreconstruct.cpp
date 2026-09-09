/**
 * @file sscombatreconstruct.cpp
 * @brief Combat Log Reconstruction: the ghosted replay of one death (doc/combat_log_ux.md 3.10).
 *        Opening it resamples the tracks of everyone with a damage edge into or out of the death, plus anyone
 *        inside the engagement at that time, into a 10 Hz ring of prepared frames, so scrubbing costs an array
 *        index. It runs zero raycasts: every geometric claim was computed before it opened, and the killer's
 *        line draws neutral grey captioned "not tested" until a sweep has actually run.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatreconstruct.h"

#include "sscombatlog.h"
#include "sscombatoverlay.h"

#include "indra_constants.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglstates.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrootview.h"
#include "llslider.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

// ---------------------------------------------------------------------------------------------------------
// Palette. Reconstruction reuses the overlay's meanings and adds only the ones it alone needs.
// ---------------------------------------------------------------------------------------------------------

// A ghost that is neither the victim nor the killer: present, but not the subject.
static const LLColor4 COL_GHOST(0.78f, 0.80f, 0.86f, 1.f);
// The victim's ghost.
static const LLColor4 COL_GHOST_VICTIM(1.00f, 0.42f, 0.36f, 1.f);
// The killer's ghost.
static const LLColor4 COL_GHOST_KILLER(1.00f, 0.72f, 0.30f, 1.f);
// A geometric claim Stage 0 never tested; the killer's line wears this until a sweep has run.
static const LLColor4 COL_UNTESTED(0.64f, 0.64f, 0.67f, 1.f);
// Ghost labels.
static const LLColor4 COL_LABEL(0.93f, 0.93f, 0.96f, 1.f);
// The health the estimate still credits the victim with; hatched in the drawing because it is estimated.
static const LLColor4 COL_HEALTH(0.34f, 0.86f, 0.40f, 1.f);
// The health the estimate says is already gone.
static const LLColor4 COL_HEALTH_LOST(0.52f, 0.16f, 0.16f, 1.f);
// A measured projectile flight, solid because it was seen.
static const LLColor4 COL_FLIGHT(0.96f, 0.92f, 0.72f, 1.f);
// The head of a flight that is in the air at the current instant.
static const LLColor4 COL_FLIGHT_HEAD(1.00f, 0.98f, 0.86f, 1.f);

// The frame ring's rate; 10 Hz over an eight-second window is 80 frames per ghost.
static const F64 RECON_HZ = 10.0;
// The budget of ux 3.10: at most 24 ghosts, which is the order of a busy Engagement and cheaper.
static const size_t RECON_MAX_GHOSTS = 24;
// Fallback window when the store has not set one; ux 3.10 fixes it at [t - 6 s, t + 2 s].
static const F64 RECON_PRE = 6.0;
static const F64 RECON_POST = 2.0;

// ---------------------------------------------------------------------------------------------------------
// Prepared frames.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // One combatant resampled over the window. mValid is separate from mFrames because a track gap must draw
    // hollow and stop rather than sliding across the missing seconds.
    struct Ghost
    {
        LLUUID                      mId;
        bool                        mVictim = false;
        bool                        mKiller = false;
        std::vector<SSCombat::Sample> mFrames;
        std::vector<bool>           mValid;
    };

    std::vector<Ghost>  sGhosts;
    U32                 sDeath = 0;
    F64                 sStart = 0.0;
    F64                 sEnd = 0.0;
    F64                 sDeathTime = 0.0;
    LLUUID              sVictim;
    LLUUID              sKiller;
    bool                sLoop = false;
    F64                 sLastAdvance = 0.0;
    U32                 sAdvanceFrame = 0xFFFFFFFFu;

    LLPanel*            sPanel = NULL;
    bool                sSyncingWidgets = false;
}

// The number of prepared frames in the window.
static S32 frame_count()
{
    return llmax(2, (S32)((sEnd - sStart) * RECON_HZ) + 1);
}

// The prepared-frame index for a time, clamped into the window; the transport never leaves it.
static S32 frame_index(F64 t)
{
    return llclamp((S32)((t - sStart) * RECON_HZ + 0.5), 0, frame_count() - 1);
}

// The reconstruction's own clock: the shared cursor, clamped to the window while the mode is on.
static F64 recon_time()
{
    return llclamp(SSCombatLog::instance().view().mCursor, sStart, sEnd);
}

// Resamples one combatant into the ring.
static void build_ghost(const SSCombatLog& store, const LLUUID& id, bool victim, bool killer)
{
    Ghost ghost;
    ghost.mId = id;
    ghost.mVictim = victim;
    ghost.mKiller = killer;
    const S32 frames = frame_count();
    ghost.mFrames.resize((size_t)frames);
    ghost.mValid.resize((size_t)frames, false);
    bool any = false;
    for (S32 i = 0; i < frames; ++i)
    {
        const F64 t = sStart + (F64)i / RECON_HZ;
        SSCombat::Sample sample;
        if (store.sampleAt(id, t, sample))
        {
            ghost.mFrames[(size_t)i] = sample;
            ghost.mValid[(size_t)i] = true;
            any = true;
        }
    }
    if (any || victim || killer)
    {
        sGhosts.push_back(ghost);
    }
}

// Collects the cast and resamples it. Everyone with a damage edge into or out of the death, plus anyone the
// containing engagement had inside its radius at the time, capped at the ghost budget.
static void build_frames(U32 death_event)
{
    sGhosts.clear();
    const SSCombatLog& store = SSCombatLog::instance();
    const SSCombat::Event* death = store.event(death_event);
    if (!death)
    {
        return;
    }

    sDeath = death_event;
    sDeathTime = death->mTime;
    sVictim = death->mTarget;
    sKiller = death->mOwner;

    const SSCombat::View& view = store.view();
    sStart = view.mReconStart;
    sEnd = view.mReconEnd;
    if (sEnd <= sStart)
    {
        sStart = sDeathTime - RECON_PRE;
        sEnd = sDeathTime + RECON_POST;
    }

    std::vector<LLUUID> cast;
    std::set<LLUUID> seen;
    auto add = [&cast, &seen](const LLUUID& id)
    {
        if (id.notNull() && seen.insert(id).second)
        {
            cast.push_back(id);
        }
    };

    add(sVictim);
    add(sKiller);

    // Damage edges into and out of the death, from the attribution the store already computed.
    const SSCombat::Attribution attribution = store.attribution(death_event);
    for (const SSCombat::VolleyShare& share : attribution.mShares)
    {
        add(share.mAttacker);
    }
    // Anyone the victim was shooting at inside the window is an edge out of the death.
    const size_t first = store.eventIndexAt(sStart);
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = first; i < events.size(); ++i)
    {
        const SSCombat::Event& ev = events[i];
        if (ev.mTime > sEnd)
        {
            break;
        }
        if (ev.mKind == SSCombat::EVENT_DAMAGE && ev.mOwner == sVictim)
        {
            add(ev.mTarget);
        }
    }
    // Everyone inside the engagement that contains the death.
    for (const SSCombat::Engagement& eng : store.engagements())
    {
        if (sDeathTime < eng.mStart || sDeathTime > eng.mEnd)
        {
            continue;
        }
        for (const LLUUID& id : eng.mCombatants)
        {
            add(id);
        }
    }

    if (cast.size() > RECON_MAX_GHOSTS)
    {
        cast.resize(RECON_MAX_GHOSTS);
    }
    for (const LLUUID& id : cast)
    {
        build_ghost(store, id, id == sVictim, id == sKiller && id != sVictim);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Health estimate (analysis 5.25 in its Stage 0 form: 100 minus cumulative damage, plus regen).
// ---------------------------------------------------------------------------------------------------------

// The victim's estimated health at a time, walking their damage from the start of the life that ended here.
static F32 estimate_health(const SSCombatLog& store, F64 at)
{
    F64 life_start = sStart;
    for (const SSCombat::Life& life : store.lives(sVictim))
    {
        if (life.mDeathEvent == sDeath)
        {
            life_start = life.mStart;
            break;
        }
    }

    const SSCombat::RegionSettings& settings = store.regionSettings();
    const F32 regen = (settings.mKnown && settings.mRestoreHealth) ? llmax(0.f, settings.mHealthRegenRate) : 0.f;

    F32 health = 100.f;
    F64 last = life_start;
    const size_t first = store.eventIndexAt(life_start);
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = first; i < events.size(); ++i)
    {
        const SSCombat::Event& ev = events[i];
        if (ev.mTime > at)
        {
            break;
        }
        if (ev.mKind != SSCombat::EVENT_DAMAGE || ev.mTarget != sVictim)
        {
            continue;
        }
        health = llmin(100.f, health + (F32)(ev.mTime - last) * regen * 100.f);
        health -= ev.mDamage;
        last = ev.mTime;
    }
    health = llmin(100.f, health + (F32)(at - last) * regen * 100.f);
    return llclamp(health, 0.f, 100.f);
}

// ---------------------------------------------------------------------------------------------------------
// Drawing.
// ---------------------------------------------------------------------------------------------------------

// A translucent capsule: a cylinder of ten segments with both caps, no depth writes, standing on the feet.
static void draw_capsule(const LLVector3& feet, F32 radius, F32 height, const LLColor4& color)
{
    const S32 segments = 10;
    const LLVector3 top(feet.mV[VX], feet.mV[VY], feet.mV[VZ] + height);
    LLVector3 prev_b(feet.mV[VX] + radius, feet.mV[VY], feet.mV[VZ]);
    LLVector3 prev_t(prev_b.mV[VX], prev_b.mV[VY], top.mV[VZ]);
    for (S32 i = 1; i <= segments; ++i)
    {
        const F32 angle = F_TWO_PI * (F32)i / (F32)segments;
        const LLVector3 cur_b(feet.mV[VX] + radius * cosf(angle), feet.mV[VY] + radius * sinf(angle), feet.mV[VZ]);
        const LLVector3 cur_t(cur_b.mV[VX], cur_b.mV[VY], top.mV[VZ]);
        SSCombatDraw::tri(prev_b, cur_b, cur_t, color);
        SSCombatDraw::tri(prev_b, cur_t, prev_t, color);
        SSCombatDraw::tri(feet, cur_b, prev_b, color);
        SSCombatDraw::tri(top, prev_t, cur_t, color);
        prev_b = cur_b;
        prev_t = cur_t;
    }
}

// One ghost at the current instant: capsule (or a hollow ring across a track gap), yaw arrow, label, pick rect.
static void draw_ghost(const SSCombatLog& store, const Ghost& ghost, S32 index, F64 t)
{
    LLColor4 color = ghost.mVictim ? COL_GHOST_VICTIM : (ghost.mKiller ? COL_GHOST_KILLER : COL_GHOST);
    // Victim and killer at 60 %, everyone else at 25 %; the region stays readable underneath either way.
    color.mV[VALPHA] = (ghost.mVictim || ghost.mKiller) ? 0.60f : 0.25f;

    if (index < 0 || index >= (S32)ghost.mValid.size())
    {
        return;
    }
    if (!ghost.mValid[(size_t)index])
    {
        // A track gap draws hollow and stops rather than sliding a body across seconds nobody saw: the last
        // place this combatant was actually seen keeps a ring, and nothing else is claimed about them.
        S32 last = -1;
        for (S32 i = index; i >= 0; --i)
        {
            if (ghost.mValid[(size_t)i])
            {
                last = i;
                break;
            }
        }
        if (last < 0)
        {
            return;
        }
        LLColor4 hollow = color;
        hollow.mV[VALPHA] = color.mV[VALPHA] * 0.5f;
        const LLVector3 stopped = SSCombatDraw::agentFromRegion(ghost.mFrames[(size_t)last].mPos);
        SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, stopped, 0.42f, hollow, SSCombatDraw::WIDTH_THIN);
        SSCombatDraw::pushLabel(store.displayName(ghost.mId) + " ?", stopped + LLVector3(0.f, 0.f, 2.05f), hollow);
        return;
    }
    const SSCombat::Sample& sample = ghost.mFrames[(size_t)index];
    const LLVector3 feet = SSCombatDraw::agentFromRegion(sample.mPos);

    draw_capsule(feet, 0.32f, 1.80f, color);

    LLColor4 outline = color;
    outline.mV[VALPHA] = llmin(1.f, color.mV[VALPHA] + 0.30f);
    SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, feet, 0.42f, outline, SSCombatDraw::WIDTH_THIN);
    SSCombatDraw::arrow(SSCombatDraw::LAYER_MARKER, feet + LLVector3(0.f, 0.f, 0.05f), sample.mYaw, 1.1f, outline, SSCombatDraw::WIDTH_MID);
    if (sample.mFlags & SSCombat::FLAG_MOUSELOOK)
    {
        SSCombatDraw::eyeGlyph(feet + LLVector3(0.f, 0.f, 2.35f), 0.22f, outline);
    }

    SSCombatDraw::pushLabel(store.displayName(ghost.mId), feet + LLVector3(0.f, 0.f, 2.05f), COL_LABEL);

    // Ghosts push their rects like any other marker, so the pick stack resolves overlapping ghosts exactly
    // as it resolves live ones (ux 3.10, 4.6).
    SSCombat::NounRef ref;
    ref.mType = SSCombat::NOUN_COMBATANT;
    ref.mId = ghost.mId;
    ref.mTime = t;
    SSCombatDraw::pushPick(feet + LLVector3(0.f, 0.f, 1.f), 12, ref);
}

// The bullet flights whose measured span touches the window, animated along their own polylines at their own
// measured speed: the drawn head is wherever the path says the round was at this instant.
static void draw_flights(const SSCombatLog& store, F64 t)
{
    const F64 session = store.sessionStart();
    for (const SSCombat::Flight& flight : store.flights())
    {
        if (flight.mEnd < sStart || flight.mStart > sEnd || flight.mPath.size() < 2)
        {
            continue;
        }
        // Path times are session seconds; the transport is in viewer seconds.
        const F64 head_t = t - session;
        bool drew_head = false;
        for (size_t i = 1; i < flight.mPath.size(); ++i)
        {
            const F64 t0 = (F64)flight.mPath[i - 1].first;
            const F64 t1 = (F64)flight.mPath[i].first;
            if (t0 > head_t)
            {
                break; // the round has not reached this leg yet
            }
            const LLVector3 a = SSCombatDraw::agentFromRegion(flight.mPath[i - 1].second);
            LLVector3 b = SSCombatDraw::agentFromRegion(flight.mPath[i].second);
            if (t1 > head_t && t1 > t0)
            {
                b = lerp(a, b, (F32)((head_t - t0) / (t1 - t0)));
                drew_head = true;
            }
            SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, a, b, COL_FLIGHT, COL_FLIGHT, SSCombatDraw::WIDTH_THIN);
            if (drew_head)
            {
                SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, b, 0.16f, COL_FLIGHT_HEAD, SSCombatDraw::WIDTH_MID);
                break;
            }
        }
        if (!drew_head && flight.mHitGeometry && (F64)flight.mPath.back().first <= head_t)
        {
            // A flight with no DAMAGE keeps its terminal tick on the geometry it hit.
            SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, SSCombatDraw::agentFromRegion(flight.mPath.back().second),
                                0.20f, COL_FLIGHT, SSCombatDraw::WIDTH_THIN);
        }
    }
}

// A disc on the victim's body at each DAMAGE, sized by damage, coloured by type, fading after one second.
static void draw_hit_discs(const SSCombatLog& store, F64 t)
{
    const Ghost* victim = NULL;
    for (const Ghost& ghost : sGhosts)
    {
        if (ghost.mVictim)
        {
            victim = &ghost;
            break;
        }
    }
    if (!victim)
    {
        return;
    }

    const size_t first = store.eventIndexAt(sStart);
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = first; i < events.size(); ++i)
    {
        const SSCombat::Event& ev = events[i];
        if (ev.mTime > t)
        {
            break;
        }
        if (ev.mKind != SSCombat::EVENT_DAMAGE || ev.mTarget != sVictim)
        {
            continue;
        }
        const S32 index = frame_index(ev.mTime);
        if (index >= (S32)victim->mValid.size() || !victim->mValid[(size_t)index])
        {
            continue;
        }
        const LLVector3 feet = SSCombatDraw::agentFromRegion(victim->mFrames[(size_t)index].mPos);
        // Spread the discs up the body by event id so a burst does not stack into one z-fighting blob.
        const F32 height = 0.70f + 0.90f * (F32)(ev.mId % 5u) / 5.f;
        const LLVector3 at = feet + LLVector3(0.f, 0.f, height);

        // Sized by damage, coloured by type, fading after one second to a faint mark that stays readable.
        const F64 age = t - ev.mTime;
        LLColor4 color = SSCombatDraw::damageTypeColor(ev.mType);
        color.mV[VALPHA] = (age < 1.0) ? (F32)(1.0 - 0.75 * age) : 0.25f;
        SSCombatDraw::disc(at, llclamp(0.10f + ev.mDamage * 0.008f, 0.10f, 0.55f), color);
    }
}

// The killer's line, at full alpha because the shot is not in doubt, in neutral grey because Stage 0 has run
// no sightline analysis at all and a coloured verdict here would be an assertion nobody tested.
static void draw_killer_line(F64 t)
{
    const Ghost* victim = NULL;
    const Ghost* killer = NULL;
    for (const Ghost& ghost : sGhosts)
    {
        if (ghost.mVictim) victim = &ghost;
        if (ghost.mKiller) killer = &ghost;
    }
    if (!victim || !killer)
    {
        return;
    }
    const S32 index = frame_index(llmin(t, sDeathTime));
    if (index >= (S32)victim->mValid.size() || !victim->mValid[(size_t)index] || !killer->mValid[(size_t)index])
    {
        return;
    }
    const LLVector3 from = SSCombatDraw::agentFromRegion(killer->mFrames[(size_t)index].mPos) + LLVector3(0.f, 0.f, 1.60f);
    const LLVector3 to = SSCombatDraw::agentFromRegion(victim->mFrames[(size_t)index].mPos) + LLVector3(0.f, 0.f, 1.20f);
    LLColor4 color = COL_UNTESTED;
    color.mV[VALPHA] = 1.f;
    SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, from, to, color, color, SSCombatDraw::WIDTH_MID);
    SSCombatDraw::pushLabel("not tested", lerp(from, to, 0.5f), COL_UNTESTED);
}

// The victim's health estimate as a hatched bar over the ghost; hatched because it is an estimate, not a read.
static void draw_health_bar(const SSCombatLog& store, F64 t)
{
    const Ghost* victim = NULL;
    for (const Ghost& ghost : sGhosts)
    {
        if (ghost.mVictim)
        {
            victim = &ghost;
            break;
        }
    }
    const S32 index = frame_index(t);
    if (!victim || index >= (S32)victim->mValid.size() || !victim->mValid[(size_t)index])
    {
        return;
    }

    const F32 health = estimate_health(store, t) / 100.f;
    const LLVector3 feet = SSCombatDraw::agentFromRegion(victim->mFrames[(size_t)index].mPos);
    const LLVector3 centre = feet + LLVector3(0.f, 0.f, 2.45f);

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLVector3 right = camera->getLeftAxis() * -1.f;
    right.mV[VZ] = 0.f;
    if (right.magVecSquared() < 0.0001f)
    {
        right.setVec(1.f, 0.f, 0.f);
    }
    right.normalize();
    const LLVector3 up(0.f, 0.f, 1.f);

    const F32 half = 0.55f;
    const F32 bar_h = 0.10f;
    const LLVector3 left_end = centre - right * half;
    const LLVector3 right_end = centre + right * half;

    // Outline, then hatch: one short upright per tick, filled ticks in the health colour and spent ticks in
    // the lost colour, so the bar reads as an estimate at a glance and never as a health bar the sim gave us.
    SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, left_end + up * bar_h, right_end + up * bar_h, COL_UNTESTED, COL_UNTESTED, SSCombatDraw::WIDTH_THIN);
    SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, left_end - up * bar_h, right_end - up * bar_h, COL_UNTESTED, COL_UNTESTED, SSCombatDraw::WIDTH_THIN);
    const S32 ticks = 20;
    for (S32 i = 0; i < ticks; ++i)
    {
        const F32 f = ((F32)i + 0.5f) / (F32)ticks;
        const LLVector3 at = lerp(left_end, right_end, f);
        const LLColor4 color = (f <= health) ? COL_HEALTH : COL_HEALTH_LOST;
        SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, at - up * bar_h, at + up * bar_h, color, color, SSCombatDraw::WIDTH_THIN);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------------------------------------

// Every event time inside the window, ascending; the step buttons and keys walk exactly this list.
static std::vector<F64> window_event_times(const SSCombatLog& store)
{
    std::vector<F64> times;
    const size_t first = store.eventIndexAt(sStart);
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = first; i < events.size(); ++i)
    {
        if (events[i].mTime > sEnd)
        {
            break;
        }
        times.push_back(events[i].mTime);
    }
    return times;
}

// Moves the cursor to the previous or next event inside the window.
static void step_event(S32 direction)
{
    SSCombatLog& store = SSCombatLog::instance();
    const std::vector<F64> times = window_event_times(store);
    if (times.empty())
    {
        return;
    }
    const F64 now = recon_time();
    F64 target = now;
    if (direction < 0)
    {
        for (std::vector<F64>::const_reverse_iterator it = times.rbegin(); it != times.rend(); ++it)
        {
            if (*it < now - 0.001)
            {
                target = *it;
                break;
            }
        }
    }
    else
    {
        for (F64 t : times)
        {
            if (t > now + 0.001)
            {
                target = t;
                break;
            }
        }
    }
    store.view().mCursor = llclamp(target, sStart, sEnd);
    store.view().mPlaying = false;
    store.notifyViewChanged();
}

// Advances the transport once per frame while playing; the cursor never leaves the death window.
static void advance_transport()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == sAdvanceFrame)
    {
        return;
    }
    sAdvanceFrame = frame;

    const F64 now = LLFrameTimer::getTotalSeconds();
    const F64 dt = (sLastAdvance > 0.0) ? llclamp(now - sLastAdvance, 0.0, 0.25) : 0.0;
    sLastAdvance = now;

    SSCombatLog& store = SSCombatLog::instance();
    SSCombat::View& view = store.view();
    if (!view.mPlaying)
    {
        view.mCursor = llclamp(view.mCursor, sStart, sEnd);
        return;
    }
    view.mCursor += dt * (F64)llmax(0.01f, view.mSpeed);
    if (view.mCursor >= sEnd)
    {
        if (sLoop)
        {
            view.mCursor = sStart;
        }
        else
        {
            view.mCursor = sEnd;
            view.mPlaying = false;
        }
    }
    store.notifyViewChanged();
}

// ---------------------------------------------------------------------------------------------------------
// Control panel.
// ---------------------------------------------------------------------------------------------------------

// Places the panel bottom-centre of the world view, which is where the officer's camera is not.
static void dock_panel()
{
    if (!sPanel || !gViewerWindow)
    {
        return;
    }
    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    const S32 w = sPanel->getRect().getWidth();
    const S32 h = sPanel->getRect().getHeight();
    const S32 left = world.mLeft + (world.getWidth() - w) / 2;
    const S32 bottom = world.mBottom + 96;
    sPanel->setOrigin(llmax(0, left), llmax(0, bottom));
    sPanel->reshape(w, h, false);
}

// Pushes the current transport state into the widgets; the slider is left alone while it is being dragged.
static void sync_panel()
{
    if (!sPanel || !sPanel->getVisible())
    {
        return;
    }
    sSyncingWidgets = true;

    SSCombatLog& store = SSCombatLog::instance();
    const F64 t = recon_time();

    if (LLButton* play = sPanel->findChild<LLButton>("play_btn"))
    {
        play->setLabel(store.view().mPlaying ? std::string("Pause") : std::string("Play"));
    }
    if (LLSlider* scrub = sPanel->findChild<LLSlider>("scrub_slider"))
    {
        scrub->setMinValue((F32)(sStart - sDeathTime));
        scrub->setMaxValue((F32)(sEnd - sDeathTime));
        if (!scrub->hasMouseCapture())
        {
            scrub->setValue((F32)(t - sDeathTime), false);
        }
    }
    if (LLTextBox* readout = sPanel->findChild<LLTextBox>("time_readout"))
    {
        readout->setValue(LLSD(llformat("%+.2f s", t - sDeathTime)));
    }
    if (LLCheckBoxCtrl* loop = sPanel->findChild<LLCheckBoxCtrl>("loop_check"))
    {
        if (loop->get() != sLoop)
        {
            loop->setValue(LLSD(sLoop));
        }
    }
    sSyncingWidgets = false;
}

// Play/pause.
static void on_play(LLUICtrl*, const LLSD&)
{
    SSCombatLog& store = SSCombatLog::instance();
    store.view().mPlaying = !store.view().mPlaying;
    if (store.view().mPlaying && store.view().mCursor >= sEnd - 0.01)
    {
        store.view().mCursor = sStart;
    }
    store.notifyViewChanged();
}

// Step one event back.
static void on_step_back(LLUICtrl*, const LLSD&)
{
    step_event(-1);
}

// Step one event forward.
static void on_step_forward(LLUICtrl*, const LLSD&)
{
    step_event(1);
}

// Scrub: the slider spans the death window and nothing else, so a scrub cannot wander into the session.
static void on_scrub(LLUICtrl* ctrl, const LLSD&)
{
    if (sSyncingWidgets || !ctrl)
    {
        return;
    }
    SSCombatLog& store = SSCombatLog::instance();
    store.view().mCursor = llclamp(sDeathTime + (F64)ctrl->getValue().asReal(), sStart, sEnd);
    store.view().mPlaying = false;
    store.notifyViewChanged();
}

// Playback speed.
static void on_speed(LLUICtrl* ctrl, const LLSD&)
{
    if (sSyncingWidgets || !ctrl)
    {
        return;
    }
    const F32 speed = (F32)ctrl->getValue().asReal();
    SSCombatLog::instance().view().mSpeed = (speed > 0.f) ? speed : 1.f;
}

// Loop at the end of the window.
static void on_loop(LLUICtrl* ctrl, const LLSD&)
{
    if (sSyncingWidgets || !ctrl)
    {
        return;
    }
    sLoop = ctrl->getValue().asBoolean();
}

// The one [article] escape from the replay: the death's own page.
static void on_page(LLUICtrl*, const LLSD&)
{
    SSCombat::NounRef ref;
    ref.mType = SSCombat::NOUN_DEATH;
    ref.mIndex = sDeath;
    ref.mTime = sDeathTime;
    LLFloaterReg::showInstance("ss_combat_events"); // the Details floater is not built yet; the log floater follows the selection
}

// Close: leave the mode, exactly as Esc does.
static void on_close(LLUICtrl*, const LLSD&)
{
    SSCombatReconstruct::leave();
}

// static
void SSCombatReconstruct::createPanel()
{
    if (sPanel || !gViewerWindow)
    {
        return;
    }
    sPanel = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(
        "panel_ss_combat_reconstruct.xml", gViewerWindow->getRootView(), LLPanel::child_registry_t::instance());
    if (!sPanel)
    {
        return;
    }
    sPanel->setVisible(false);

    if (LLButton* button = sPanel->findChild<LLButton>("play_btn"))       button->setCommitCallback(boost::bind(&on_play, _1, _2));
    if (LLButton* button = sPanel->findChild<LLButton>("step_back_btn"))  button->setCommitCallback(boost::bind(&on_step_back, _1, _2));
    if (LLButton* button = sPanel->findChild<LLButton>("step_fwd_btn"))   button->setCommitCallback(boost::bind(&on_step_forward, _1, _2));
    if (LLButton* button = sPanel->findChild<LLButton>("page_btn"))       button->setCommitCallback(boost::bind(&on_page, _1, _2));
    if (LLButton* button = sPanel->findChild<LLButton>("close_btn"))      button->setCommitCallback(boost::bind(&on_close, _1, _2));
    if (LLSlider* scrub = sPanel->findChild<LLSlider>("scrub_slider"))    scrub->setCommitCallback(boost::bind(&on_scrub, _1, _2));
    if (LLComboBox* speed = sPanel->findChild<LLComboBox>("speed_combo")) speed->setCommitCallback(boost::bind(&on_speed, _1, _2));
    if (LLCheckBoxCtrl* loop = sPanel->findChild<LLCheckBoxCtrl>("loop_check")) loop->setCommitCallback(boost::bind(&on_loop, _1, _2));

    dock_panel();
}

// static
void SSCombatReconstruct::cleanup()
{
    sGhosts.clear();
    sPanel = NULL; // owned by the root view, which deletes it
}

// static
LLPanel* SSCombatReconstruct::getPanel()
{
    return sPanel;
}

// ---------------------------------------------------------------------------------------------------------
// Mode.
// ---------------------------------------------------------------------------------------------------------

// static
bool SSCombatReconstruct::active()
{
    return SSCombatLog::instanceExists() && SSCombatLog::instance().view().mReconstruct && !sGhosts.empty();
}

// static
void SSCombatReconstruct::enter(U32 death_event)
{
    if (!SSCombatLog::instanceExists() || death_event == 0)
    {
        return;
    }
    if (death_event != sDeath || sGhosts.empty())
    {
        build_frames(death_event);
    }
    if (sGhosts.empty())
    {
        return;
    }

    SSCombatLog& store = SSCombatLog::instance();
    SSCombat::View& view = store.view();
    view.mCursor = llclamp(view.mCursor, sStart, sEnd);
    sLastAdvance = 0.0;

    createPanel();
    if (sPanel)
    {
        if (LLComboBox* speed = sPanel->findChild<LLComboBox>("speed_combo"))
        {
            sSyncingWidgets = true;
            speed->setValue(LLSD(llformat("%g", view.mSpeed)));
            sSyncingWidgets = false;
        }
        dock_panel();
        sPanel->setVisible(true);
    }
    sync_panel();
}

// static
void SSCombatReconstruct::leave()
{
    sGhosts.clear();
    sDeath = 0;
    if (sPanel)
    {
        sPanel->setVisible(false);
    }
    if (SSCombatLog::instanceExists())
    {
        SSCombatLog::instance().leaveReconstruction();
    }
}

// static
bool SSCombatReconstruct::handleKey(KEY key, MASK mask)
{
    if (!active() || (mask & (MASK_CONTROL | MASK_ALT)))
    {
        return false;
    }
    if (key == KEY_ESCAPE)
    {
        leave();
        return true;
    }
    if (key == ' ')
    {
        on_play(NULL, LLSD());
        return true;
    }
    if (key == ',' || key == '<')
    {
        step_event(-1);
        return true;
    }
    if (key == '.' || key == '>')
    {
        step_event(1);
        return true;
    }
    return false;
}

// static
void SSCombatReconstruct::render()
{
    if (!SSCombatLog::instanceExists())
    {
        return;
    }
    SSCombatLog& store = SSCombatLog::instance();
    SSCombat::View& view = store.view();

    if (!view.mReconstruct)
    {
        if (!sGhosts.empty())
        {
            leave(); // the floater dropped the mode out from under us
        }
        return;
    }
    // The store may have entered the mode without going through the overlay's click path.
    if (sGhosts.empty() && view.mSubject.mType == SSCombat::NOUN_DEATH)
    {
        enter(view.mSubject.mIndex);
    }
    if (sGhosts.empty())
    {
        return;
    }

    advance_transport();
    sync_panel();
    if (sPanel && sPanel->getVisible())
    {
        dock_panel();
    }

    const F64 t = recon_time();
    const S32 index = frame_index(t);

    SSCombatDraw::clear();
    for (const Ghost& ghost : sGhosts)
    {
        draw_ghost(store, ghost, index, t);
    }
    draw_flights(store, t);
    draw_hit_discs(store, t);
    draw_killer_line(t);
    draw_health_bar(store, t);

    if (SSCombatDraw::empty())
    {
        return;
    }

    static LLCachedControl<bool> xray_setting(gSavedSettings, "SSCombatLogOverlayXray", false);
    const bool xray = view.mXray || (bool)xray_setting;

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);

    if (xray)
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        SSCombatDraw::emitLines(SSCombatDraw::LAYER_LINE, 1.f);
    }
    else
    {
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            SSCombatDraw::emitLines(SSCombatDraw::LAYER_LINE, 1.f);
        }
        {
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            SSCombatDraw::emitLines(SSCombatDraw::LAYER_LINE, 0.30f);
        }
    }

    // Ghost bodies write no depth, so the region stays readable straight through them.
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        SSCombatDraw::emitTris(1.f);
        SSCombatDraw::emitLines(SSCombatDraw::LAYER_MARKER, 1.f);
        SSCombatDraw::emitLabels();
    }
    gGL.flush();
}
