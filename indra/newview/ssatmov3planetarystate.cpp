/**
 * @file ssatmov3planetarystate.cpp
 * @brief Atmo Magic v3 planetary body resolution implementation.
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

#include "ssatmov3planetarystate.h"

#include <cmath>

// <SS:Nexii> Atmo Magic v3: planetary body resolution

// static
LLVector3 SSAtmoV3PlanetaryResolver::orbitOffset(F32 radius, F32 inclination_deg, F32 phase_deg)
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
std::vector<LLVector3> SSAtmoV3PlanetaryResolver::resolveWorldPositions(const SSAtmoV3Planetary& planetary)
{
    const size_t n = planetary.mBodies.size();
    std::vector<LLVector3> positions(n, LLVector3::zero);
    std::vector<bool> resolved(n, false);

    // A body can only be placed once its parent (and that parent's bound
    // partner, if any) already is. Sun->Planet->Moon is at most three
    // levels deep by design, so a handful of passes always converges
    // rather than needing a real topological sort.
    for (S32 pass = 0; pass < 4; ++pass)
    {
        bool progressed = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (resolved[i]) continue;
            const SSAtmoV3CelestialBody& body = planetary.mBodies[i];

            if (body.mParentIndex < 0)
            {
                positions[i] = LLVector3::zero;
                resolved[i] = true;
                progressed = true;
                continue;
            }
            if (body.mParentIndex >= (S32)n || !resolved[body.mParentIndex]) continue;

            LLVector3 anchor = positions[body.mParentIndex];
            const SSAtmoV3CelestialBody& parent = planetary.mBodies[body.mParentIndex];

            // The parent being half of a bound pair means this body orbits
            // the pair's mass-weighted barycenter, not the named parent
            // alone - the entire mechanism a third body needs to "orbit
            // the pair" is pointing mParentIndex at either paired body.
            if (parent.mBoundPartnerIndex >= 0 && parent.mBoundPartnerIndex < (S32)n
                && resolved[parent.mBoundPartnerIndex])
            {
                const SSAtmoV3CelestialBody& partner = planetary.mBodies[parent.mBoundPartnerIndex];
                const F32 total_mass = llmax(0.0001f, parent.mMassRelative + partner.mMassRelative);
                anchor = (positions[body.mParentIndex] * parent.mMassRelative
                          + positions[parent.mBoundPartnerIndex] * partner.mMassRelative) / total_mass;
            }

            // Which scale dial applies is this body's own kind, not its
            // parent's - a moon orbiting a bound planet pair still uses
            // the Planet<->Moon dial, not Sun<->Planet.
            const F32 scale = (body.mKind == SSAtmoV3CelestialBody::MOON)
                ? planetary.mPlanetMoonScale : planetary.mSunPlanetScale;

            positions[i] = anchor + orbitOffset(body.mOrbitalRadius * scale,
                                                 body.mOrbitalInclinationDeg,
                                                 body.mOrbitalPhaseDeg);
            resolved[i] = true;
            progressed = true;
        }
        if (!progressed) break;
    }

    return positions;
}

// static
std::vector<SSAtmoV3ResolvedBody> SSAtmoV3PlanetaryResolver::resolveSky(const SSAtmoV3Planetary& planetary)
{
    std::vector<SSAtmoV3ResolvedBody> out;

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

        SSAtmoV3ResolvedBody resolved;
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
void SSAtmoV3PlanetaryResolver::resolvePrimarySunArc(F32 home_axial_tilt_deg, F64 day_length_seconds, F64 time,
                                                      F32& out_azimuth_deg, F32& out_elevation_deg)
{
    if (day_length_seconds <= 0.0)
    {
        out_azimuth_deg = 180.f;
        out_elevation_deg = 90.f;
        return;
    }

    // 0 at day start, 1 at day end - the caller already wrapped `time`
    // into this track's own day-cycle loop.
    const F32 fraction = (F32)(fmod(time, day_length_seconds) / day_length_seconds);
    const F32 hour_angle_deg = (fraction - 0.5f) * 360.f; // -180 at start, 0 at "noon", +180 at end

    // Stylised, not astronomically exact: a tent-shaped arc peaking at
    // fraction 0.5, raised or lowered by the home body's axial tilt as a
    // stand-in for "what season this world is fixed at". There is no
    // orbital position around a star being tracked to derive a real solar
    // declination from - the design doc's no-orbital-motion decision means
    // the artist fixes the season directly via tilt, rather than it
    // falling out of a year the system doesn't simulate.
    out_elevation_deg = 90.f - std::fabs(hour_angle_deg) - home_axial_tilt_deg;
    out_azimuth_deg = fmodf(90.f + fraction * 270.f, 360.f);
}

// </SS:Nexii>
