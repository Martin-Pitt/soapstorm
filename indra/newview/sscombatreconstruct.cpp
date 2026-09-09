/**
 * @file sscombatreconstruct.cpp
 * @brief Combat Log Reconstruction: the ghosted replay of one combat event, DEATH or DAMAGE (doc/combat_log_ux.md
 *        3.10). Selecting the event in any pane opens this directly (ux rework 2026-09-09); opening it resamples
 *        the tracks of everyone with a damage edge into or out of the event, plus anyone inside the engagement
 *        at that time, into a 10 Hz ring of prepared frames, so scrubbing costs an array index. Everyone in that
 *        ring who is not the victim or an attributed attacker is a wider-cast bystander and fades. The kill line
 *        is the one exception to "everything here is a resampled ghost": it is fixed at the event's own recorded
 *        source/target position and never moves as the scrub cursor does, with a single line-of-sight raycast
 *        run once when the reconstruction is built (owner correction 2026-09-09), not swept every frame.
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

#include "sscombaticons.h"
#include "sscombatlog.h"
#include "sscombatoverlay.h"

#include "indra_constants.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglstates.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llrootview.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "lluiimage.h"
#include "llvector4a.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

// ---------------------------------------------------------------------------------------------------------
// Palette. Reconstruction reuses the overlay's meanings and adds only the ones it alone needs.
// ---------------------------------------------------------------------------------------------------------

// The kill line's own neutral colour: dashed rather than damage-type-coloured, so it is never mistaken for one
// of the solid hit lines draw_hit_discs()/draw_hit_line() draw for the DAMAGE evidence.
static const LLColor4 COL_KILL_LINE(0.64f, 0.64f, 0.67f, 1.f);
// Ghost labels.
static const LLColor4 COL_LABEL(0.93f, 0.93f, 0.96f, 1.f);
// A measured projectile flight, solid because it was seen.
static const LLColor4 COL_FLIGHT(0.96f, 0.92f, 0.72f, 1.f);
// The head of a flight that is in the air at the current instant.
static const LLColor4 COL_FLIGHT_HEAD(1.00f, 0.98f, 0.86f, 1.f);
// The health bar (a hatched estimate over the ghost) was pulled 2026-09-09: a Stage 0 walk of cumulative
// damage from life-start is too rough a guess to show as a bar, and the owner called it a bad implementation.
// estimate_health()/draw_health_bar() are gone with it; if a real read comes in Stage 2, redesign rather than
// resurrect this.

// The frame ring's rate; 10 Hz over an eight-second window is 80 frames per ghost.
static const F64 RECON_HZ = 10.0;
// The budget of ux 3.10: at most 24 ghosts, which is the order of a busy Engagement and cheaper.
static const size_t RECON_MAX_GHOSTS = 24;
// Fallback window when the store has not set one; ux 3.10 fixes it at [t - 6 s, t + 2 s].
static const F64 RECON_PRE = 6.0;
static const F64 RECON_POST = 2.0;
// Bystanders (everyone in the cast who is not the victim or an attributed attacker) fade to this fraction of
// their normal alpha and lose their label while a reconstruction is up (ux rework 2026-09-09, item 3).
static const F32 FADE_ALPHA_SCALE = 0.30f;

// ---------------------------------------------------------------------------------------------------------
// Prepared frames.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // One combatant resampled over the window. mValid is separate from mFrames because a track gap must draw
    // hollow and stop rather than sliding across the missing seconds. mVictim/mAttacker mark the two party
    // roles (the victim, and the recorded blow's owner or any other attributed attacker); everyone else in the
    // cast is a bystander that fades to FADE_ALPHA_SCALE with no label while the reconstruction is up (ux
    // rework 2026-09-09, item 3).
    struct Ghost
    {
        LLUUID                      mId;
        bool                        mVictim = false;
        bool                        mAttacker = false;
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

    // The kill line's fixed endpoints (agent space, eye/chest heights already added) and its line-of-sight
    // verdict, both resolved once by resolve_kill_line() when the ring is built, never per frame.
    enum ELosState : U8 { LOS_UNTESTED = 0, LOS_CLEAR, LOS_BLOCKED };
    LLVector3           sKillFrom, sKillTo;
    bool                sHaveKillLine = false;
    U8                  sLos = LOS_UNTESTED;

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

// Resamples one combatant into the ring. sampleAt already holds position steady across a teleport rather than
// sliding, but the ring goes further: once the query time actually reaches the flagged sample's own instant,
// the frame is invalid (hollow) rather than showing the far side, so the victim's ghost stays put at the death
// spot and then simply stops instead of ever popping to the respawn point inside the window.
static void build_ghost(const SSCombatLog& store, const LLUUID& id, bool victim, bool attacker)
{
    Ghost ghost;
    ghost.mId = id;
    ghost.mVictim = victim;
    ghost.mAttacker = attacker;
    const S32 frames = frame_count();
    ghost.mFrames.resize((size_t)frames);
    ghost.mValid.resize((size_t)frames, false);

    // The first teleport at or after the death, if any is inside the window; only this one stops the ghost.
    F64 teleport_at = -1.0;
    if (const SSCombat::Track* track = store.track(id))
    {
        const F64 session = store.sessionStart();
        for (const SSCombat::Sample& s : track->mSamples)
        {
            if (!(s.mFlags & SSCombat::FLAG_TELEPORT))
            {
                continue;
            }
            const F64 st = session + (F64)s.mTime;
            if (st < sDeathTime - 0.001)
            {
                continue; // an older, unrelated jump
            }
            if (st > sEnd + 0.001)
            {
                break; // samples are ascending; nothing left in this window
            }
            teleport_at = st;
            break;
        }
    }

    bool any = false;
    for (S32 i = 0; i < frames; ++i)
    {
        const F64 t = sStart + (F64)i / RECON_HZ;
        if (teleport_at >= 0.0 && t >= teleport_at - 0.001)
        {
            continue; // past the jump: hollow, never the far side
        }
        SSCombat::Sample sample;
        if (store.sampleAt(id, t, sample))
        {
            ghost.mFrames[(size_t)i] = sample;
            ghost.mValid[(size_t)i] = true;
            any = true;
        }
    }
    if (any || victim || attacker)
    {
        sGhosts.push_back(ghost);
    }
}

// Resolves the kill line's two fixed endpoints and, once, whether the straight line between them is blocked by
// static geometry: a single raycast per reconstruction, run here rather than every frame the way the overlay's
// broader detective view deliberately never does. The endpoints come from the event's own recorded source/
// target position when it carries one (a DEATH does; a DAMAGE does not, struct Event's own comment), else the
// ghosts' resampled position at the event's own instant -- never the scrub cursor, so the line stays put while
// the officer scrubs (owner correction 2026-09-09: it used to resample at the current cursor, which dragged the
// line along with the moving ghosts before the death and made it look like a claim about wherever they were
// standing right now, not about the recorded shot).
static void resolve_kill_line(const SSCombatLog& store, const SSCombat::Event& death)
{
    sHaveKillLine = false;
    sLos = LOS_UNTESTED;
    if (sKiller.isNull())
    {
        return;
    }

    LLVector3 from_region, to_region;
    bool have_from = false, have_to = false;
    if (death.mHasPositions)
    {
        from_region = death.mSourcePos;
        to_region = death.mTargetPos;
        have_from = have_to = true;
    }
    else
    {
        SSCombat::Sample killer_sample, victim_sample;
        have_from = store.sampleAt(sKiller, sDeathTime, killer_sample);
        have_to = store.sampleAt(sVictim, sDeathTime, victim_sample);
        if (have_from) from_region = killer_sample.mPos;
        if (have_to)   to_region = victim_sample.mPos;
    }
    if (!have_from || !have_to)
    {
        return;
    }

    sKillFrom = SSCombatDraw::agentFromRegion(from_region) + LLVector3(0.f, 0.f, 1.60f);
    sKillTo   = SSCombatDraw::agentFromRegion(to_region) + LLVector3(0.f, 0.f, 1.20f);
    sHaveKillLine = true;

    LLVector4a start;
    start.load3(sKillFrom.mV);
    LLVector4a end;
    end.load3(sKillTo.mV);
    LLVector4a hit;
    sLos = (gPipeline.lineSegmentIntersectWorldGeometry(start, end, &hit) == NULL) ? LOS_CLEAR : LOS_BLOCKED;
}

// Collects the cast and resamples it. Everyone with a damage edge into or out of the death, plus anyone the
// containing engagement had inside its radius at the time, capped at the ghost budget. Tracks which of them
// are the victim or an attributed attacker (the party) versus a wider-cast bystander, so build_ghost can mark
// the fade (ux rework 2026-09-09, item 3).
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
    resolve_kill_line(store, *death);

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

    // The party: the recorded blow's owner plus every other attributed attacker from the attribution the store
    // already computed (empty for a DAMAGE event, which has no attribution and needs none -- sKiller alone is
    // its one attacker). Anyone here draws in the attacker palette at full strength; everyone else in the cast
    // below is a bystander.
    std::set<LLUUID> attackers;
    if (sKiller.notNull())
    {
        add(sKiller);
        attackers.insert(sKiller);
    }
    const SSCombat::Attribution attribution = store.attribution(death_event);
    for (const SSCombat::VolleyShare& share : attribution.mShares)
    {
        if (share.mAttacker.notNull())
        {
            add(share.mAttacker);
            attackers.insert(share.mAttacker);
        }
    }
    // Anyone the victim was shooting at inside the window is an edge out of the death, but a bystander to it,
    // not a party: they fade with the rest of the wider cast.
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
    // Everyone inside the engagement that contains the death; also bystanders.
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
        build_ghost(store, id, id == sVictim, id != sVictim && attackers.count(id) != 0);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Drawing.
// ---------------------------------------------------------------------------------------------------------

// One ghost at the current instant: capsule (or a hollow ring across a track gap), yaw arrow, label, pick rect.
// The solid branch is SSCombatDraw::ghostBody(), the shared body also used by the overlay's detective view. A
// ghost that is neither the victim nor an attributed attacker is a bystander: it fades to FADE_ALPHA_SCALE and
// draws no label, party members stay at full strength (ux rework 2026-09-09, item 3).
static void draw_ghost(const SSCombatLog& store, const Ghost& ghost, S32 index, F64 t)
{
    const bool party = ghost.mVictim || ghost.mAttacker;
    const F32 alpha_scale = party ? 1.f : FADE_ALPHA_SCALE;
    const LLColor4 color = SSCombatDraw::ghostColor(ghost.mVictim, ghost.mAttacker);

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
        hollow.mV[VALPHA] = color.mV[VALPHA] * 0.5f * alpha_scale;
        const LLVector3 stopped = SSCombatDraw::agentFromRegion(ghost.mFrames[(size_t)last].mPos);
        SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, stopped, 0.42f, hollow, SSCombatDraw::WIDTH_THIN);
        if (party)
        {
            SSCombatDraw::pushLabel(store.displayName(ghost.mId) + " ?", stopped + LLVector3(0.f, 0.f, 2.05f), hollow);
        }
        return;
    }
    const SSCombat::Sample& sample = ghost.mFrames[(size_t)index];
    const LLVector3 feet = SSCombatDraw::agentFromRegion(sample.mPos);

    // Ghosts push their rects like any other marker, so the pick stack resolves overlapping ghosts exactly
    // as it resolves live ones.
    SSCombat::NounRef ref;
    ref.mType = SSCombat::NOUN_COMBATANT;
    ref.mId = ghost.mId;
    ref.mTime = t;
    const std::string label = party ? store.displayName(ghost.mId) : std::string();
    SSCombatDraw::ghostBody(feet, sample.mYaw, sample.mFlags, color, label, ref, true, alpha_scale);
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

// The killer's line: fixed at the endpoints resolve_kill_line() resolved when the ring was built (the event's
// own recorded source/target position, never the current scrub position), dashed rather than solid so it is
// never mistaken for one of draw_hit_discs()'s or draw_hit_line()'s solid, damage-type-coloured hit lines. An
// eye icon at the midpoint carries the line-of-sight verdict the one raycast actually found: on/off once it
// has run, no icon at all while sLos is still LOS_UNTESTED (no positions to test).
static void draw_killer_line()
{
    if (!sHaveKillLine)
    {
        return;
    }
    SSCombatDraw::dashedSeg(SSCombatDraw::LAYER_LINE, sKillFrom, sKillTo, COL_KILL_LINE, COL_KILL_LINE, SSCombatDraw::WIDTH_MID);

    const char* icon_name = (sLos == LOS_CLEAR) ? "Profile_Group_Visibility_On"
                           : (sLos == LOS_BLOCKED) ? "Profile_Group_Visibility_Off" : NULL;
    if (!icon_name)
    {
        return;
    }
    LLPointer<LLUIImage> image = LLUI::getUIImage(icon_name);
    if (!image)
    {
        return;
    }
    const LLVector3 mid = lerp(sKillFrom, sKillTo, 0.5f);
    const F32 half = llmax(0.02f, 9.f * SSCombatDraw::metresPerPixelAt(mid)); // ~18 px, screen-constant
    SSCombatDraw::icon(mid, image, half, LLColor4::white);
}

// ---------------------------------------------------------------------------------------------------------
// Transport.
// ---------------------------------------------------------------------------------------------------------

// Every event time inside the window, ascending; the hidden ',' '.' step keys walk exactly this list.
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
// The panel's own "0 s" marker: a tick across the scrub track at the reconstructed event's own instant, plus
// its small icon just below, so the officer can read how far the transport has scrubbed from the event
// without doing the arithmetic on the readout. A tiny LLPanel subclass registered like the codebase's other
// custom ss_ controls (e.g. SSOrbitViewCtrl), rather than a second overlay pass, because it only ever needs
// to draw over this one panel's own children in this one panel's own coordinate space (owner addition,
// ux rework 2026-09-09).
// ---------------------------------------------------------------------------------------------------------

class SSCombatReconPanel : public LLPanel
{
public:
    struct Params : public LLInitParam::Block<Params, LLPanel::Params> {};
    SSCombatReconPanel() {}
    /*virtual*/ void draw();
};

static LLPanelInjector<SSCombatReconPanel> register_ss_combat_recon_panel("ss_combat_recon_panel");

// The atlas icon for the reconstructed event itself: the damage-type glyph for a DAMAGE, the death glyph for a
// DEATH or object death. Mirrors the Events list's and the overlay's own icon choice (sscombaticons.h) so the
// marker never shows a different symbol than everywhere else calls this event.
static std::string marker_icon_name()
{
    const SSCombat::Event* ev = SSCombatLog::instanceExists() ? SSCombatLog::instance().event(sDeath) : NULL;
    if (!ev)
    {
        return std::string();
    }
    switch (ev->mKind)
    {
        case SSCombat::EVENT_DAMAGE:       return SSCombatIcons::forType(ev->mType);
        case SSCombat::EVENT_OBJECT_DEATH: return SSCombatIcons::forType(SSCombatIcons::ICON_OBJECT_DEATH);
        default:                           return SSCombatIcons::forType(SSCombatIcons::ICON_DEATH);
    }
}

void SSCombatReconPanel::draw()
{
    LLPanel::draw();
    if (sEnd <= sStart)
    {
        return;
    }
    LLSliderCtrl* scrub = findChild<LLSliderCtrl>("scrub_slider");
    if (!scrub)
    {
        return;
    }

    // Where t = 0 s (the event's own instant) falls in the window's own span, as a fraction of the track.
    const F32 frac = (F32)llclamp((sDeathTime - sStart) / (sEnd - sStart), 0.0, 1.0);
    const LLRect& track = scrub->getRect();
    const S32 x = track.mLeft + (S32)(frac * (F32)track.getWidth() + 0.5f);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_line_2d(x, track.mTop, x, track.mBottom, COL_LABEL);

    const std::string icon_name = marker_icon_name();
    LLPointer<LLUIImage> icon = icon_name.empty() ? LLPointer<LLUIImage>() : LLUI::getUIImage(icon_name);
    if (icon)
    {
        const S32 size = 16;
        icon->draw(x - size / 2, track.mBottom - size, size, size, LLColor4::white);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Control panel.
// ---------------------------------------------------------------------------------------------------------

// Docks the panel top-centre of the world view, just below the menu/top bar: the root view's own top edge
// minus ~40 px, never the legend's territory at the bottom (ux rework 2026-09-09; the panel used to be
// bottom-centre).
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
    const LLView* root = gViewerWindow->getRootView();
    const S32 root_top = root ? root->getRect().mTop : world.mTop;
    const S32 top = root_top - 40;
    sPanel->setOrigin(llmax(0, left), llmax(0, top - h));
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
        // Same icon-swap idiom as the Atmo Magic environment editor's preview_play_button.
        play->setImageOverlay(store.view().mPlaying ? "Pause_Off" : "Play_Off");
    }
    if (LLSliderCtrl* scrub = sPanel->findChild<LLSliderCtrl>("scrub_slider"))
    {
        // 0..100 across the window regardless of its span, so the slider never needs reconfiguring per event.
        const F64 span = sEnd - sStart;
        if (!scrub->isMouseHeldDown())
        {
            const F32 frac = (span > 0.0) ? (F32)llclamp((t - sStart) / span, 0.0, 1.0) : 0.f;
            scrub->setValue(frac * 100.f, false);
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

// Scrub: the slider spans the death window and nothing else (as a 0..100 fraction of it), so a scrub cannot
// wander into the session.
static void on_scrub(LLUICtrl* ctrl, const LLSD&)
{
    if (sSyncingWidgets || !ctrl)
    {
        return;
    }
    SSCombatLog& store = SSCombatLog::instance();
    const F32 frac = llclamp((F32)ctrl->getValue().asReal() / 100.f, 0.f, 1.f);
    store.view().mCursor = llclamp(sStart + (F64)frac * (sEnd - sStart), sStart, sEnd);
    store.view().mPlaying = false;
    store.notifyViewChanged();
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

    if (LLButton* button = sPanel->findChild<LLButton>("play_btn"))            button->setCommitCallback(boost::bind(&on_play, _1, _2));
    if (LLButton* button = sPanel->findChild<LLButton>("close_btn"))           button->setCommitCallback(boost::bind(&on_close, _1, _2));
    if (LLSliderCtrl* scrub = sPanel->findChild<LLSliderCtrl>("scrub_slider")) scrub->setCommitCallback(boost::bind(&on_scrub, _1, _2));
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
    sHaveKillLine = false;
    sLos = LOS_UNTESTED;
    if (sPanel)
    {
        sPanel->setVisible(false);
    }
    if (SSCombatLog::instanceExists())
    {
        SSCombatLog::instance().leaveReconstruction(); // also clears the selection now (rework 2026-09-09)
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
    // Selecting any event now enters its reconstruction directly through SSCombatLog::select() (ux rework
    // 2026-09-09), which only ever touches the View, so this is the normal path -- not a fallback -- for
    // picking that up: it (re)builds the ring whenever the subject is a different event than the one already
    // loaded, which is also how switching the selection to another event while already reconstructing gets
    // the whole ghost cast moved onto it.
    const bool subject_is_event = (view.mSubject.mType == SSCombat::NOUN_DEATH || view.mSubject.mType == SSCombat::NOUN_DAMAGE);
    if (subject_is_event && (sGhosts.empty() || sDeath != view.mSubject.mIndex))
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
    draw_killer_line();

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
        SSCombatDraw::emitIcons(1.f); // the kill line's line-of-sight eye
        SSCombatDraw::emitLabels();
    }
    gGL.flush();
}
