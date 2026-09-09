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
#include "llviewerregion.h"
#include "pipeline.h"
#include "v3dmath.h"

#include <cmath>

// Camera vantage tunables: elevation above the ground plane, eye height for the LOS test, and how many
// azimuths are tried before giving up on an elevated shot.
namespace
{
    const F32 VANTAGE_ELEV_DEG = 40.f;
    const S32 VANTAGE_AZIMUTHS = 8;
    const F32 PARTY_EYE_HEIGHT = 1.5f;
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

    LLVector3 focus_region = mid_region;
    if (!found)
    {
        // Indoors: nothing had a clear line, so no further LOS test is meaningful; take the shoulder shot,
        // 3 m behind the victim/target on the side opposite the attacker, 1.8 m up, looking at the subject.
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
