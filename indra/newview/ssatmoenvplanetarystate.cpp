/**
 * @file ssatmoenvplanetarystate.cpp
 * @brief Atmo Magic planetary body resolution implementation.
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

#include "ssatmoenvplanetarystate.h"

#include "llquaternion.h"

#include <cmath>

// <SS:Nexii> Atmo Magic: planetary body resolution

// static
LLVector3 SSAtmoEnvPlanetaryResolver::orbitOffset(F32 radius, F32 inclination_deg, F32 phase_deg)
{
    const F32 phase_rad = phase_deg * DEG_TO_RAD;
    const F32 incl_rad  = inclination_deg * DEG_TO_RAD;

    // A single fixed point on a circle of the given radius, tilted out of
    // the reference plane by inclination - not a swept path, since nothing
    // here is simulated.
    const F32 x = radius * cosf(phase_rad);
    const F32 y = radius * sinf(phase_rad) * cosf(incl_rad);
    const F32 z = radius * sinf(phase_rad) * sinf(incl_rad);
    return LLVector3(x, y, z);
}

// static
std::vector<LLVector3> SSAtmoEnvPlanetaryResolver::resolveWorldPositions(const SSAtmoEnvPlanetary& planetary)
{
    const size_t n = planetary.mBodies.size();
    std::vector<LLVector3> positions(n, LLVector3::zero);
    std::vector<bool> resolved(n, false);

    // --- Suns first, in two waves: roots (a lone star, or the inner bound
    // pair), then suns that orbit them (the canonical 3rd/4th). A bound
    // pair is placed around its pair centre by mass ratio: the JUNIOR
    // member's orbital radius is the pair separation (its phase sets the
    // pair's orientation), and each member sits at
    // separation * other_mass / total_mass from the centre, on opposite
    // sides - the physical barycenter relation, so the pair's own
    // mass-weighted barycenter is its centre by construction. The senior
    // member's own orbital fields describe the PAIR's orbit around its
    // anchor instead, which is what lets an outer pair orbit the inner
    // pair as one unit.
    auto placePair = [&](S32 senior, S32 junior, const LLVector3& centre)
    {
        const SSAtmoEnvCelestialBody& a_body = planetary.mBodies[senior];
        const SSAtmoEnvCelestialBody& b_body = planetary.mBodies[junior];
        const F32 total_mass = llmax(0.0001f, a_body.mMassRelative + b_body.mMassRelative);
        const F32 separation = b_body.mOrbitalRadius * planetary.mSunPlanetScale;

        const LLVector3 dir = orbitOffset(1.f, b_body.mOrbitalInclinationDeg,
                                          b_body.mOrbitalPhaseDeg);
        positions[senior] = centre - dir * (separation * b_body.mMassRelative / total_mass);
        positions[junior] = centre + dir * (separation * a_body.mMassRelative / total_mass);
        resolved[senior] = true;
        resolved[junior] = true;
    };

    auto isSun = [&](S32 i)
    {
        return i >= 0 && i < (S32)n
               && planetary.mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN;
    };
    auto pairJunior = [&](S32 i) -> S32
    {
        const S32 partner = planetary.mBodies[i].mBoundPartnerIndex;
        if (partner > i && partner < (S32)n && isSun(partner)
            && planetary.mBodies[partner].mBoundPartnerIndex == i)
        {
            return partner;
        }
        return -1;
    };

    // Wave 1: root suns (parent < 0).
    for (S32 i = 0; i < (S32)n; ++i)
    {
        if (!isSun(i) || resolved[i]) continue;
        if (planetary.mBodies[i].mParentIndex >= 0) continue;

        const S32 junior = pairJunior(i);
        if (junior >= 0) placePair(i, junior, LLVector3::zero);
        else if (planetary.mBodies[i].mBoundPartnerIndex < 0)
        {
            positions[i] = LLVector3::zero;
            resolved[i] = true;
        }
        // A junior root pair member resolves with its senior above.
    }

    // Wave 2: suns orbiting a resolved sun (the canonical outer single or
    // outer pair). Their anchor is the parent's pair barycenter when the
    // parent is paired - with wave 1's construction that barycenter is
    // exactly the pair centre.
    for (S32 i = 0; i < (S32)n; ++i)
    {
        if (!isSun(i) || resolved[i]) continue;
        const S32 parent = planetary.mBodies[i].mParentIndex;
        if (!isSun(parent) || !resolved[parent]) continue;

        LLVector3 anchor = positions[parent];
        const S32 parent_partner = planetary.mBodies[parent].mBoundPartnerIndex;
        if (parent_partner >= 0 && parent_partner < (S32)n && resolved[parent_partner])
        {
            const SSAtmoEnvCelestialBody& pa = planetary.mBodies[parent];
            const SSAtmoEnvCelestialBody& pb = planetary.mBodies[parent_partner];
            const F32 total_mass = llmax(0.0001f, pa.mMassRelative + pb.mMassRelative);
            anchor = (positions[parent] * pa.mMassRelative
                      + positions[parent_partner] * pb.mMassRelative) / total_mass;
        }

        const SSAtmoEnvCelestialBody& body = planetary.mBodies[i];
        const LLVector3 centre = anchor
            + orbitOffset(body.mOrbitalRadius * planetary.mSunPlanetScale,
                          body.mOrbitalInclinationDeg, body.mOrbitalPhaseDeg);

        const S32 junior = pairJunior(i);
        if (junior >= 0) placePair(i, junior, centre);
        else
        {
            positions[i] = centre;
            resolved[i] = true;
        }
    }

    // --- Shift the whole sun group so its collective mass-weighted
    // barycenter lands exactly on the origin. THIS is what planets orbit:
    // the system's true centre of mass, not any individual star - so a
    // lopsided trinary leans its stars around the origin rather than
    // dragging every planet's anchor off to one side.
    {
        LLVector3 weighted = LLVector3::zero;
        F32 total_mass = 0.f;
        for (S32 i = 0; i < (S32)n; ++i)
        {
            if (!isSun(i) || !resolved[i]) continue;
            weighted += positions[i] * planetary.mBodies[i].mMassRelative;
            total_mass += planetary.mBodies[i].mMassRelative;
        }
        if (total_mass > 0.f)
        {
            const LLVector3 barycenter = weighted / total_mass;
            for (S32 i = 0; i < (S32)n; ++i)
            {
                if (isSun(i) && resolved[i]) positions[i] -= barycenter;
            }
        }
    }

    // --- Planets: anchored at the origin (the sun group's barycenter, per
    // the shift above) regardless of any stored parent - see
    // SSAtmoEnvPlanetary::effectiveParent for why nothing is stored.
    for (S32 i = 0; i < (S32)n; ++i)
    {
        const SSAtmoEnvCelestialBody& body = planetary.mBodies[i];
        if (body.mKind != SSAtmoEnvCelestialBody::PLANET) continue;

        positions[i] = orbitOffset(body.mOrbitalRadius * planetary.mSunPlanetScale,
                                   body.mOrbitalInclinationDeg, body.mOrbitalPhaseDeg);
        resolved[i] = true;
    }

    // --- Moons: anchored at their planet. A moon whose parent is missing
    // or unresolved (hand-edited notecard) anchors at the origin rather
    // than being skipped - visible beats vanished.
    for (S32 i = 0; i < (S32)n; ++i)
    {
        const SSAtmoEnvCelestialBody& body = planetary.mBodies[i];
        if (body.mKind != SSAtmoEnvCelestialBody::MOON || resolved[i]) continue;

        const S32 parent = planetary.effectiveParent(i);
        const LLVector3 anchor = (parent >= 0 && resolved[parent])
            ? positions[parent] : LLVector3::zero;

        positions[i] = anchor
            + orbitOffset(body.mOrbitalRadius * planetary.mPlanetMoonScale,
                          body.mOrbitalInclinationDeg, body.mOrbitalPhaseDeg);
        resolved[i] = true;
    }

    return positions;
}

// static
std::vector<SSAtmoEnvResolvedBody> SSAtmoEnvPlanetaryResolver::resolveSky(const SSAtmoEnvPlanetary& planetary)
{
    std::vector<SSAtmoEnvResolvedBody> out;

    const S32 home_index = planetary.homeBodyIndex();
    if (home_index < 0) return out;

    const std::vector<LLVector3> positions = resolveWorldPositions(planetary);
    const LLVector3 home_pos = positions[home_index];

    for (size_t i = 0; i < planetary.mBodies.size(); ++i)
    {
        if ((S32)i == home_index) continue;

        LLVector3 offset = positions[i] - home_pos;
        const F32 distance = offset.magVec();
        if (distance < 0.0001f) continue; // coincident with home - degenerate, skip rather than divide by ~0

        SSAtmoEnvResolvedBody resolved;
        resolved.mBodyIndex = (S32)i;
        offset.normVec();
        resolved.mDirection = offset;
        resolved.mDistance = distance;

        const F32 radius = planetary.mBodies[i].mDiameterM * 0.5f;
        resolved.mAngularDiameterDeg = RAD_TO_DEG * 2.f * atanf(llclamp(radius / distance, 0.f, 1.f));

        out.push_back(resolved);
    }

    return out;
}

// static
void SSAtmoEnvPlanetaryResolver::observerAxes(F32 latitude_deg, LLVector3& out_east,
                                              LLVector3& out_north, LLVector3& out_up)
{
    // In the equatorial frame - +Z the celestial pole, +X the observer's
    // meridian at hour angle zero - someone at latitude phi has their
    // zenith phi degrees from the equator along their own meridian.
    //
    //   up    = ( cos phi, 0, sin phi)   toward the zenith
    //   north = (-sin phi, 0, cos phi)   along the meridian, toward the pole
    //   east  = ( 0,       1, 0     )    completing a right-handed set
    //
    // Check the handedness: east x north = up, and at phi = 90 (standing on
    // the pole) up becomes the pole itself while north points to what was
    // the equator - which is what "north" degenerates to there.
    const F32 lat = llclamp(latitude_deg, -90.f, 90.f) * DEG_TO_RAD;
    const F32 c = cosf(lat);
    const F32 sn = sinf(lat);

    out_east.setVec(0.f, 1.f, 0.f);
    out_north.setVec(-sn, 0.f, c);
    out_up.setVec(c, 0.f, sn);
}

// static
LLVector3 SSAtmoEnvPlanetaryResolver::resolveObserverDirection(const LLVector3& ecliptic_dir,
                                                               F32 obliquity_deg,
                                                               F32 latitude_deg, F64 phase)
{
    // 1. Ecliptic to equatorial: tip by the home body's obliquity about the
    //    line where the two planes cross (+X here). This is the step that
    //    gives a world seasons - a body sitting in its orbital plane comes
    //    out north or south of the celestial equator depending on where in
    //    the orbit it is.
    LLQuaternion tilt;
    tilt.setAngleAxis(obliquity_deg * DEG_TO_RAD, LLVector3::x_axis);
    LLVector3 eq = ecliptic_dir * tilt;

    // 2. The day: one turn about the celestial pole. NEGATIVE, so the sky
    //    sweeps east to west and a body rises rather than sets as the phase
    //    advances.
    LLQuaternion spin;
    spin.setAngleAxis((F32)(-F_TWO_PI * phase), LLVector3::z_axis);
    eq = eq * spin;

    // 3. Equatorial to horizon: project onto where the observer's own east,
    //    north and up point. This is the step latitude lives in, and the
    //    reason the sky turns at an angle anywhere but the equator.
    LLVector3 east, north, up;
    observerAxes(latitude_deg, east, north, up);

    LLVector3 out(eq * east, eq * north, eq * up);
    out.normalize();
    return out;
}

// static
void SSAtmoEnvPlanetaryResolver::resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                                   const std::vector<SSAtmoEnvResolvedBody>& sky_bodies,
                                                   SSAtmoEnvResolvedBody& out_sun,
                                                   SSAtmoEnvResolvedBody& out_moon)
{
    out_sun = SSAtmoEnvResolvedBody();
    out_moon = SSAtmoEnvResolvedBody();

    if (planetary.homeBodyIndex() < 0) return;

    // INVARIANT: sky_bodies skips the home body (and anything coincident
    // with it), so an entry's position in the vector is NOT its body
    // index - every correlation goes through mBodyIndex. A positional
    // lookup here is exactly the "one body's direction with another
    // body's texture" cross-wiring bug.
    auto resolvedFor = [&sky_bodies](S32 body_index) -> const SSAtmoEnvResolvedBody*
    {
        for (const SSAtmoEnvResolvedBody& body : sky_bodies)
        {
            if (body.mBodyIndex == body_index) return &body;
        }
        return nullptr;
    };

    // Slot assignment is by PHYSICAL diameter, not authoring order and
    // deliberately not apparent size: a moon is usually far closer than a
    // star, so a big-in-the-sky moon (Masser looming over Nirn) would
    // out-subtend a distant giant sun and steal the sun slot the moment
    // apparent size decided. Physical diameter is the stable
    // discriminator - suns are big, moons are merely near. Ties break to
    // the lower body index: lightEmitterIndices() is ascending, so the
    // first-kept candidate wins by construction. An emitter with no
    // resolveSky() entry is coincident with the home body - it has no
    // direction to place, so it is treated as absent rather than handed a
    // fabricated one (a home-star world's "sun" is the ground underfoot,
    // not a sky object).
    bool have_sun = false;
    for (const S32 emitter : planetary.lightEmitterIndices())
    {
        const SSAtmoEnvResolvedBody* resolved = resolvedFor(emitter);
        if (!resolved) continue;

        if (!have_sun)
        {
            out_sun = *resolved;
            have_sun = true;
        }
        else if (planetary.mBodies[static_cast<size_t>(emitter)].mDiameterM
                 > planetary.mBodies[static_cast<size_t>(out_sun.mBodyIndex)].mDiameterM)
        {
            out_moon = out_sun;
            out_sun = *resolved;
        }
        else
        {
            out_moon = *resolved;
        }
    }
}

// static
void SSAtmoEnvPlanetaryResolver::resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                                   SSAtmoEnvResolvedBody& out_sun,
                                                   SSAtmoEnvResolvedBody& out_moon)
{
    resolveLightRoles(planetary, resolveSky(planetary), out_sun, out_moon);
}

namespace
{
    // theta (the rotation angle resolveDiurnalDirection() applies) back to
    // the phase that produces it, wrapped into [0, 1). Phase runs BACKWARDS
    // against theta - the rotation is negative about the celestial axis so
    // the sky sweeps east to west - which is the whole content of the sign
    // here.
    F64 ss_phase_from_theta(F32 theta)
    {
        F64 phase = std::fmod(-(F64)theta / F_TWO_PI, 1.0);
        if (phase < 0.0) phase += 1.0;
        return phase;
    }
}

// static
SSAtmoEnvDiurnalArc SSAtmoEnvPlanetaryResolver::diurnalArc(const LLVector3& ecliptic_dir,
                                                           F32 obliquity_deg, F32 latitude_deg)
{
    // Elevation over a day is still one sinusoid, for the same reason as
    // before: the only thing moving is a rotation about a fixed axis, and
    // everything either side of it is a fixed rotation.
    //
    // Working in the EQUATORIAL frame, where that axis is +Z: a body sits
    // at a fixed u, the day spins it about +Z, and the observer's up is a
    // fixed w. Elevation is the dot of the two, and Rodrigues gives
    //
    //   sin(elev)(theta) = (z.u)(z.w)
    //                    + cos(theta)[u.w - (z.u)(z.w)]
    //                    + sin(theta)[(z x u).w]
    //
    // which is a constant plus one sinusoid, gathered below into
    // amplitude/angle form exactly as it was.
    LLVector3 v = ecliptic_dir;
    v.normalize();

    LLQuaternion tilt;
    tilt.setAngleAxis(obliquity_deg * DEG_TO_RAD, LLVector3::x_axis);
    const LLVector3 u = v * tilt;

    LLVector3 east, north, w;
    observerAxes(latitude_deg, east, north, w);

    const LLVector3 z = LLVector3::z_axis;
    const F32 zu = z * u;
    const F32 zw = z * w;

    const F32 offset   = zu * zw;
    const F32 cos_term = (u * w) - offset;
    const F32 sin_term = (z % u) * w;

    SSAtmoEnvDiurnalArc arc;
    arc.mOffset    = offset;
    arc.mAmplitude = sqrtf(cos_term * cos_term + sin_term * sin_term);
    arc.mThetaZero = atan2f(sin_term, cos_term);
    return arc;
}

// static
F64 SSAtmoEnvPlanetaryResolver::culminationPhase(const SSAtmoEnvDiurnalArc& arc, bool highest)
{
    // cos(theta - thetaZero) is +1 at thetaZero and -1 half a turn later.
    return ss_phase_from_theta(highest ? arc.mThetaZero : (arc.mThetaZero + F_PI));
}

// static
bool SSAtmoEnvPlanetaryResolver::phaseForElevation(const SSAtmoEnvDiurnalArc& arc,
                                                   F32 sin_elevation, bool rising,
                                                   F64& out_phase)
{
    // Degenerate: the direction lies along the rotation axis, so it never
    // moves - a body nailed to the celestial pole. It holds one elevation
    // forever and no phase "reaches" any other; report the failure and
    // hand back phase 0 rather than dividing by ~0.
    if (arc.mAmplitude < 1e-6f)
    {
        out_phase = 0.0;
        return false;
    }

    const F32 u = (sin_elevation - arc.mOffset) / arc.mAmplitude;
    if (u > 1.f)
    {
        out_phase = culminationPhase(arc, true);   // never that high: noon is as close as it gets
        return false;
    }
    if (u < -1.f)
    {
        out_phase = culminationPhase(arc, false);  // never that low: midnight is
        return false;
    }

    // Two solutions, thetaZero +/- acos(u). The + branch is the RISING
    // one: there dz/dtheta = -amplitude*sin(theta - thetaZero) is negative,
    // and phase runs backwards against theta, so elevation climbs as phase
    // advances.
    const F32 delta = acosf(llclamp(u, -1.f, 1.f));
    out_phase = ss_phase_from_theta(rising ? (arc.mThetaZero + delta)
                                           : (arc.mThetaZero - delta));
    return true;
}

// static
F64 SSAtmoEnvPlanetaryResolver::phaseForSunDirection(const LLVector3& ecliptic_dir,
                                                     F32 obliquity_deg, F32 latitude_deg,
                                                     const LLVector3& target_dir)
{
    LLVector3 v = ecliptic_dir;
    LLVector3 t = target_dir;
    if (v.normalize() < 0.0001f || t.normalize() < 0.0001f) return 0.0;

    // Same shape as the elevation arc, with the observer's up swapped for
    // the target direction: the closest approach is where the dot product
    // between the swept body and the target is largest, and that dot is a
    // constant plus one sinusoid in the rotation angle.
    //
    // The target arrives in HORIZON coordinates (it is a sky direction, out
    // of a settings asset), so it has to be carried back into the equatorial
    // frame the sweep happens in.
    LLQuaternion tilt;
    tilt.setAngleAxis(obliquity_deg * DEG_TO_RAD, LLVector3::x_axis);
    const LLVector3 u = v * tilt;

    LLVector3 east, north, up;
    observerAxes(latitude_deg, east, north, up);
    const LLVector3 t_eq = east * t.mV[VX] + north * t.mV[VY] + up * t.mV[VZ];

    const LLVector3 z = LLVector3::z_axis;
    const F32 zu = z * u;
    const F32 zt = z * t_eq;

    const F32 cos_term = (u * t_eq) - zu * zt;
    const F32 sin_term = (z % u) * t_eq;

    if (cos_term * cos_term + sin_term * sin_term < 1e-12f)
    {
        // The sweep never changes how well it matches: the body sits on the
        // rotation axis, so every phase is equally (un)close.
        return 0.0;
    }

    return ss_phase_from_theta(atan2f(sin_term, cos_term));
}

// static
bool SSAtmoEnvPlanetaryResolver::riseSetPhases(const LLVector3& ecliptic_dir,
                                               F32 obliquity_deg, F32 latitude_deg,
                                               F64& out_rise, F64& out_set)
{
    const SSAtmoEnvDiurnalArc arc = diurnalArc(ecliptic_dir, obliquity_deg, latitude_deg);

    F64 rise = 0.0, set = 0.0;
    // Elevation 0 is the horizon crossing proper - the geometric horizon,
    // with no refraction or disc-radius allowance. Both halves have to
    // succeed or the body never crosses at all (circumpolar or never
    // rising), and half an answer would be worse than none.
    if (!phaseForElevation(arc, 0.f, true, rise)) return false;
    if (!phaseForElevation(arc, 0.f, false, set)) return false;

    out_rise = rise;
    out_set = set;
    return true;
}

// </SS:Nexii>
