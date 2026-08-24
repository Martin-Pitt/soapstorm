/**
 * @file ssatmoenvplanetarystate.h
 * @brief Atmo Magic: resolves a track's celestial bodies into fixed sky
 *        directions/angular sizes, and computes the sky's diurnal rotation
 *        from the home body's spin - phase 6 of
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

// The daily elevation curve of one authored sky direction, reduced to the
// three constants that describe it. Sweeping a direction through the home
// body's diurnal rotation makes its elevation a plain sinusoid in the
// rotation angle:
//
//     sin(elevation)(theta) = mOffset + mAmplitude * cos(theta - mThetaZero)
//
// with theta = -2*pi*phase (see resolveDiurnalDirection()). Every rise,
// set and culmination question below is that single identity solved for
// phase, so none of them re-derives the axis convention.
struct SSAtmoEnvDiurnalArc
{
    F32 mOffset = 0.f;      // sin(elevation) about which the day swings
    F32 mAmplitude = 0.f;   // half the day's swing; 0 = body pinned to the pole
    F32 mThetaZero = 0.f;   // rotation angle of the highest point
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

    // Sweeps an authored home-relative direction (resolveSky()'s output)
    // through the home body's diurnal rotation: one rotation for the
    // whole sky - every celestial direction, primary sun included, is
    // authored position x this rotation, so orbital radius/phase/
    // inclination visibly place every body and the whole sky rises and
    // sets as one. (The old stylised primary-sun arc is gone: it ignored
    // the authored position, which made the designer canvas a lie for
    // the one body that matters most.)
    // The observer's sky, properly.
    //
    // resolveSky() hands back directions in the ORBITAL plane's frame - the
    // ecliptic. Getting from there to what someone standing on the home
    // body sees is three rotations, and each one is a thing a world
    // actually has:
    //
    //   1. Obliquity. The home body's spin axis leans out of its orbital
    //      plane by mAxialTiltDeg, which tips the sun's daily circle north
    //      and south of the celestial equator through the year. This is
    //      what seasons ARE.
    //   2. The day. One turn about the celestial pole per cycle.
    //   3. Latitude. The pole sits mLatitudeDeg above the northern horizon,
    //      which is what decides whether the sun climbs overhead, skims the
    //      horizon, or never sets.
    //
    // These used to be one rotation about an axis derived from the axial
    // tilt alone, which made tilt do latitude's job: a world could have
    // seasons or an observer position but not both, and neither came out
    // where astronomy says it should.
    static LLVector3 resolveObserverDirection(const LLVector3& ecliptic_dir,
                                              F32 obliquity_deg, F32 latitude_deg,
                                              F64 phase);

    // Which resolved bodies hold EEP's two light slots. Both outputs come
    // back with mBodyIndex == -1 when that slot is empty (no emitters, no
    // home body, or an emitter coincident with home and therefore not in
    // the sky at all). Larger PHYSICAL diameter takes the sun slot - see
    // the rule's full rationale in ssatmoenvapplier.cpp, which is this
    // function's original home and still its main caller. It lives here
    // so the editor's rise/set markers and creation-time sky seeding ask
    // the same question the renderer answers, instead of each re-deriving
    // "which one is the sun" and drifting apart.
    static void resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                  const std::vector<SSAtmoEnvResolvedBody>& sky_bodies,
                                  SSAtmoEnvResolvedBody& out_sun,
                                  SSAtmoEnvResolvedBody& out_moon);

    // Same, resolving the sky itself - for callers that only want the
    // roles and are not already walking every body (the editor, seeding).
    static void resolveLightRoles(const SSAtmoEnvPlanetary& planetary,
                                  SSAtmoEnvResolvedBody& out_sun,
                                  SSAtmoEnvResolvedBody& out_moon);

    // The elevation sinusoid of one authored direction - see
    // SSAtmoEnvDiurnalArc.
    static SSAtmoEnvDiurnalArc diurnalArc(const LLVector3& ecliptic_dir,
                                          F32 obliquity_deg, F32 latitude_deg);

    // Phase of the arc's highest (`highest`) or lowest point - local noon
    // and local midnight for a sun. Always defined: even a body that never
    // rises has a moment it comes closest.
    static F64 culminationPhase(const SSAtmoEnvDiurnalArc& arc, bool highest);

    // Phase at which the arc reaches `sin_elevation`, on the rising half
    // of the day (`rising`) or the setting half. False when that elevation
    // is never reached, and then out_phase is the nearest the day gets to
    // it (culmination for a target too high, anti-culmination for one too
    // low) - a caller placing something at "this elevation" still gets the
    // most defensible phase rather than nothing.
    static bool phaseForElevation(const SSAtmoEnvDiurnalArc& arc,
                                  F32 sin_elevation, bool rising, F64& out_phase);

    // The phase at which `inertial_dir`, swept through the diurnal
    // rotation, points as nearly as it can at `target_dir` - the whole
    // direction, not just its height.
    //
    // This is what places an arbitrary authored sky on a cycle: a sky
    // carries the sun position it was painted for, and the honest home for
    // it is the moment this world's sun stands closest to standing there.
    // Elevation alone cannot answer that, since every height below the
    // peak happens twice a day - matching the full direction is what tells
    // a dawn sky from a dusk one, because their suns are on opposite sides
    // of the sky even at identical heights.
    //
    // Always defined: an unreachable target still has a closest approach,
    // and that is the phase returned.
    static F64 phaseForSunDirection(const LLVector3& ecliptic_dir,
                                    F32 obliquity_deg, F32 latitude_deg,
                                    const LLVector3& target_dir);

    // Rise and set phases (elevation exactly 0) of an authored direction.
    // False - and both outputs left alone - for a body that never crosses
    // the horizon at all: circumpolar, never-rising, or pinned to the
    // pole by a degenerate direction.
    static bool riseSetPhases(const LLVector3& ecliptic_dir,
                              F32 obliquity_deg, F32 latitude_deg,
                              F64& out_rise, F64& out_set);

private:
    // The observer's own axes, expressed in the EQUATORIAL frame (+Z the
    // celestial pole, +X the meridian at hour angle zero). Projecting a
    // body's equatorial direction onto these three is what turns it into
    // the east/north/up the renderer wants.
    static void observerAxes(F32 latitude_deg, LLVector3& out_east,
                             LLVector3& out_north, LLVector3& out_up);

    // Fixed offset from whatever a body orbits (its parent, or a bound
    // pair's shared barycenter) - a single point on a tilted circle at a
    // fixed phase, not a swept path.
    static LLVector3 orbitOffset(F32 radius, F32 inclination_deg, F32 phase_deg);
};

// </SS:Nexii>

#endif // SS_ATMOENVPLANETARYSTATE_H
