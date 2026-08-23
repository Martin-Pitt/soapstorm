/**
 * @file ssatmoenvplanetarystate.h
 * @brief Atmo Magic: resolves a track's celestial bodies into fixed sky
 *        directions/angular sizes, and computes the primary sun's
 *        azimuth/elevation arc from the home body's spin - phase 6 of
 *        doc/atmo_magic_environment.md. Pure logic only, same as the
 *        track/weather resolvers before it: no rendering hookup yet, no
 *        quad/billboard drawing, just the math a future renderer consumes.
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

#ifndef SS_ATMOENVPLANETARYSTATE_H
#define SS_ATMOENVPLANETARYSTATE_H

// <SS:Nexii> Atmo Magic: planetary body resolution

#include "ssatmoenvasset.h"
#include "v3math.h"

#include <vector>

// One body as seen from the home body: direction, apparent angular
// diameter, and the distance that produced it (for sorting/size-reference
// purposes a renderer might want).
struct SSAtmoEnvResolvedBody
{
    S32 mBodyIndex = -1;
    LLVector3 mDirection;           // unit vector, home body -> this body
    F32 mAngularDiameterDeg = 0.f;
    F32 mDistance = 0.f;            // arbitrary distance units, post-scale
};

class SSAtmoEnvPlanetaryResolver
{
public:
    // World-space position of every body in planetary.mBodies (same
    // indices), a Sun-hierarchy-root sitting at the origin. Everything is
    // fixed/authored - see SSAtmoEnvCelestialBody - there is no simulated
    // motion to evaluate at a point in time.
    //
    // A body whose *parent* is one half of a hierarchical bound pair
    // orbits that pair's mass-weighted barycenter rather than the named
    // parent alone - this is the entire mechanism a third body needs to
    // "orbit the pair": point its mParentIndex at either paired body.
    static std::vector<LLVector3> resolveWorldPositions(const SSAtmoEnvPlanetary& planetary);

    // Direction + apparent angular size of every *other* body as seen from
    // whichever one is flagged home - the home body itself is skipped (you
    // don't see yourself in the sky). Empty if no home body is set.
    static std::vector<SSAtmoEnvResolvedBody> resolveSky(const SSAtmoEnvPlanetary& planetary);

    // The computed primary sun arc, replacing a keyframed sun position
    // entirely per the design doc: driven by the home body's axial tilt
    // and the track's own day-length/offset (already wrapped into `time`
    // by the caller - see SSAtmoEnvTrack::currentDayCycleTime()), not by any
    // body's authored orbital position. Stylised rather than
    // astronomically exact - see the .cpp - since there is no orbital
    // position around a star being tracked to derive a real solar
    // declination from; the artist fixes "what season this is" via tilt
    // alone, per the design doc's no-orbital-motion decision.
    static void resolvePrimarySunArc(F32 home_axial_tilt_deg, F64 day_length_seconds, F64 time,
                                      F32& out_azimuth_deg, F32& out_elevation_deg);

private:
    // Fixed offset from whatever a body orbits (its parent, or a bound
    // pair's shared barycenter) - a single point on a tilted circle at a
    // fixed phase, not a swept path.
    static LLVector3 orbitOffset(F32 radius, F32 inclination_deg, F32 phase_deg);
};

// </SS:Nexii>

#endif // SS_ATMOENVPLANETARYSTATE_H
