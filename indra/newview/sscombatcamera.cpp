/**
 * @file sscombatcamera.cpp
 * @brief Combat Log camera vantage. All geometry here is region-local until the very last step, matching the
 *        store's own convention; line-of-sight tests happen in agent space, which is what
 *        gPipeline::lineSegmentIntersectWorldGeometry expects.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatcamera.h"

#include "sscombatlog.h"
#include "sscombatoverlay.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llmath.h"
#include "lluuid.h"
#include "llvector4a.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "pipeline.h"
#include "v3dmath.h"
#include "v3math.h"

#include <cmath>

// Camera vantage tunables: elevation above the ground plane, eye height for the LOS test, and how many
// azimuths are tried before giving up on an elevated shot.
namespace
{
    const F32 VANTAGE_ELEV_DEG = 40.f;
    const S32 VANTAGE_AZIMUTHS = 8;
    const F32 PARTY_EYE_HEIGHT = 1.5f;

    // Close-quarters composed shot (indoor fallback, ux request 2026-09-09): victim framed about 80% down the
    // screen, attacker about 20% down. Candidate azimuths are relative to -u (straight behind the victim,
    // away from the attacker); +-45/+-90 already cover both mirrors of the side composition, so "behind, then
    // sides, then mirrors" collapses to trying this list in order.
    const F32 CQ_AZIMUTH_STEPS_DEG[] = { 0.f, 45.f, -45.f, 90.f, -90.f };
    const F32 CQ_MIN_RADIUS = 3.f;
    const F32 CQ_MAX_RADIUS = 10.f;
    const F32 CQ_WALL_MARGIN = 0.5f;
    const F32 CQ_MIN_HEIGHT_ABOVE_VICTIM = 1.f;
    const F32 CQ_MAX_HEIGHT_ABOVE_VICTIM = 15.f;
    const F32 CQ_FRAME_NDC = 0.6f; // victim at normalised device y ~ -0.6, attacker at ~ +0.6
}

// True when the straight segment between two agent-space points is clear of static world geometry.
static bool los_clear(const LLVector3& from_agent, const LLVector3& to_agent)
{
    LLVector4a start;
    start.load3(from_agent.mV);
    LLVector4a end;
    end.load3(to_agent.mV);
    LLVector4a hit;
    return gPipeline.lineSegmentIntersectWorldGeometry(start, end, &hit) == NULL;
}

// The best-known region-local position of a party at the event's time: the track sample, or the event's own
// carried position when the track has none. False when neither exists.
static bool party_pos(const SSCombatLog& store, const LLUUID& id, F64 t,
                      const LLVector3& fallback, bool has_fallback, LLVector3& out)
{
    SSCombat::Sample sample;
    if (id.notNull() && store.sampleAt(id, t, sample))
    {
        out = sample.mPos;
        return true;
    }
    if (has_fallback)
    {
        out = fallback;
        return true;
    }
    return false;
}

// Rotates a horizontal vector (Z ignored, Z out is 0) by the given angle, positive counter-clockwise, matching
// the azimuth convention already used by the slanted-overhead search above.
static LLVector3 rotate_horizontal(const LLVector3& dir, F32 degrees)
{
    const F32 rad = degrees * DEG_TO_RAD;
    const F32 c = cosf(rad);
    const F32 s = sinf(rad);
    return LLVector3(dir.mV[VX] * c - dir.mV[VY] * s, dir.mV[VX] * s + dir.mV[VY] * c, 0.f);
}

// How much clear horizontal room exists from v_agent along dir before static geometry, clamped to
// [CQ_MIN_RADIUS, CQ_MAX_RADIUS] with a safety margin off the wall; false when there is not even
// CQ_MIN_RADIUS of room, in which case this azimuth cannot seat a camera at all.
static bool room_along(const LLVector3& v_agent, const LLVector3& dir_horiz, F32& out_radius)
{
    const F32 probe = CQ_MAX_RADIUS + CQ_WALL_MARGIN + 1.f;
    LLVector4a start;
    start.load3(v_agent.mV);
    const LLVector3 end_point = v_agent + dir_horiz * probe;
    LLVector4a end;
    end.load3(end_point.mV);
    LLVector4a hit;
    F32 room = CQ_MAX_RADIUS;
    if (gPipeline.lineSegmentIntersectWorldGeometry(start, end, &hit))
    {
        room = dist_vec(v_agent, LLVector3(hit.getF32ptr())) - CQ_WALL_MARGIN;
    }
    if (room < CQ_MIN_RADIUS)
    {
        return false;
    }
    out_radius = llmin(CQ_MAX_RADIUS, room);
    return true;
}

// The height above v_region (region-local Z offset, at least CQ_MIN_HEIGHT_ABOVE_VICTIM) at which the angle
// between camera->V and camera->A best matches target_angle. That angle is not monotonic in height (near zero
// at ground level, rising to a peak, then falling back toward zero as the camera climbs very high), so this
// scans a fixed set of samples rather than assuming a single root. Two heights can match target_angle equally
// well, one on the rising limb near the ground and one on the falling limb much higher up; scanning tallest
// first and returning the first one inside a tolerance keeps the owner's "somewhat above the victim" vantage
// (owner correction 2026-09-09: scanning short-to-first and taking the strict minimum used to hand back the
// low, close, ground-hugging match every time, which read as a low-angle shot glued to the victim).
static F32 solve_height(const LLVector3& v_region, const LLVector3& a_region, const LLVector3& horiz_offset, F32 target_angle)
{
    const S32 STEPS = 40;
    const F32 TOLERANCE = 4.f * DEG_TO_RAD;
    F32 best_h = CQ_MAX_HEIGHT_ABOVE_VICTIM;
    F32 best_err = 1.0e6f;
    for (S32 i = STEPS; i >= 0; --i)
    {
        const F32 h = CQ_MIN_HEIGHT_ABOVE_VICTIM
                    + (CQ_MAX_HEIGHT_ABOVE_VICTIM - CQ_MIN_HEIGHT_ABOVE_VICTIM) * ((F32)i / (F32)STEPS);
        const LLVector3 cam = v_region + horiz_offset + LLVector3(0.f, 0.f, h);
        const F32 angle = angle_between(v_region - cam, a_region - cam);
        const F32 err = fabsf(angle - target_angle);
        if (err < best_err)
        {
            best_err = err;
            best_h = h;
        }
        if (err <= TOLERANCE)
        {
            return h; // the tallest candidate scanned so far that is already good enough
        }
    }
    return best_h;
}

// The owner's composed close-quarters shot: replaces the old plain shoulder shot as the indoor fallback. Tries
// a vantage behind the victim first, then to either side, at a distance the room actually allows, with the
// height solved so the victim reads about 80% down the screen and the attacker about 20% down (ux request
// 2026-09-09). Returns false when every candidate is blocked, either by lack of room or by a wall in the way.
static bool find_close_quarters_vantage(const LLVector3& target_region, const LLVector3& attacker_region,
                                        LLVector3& out_chosen_region, LLVector3& out_focus_region)
{
    const F32 fov_y = LLViewerCamera::getInstance()->getView(); // vertical FOV, radians
    const F32 target_angle = 2.f * atanf(CQ_FRAME_NDC * tanf(fov_y * 0.5f));

    const LLVector3 v_region = target_region + LLVector3(0.f, 0.f, PARTY_EYE_HEIGHT);
    const LLVector3 a_region = attacker_region + LLVector3(0.f, 0.f, PARTY_EYE_HEIGHT);
    LLVector3 u = a_region - v_region;
    u.mV[VZ] = 0.f;
    if (u.normalize() < 0.0001f)
    {
        u.setVec(1.f, 0.f, 0.f);
    }
    const LLVector3 base_dir = u * -1.f; // "behind the victim": horizontally away from the attacker

    const LLVector3 v_agent = SSCombatDraw::agentFromRegion(v_region);
    const LLVector3 a_agent = SSCombatDraw::agentFromRegion(a_region);

    for (F32 az_deg : CQ_AZIMUTH_STEPS_DEG)
    {
        const LLVector3 dir = rotate_horizontal(base_dir, az_deg);
        F32 radius = 0.f;
        if (!room_along(v_agent, dir, radius))
        {
            continue;
        }
        const LLVector3 horiz_offset = dir * radius;
        const F32 height = solve_height(v_region, a_region, horiz_offset, target_angle);
        const LLVector3 candidate_region = v_region + horiz_offset + LLVector3(0.f, 0.f, height);
        const LLVector3 candidate_agent = SSCombatDraw::agentFromRegion(candidate_region);
        if (!los_clear(candidate_agent, v_agent) || !los_clear(candidate_agent, a_agent))
        {
            continue;
        }

        // Focus point: the midpoint between the two parties on the ground, not a ray bisected from the camera.
        // The "behind the victim" azimuth (tried first, and the common case) puts the camera, victim and
        // attacker close to collinear, which made the bisector of camera->V and camera->A nearly cancel and
        // fall back to dir_to_v alone -- the camera ended up looking at the victim only, with the attacker off
        // to one side out of frame (owner bug report 2026-09-09: "entirely focused on the victim"). The height
        // solve above already places the split between the two in frame; this just needs a focus point that
        // is never degenerate.
        out_chosen_region = candidate_region;
        out_focus_region = lerp(v_region, a_region, 0.5f);
        return true;
    }
    return false;
}

// static
void SSCombatCamera::flyTo(U32 eventId)
{
    if (!SSCombatLog::instanceExists())
    {
        return;
    }
    SSCombatLog& store = SSCombatLog::instance();
    const SSCombat::Event* ev = store.event(eventId);
    LLViewerRegion* region = gAgent.getRegion();
    if (!ev || !region)
    {
        return;
    }

    LLVector3 attacker_region, target_region;
    const bool have_attacker = party_pos(store, ev->mOwner, ev->mTime, ev->mSourcePos,
                                         ev->mHasPositions && !ev->mSourcePos.isExactlyZero(), attacker_region);
    const bool have_target = party_pos(store, ev->mTarget, ev->mTime, ev->mTargetPos,
                                       ev->mHasPositions && !ev->mTargetPos.isExactlyZero(), target_region);
    if (!have_attacker && !have_target)
    {
        return; // nothing in the store to look at
    }

    const LLVector3 mid_region = (have_attacker && have_target) ? (attacker_region + target_region) * 0.5f
                                                                 : (have_target ? target_region : attacker_region);
    const F32 separation = (have_attacker && have_target) ? (target_region - attacker_region).magVec() : 0.f;
    const F32 distance = llmax(8.f, 1.6f * separation);

    // The azimuth walk starts perpendicular to the attacker-target line (or +X when there is only one party).
    LLVector3 perpendicular(1.f, 0.f, 0.f);
    if (have_attacker && have_target && separation > 0.01f)
    {
        LLVector3 line = target_region - attacker_region;
        line.mV[VZ] = 0.f;
        if (line.normalize() > 0.0001f)
        {
            perpendicular.setVec(-line.mV[VY], line.mV[VX], 0.f);
        }
    }
    const F32 start_azimuth = atan2f(perpendicular.mV[VY], perpendicular.mV[VX]);

    const LLVector3 attacker_eye_agent = SSCombatDraw::agentFromRegion(attacker_region + LLVector3(0.f, 0.f, PARTY_EYE_HEIGHT));
    const LLVector3 target_eye_agent = SSCombatDraw::agentFromRegion(target_region + LLVector3(0.f, 0.f, PARTY_EYE_HEIGHT));

    // A candidate is accepted when the line from it to every present party's eye point is clear.
    auto candidate_clear = [&](const LLVector3& candidate_region) -> bool
    {
        const LLVector3 candidate_agent = SSCombatDraw::agentFromRegion(candidate_region);
        if (have_attacker && !los_clear(candidate_agent, attacker_eye_agent))
        {
            return false;
        }
        if (have_target && !los_clear(candidate_agent, target_eye_agent))
        {
            return false;
        }
        return true;
    };

    LLVector3 chosen_region;
    bool found = false;

    const F32 elev_rad = VANTAGE_ELEV_DEG * DEG_TO_RAD;
    for (S32 i = 0; i < VANTAGE_AZIMUTHS && !found; ++i)
    {
        const F32 azimuth = start_azimuth + F_TWO_PI * (F32)i / (F32)VANTAGE_AZIMUTHS;
        const LLVector3 horiz(cosf(azimuth), sinf(azimuth), 0.f);
        const LLVector3 candidate = mid_region + horiz * (distance * cosf(elev_rad)) +
                                    LLVector3(0.f, 0.f, distance * sinf(elev_rad));
        if (candidate_clear(candidate))
        {
            chosen_region = candidate;
            found = true;
        }
    }

    LLVector3 focus_region = mid_region;

    // Indoors, both parties present: the composed low-angle shot (victim ~80% down the screen, attacker ~20%
    // down), tried before the straight-above fallback per the owner's preference for this look over a bird's
    // eye view whenever it can be had.
    if (!found && have_attacker && have_target)
    {
        if (find_close_quarters_vantage(target_region, attacker_region, chosen_region, focus_region))
        {
            found = true;
        }
    }

    if (!found)
    {
        const F32 above = llmax(12.f, 1.5f * separation);
        const LLVector3 candidate = mid_region + LLVector3(0.f, 0.f, above);
        if (candidate_clear(candidate))
        {
            chosen_region = candidate;
            found = true;
        }
    }

    if (!found)
    {
        // Last resort: only one party is known, or even straight-above found no clear line. No further LOS
        // test is meaningful here; take the plain shoulder shot, 3 m behind the victim/target on the side
        // opposite the attacker (when known), 1.8 m up, looking at the subject.
        if (have_target)
        {
            LLVector3 away = have_attacker ? (target_region - attacker_region) : LLVector3(1.f, 0.f, 0.f);
            away.mV[VZ] = 0.f;
            if (away.normalize() < 0.0001f)
            {
                away.setVec(1.f, 0.f, 0.f);
            }
            chosen_region = target_region + away * 3.f + LLVector3(0.f, 0.f, 1.8f);
            focus_region = target_region;
        }
        else
        {
            chosen_region = attacker_region + LLVector3(0.f, 0.f, 1.8f);
            focus_region = attacker_region;
        }
    }

    gAgentCamera.unlockView();
    gAgentCamera.setFocusOnAvatar(false, false);
    const LLVector3d cam_global = region->getPosGlobalFromRegion(chosen_region);
    const LLVector3d focus_global = region->getPosGlobalFromRegion(focus_region);
    gAgentCamera.setCameraPosAndFocusGlobal(cam_global, focus_global, LLUUID::null);
}
