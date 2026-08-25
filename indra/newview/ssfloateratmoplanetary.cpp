/**
 * @file ssfloateratmoplanetary.cpp
 * @brief Atmo Magic planetary system designer implementation.
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

#include "ssfloateratmoplanetary.h"
#include "ssatmoenvmanager.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllineeditor.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltexturectrl.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "lluiimage.h"

#include <algorithm>
#include <cmath>

// <SS:Nexii> Atmo Magic planetary system designer

static LLDefaultChildRegistry::Register<SSOrbitViewCtrl> register_ss_orbit_view("ss_orbit_view");

static const F64 STATUS_POLL_INTERVAL = 0.5;

// The "angled top-down" of the whole view: every orbit's minor axis is the major times this. 0.45 reads as a tilted plane without foreshortening the vertical so far that rings collapse into lines.
static const F32 ORBIT_VIEW_TILT = 0.45f;

// The other half of the view direction. ORBIT_VIEW_TILT is how much of a ring's in-plane depth survives the projection - the sine of the camera's elevation above the orbital plane - so this is its
// cosine, and it is how much of a body's HEIGHT above that plane shows on screen. Derived rather than picked, so the diagram is one consistent view: at tilt 0.45 the camera sits about 27 degrees
// above the plane.
static const F32 ORBIT_VIEW_LIFT =
    0.893f; // sqrt(1 - ORBIT_VIEW_TILT^2), for ORBIT_VIEW_TILT = 0.45

// Below this the projected ring has collapsed to a line and a screen position no longer says which way round the orbit a body is - see inversePhaseDeg.
static const F32 ORBIT_RING_FLAT_EPS = 0.05f;

// Display-ring geometry. Radii here are NOT the authored orbital radii - those span metres to AU, so each level is normalised instead: the sun's children keep their authored ORDER (with a partial
// nod to authored scale - see ORBIT_RADIUS_LOG_WEIGHT) and spread out to the canvas edge, and moons get small fixed-pitch rings around their planet. First-ring starts raised after hands-on feedback:
// 90px keeps the innermost planet clear of a sun pair's discs, 26px keeps the first moon ring outside its planet's disc plus label.
static const F32 ORBIT_PLANET_FIRST_RING = 90.f; // px, innermost planet ring
static const F32 ORBIT_MOON_FIRST_RING = 26.f;   // px, innermost moon ring
static const F32 ORBIT_MOON_RING_SPACING = 16.f; // px between moon rings

// How much of a planet ring's position comes from log-scaled authored radii rather than pure even rank spacing. Pure rank hides that one planet sits ten times farther out than the rest; pure (or
// linear) authored scale lets AU-ratios flatten the whole inner system onto the first ring. 0.4 of the log term makes a far-out planet visibly farther without crushing the inner rings together.
static const F32 ORBIT_RADIUS_LOG_WEIGHT = 0.4f;
// The scale dials compress display spacing multiplicatively, but never all the way to zero - a floor keeps a fully compressed system readable (and clickable) rather than stacking every body onto its
// parent's pixel.
static const F32 ORBIT_PLANET_RING_FLOOR = 30.f;
static const F32 ORBIT_MOON_RING_FLOOR = 10.f;
// Total on-screen separation of a bound sun pair around their pair centre.
static const F32 ORBIT_PAIR_SEPARATION = 56.f;

// Hit-test tolerances: a body wins within this many pixels of its centre (or its own radius if larger); a ring is the thin fallback hitbox.
static const F32 ORBIT_BODY_HIT_RADIUS = 10.f;
static const F32 ORBIT_RING_HIT_RADIUS = 5.f;

// The pair-centre handle (the outer pair's grab point for its orbit around the inner barycenter). The drawn circle sits well inside its hitbox so the handle reads as a control, not another body
// disc.
static const F32 ORBIT_PAIR_HANDLE_HIT_RADIUS = 8.f;
static const F32 ORBIT_PAIR_HANDLE_DRAW_RADIUS = 5.f;
static const F32 ORBIT_PAIR_HANDLE_DOT_RADIUS = 1.5f;

// Segments per ring ellipse - shared by drawing and ring hit-testing so a click lands on exactly the curve that was drawn.
static const S32 ORBIT_RING_SEGMENTS = 96;

// Zoom: exponential steps (3 notches per doubling), clamped. 1.0 is the fit-everything layout by construction - computeLayout() sizes the outermost ring to the canvas at zoom 1 - so the minimum only
// ever shrinks an already-fitting system and nothing can be zoomed out of reach.
static const F32 ORBIT_ZOOM_MIN = 0.5f;
static const F32 ORBIT_ZOOM_MAX = 4.f;
static const F32 ORBIT_ZOOM_STEP = 1.2599f; // 2^(1/3)

// Home marker: a small house icon over the body instead of a name suffix.
static const S32 ORBIT_HOME_ICON_SIZE = 16;

// Label fan-out (see draw()): the exclusion circle a displaced label's near edge kisses is the body's draw radius plus this pad (which also makes the undisplaced below-the-body spot identical to the
// old fixed 2px gap), and the ray walks the circle in 360/steps-degree notches.
static const F32 ORBIT_LABEL_PAD = 2.f;
static const S32 ORBIT_LABEL_ANGLE_STEPS = 24;

// Unit conversions for the properties panel. Storage is always metres (and per-level relative mass); only the spinners speak AU (planets), km (moons, and every diameter but a sun's) and solar
// diameters (suns).
static const F32 SS_METRES_PER_AU = 1.496e11f;
static const F32 SS_METRES_PER_KM = 1000.f;
static const F32 SS_METRES_PER_SOLAR_DIAMETER = 1.392e9f;

//-----------------------------------------------------------------------------
// SSOrbitViewCtrl
//-----------------------------------------------------------------------------

// Wraps any angle into the stored 0..360 phase domain - grab offsets can push the raw cursor angle well outside it in either direction.
static F32 ss_wrap_phase_deg(F32 deg)
{
    deg = fmodf(deg, 360.f);
    return (deg < 0.f) ? deg + 360.f : deg;
}

SSOrbitViewCtrl::Params::Params()
{
}

SSOrbitViewCtrl::SSOrbitViewCtrl(const Params& p) :
    LLUICtrl(p)
{
}

// static
void SSOrbitViewCtrl::projectOnRing(F32 anchor_x, F32 anchor_y, F32 ring_radius, F32 tilt_rad,
                                    F32 phase_deg, F32& out_x, F32& out_y)
{
    // The same orbit the RESOLVER builds, projected. SSAtmoEnvPlanetaryResolver::orbitOffset places a body at ( r cos p,  r sin p cos i,  r sin p sin i ) so an inclined orbit is a circle tipped out
    // of the reference plane - half of it lifts above, half drops below. This projects exactly that. It used to rotate the finished ellipse in the picture plane by a fraction of the inclination
    // instead, which is not what inclination does to an orbit: it spun the ring like a dial while leaving it flat, so the diagram disagreed with the sky it was supposed to be describing and no
    // amount of inclination ever tipped anything.
    const F32 a = phase_deg * DEG_TO_RAD;
    const F32 in_plane = sinf(a) * ring_radius;

    const F32 dx = cosf(a) * ring_radius;
    const F32 dy = in_plane * cosf(tilt_rad);   // along the plane, into the screen
    const F32 dz = in_plane * sinf(tilt_rad);   // out of the plane, upward

    out_x = anchor_x + dx;
    out_y = anchor_y + dy * ORBIT_VIEW_TILT + dz * ORBIT_VIEW_LIFT;
}

// static
F32 SSOrbitViewCtrl::inversePhaseDeg(F32 anchor_x, F32 anchor_y, F32 tilt_rad, S32 x, S32 y)
{
    // Inverse of projectOnRing. Screen x is r cos p outright, and screen y is r sin p times one combined factor - the ring's flattening and its inclination lift added together - so dividing that out
    // leaves sin p against cos p and the angle follows. The cursor's radius never enters, so a drag can wander off the curve without the phase fighting back.
    const F32 dx = (F32)x - anchor_x;
    const F32 dy = (F32)y - anchor_y;

    const F32 depth = cosf(tilt_rad) * ORBIT_VIEW_TILT + sinf(tilt_rad) * ORBIT_VIEW_LIFT;

    // A ring seen exactly edge-on projects to a line: every phase on one side of it lands on the same pixel, so a drag there has no answer to give. Reading the phase off x alone at least keeps the
    // near half tracking the cursor instead of thrashing between two branches.
    if (fabsf(depth) < ORBIT_RING_FLAT_EPS)
    {
        F32 edge_deg = (dx >= 0.f) ? 0.f : 180.f;
        return edge_deg;
    }

    F32 phase_deg = atan2f(dy / depth, dx) * RAD_TO_DEG;
    if (phase_deg < 0.f) phase_deg += 360.f;
    return phase_deg;
}

// static
bool SSOrbitViewCtrl::sunPairMembers(const SSAtmoEnvPlanetary& planetary, S32 index,
                                     S32& out_senior, S32& out_junior)
{
    const S32 n = (S32)planetary.mBodies.size();
    if (index < 0 || index >= n) return false;
    if (planetary.mBodies[index].mKind != SSAtmoEnvCelestialBody::SUN) return false;

    const S32 partner = planetary.mBodies[index].mBoundPartnerIndex;
    if (partner < 0 || partner >= n || partner == index) return false;
    if (planetary.mBodies[partner].mKind != SSAtmoEnvCelestialBody::SUN) return false;
    // Symmetry check, same as the resolver's pairJunior() - a dangling one-way reference from a hand-edited notecard is not a pair.
    if (planetary.mBodies[partner].mBoundPartnerIndex != index) return false;

    out_senior = llmin(index, partner);
    out_junior = llmax(index, partner);
    return true;
}

// static
void SSOrbitViewCtrl::placePairMembers(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out,
                                       S32 senior, S32 junior, F32 centre_x, F32 centre_y)
{
    // Mirrors the resolver's placePair(): the heavier member sits closer to the shared centre, each at separation * other_mass / total, on opposite sides. Orientation is the JUNIOR's phase (and its
    // display tilt) - the same field the canvas's pair drag edits, so the diagram swings exactly where a drag puts it, and the members' mass-weighted barycenter stays on the centre by construction
    // (which is what keeps the child-anchor barycenter math downstream correct).
    const F32 mass_s = llmax(planetary.mBodies[senior].mMassRelative, 0.0001f);
    const F32 mass_j = llmax(planetary.mBodies[junior].mMassRelative, 0.0001f);
    const F32 total = mass_s + mass_j;
    const F32 phase_deg = planetary.mBodies[junior].mOrbitalPhaseDeg;
    const F32 tilt_rad = out[junior].mTiltRad;

    projectOnRing(centre_x, centre_y, ORBIT_PAIR_SEPARATION * (mass_s / total), tilt_rad,
                  phase_deg, out[junior].mX, out[junior].mY);
    projectOnRing(centre_x, centre_y, ORBIT_PAIR_SEPARATION * (mass_j / total), tilt_rad,
                  phase_deg + 180.f, out[senior].mX, out[senior].mY);

    out[senior].mPairPartner = junior;
    out[junior].mPairPartner = senior;
    out[senior].mPairCentreX = centre_x;
    out[senior].mPairCentreY = centre_y;
    out[junior].mPairCentreX = centre_x;
    out[junior].mPairCentreY = centre_y;
    // The junior's anchor is the pair centre; the senior's is the caller's to set (an outer pair's senior anchors its drawn ring at the inner barycenter, which is not the centre the members swing
    // about).
    out[junior].mAnchorX = centre_x;
    out[junior].mAnchorY = centre_y;
    out[senior].mResolved = true;
    out[junior].mResolved = true;
}

void SSOrbitViewCtrl::computeLayout(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out) const
{
    const S32 n = (S32)planetary.mBodies.size();
    out.assign((size_t)n, Placement());
    for (S32 i = 0; i < n; ++i)
    {
        out[i].mIndex = i;
        // Body discs are sized by log of diameter: absolute sizes span five orders of magnitude, and a diagram only needs "sun > planet > moon" to read, not the true ratio.
        const F32 diameter = llmax(planetary.mBodies[i].mDiameterM, 2.f);
        out[i].mDrawRadius = llclamp(1.5f * log10f(diameter), 4.f, 14.f);
        // The authored inclination in full: projectOnRing now tips the orbit the way the resolver does, so there is nothing to scale down. The old fraction existed because the display rotated the
        // ellipse in the picture plane, where a full inclination tangled every ring through every other one.
        out[i].mTiltRad = llclamp(planetary.mBodies[i].mOrbitalInclinationDeg, -90.f, 90.f)
                          * DEG_TO_RAD;
    }
    if (n == 0) return;

    // Derived lineage - the schema's effectiveParent() is the single authority (planets always hang under the first sun, moons and suns under their stored parent with invalids degraded to root).
    std::vector<S32> eff_parent((size_t)n, -1);
    for (S32 i = 0; i < n; ++i)
    {
        eff_parent[i] = planetary.effectiveParent(i);
    }

    // Sibling rank by authored orbital radius: display rings keep authored ORDER, not authored scale. For non-moon children a 0..1 spread fraction is also computed, blending even rank spacing with
    // log-scaled authored radii (ORBIT_RADIUS_LOG_WEIGHT) so real distance differences partially show through.
    std::vector<S32> rank((size_t)n, 0);
    std::vector<F32> spread_frac((size_t)n, 0.f);
    for (S32 parent = -1; parent < n; ++parent)
    {
        std::vector<S32> siblings;
        for (S32 i = 0; i < n; ++i)
        {
            if (eff_parent[i] == parent) siblings.push_back(i);
        }
        std::stable_sort(siblings.begin(), siblings.end(),
            [&planetary](S32 a, S32 b)
            { return planetary.mBodies[a].mOrbitalRadius < planetary.mBodies[b].mOrbitalRadius; });

        // Log-space extent of the group's authored radii. Radii clamped away from zero before the log - an unauthored 0 shouldn't put a ring at minus infinity.
        const F32 log_min = (siblings.empty()) ? 0.f
            : log10f(llmax(planetary.mBodies[siblings.front()].mOrbitalRadius, 1.f));
        const F32 log_max = (siblings.empty()) ? 0.f
            : log10f(llmax(planetary.mBodies[siblings.back()].mOrbitalRadius, 1.f));
        const F32 log_span = log_max - log_min;

        for (size_t r = 0; r < siblings.size(); ++r)
        {
            rank[siblings[r]] = (S32)r;

            const F32 steps = (F32)llmax((S32)siblings.size() - 1, 1);
            const F32 rank_frac = (F32)r / steps;
            // Degenerate spans (one sibling, or all radii equal) fall back to pure rank spacing rather than dividing by zero.
            const F32 log_frac = (log_span > 0.0001f)
                ? (log10f(llmax(planetary.mBodies[siblings[r]].mOrbitalRadius, 1.f)) - log_min) / log_span
                : rank_frac;
            spread_frac[siblings[r]] = rank_frac * (1.f - ORBIT_RADIUS_LOG_WEIGHT)
                                     + log_frac * ORBIT_RADIUS_LOG_WEIGHT;
        }
    }

    // Roots, grouped into units: a bound pair of roots is one unit offset around its barycenter; everything else is a unit of one. Multiple units share the canvas side by side.
    std::vector<std::vector<S32>> units;
    {
        std::vector<bool> grouped((size_t)n, false);
        for (S32 i = 0; i < n; ++i)
        {
            if (eff_parent[i] != -1 || grouped[i]) continue;
            grouped[i] = true;
            const S32 partner = planetary.mBodies[i].mBoundPartnerIndex;
            if (partner >= 0 && partner < n && partner != i && !grouped[partner]
                && eff_parent[partner] == -1
                && planetary.mBodies[partner].mBoundPartnerIndex == i)
            {
                grouped[partner] = true;
                units.push_back({ i, partner });
            }
            else
            {
                units.push_back({ i });
            }
        }
    }

    const F32 width = (F32)getLocalRect().getWidth();
    const F32 height = (F32)getLocalRect().getHeight();
    const F32 centre_y = height * 0.5f;
    const S32 unit_count = (S32)units.size();
    const F32 unit_span = (unit_count > 1) ? (width - 80.f) / (F32)unit_count : width;

    // How far the outermost planet ring may reach. The height is divided by the tilt so the flattened ellipse fills vertically as generously as horizontally.
    F32 extent = llmin(width, height / ORBIT_VIEW_TILT) * 0.5f - 40.f;
    if (unit_count > 1) extent = llmin(extent, unit_span * 0.5f - 12.f);
    // The floor stays above the first ring so a cramped canvas still leaves the outer planets somewhere to spread to.
    extent = llmax(extent, ORBIT_PLANET_FIRST_RING + 50.f);

    // Where the sun group's collective barycenter must land after the shift below: the first root sun unit's slot anchor. Planets ring this exact point (their anchors resolve against the PRE-shift
    // inner centre, which sits here), so shifting the suns onto it is what makes planets ring the group barycenter, mirroring the resolver.
    F32 sun_group_anchor_x = width * 0.5f;
    F32 sun_group_anchor_y = centre_y;
    bool have_sun_group_anchor = false;

    for (S32 u = 0; u < unit_count; ++u)
    {
        const F32 anchor_x = (unit_count > 1)
            ? 40.f + unit_span * ((F32)u + 0.5f)
            : width * 0.5f;

        if (!have_sun_group_anchor
            && planetary.mBodies[units[u][0]].mKind == SSAtmoEnvCelestialBody::SUN)
        {
            sun_group_anchor_x = anchor_x;
            sun_group_anchor_y = centre_y;
            have_sun_group_anchor = true;
        }

        if (units[u].size() == 2)
        {
            // A bound root pair swings about the unit anchor by the junior's phase - units[] grouped ascending, so [0] is the lower index, the SENIOR. The centre itself is pinned (it is the system
            // origin), which is why root members keep ring radius 0: no pair-as-a-unit orbit exists to draw or drag.
            const S32 senior = units[u][0];
            const S32 junior = units[u][1];
            out[senior].mAnchorX = anchor_x;
            out[senior].mAnchorY = centre_y;
            placePairMembers(planetary, out, senior, junior, anchor_x, centre_y);
        }
        else
        {
            const S32 i = units[u][0];
            out[i].mX = anchor_x;
            out[i].mY = centre_y;
            out[i].mAnchorX = anchor_x;
            out[i].mAnchorY = centre_y;
            out[i].mResolved = true;
        }
    }

    // Children, a level per pass - Sun -> Planet -> Moon is three levels by
    // design, so a fixed handful of passes always converges (same approach
    // as SSAtmoEnvPlanetaryResolver::resolveWorldPositions).
    const F32 sun_planet_scale = llclamp(planetary.mSunPlanetScale, 0.f, 1.f);
    const F32 planet_moon_scale = llclamp(planetary.mPlanetMoonScale, 0.f, 1.f);
    for (S32 pass = 0; pass < 4; ++pass)
    {
        bool progressed = false;
        for (S32 i = 0; i < n; ++i)
        {
            if (out[i].mResolved) continue;
            const S32 parent = eff_parent[i];
            if (parent < 0 || !out[parent].mResolved) continue;

            // A child of one half of a bound pair orbits the pair's mass-weighted barycenter, matching the resolver's rule.
            F32 anchor_x = out[parent].mX;
            F32 anchor_y = out[parent].mY;
            const S32 partner = planetary.mBodies[parent].mBoundPartnerIndex;
            if (partner >= 0 && partner < n && partner != parent && out[partner].mResolved)
            {
                const F32 mass_p = llmax(planetary.mBodies[parent].mMassRelative, 0.0001f);
                const F32 mass_q = llmax(planetary.mBodies[partner].mMassRelative, 0.0001f);
                anchor_x = (out[parent].mX * mass_p + out[partner].mX * mass_q) / (mass_p + mass_q);
                anchor_y = (out[parent].mY * mass_p + out[partner].mY * mass_q) / (mass_p + mass_q);
            }

            // Which level's spacing applies is the child's own kind, same as the resolver's scale-dial rule: moons get the tight fixed pitch, everything else spreads over the canvas.
            F32 base;
            F32 floor_radius;
            F32 scale;
            if (planetary.mBodies[i].mKind == SSAtmoEnvCelestialBody::MOON)
            {
                base = ORBIT_MOON_FIRST_RING + ORBIT_MOON_RING_SPACING * (F32)rank[i];
                floor_radius = ORBIT_MOON_RING_FLOOR;
                scale = planet_moon_scale;
            }
            else
            {
                base = ORBIT_PLANET_FIRST_RING
                     + (extent - ORBIT_PLANET_FIRST_RING) * spread_frac[i];
                floor_radius = ORBIT_PLANET_RING_FLOOR;
                scale = sun_planet_scale;
            }
            const F32 ring = floor_radius + (base - floor_radius) * scale;

            out[i].mAnchorX = anchor_x;
            out[i].mAnchorY = anchor_y;
            out[i].mRingCentreX = anchor_x;
            out[i].mRingCentreY = anchor_y;
            out[i].mRingRadius = ring;

            // A bound sun pair below root (the canonical outer pair): the senior's ring carries the pair-as-a-unit around the inner barycenter - its own phase places the pair centre on that ring -
            // and both members then swing about that centre by the junior's phase, the resolver's convention exactly. The ascending scan reaches the senior first (same parent, same pass), so `i` is
            // the senior whenever this fires; a partner parented elsewhere (hand-edited notecard) falls through to an ordinary individual ring instead.
            S32 pair_senior = -1;
            S32 pair_junior = -1;
            if (sunPairMembers(planetary, i, pair_senior, pair_junior)
                && pair_senior == i
                && eff_parent[pair_junior] == parent
                && !out[pair_junior].mResolved)
            {
                F32 pair_cx = 0.f;
                F32 pair_cy = 0.f;
                projectOnRing(anchor_x, anchor_y, ring, out[i].mTiltRad,
                              planetary.mBodies[i].mOrbitalPhaseDeg, pair_cx, pair_cy);
                placePairMembers(planetary, out, pair_senior, pair_junior, pair_cx, pair_cy);
            }
            else
            {
                projectOnRing(anchor_x, anchor_y, ring, out[i].mTiltRad,
                              planetary.mBodies[i].mOrbitalPhaseDeg, out[i].mX, out[i].mY);
                out[i].mResolved = true;
            }
            progressed = true;
        }
        if (!progressed) break;
    }

    // --- Shift the whole sun group so its mass-weighted display barycenter lands on the system anchor - the same move the resolver makes at the origin, and what makes two equal-mass pairs render
    // antipodal about the centre (mutually orbiting) instead of one pinned with the other circling it. Planets need no touch: their anchors resolved against the PRE-shift inner centre, which is the
    // system anchor - exactly the group barycenter they should ring.
    if (have_sun_group_anchor)
    {
        F32 weighted_x = 0.f;
        F32 weighted_y = 0.f;
        F32 total_mass = 0.f;
        for (S32 i = 0; i < n; ++i)
        {
            if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN || !out[i].mResolved) continue;
            const F32 mass = llmax(planetary.mBodies[i].mMassRelative, 0.0001f);
            weighted_x += out[i].mX * mass;
            weighted_y += out[i].mY * mass;
            total_mass += mass;
        }
        if (total_mass > 0.f)
        {
            const F32 shift_x = sun_group_anchor_x - weighted_x / total_mass;
            const F32 shift_y = sun_group_anchor_y - weighted_y / total_mass;
            for (S32 i = 0; i < n; ++i)
            {
                if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN || !out[i].mResolved) continue;
                out[i].mX += shift_x;
                out[i].mY += shift_y;
                out[i].mAnchorX += shift_x;
                out[i].mAnchorY += shift_y;
                out[i].mPairCentreX += shift_x;
                out[i].mPairCentreY += shift_y;
            }
        }

        // The single ring an orbiting sun unit's senior carried around the inner centre would now be a lie - after the shift, no body travels it. Replace it with the actual paths: the senior's
        // authored orbit vector R between the unit centres decomposes by opposing masses about the group barycenter, the outer unit's centre at R * m_inner / total and the inner unit's at R *
        // m_outer / total on the opposite side (equal masses: two equal rings, the perfectly barycentred look). The shift above has already put every centre exactly on these rings. The senior's
        // ANCHOR deliberately stays on the inner unit's shifted centre: its phase is the direction inner->outer, so that point is what the centre-handle drag inverts about.
        for (S32 i = 0; i < n; ++i)
        {
            if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN) continue;
            if (!out[i].mResolved || out[i].mRingRadius <= 0.5f) continue;
            const S32 parent = eff_parent[i];
            if (parent < 0 || parent >= n || !out[parent].mResolved) continue;
            if (planetary.mBodies[parent].mKind != SSAtmoEnvCelestialBody::SUN) continue;

            F32 mass_outer = llmax(planetary.mBodies[i].mMassRelative, 0.0001f);
            if (out[i].mPairPartner >= 0)
            {
                mass_outer += llmax(planetary.mBodies[out[i].mPairPartner].mMassRelative, 0.0001f);
            }
            F32 mass_inner = llmax(planetary.mBodies[parent].mMassRelative, 0.0001f);
            const S32 parent_partner = out[parent].mPairPartner;
            if (parent_partner >= 0)
            {
                mass_inner += llmax(planetary.mBodies[parent_partner].mMassRelative, 0.0001f);
            }
            const F32 total = mass_inner + mass_outer;
            const F32 r_full = out[i].mRingRadius;

            out[i].mRingCentreX = sun_group_anchor_x;
            out[i].mRingCentreY = sun_group_anchor_y;
            out[i].mRingRadius = r_full * (mass_inner / total);
            out[i].mCounterRingRadius = r_full * (mass_outer / total);
            // The inner PAIR's shifted centre doubles as a second grab point for the same phase (+180 - it sits antipodal). Only a paired parent offers it: a lone parent sun IS the inner centre, and
            // with the handle outranking bodies on mouse-down a handle there would steal every click on that sun's disc.
            if (parent_partner >= 0)
            {
                out[i].mHasCounterHandle = true;
                out[i].mCounterCentreX = out[parent].mPairCentreX;
                out[i].mCounterCentreY = out[parent].mPairCentreY;
            }
        }
    }

    // Anything still unresolved is a parent cycle from a hand-edited notecard - stack them in the corner where they are at least visible and selectable rather than absent.
    S32 stranded = 0;
    for (S32 i = 0; i < n; ++i)
    {
        if (out[i].mResolved) continue;
        out[i].mX = 24.f + 28.f * (F32)stranded;
        out[i].mY = 24.f;
        out[i].mAnchorX = out[i].mX;
        out[i].mAnchorY = out[i].mY;
        out[i].mResolved = true;
        ++stranded;
    }

    // Zoom last, as a uniform scale of every position and ring radius about the canvas centre - applied inside the layout (rather than a GL transform at draw time) so hit-testing and the phase-drag
    // inverse see exactly the zoomed geometry for free. Body discs and fonts are diagram glyphs and deliberately keep their size.
    if (mZoom != 1.f)
    {
        const F32 cx = width * 0.5f;
        const F32 cy = height * 0.5f;
        for (Placement& p : out)
        {
            p.mX = cx + (p.mX - cx) * mZoom;
            p.mY = cy + (p.mY - cy) * mZoom;
            p.mAnchorX = cx + (p.mAnchorX - cx) * mZoom;
            p.mAnchorY = cy + (p.mAnchorY - cy) * mZoom;
            p.mPairCentreX = cx + (p.mPairCentreX - cx) * mZoom;
            p.mPairCentreY = cy + (p.mPairCentreY - cy) * mZoom;
            p.mRingCentreX = cx + (p.mRingCentreX - cx) * mZoom;
            p.mRingCentreY = cy + (p.mRingCentreY - cy) * mZoom;
            p.mCounterCentreX = cx + (p.mCounterCentreX - cx) * mZoom;
            p.mCounterCentreY = cy + (p.mCounterCentreY - cy) * mZoom;
            p.mRingRadius *= mZoom;
            p.mCounterRingRadius *= mZoom;
        }
    }
}

void SSOrbitViewCtrl::drawRing(F32 centre_x, F32 centre_y, F32 radius, F32 tilt_rad, const LLColor4& color) const
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::LINE_LOOP);
    for (S32 seg = 0; seg < ORBIT_RING_SEGMENTS; ++seg)
    {
        const F32 phase_deg = 360.f * (F32)seg / (F32)ORBIT_RING_SEGMENTS;
        F32 x = 0.f, y = 0.f;
        projectOnRing(centre_x, centre_y, radius, tilt_rad, phase_deg, x, y);
        gGL.vertex2f(x, y);
    }
    gGL.end();
}

void SSOrbitViewCtrl::draw()
{
    const LLRect local = getLocalRect();
    gl_rect_2d(local, LLColor4(0.06f, 0.07f, 0.10f, 1.f), true);
    gl_rect_2d(local, LLColor4(0.32f, 0.34f, 0.38f, 1.f), false);

    SSAtmoEnvPlanetary* planetary = mPlanetary ? mPlanetary() : nullptr;
    if (!planetary)
    {
        LLView::draw();
        return;
    }

    // Read fresh and laid out per frame - see computeLayout()'s comment.
    std::vector<Placement> placements;
    computeLayout(*planetary, placements);

    const std::vector<S32> emitters = planetary->lightEmitterIndices();

    // Rings first so every body draws on top of every curve. An orbiting sun unit's counter-ring (the inner unit's antipodal path) shares its senior's colour and selection state - the two curves are
    // one mutual orbit, not two features.
    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const bool selected = (p.mIndex == mSelectedIndex);
        // A hovered ring brightens toward the selected colour - the "this curve is clickable" cue before the user commits.
        const bool ring_hovered = (p.mIndex == mHoverIndex && !mHoverOnBody);
        const LLColor4 ring_color = selected ? LLColor4(0.75f, 0.78f, 0.85f, 0.9f)
                                             : ring_hovered ? LLColor4(0.62f, 0.65f, 0.72f, 0.6f)
                                                            : LLColor4(0.45f, 0.45f, 0.50f, 0.35f);
        if (p.mRingRadius > 0.5f)
        {
            drawRing(p.mRingCentreX, p.mRingCentreY, p.mRingRadius, p.mTiltRad, ring_color);
        }
        if (p.mCounterRingRadius > 0.5f)
        {
            drawRing(p.mRingCentreX, p.mRingCentreY, p.mCounterRingRadius, p.mTiltRad, ring_color);
        }
    }

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const SSAtmoEnvCelestialBody& body = planetary->mBodies[p.mIndex];
        const bool selected = (p.mIndex == mSelectedIndex);
        const bool is_emitter =
            std::find(emitters.begin(), emitters.end(), p.mIndex) != emitters.end();

        // Light emitters get a soft warm halo behind everything else of theirs - the "this is what lights the scene" marker.
        if (is_emitter)
        {
            gGL.color4f(1.f, 0.85f, 0.45f, 0.35f);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius + 5.f, 24, false);
        }

        LLColor4 fill;
        switch (body.mKind)
        {
            case SSAtmoEnvCelestialBody::SUN:    fill = LLColor4(1.00f, 0.82f, 0.35f, 1.f); break;
            case SSAtmoEnvCelestialBody::PLANET: fill = LLColor4(0.55f, 0.72f, 0.95f, 1.f); break;
            case SSAtmoEnvCelestialBody::MOON:   fill = LLColor4(0.72f, 0.72f, 0.78f, 1.f); break;
        }
        // Hover gets the same brightening as selection but stops short of the white selection ring below - enough to say "clickable" without pretending the body is already selected.
        const bool body_hovered = (p.mIndex == mHoverIndex && mHoverOnBody);
        if (selected || body_hovered)
        {
            fill.mV[0] = llmin(fill.mV[0] * 1.25f, 1.f);
            fill.mV[1] = llmin(fill.mV[1] * 1.25f, 1.f);
            fill.mV[2] = llmin(fill.mV[2] * 1.25f, 1.f);
        }
        gGL.color4fv(fill.mV);
        gl_circle_2d(p.mX, p.mY, p.mDrawRadius, 24, true);

        // The home body's distinct outline sits inside the selection ring so both stay readable when the home body is also selected.
        if (body.mIsHome)
        {
            gGL.color4f(0.30f, 0.95f, 0.70f, 0.9f);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius + 2.f, 24, false);
        }
        if (selected)
        {
            gGL.color4f(1.f, 1.f, 1.f, 0.9f);
            gl_circle_2d(p.mX, p.mY, p.mDrawRadius + (body.mIsHome ? 4.f : 3.f), 24, false);
        }

        // Home reads as a small house icon centred on the body rather than a " (home)" name suffix - the label stays just the name, matching the list. Tinted the same green as the home outline so
        // the two read as one marker.
        if (body.mIsHome)
        {
            LLPointer<LLUIImage> home_icon = LLUI::getUIImage("Home_Off");
            if (home_icon)
            {
                home_icon->draw((S32)(p.mX - (F32)(ORBIT_HOME_ICON_SIZE / 2)),
                                (S32)(p.mY - (F32)(ORBIT_HOME_ICON_SIZE / 2)),
                                ORBIT_HOME_ICON_SIZE, ORBIT_HOME_ICON_SIZE,
                                LLColor4(0.30f, 0.95f, 0.70f, 0.9f));
            }
        }
    }

    // Labels last, on top of every disc, with name-tag style fan-out: a label keeps the plain below-the-body spot unless its rect would overlap an already-placed label's (tight pair, clustered
    // moons, zoomed out); then its ray walks around the body's exclusion circle - both directions, nearest angle first - until a clear spot is found, the box's near edge always kissing the circle so
    // it stays visually attached. Greedy, in body-index order with the SELECTED body first so neighbours can never displace it - a fixed order is also what keeps the result deterministic frame to
    // frame, since the layout recomputes every draw and jittering labels would be worse than any single imperfect arrangement.
    {
        struct SSLabelRect { F32 mLeft; F32 mRight; F32 mTop; F32 mBottom; };
        std::vector<SSLabelRect> placed_rects;
        placed_rects.reserve(placements.size());

        std::vector<S32> label_order;
        label_order.reserve(placements.size());
        if (mSelectedIndex >= 0 && mSelectedIndex < (S32)placements.size())
        {
            label_order.push_back(mSelectedIndex);
        }
        for (S32 i = 0; i < (S32)placements.size(); ++i)
        {
            if (i != mSelectedIndex) label_order.push_back(i);
        }

        const F32 line_h = (F32)font->getLineHeight();
        const F32 angle_step = 360.f / (F32)ORBIT_LABEL_ANGLE_STEPS;

        for (const S32 index : label_order)
        {
            const Placement& p = placements[index];
            if (!p.mResolved) continue;
            const SSAtmoEnvCelestialBody& body = planetary->mBodies[p.mIndex];
            const bool selected = (p.mIndex == mSelectedIndex);

            const F32 half_w = 0.5f * (F32)font->getWidth(body.mName);
            const F32 exclusion = p.mDrawRadius + ORBIT_LABEL_PAD;

            SSLabelRect rect = SSLabelRect();
            bool found = false;
            for (S32 probe = 0; probe < ORBIT_LABEL_ANGLE_STEPS && !found; ++probe)
            {
                // Probe 0 is straight down (the default spot); then +step, -step, +2*step, -2*step ... fanning colliding neighbours apart to opposite sides.
                const F32 delta = (F32)((probe + 1) / 2) * angle_step
                                * ((probe % 2 == 1) ? 1.f : -1.f);
                const F32 theta = (-90.f + delta) * DEG_TO_RAD;
                const F32 dir_x = cosf(theta);
                const F32 dir_y = sinf(theta);
                // The rect's support extent along the ray places its near edge exactly on the exclusion circle: pushed sideways it sits beside the body, pushed up it sits above.
                const F32 offset = exclusion + half_w * fabsf(dir_x) + 0.5f * line_h * fabsf(dir_y);
                const F32 cx = p.mX + dir_x * offset;
                const F32 cy = p.mY + dir_y * offset;

                SSLabelRect candidate;
                candidate.mLeft = cx - half_w;
                candidate.mRight = cx + half_w;
                candidate.mTop = cy + 0.5f * line_h;
                candidate.mBottom = cy - 0.5f * line_h;

                bool collides = false;
                for (const SSLabelRect& other : placed_rects)
                {
                    if (candidate.mLeft < other.mRight && candidate.mRight > other.mLeft
                        && candidate.mBottom < other.mTop && candidate.mTop > other.mBottom)
                    {
                        collides = true;
                        break;
                    }
                }
                if (probe == 0) rect = candidate; // the fallback if every angle collides
                if (!collides)
                {
                    rect = candidate;
                    found = true;
                }
            }
            // A full circle of collisions (pathological pile-up) falls back to the default spot - overlapped beats vanished.

            placed_rects.push_back(rect);
            font->renderUTF8(body.mName, 0,
                             (S32)(0.5f * (rect.mLeft + rect.mRight)), (S32)rect.mTop,
                             selected ? LLColor4::white : LLColor4(0.85f, 0.85f, 0.85f, 0.9f),
                             LLFontGL::HCENTER, LLFontGL::TOP);
        }
    }

    // The pair-centre handles: outline circle plus centre dot at an orbiting pair's projected centre - or, antipodal flavour, at the inner pair's shifted centre - in the selection colour. Hover-only
    // chrome (or while its own drag runs): drawn permanently it would read as one more body sitting between a pair's members.
    const S32 handle_index = (mDragMode == DRAG_CENTRE) ? mDragIndex : mHoverHandleIndex;
    const bool handle_antipodal = (mDragMode == DRAG_CENTRE) ? mDragAntipodal : mHoverHandleAntipodal;
    if (handle_index >= 0 && handle_index < (S32)placements.size())
    {
        const Placement& p = placements[handle_index];
        // Same qualifying rules handleHitTest() applies - the hover index can go stale between events if a poll reload re-shapes the system, so re-check against this frame's placements.
        const bool handle_valid = handle_antipodal
            ? p.mHasCounterHandle
            : (p.mPairPartner >= 0 && (p.mRingRadius > 0.5f || p.mCounterRingRadius > 0.5f));
        if (p.mResolved && handle_valid)
        {
            const F32 hx = handle_antipodal ? p.mCounterCentreX : p.mPairCentreX;
            const F32 hy = handle_antipodal ? p.mCounterCentreY : p.mPairCentreY;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.color4f(1.f, 1.f, 1.f, 0.9f);
            gl_circle_2d(hx, hy, ORBIT_PAIR_HANDLE_DRAW_RADIUS, 24, false);
            gl_circle_2d(hx, hy, ORBIT_PAIR_HANDLE_DOT_RADIUS, 8, true);
        }
    }

    LLView::draw();
}

S32 SSOrbitViewCtrl::hitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_on_body) const
{
    const F32 fx = (F32)x;
    const F32 fy = (F32)y;

    // Nearest body within tolerance wins outright.
    S32 best = -1;
    F32 best_dist = F32_MAX;
    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const F32 dist = sqrtf((fx - p.mX) * (fx - p.mX) + (fy - p.mY) * (fy - p.mY));
        const F32 tolerance = llmax(ORBIT_BODY_HIT_RADIUS, p.mDrawRadius + 2.f);
        if (dist <= tolerance && dist < best_dist)
        {
            best = p.mIndex;
            best_dist = dist;
        }
    }
    if (best >= 0)
    {
        out_on_body = true;
        return best;
    }

    // Fallback: nearest ring curve, sampled at the same points it is drawn with, so the hitbox is exactly the visible curve.
    out_on_body = false;
    best_dist = F32_MAX;
    for (const Placement& p : placements)
    {
        if (!p.mResolved || p.mRingRadius <= 0.5f) continue;
        for (S32 seg = 0; seg < ORBIT_RING_SEGMENTS; ++seg)
        {
            const F32 phase_deg = 360.f * (F32)seg / (F32)ORBIT_RING_SEGMENTS;
            F32 px = 0.f, py = 0.f;
            projectOnRing(p.mRingCentreX, p.mRingCentreY, p.mRingRadius, p.mTiltRad, phase_deg, px, py);
            const F32 dist = sqrtf((fx - px) * (fx - px) + (fy - py) * (fy - py));
            if (dist <= ORBIT_RING_HIT_RADIUS && dist < best_dist)
            {
                best = p.mIndex;
                best_dist = dist;
            }
        }
    }
    return best;
}

S32 SSOrbitViewCtrl::handleHitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_antipodal) const
{
    // Only a unit whose centre itself travels a ring offers handles: the orbiting senior carries that ring, while a lone root pair's centre is the pinned unit anchor with no phase to edit. Nearest
    // within tolerance across BOTH handle flavours, matching the body pass's nearest-wins rule.
    const F32 fx = (F32)x;
    const F32 fy = (F32)y;
    S32 best = -1;
    F32 best_dist = F32_MAX;
    out_antipodal = false;
    for (const Placement& p : placements)
    {
        if (!p.mResolved) continue;
        const bool orbiting = (p.mRingRadius > 0.5f || p.mCounterRingRadius > 0.5f);

        // The orbiting PAIR's own centre (pair seniors only - a single outer sun's "unit centre" is the sun itself, which body clicks already cover).
        if (p.mPairPartner >= 0 && p.mIndex < p.mPairPartner && orbiting)
        {
            const F32 dist = sqrtf((fx - p.mPairCentreX) * (fx - p.mPairCentreX)
                                   + (fy - p.mPairCentreY) * (fy - p.mPairCentreY));
            if (dist <= ORBIT_PAIR_HANDLE_HIT_RADIUS && dist < best_dist)
            {
                best = p.mIndex;
                best_dist = dist;
                out_antipodal = false;
            }
        }

        // The inner pair's shifted centre - the antipodal grab point for the same senior phase, present in 3-4 sun systems only.
        if (p.mHasCounterHandle)
        {
            const F32 dist = sqrtf((fx - p.mCounterCentreX) * (fx - p.mCounterCentreX)
                                   + (fy - p.mCounterCentreY) * (fy - p.mCounterCentreY));
            if (dist <= ORBIT_PAIR_HANDLE_HIT_RADIUS && dist < best_dist)
            {
                best = p.mIndex;
                best_dist = dist;
                out_antipodal = true;
            }
        }
    }
    return best;
}

bool SSOrbitViewCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    SSAtmoEnvPlanetary* planetary = mPlanetary ? mPlanetary() : nullptr;
    if (!planetary) return LLUICtrl::handleMouseDown(x, y, mask);

    std::vector<Placement> placements;
    computeLayout(*planetary, placements);

    // Idle hover feedback goes stale the moment a drag owns the mouse - cleared here, re-established by the first idle hover after release.
    mHoverIndex = -1;
    mHoverOnBody = false;
    mHoverHandleIndex = -1;
    mHoverHandleAntipodal = false;

    // Hit priority: the pair-centre handle outranks bodies, which outrank bare rings. The handle must win over a body because a tight pair (or a zoomed-out view) can park a sun's disc on top of the
    // pair centre, and the visibly hovered handle would then be impossible to grab; the reverse shadowing cannot happen, because the handle only hits inside the same few pixels its hover circle is
    // showing in - nowhere else on the canvas does a body click have a handle to lose to. Every drag records a scrollbar-thumb style grab offset (see mDragOffsetDeg): phase-at-grab minus
    // cursor-angle-at-grab, added back on every hover, so a motionless click edits nothing and a real drag never snaps the grabbed thing to the cursor. The offset is exact - the inverse is pure
    // angle and the drag's reference frame cannot move until the first actual write - so no movement threshold backstop is needed.
    bool handle_antipodal = false;
    const S32 handle = handleHitTest(placements, x, y, handle_antipodal);
    bool on_body = false;
    const S32 hit = (handle >= 0) ? -1 : hitTest(placements, x, y, on_body);

    if (handle >= 0)
    {
        // The centre drag edits the orbiting SENIOR's phase (the pair-as-a-unit's orbit), so the senior is what gets selected - whichever of the two handle flavours was grabbed.
        if (mOnSelect) mOnSelect(handle);
        mSelectedIndex = handle;
        mDragMode = DRAG_CENTRE;
        mDragAntipodal = handle_antipodal;
        mDragIndex = handle;
        const Placement& p = placements[handle];
        // The outer handle inverts about the inner unit's shifted centre (the senior's anchor - its phase IS the inner->outer direction); the antipodal handle about the ring centre its counter-ring
        // is drawn around, where the cursor angle is stable (angles about the grabbed point itself would not be). Its half-turn opposition folds into the grab offset.
        const F32 raw = handle_antipodal
            ? inversePhaseDeg(p.mRingCentreX, p.mRingCentreY, p.mTiltRad, x, y)
            : inversePhaseDeg(p.mAnchorX, p.mAnchorY, p.mTiltRad, x, y);
        mDragOffsetDeg = planetary->mBodies[handle].mOrbitalPhaseDeg - raw;
        gFocusMgr.setMouseCapture(this);
    }
    else if (hit >= 0 && on_body)
    {
        if (mOnSelect) mOnSelect(hit);
        mSelectedIndex = hit;

        if (placements[hit].mPairPartner >= 0)
        {
            // A bound pair member drags as the PAIR's swing about its centre (the junior's phase - see handleHover). This includes the ROOT pair, whose members draw no ring at all: ringlessness
            // there only means the pair's centre is the pinned origin, not that the pair's orientation isn't editable. The offset is against the JUNIOR's phase; grabbing the senior (which sits
            // opposite the junior) just bakes a half-turn into it.
            const S32 junior = llmax(hit, placements[hit].mPairPartner);
            mDragMode = DRAG_PAIR;
            mDragIndex = hit;
            const F32 raw = inversePhaseDeg(placements[hit].mPairCentreX,
                                            placements[hit].mPairCentreY,
                                            placements[junior].mTiltRad, x, y);
            mDragOffsetDeg = planetary->mBodies[junior].mOrbitalPhaseDeg - raw;
            gFocusMgr.setMouseCapture(this);
        }
        else if (placements[hit].mRingRadius > 0.5f)
        {
            // Only a direct hit on a body that actually has a ring starts a phase drag - covers planets, moons and the outer single sun alike; a lone root has no ring to drag along at all.
            mDragMode = DRAG_RING;
            mDragIndex = hit;
            const F32 raw = inversePhaseDeg(placements[hit].mAnchorX,
                                            placements[hit].mAnchorY,
                                            placements[hit].mTiltRad, x, y);
            mDragOffsetDeg = planetary->mBodies[hit].mOrbitalPhaseDeg - raw;
            gFocusMgr.setMouseCapture(this);
        }
    }
    else if (hit >= 0)
    {
        // A ring click is selection only - even with the grab offset a ring drag from the bare curve would read as grabbing nothing.
        if (mOnSelect) mOnSelect(hit);
        mSelectedIndex = hit;
    }
    // Eat the click either way - a miss inside the canvas should not fall through to whatever sits behind it.
    return true;
}

bool SSOrbitViewCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    SSAtmoEnvPlanetary* planetary = mPlanetary ? mPlanetary() : nullptr;

    if (!hasMouseCapture() || mDragMode == DRAG_NONE || mDragIndex < 0)
    {
        // Idle hover: the pair-centre handle (drawn only while hovered) plus body/ring highlights, through the same priority chain mouse-down walks - what lights up is what a click would grab.
        mHoverHandleIndex = -1;
        mHoverHandleAntipodal = false;
        mHoverIndex = -1;
        mHoverOnBody = false;
        if (planetary)
        {
            std::vector<Placement> placements;
            computeLayout(*planetary, placements);
            bool antipodal = false;
            mHoverHandleIndex = handleHitTest(placements, x, y, antipodal);
            mHoverHandleAntipodal = antipodal;
            if (mHoverHandleIndex < 0)
            {
                bool on_body = false;
                mHoverIndex = hitTest(placements, x, y, on_body);
                mHoverOnBody = on_body;
            }
        }
        return LLUICtrl::handleHover(x, y, mask);
    }

    if (!planetary || mDragIndex >= (S32)planetary->mBodies.size())
    {
        // The system shrank mid-drag (poll-driven reload) - drop the drag rather than writing through a stale index.
        mDragIndex = -1;
        mDragMode = DRAG_NONE;
        gFocusMgr.setMouseCapture(nullptr);
        return true;
    }

    std::vector<Placement> placements;
    computeLayout(*planetary, placements);
    const Placement& p = placements[mDragIndex];
    if (!p.mResolved) return true;

    if (mDragMode == DRAG_PAIR)
    {
        const S32 partner = p.mPairPartner;
        if (partner < 0 || partner >= (S32)placements.size())
        {
            // The pairing dissolved mid-drag (poll-driven reload) - same rule as the stale-index drop above.
            mDragIndex = -1;
            mDragMode = DRAG_NONE;
            gFocusMgr.setMouseCapture(nullptr);
            return true;
        }
        // Cursor angle about the pair centre, in the JUNIOR's display tilt frame (placePairMembers projects the whole pair in it), plus the grab offset - the junior's phase is the pair's
        // orientation, per the resolver's convention, and the offset already carries the half-turn when the grabbed member was the senior on the opposite side.
        const S32 junior = llmax(mDragIndex, partner);
        const F32 raw = inversePhaseDeg(p.mPairCentreX, p.mPairCentreY,
                                        placements[junior].mTiltRad, x, y);
        planetary->mBodies[junior].mOrbitalPhaseDeg = ss_wrap_phase_deg(raw + mDragOffsetDeg);
    }
    else
    {
        // DRAG_RING and DRAG_CENTRE are the same edit: mDragIndex's own phase, cursor angle plus grab offset. For a centre drag mDragIndex is the orbiting SENIOR: the outer handle inverts about its
        // anchor (the inner unit's shifted centre - the point its phase is measured from), the antipodal handle about the counter-ring's centre, matching what mouse-down recorded the offset against.
        if (p.mRingRadius <= 0.5f && p.mCounterRingRadius <= 0.5f) return true;
        const F32 raw = (mDragMode == DRAG_CENTRE && mDragAntipodal)
            ? inversePhaseDeg(p.mRingCentreX, p.mRingCentreY, p.mTiltRad, x, y)
            : inversePhaseDeg(p.mAnchorX, p.mAnchorY, p.mTiltRad, x, y);
        planetary->mBodies[mDragIndex].mOrbitalPhaseDeg = ss_wrap_phase_deg(raw + mDragOffsetDeg);
    }

    if (mOnDrag) mOnDrag();
    return true;
}

bool SSOrbitViewCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        mDragIndex = -1;
        mDragMode = DRAG_NONE;
        mDragAntipodal = false;
        mDragOffsetDeg = 0.f;
        gFocusMgr.setMouseCapture(nullptr);
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

void SSOrbitViewCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
{
    // All of it is hover-only chrome; a cursor that left the canvas is not hovering anything.
    mHoverHandleIndex = -1;
    mHoverHandleAntipodal = false;
    mHoverIndex = -1;
    mHoverOnBody = false;
    LLUICtrl::onMouseLeave(x, y, mask);
}

void SSOrbitViewCtrl::zoomBy(S32 steps)
{
    mZoom = llclamp(mZoom * powf(ORBIT_ZOOM_STEP, (F32)steps), ORBIT_ZOOM_MIN, ORBIT_ZOOM_MAX);
}

bool SSOrbitViewCtrl::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    // Positive clicks are wheel-down; maps convention is wheel-up zooms in, hence the negation. Eaten even at the clamp so the scroll can't fall through and drag some list behind the canvas around.
    zoomBy(-clicks);
    return true;
}

//-----------------------------------------------------------------------------
// SSFloaterAtmoPlanetary
//-----------------------------------------------------------------------------

namespace
{
    const char* bodyKindName(SSAtmoEnvCelestialBody::EKind kind)
    {
        switch (kind)
        {
            case SSAtmoEnvCelestialBody::SUN:    return "Sun";
            case SSAtmoEnvCelestialBody::PLANET: return "Planet";
            case SSAtmoEnvCelestialBody::MOON:   return "Moon";
        }
        return "Body";
    }

    // The star-type presets: a diameter+mass pair (solar diameters/masses) and nothing else - colour/temperature are a rendering concern for the phase that draws suns as more than warm discs. The
    // combo's item values are indices into this table; -1 is the "(Custom)" entry the refresh selects when the body matches no preset.
    struct SSStarTypePreset
    {
        const char* mLabel;
        F32 mDiameterD; // solar diameters
        F32 mMassM;     // solar masses
    };
    const SSStarTypePreset STAR_TYPE_PRESETS[] = {
        { "M Dwarf",     0.3f,   0.3f },
        { "K Dwarf",     0.8f,   0.8f },
        { "G (Sol)",     1.f,    1.f  },
        { "F",           1.3f,   1.3f },
        { "A",           1.8f,   2.1f },
        { "B Giant",     5.f,   10.f  },
        { "O Giant",    10.f,   30.f  },
        { "White Dwarf", 0.013f, 0.6f },
        { "Red Giant", 100.f,    1.f  },
    };
    const S32 STAR_TYPE_PRESET_COUNT = (S32)(sizeof(STAR_TYPE_PRESETS) / sizeof(STAR_TYPE_PRESETS[0]));

    // Preset matching is on displayed units, so the tolerance can be relative and tight - a float's storage round-trip is far inside it, while any user edit of the last shown decimal is far outside.
    bool nearlyEqual(F32 a, F32 b)
    {
        return fabsf(a - b) <= llmax(fabsf(b) * 0.001f, 0.0001f);
    }

    // Which of a SUN's orbit fields the author owns, per the topology's pair conventions (normalizeSunTopology / the resolver's placePair): a pair JUNIOR's orbital radius IS the pair separation and
    // its phase the pair's orientation - both meaningful, both the same fields the canvas's pair drag edits; a sun WITH a parent (the outer single, or an outer pair's SENIOR) owns its unit's orbit
    // around the inner barycenter. Only a ROOT senior - a lone root sun, or a root pair's senior - has genuinely unused orbit fields, and only those stay disabled. (Disabling all sun orbit spinners
    // wholesale locked a pair junior's separation at the normalisation default.)
    bool ss_sun_orbit_editable(const SSAtmoEnvPlanetary& p, S32 index, bool& out_pair_junior)
    {
        out_pair_junior = false;
        if (index < 0 || index >= (S32)p.mBodies.size()) return false;
        const SSAtmoEnvCelestialBody& body = p.mBodies[index];
        if (body.mKind != SSAtmoEnvCelestialBody::SUN) return false;

        // Same symmetric-partner validation as sunPairMembers(); junior = the higher index, the resolver's pairJunior() convention.
        const S32 partner = body.mBoundPartnerIndex;
        const bool paired = partner >= 0 && partner < (S32)p.mBodies.size()
            && partner != index
            && p.mBodies[partner].mKind == SSAtmoEnvCelestialBody::SUN
            && p.mBodies[partner].mBoundPartnerIndex == index;
        out_pair_junior = paired && index > partner;

        return out_pair_junior || body.mParentIndex >= 0;
    }
}

SSFloaterAtmoPlanetary::SSFloaterAtmoPlanetary(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoPlanetary::postBuild()
{
    LLScrollListCtrl* body_list = getChild<LLScrollListCtrl>("body_list");
    body_list->setCommitOnSelectionChange(true);
    body_list->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSelectBody(); });

    getChild<LLButton>("add_sun_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddBody((S32)SSAtmoEnvCelestialBody::SUN); });
    getChild<LLButton>("add_planet_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddBody((S32)SSAtmoEnvCelestialBody::PLANET); });
    getChild<LLButton>("add_moon_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickAddBody((S32)SSAtmoEnvCelestialBody::MOON); });
    getChild<LLButton>("remove_body_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveBody(); });

    getChild<LLUICtrl>("body_name_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyName(); });
    const char* body_scalar_fields[] = { "body_diameter_spinner", "body_mass_spinner",
                                         "body_orbital_radius_spinner", "body_inclination_spinner",
                                         "body_phase_spinner", "body_axial_tilt_spinner",
                                         "body_latitude_spinner" };
    for (const char* name : body_scalar_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitBodyScalars(); });
    }

    // The star-type dropdown: presets by table index, plus the inert "(Custom)" entry the refresh lands on when nothing matches. Built once here - the item set never changes, only the selection
    // does.
    LLComboBox* star_type_combo = getChild<LLComboBox>("body_star_type_combo");
    for (S32 i = 0; i < STAR_TYPE_PRESET_COUNT; ++i)
    {
        star_type_combo->add(STAR_TYPE_PRESETS[i].mLabel, LLSD(i));
    }
    star_type_combo->add("(Custom)", LLSD(-1));
    star_type_combo->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyStarType(); });

    getChild<LLUICtrl>("body_home_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyHome(); });
    getChild<LLUICtrl>("body_emissive_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyShading(); });
    getChild<LLUICtrl>("body_phase_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyShading(); });

    getChild<LLUICtrl>("body_light_check")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyLight(); });
    const char* body_ring_fields[] = { "body_ring_check", "body_ring_inner_spinner",
                                       "body_ring_outer_spinner", "body_ring_texture" };
    for (const char* name : body_ring_fields)
    {
        getChild<LLUICtrl>(name)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { onCommitBodyRing(); });
    }
    getChild<LLUICtrl>("body_custom_texture")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitBodyTexture(); });

    SSOrbitViewCtrl* orbit = getChild<SSOrbitViewCtrl>("orbit_view");
    orbit->setPlanetaryAccessor([this]() { return planetary(); });
    orbit->setSelectCallback([this](S32 index) { onOrbitSelect(index); });
    orbit->setDragCallback([this]() { onOrbitDrag(); });

    // Maps-style corner zoom cluster - the same exponential step the canvas's own mousewheel takes, with reset-to-fit sitting between the two (the maps-UI idiom).
    getChild<LLButton>("orbit_zoom_in_button")->setClickedCallback(
        [orbit](LLUICtrl*, const LLSD&) { orbit->zoomBy(1); });
    getChild<LLButton>("orbit_zoom_reset_button")->setClickedCallback(
        [orbit](LLUICtrl*, const LLSD&) { orbit->resetZoom(); });
    getChild<LLButton>("orbit_zoom_out_button")->setClickedCallback(
        [orbit](LLUICtrl*, const LLSD&) { orbit->zoomBy(-1); });

    refreshAll();
    return true;
}

void SSFloaterAtmoPlanetary::onOpen(const LLSD& key)
{
    setTrack(key.asInteger());
}

void SSFloaterAtmoPlanetary::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;
        // Same capture guard as the main floater's poll: rebuilding the list and the star-type dropdown would fight an in-progress drag on the list's scrollbar, an open dropdown, or the canvas's own
        // phase drag. The title is safe to keep fresh regardless.
        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            refreshAll();
        }
        else
        {
            refreshTitle();
        }
    }

    LLFloater::draw();
}

void SSFloaterAtmoPlanetary::setTrack(S32 index)
{
    if (index != mTrackIndex)
    {
        // A typed-but-uncommitted property value belongs to the OLD track's body - flush it there before the retarget.
        flushFocusedPropertyField();
        // Each track is its own planetary system - a body selection carried across tracks would point at some unrelated body.
        mSelectedBodyIndex = 0;
    }
    mTrackIndex = index;
    refreshAll();
}

SSAtmoEnvPlanetary* SSFloaterAtmoPlanetary::planetary()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return nullptr;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mTrackIndex < 0 || mTrackIndex >= (S32)asset.mTracks.size()) return nullptr;
    return &asset.mTracks[mTrackIndex].mPlanetary;
}

SSAtmoEnvCelestialBody* SSFloaterAtmoPlanetary::selectedBody()
{
    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return nullptr;
    if (mSelectedBodyIndex < 0 || mSelectedBodyIndex >= (S32)p->mBodies.size()) return nullptr;
    return &p->mBodies[mSelectedBodyIndex];
}

void SSFloaterAtmoPlanetary::flushFocusedPropertyField()
{
    LLView* focused = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
    if (!focused || !focused->hasAncestor(this)) return;

    // The property controls whose focus must not survive a selection switch. Ancestry, not identity: a spinner's keyboard focus actually sits on its internal line editor, a combo's on its
    // button/list.
    const char* property_controls[] = {
        "body_name_editor",
        "body_orbital_radius_spinner", "body_inclination_spinner", "body_phase_spinner",
        "body_axial_tilt_spinner", "body_latitude_spinner",
        "body_diameter_spinner", "body_mass_spinner",
        "body_ring_inner_spinner", "body_ring_outer_spinner",
        "body_star_type_combo",
    };
    for (const char* name : property_controls)
    {
        LLUICtrl* ctrl = findChild<LLUICtrl>(name);
        if (!ctrl || (focused != ctrl && !focused->hasAncestor(ctrl))) continue;

        if (LLSpinCtrl* spinner = dynamic_cast<LLSpinCtrl*>(ctrl))
        {
            // LLSpinCtrl does NOT commit typed text on focus loss - its editor-lost-focus handler reverts the display instead - so dropping focus alone would silently discard the edit. Force the
            // commit while mSelectedBodyIndex still names the body the value was typed for.
            spinner->forceEditorCommit();
        }
        else if (dynamic_cast<LLLineEditor*>(ctrl))
        {
            // The name editor: its commit handler reads the live text.
            ctrl->onCommit();
        }
        // The star-type combo commits on every pick already - nothing pending to flush, it just needs to lose focus below.

        // Focus goes regardless of whether a commit fired: the refresh after the selection switch must repopulate this field for the NEW body, and refreshBodyFields()'s hasFocus guards - right for a
        // poll landing mid-typing, wrong across a switch - would skip it otherwise.
        gFocusMgr.setKeyboardFocus(nullptr);
        return;
    }
}

void SSFloaterAtmoPlanetary::refreshAll()
{
    SSAtmoEnvPlanetary* p = planetary();
    const bool valid = (p != nullptr);

    // Stale index / no asset: the "no system to edit" state rather than a crash or a panel editing thin air.
    getChild<LLUICtrl>("no_system_text")->setVisible(!valid);
    getChild<LLUICtrl>("designer_left_panel")->setVisible(valid);
    SSOrbitViewCtrl* orbit = getChild<SSOrbitViewCtrl>("orbit_view");
    orbit->setVisible(valid);
    // The corner zoom buttons float over the canvas, so they follow its visibility rather than the left panel's.
    getChild<LLUICtrl>("orbit_zoom_in_button")->setVisible(valid);
    getChild<LLUICtrl>("orbit_zoom_reset_button")->setVisible(valid);
    getChild<LLUICtrl>("orbit_zoom_out_button")->setVisible(valid);

    refreshTitle();
    if (!valid) return;

    // Clamped against the vector on every refresh - a removal (or a load) can shrink the list out from under the selection. -1 is the "no bodies at all" state.
    const S32 body_count = (S32)p->mBodies.size();
    if (mSelectedBodyIndex >= body_count) mSelectedBodyIndex = body_count - 1;
    if (mSelectedBodyIndex < 0 && body_count > 0) mSelectedBodyIndex = 0;

    // Add Sun stops at the canonical topology's cap; Add Moon needs a planet to parent to (addBody() would orphan one otherwise, and parenting is automatic now - there is no dropdown to fix it
    // with).
    S32 sun_count = 0;
    bool any_planet = false;
    for (const SSAtmoEnvCelestialBody& body : p->mBodies)
    {
        if (body.mKind == SSAtmoEnvCelestialBody::SUN) ++sun_count;
        else if (body.mKind == SSAtmoEnvCelestialBody::PLANET) any_planet = true;
    }
    getChild<LLUICtrl>("add_sun_button")->setEnabled(sun_count < SS_ATMOENV_MAX_SUNS);
    getChild<LLUICtrl>("add_moon_button")->setEnabled(any_planet);

    rebuildBodyList();
    refreshBodyFields();
    orbit->setSelectedIndex(mSelectedBodyIndex);
}

void SSFloaterAtmoPlanetary::refreshTitle()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    std::string title = "Planetary System";
    if (mgr->hasAsset() && mTrackIndex >= 0 && mTrackIndex < (S32)mgr->asset().mTracks.size())
    {
        title += " - " + mgr->asset().mTracks[mTrackIndex].mName;
        // Same asterisk the main floater's title carries; the modified flag is asset-wide, so both titles always agree.
        if (mgr->isModified()) title += " - Unsaved changes*";
    }
    else
    {
        title += " - no system to edit";
    }
    setTitle(title);
}

void SSFloaterAtmoPlanetary::rebuildBodyList()
{
    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;

    const S32 body_count = (S32)p->mBodies.size();

    // Same derived lineage the canvas uses - see effectiveParent(): a planet nests under the first sun by definition, never by a stored index that could go stale.
    std::vector<S32> eff_parent((size_t)body_count, -1);
    for (S32 i = 0; i < body_count; ++i)
    {
        eff_parent[i] = p->effectiveParent(i);
    }

    // Suns first, as a FLAT unindented block in structure order (the order auto-naming letters them: Sol, Sol B, Sol C, Sol D) - their internal topology is automatic and canonical, so nesting pair
    // juniors and outer suns under each other communicates nothing the user can act on; a flat group of stars reads better. The planet/moon tree follows depth-first, planets one level in under the
    // sun block, moons one more under their planet; the emitted flags double as the cycle guard, and anything a cycle strands gets appended at root where it is at least visible. Planet/moon siblings
    // list in orbital-radius order, not mBodies order - the same ordering the canvas's rings and autoNameBodies()'s ordinals use, so "Sol II" always sits under "Sol I" whatever order they were added
    // in. Stable, so equal radii keep structure order.
    auto sortedSiblings = [p, body_count, &eff_parent](S32 parent)
    {
        std::vector<S32> siblings;
        for (S32 i = 0; i < body_count; ++i)
        {
            if (eff_parent[i] == parent) siblings.push_back(i);
        }
        std::stable_sort(siblings.begin(), siblings.end(),
            [p](S32 a, S32 b)
            { return p->mBodies[a].mOrbitalRadius < p->mBodies[b].mOrbitalRadius; });
        return siblings;
    };

    std::vector<std::pair<S32, S32>> ordered; // body index, depth
    std::vector<bool> emitted((size_t)body_count, false);
    std::function<void(S32, S32)> emit = [&](S32 index, S32 depth)
    {
        emitted[index] = true;
        ordered.push_back(std::make_pair(index, depth));
        for (S32 child : sortedSiblings(index))
        {
            if (!emitted[child]) emit(child, depth + 1);
        }
    };
    // The flat sun block. Marked emitted without recursing so no sun ever nests under another; their non-sun children join the tree walk below at depth 1. (A separate index list - emit() appends to
    // `ordered`, so it cannot be iterated while emitting.)
    std::vector<S32> sun_indices;
    for (S32 i = 0; i < body_count; ++i)
    {
        if (p->mBodies[i].mKind == SSAtmoEnvCelestialBody::SUN)
        {
            emitted[i] = true;
            ordered.push_back(std::make_pair(i, 0));
            sun_indices.push_back(i);
        }
    }
    for (const S32 sun : sun_indices)
    {
        for (S32 child : sortedSiblings(sun))
        {
            if (!emitted[child]) emit(child, 1);
        }
    }
    // Remaining roots: a sunless system's planets (and any orphan moon), at the depth the sun block would have had.
    for (S32 root : sortedSiblings(-1))
    {
        if (!emitted[root]) emit(root, 0);
    }
    for (S32 i = 0; i < body_count; ++i)
    {
        if (!emitted[i])
        {
            emitted[i] = true;
            ordered.push_back(std::make_pair(i, 0));
        }
    }

    // Rebuild from the asset; selection is re-applied and scroll position carried across so the periodic poll doesn't visibly reset either.
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("body_list");
    const S32 scroll_pos = list->getScrollPos();
    list->deleteAllItems();
    for (const std::pair<S32, S32>& entry : ordered)
    {
        const SSAtmoEnvCelestialBody& body = p->mBodies[entry.first];

        std::string name(std::string((size_t)(entry.second * 2), ' ') + body.mName);
        // Only a moon can genuinely be orphaned (a hand-edited notecard naming a bad parent - removal now cascades moons away with their planet). A parentless planet is just a sunless system's
        // normal state, not an error.
        if (eff_parent[entry.first] == -1 && body.mKind == SSAtmoEnvCelestialBody::MOON)
        {
            name += " (orphan)";
        }

        LLSD row;
        row["value"] = entry.first;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = name;
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"] = bodyKindName(body.mKind);
        list->addElement(row);
    }
    list->setScrollPos(scroll_pos);
    if (mSelectedBodyIndex >= 0)
    {
        list->setSelectedByValue(LLSD(mSelectedBodyIndex), true);
    }
}

void SSFloaterAtmoPlanetary::refreshBodyFields()
{
    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;

    const S32 body_count = (S32)p->mBodies.size();
    const bool has_body = mSelectedBodyIndex >= 0 && mSelectedBodyIndex < body_count;
    getChild<LLUICtrl>("remove_body_button")->setEnabled(has_body);

    // Blanket enable/disable first; the flag-, kind- and ring-specific rules below then only ever tighten it.
    const char* body_controls[] = {
        "body_name_editor",
        "body_orbital_radius_spinner", "body_inclination_spinner", "body_phase_spinner",
        "body_axial_tilt_spinner", "body_latitude_spinner", "body_star_type_combo",
        "body_diameter_spinner", "body_mass_spinner", "body_custom_texture",
        "body_ring_check", "body_ring_inner_spinner", "body_ring_outer_spinner",
        "body_ring_texture", "body_home_check", "body_light_check",
        "body_emissive_check", "body_phase_check",
    };
    for (const char* name : body_controls)
    {
        getChild<LLUICtrl>(name)->setEnabled(has_body);
    }
    if (!has_body)
    {
        getChild<LLUICtrl>("body_star_type_label")->setVisible(false);
        getChild<LLUICtrl>("body_star_type_combo")->setVisible(false);
        return;
    }

    const SSAtmoEnvCelestialBody& body = p->mBodies[mSelectedBodyIndex];
    const bool is_sun  = (body.mKind == SSAtmoEnvCelestialBody::SUN);
    const bool is_moon = (body.mKind == SSAtmoEnvCelestialBody::MOON);

    // Left alone while focused, so a refresh landing mid-typing can't yank the caret or discard a partial edit.
    LLLineEditor* name_editor = getChild<LLLineEditor>("body_name_editor");
    if (!name_editor->hasFocus())
    {
        name_editor->setText(body.mName);
    }

    // A sun's PARENTING is normalizeSunTopology()'s, but its orbit FIELDS are mostly the author's - see ss_sun_orbit_editable(): a pair junior edits its separation/orientation, an orbiting sun (or
    // outer pair's senior) its unit's own orbit. Only a root senior's spinners grey out - those fields alone are genuinely unused, and greying out more locked a pair's separation at the
    // normalisation default.
    bool sun_pair_junior = false;
    const bool orbit_editable = !is_sun
        || ss_sun_orbit_editable(*p, mSelectedBodyIndex, sun_pair_junior);
    getChild<LLUICtrl>("body_orbital_radius_spinner")->setEnabled(orbit_editable);
    getChild<LLUICtrl>("body_inclination_spinner")->setEnabled(orbit_editable);
    getChild<LLUICtrl>("body_phase_spinner")->setEnabled(orbit_editable);

    // Display units per kind - storage stays metres, only the spinners (range, precision, label) speak AU/km/solar diameters. Range and precision are reconfigured before the value lands so the value
    // can never be clamped against the previous kind's bounds; all of it is skipped while the spinner is focused, same as the value alone used to be.
    const F32 radius_display = is_moon ? body.mOrbitalRadius / SS_METRES_PER_KM
                                       : body.mOrbitalRadius / SS_METRES_PER_AU;
    LLSpinCtrl* radius_spinner = getChild<LLSpinCtrl>("body_orbital_radius_spinner");
    if (!radius_spinner->hasFocus())
    {
        if (is_moon)
        {
            radius_spinner->setMinValue(1000.f);
            radius_spinner->setMaxValue(10000000.f);
            radius_spinner->setIncrement(1000.f);
            radius_spinner->setPrecision(0);
        }
        else
        {
            // A root sun's automatic radius is 0 - its disabled display must be allowed to actually show that, hence no floor for suns where planets get one.
            radius_spinner->setMinValue(is_sun ? 0.f : 0.05f);
            radius_spinner->setMaxValue(100.f);
            radius_spinner->setIncrement(0.05f);
            radius_spinner->setPrecision(2);
        }
        radius_spinner->setValue(radius_display);
    }
    // A pair junior's radius IS the pair separation (its phase the pair's orientation) - say so, rather than letting "orbital radius" imply some orbit around the partner. Label and tooltip both
    // re-assert per selection since the same widget serves every role.
    getChild<LLTextBox>("body_orbital_radius_label")->setText(
        sun_pair_junior ? std::string("Pair Separation (AU)")
        : is_moon ? std::string("Orbital Radius (km)")
                  : std::string("Orbital Radius (AU)"));
    radius_spinner->setToolTip(
        sun_pair_junior
        ? std::string("Separation of the bound sun pair - the distance between its two members, split about their shared barycenter by mass. Dragging either member on the map edits the pair's orientation, this sets how far apart they sit")
        : is_sun
        ? std::string("Distance of this sun (or its pair's centre) from the inner barycenter it orbits, in AU")
        : std::string("Distance from whatever this body orbits - AU for planets, km for moons; the tab's scale dials then compress it. The map leans toward relative distances rather than mapping them exactly"));

    const F32 diameter_display = is_sun ? body.mDiameterM / SS_METRES_PER_SOLAR_DIAMETER
                                        : body.mDiameterM / SS_METRES_PER_KM;
    LLSpinCtrl* diameter_spinner = getChild<LLSpinCtrl>("body_diameter_spinner");
    if (!diameter_spinner->hasFocus())
    {
        if (is_sun)
        {
            // Floor at a thousandth of a solar diameter (precision 3 so it can actually be typed and shown): a geocentric build's "home star" wants to be a speck next to its giant partner, and 0.01
            // D was already hit in anger.
            diameter_spinner->setMinValue(0.001f);
            diameter_spinner->setMaxValue(1000.f);
            diameter_spinner->setIncrement(0.01f);
            diameter_spinner->setPrecision(3);
        }
        else
        {
            // 20,000,000 km (2e10 m stored): room for a giant-star-sized "planet" - the way a geocentric system authors its apparent sun as a huge body at planetary distance. Must match the XML's
            // max_val.
            diameter_spinner->setMinValue(1.f);
            diameter_spinner->setMaxValue(20000000.f);
            diameter_spinner->setIncrement(100.f);
            diameter_spinner->setPrecision(0);
        }
        diameter_spinner->setValue(diameter_display);
    }
    getChild<LLTextBox>("body_diameter_label")->setText(
        is_sun ? std::string("Diameter (D)") : std::string("Diameter (km)"));

    const struct { const char* mName; F32 mValue; } spinners[] = {
        { "body_inclination_spinner",    body.mOrbitalInclinationDeg },
        { "body_phase_spinner",          body.mOrbitalPhaseDeg },
        { "body_axial_tilt_spinner",     body.mAxialTiltDeg },
        { "body_latitude_spinner",       body.mLatitudeDeg },
        { "body_mass_spinner",           body.mMassRelative },
        { "body_ring_inner_spinner",     body.mRingInnerRadius },
        { "body_ring_outer_spinner",     body.mRingOuterRadius },
    };
    for (const auto& s : spinners)
    {
        LLUICtrl* spinner = getChild<LLUICtrl>(s.mName);
        if (!spinner->hasFocus())
        {
            spinner->setValue(s.mValue);
        }
    }

    // Star type: suns only. Selection tracks the body - a preset whose diameter+mass both match (in display units, tight tolerance) shows by name, anything else shows "(Custom)". Left alone while
    // focused so the poll can't move an open dropdown's selection.
    getChild<LLUICtrl>("body_star_type_label")->setVisible(is_sun);
    LLComboBox* star_type_combo = getChild<LLComboBox>("body_star_type_combo");
    star_type_combo->setVisible(is_sun);
    if (is_sun && !star_type_combo->hasFocus())
    {
        S32 match = -1;
        for (S32 i = 0; i < STAR_TYPE_PRESET_COUNT; ++i)
        {
            if (nearlyEqual(diameter_display, STAR_TYPE_PRESETS[i].mDiameterD)
                && nearlyEqual(body.mMassRelative, STAR_TYPE_PRESETS[i].mMassM))
            {
                match = i;
                break;
            }
        }
        star_type_combo->setSelectedByValue(LLSD(match), true);
    }

    getChild<LLTextureCtrl>("body_custom_texture")->setValue(body.mCustomTexture);
    getChild<LLTextureCtrl>("body_ring_texture")->setValue(body.mRingTexture);

    // The mutual exclusion, spelled out as disabled checkboxes rather than commits that bounce: a light emitter can't become home, and canSetLightEmitter() covers both the home flag and the
    // 2-emitter cap (an already-set emitter can always be unset).
    LLUICtrl* home_check = getChild<LLUICtrl>("body_home_check");
    home_check->setValue(body.mIsHome);
    home_check->setEnabled(!body.mIsLightEmitter);

    LLUICtrl* light_check = getChild<LLUICtrl>("body_light_check");
    light_check->setValue(body.mIsLightEmitter);
    light_check->setEnabled(p->canSetLightEmitter(mSelectedBodyIndex));

    // Phase shading has nothing to say while the body is emissive - see SSAtmoEnvCelestialBody::mEmissive - so the control says so by going grey rather than by quietly doing nothing. Latitude is the
    // observer's position ON the home body; on anything else it describes nobody, so the control says so rather than accepting a number that will never be read.
    getChild<LLUICtrl>("body_latitude_spinner")->setEnabled(body.mIsHome);

    getChild<LLUICtrl>("body_emissive_check")->setValue(body.mEmissive);
    LLUICtrl* phase_check = getChild<LLUICtrl>("body_phase_check");
    phase_check->setValue(body.mPhaseShaded);
    phase_check->setEnabled(!body.mEmissive);

    // Ring sub-controls grey out while there is no ring - the values still show what enabling would start from.
    getChild<LLUICtrl>("body_ring_check")->setValue(body.mHasRing);
    getChild<LLUICtrl>("body_ring_inner_spinner")->setEnabled(body.mHasRing);
    getChild<LLUICtrl>("body_ring_outer_spinner")->setEnabled(body.mHasRing);
    getChild<LLUICtrl>("body_ring_texture")->setEnabled(body.mHasRing);
}

void SSFloaterAtmoPlanetary::onSelectBody()
{
    LLScrollListItem* item = getChild<LLScrollListCtrl>("body_list")->getFirstSelected();
    if (!item) return;

    const S32 index = item->getValue().asInteger();
    // refreshAll()'s own programmatic reselect commits back through here (commit-on-selection-change fires for those too); this early-out is what stops that echo recursing.
    if (index == mSelectedBodyIndex) return;

    // Flush AFTER the early-out (an echo has nothing pending) and BEFORE the switch, so a typed value lands on the body it was typed for.
    flushFocusedPropertyField();
    mSelectedBodyIndex = index;
    refreshAll();
}

void SSFloaterAtmoPlanetary::onOrbitSelect(S32 index)
{
    if (index == mSelectedBodyIndex) return;

    // Same flush-before-switch as the list path - a canvas click is just another way to change the selected body.
    flushFocusedPropertyField();
    mSelectedBodyIndex = index;
    refreshAll();
}

void SSFloaterAtmoPlanetary::onOrbitDrag()
{
    // The canvas has already written the dragged body's phase; keep the field panel's phase spinner and the unsaved-changes asterisk live while the drag runs. No list rebuild - nothing structural
    // moved.
    refreshBodyFields();
    refreshTitle();
}

void SSFloaterAtmoPlanetary::onClickAddBody(S32 kind)
{
    // Adding moves the selection to the new body - same flush-before- switch rule as a direct selection change.
    flushFocusedPropertyField();

    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;

    // A new moon belongs to the planet the user is looking at: the selected body when that is a planet, or the selected moon's own planet (adding a sibling), falling through to addBody()'s
    // first-planet default otherwise. Suns and planets have no choice to express - their parenting is fully automatic.
    S32 preferred_parent = -1;
    if (kind == (S32)SSAtmoEnvCelestialBody::MOON)
    {
        const SSAtmoEnvCelestialBody* selected = selectedBody();
        if (selected)
        {
            if (selected->mKind == SSAtmoEnvCelestialBody::PLANET)
            {
                preferred_parent = mSelectedBodyIndex;
            }
            else if (selected->mKind == SSAtmoEnvCelestialBody::MOON)
            {
                preferred_parent = selected->mParentIndex;
            }
        }
    }

    const S32 index = p->addBody((SSAtmoEnvCelestialBody::EKind)kind, preferred_parent);
    if (index < 0) return;

    // Same rule as the main floater's Add Track: you asked for it, you're now editing it.
    mSelectedBodyIndex = index;
    refreshAll();
}

void SSFloaterAtmoPlanetary::onClickRemoveBody()
{
    // The flush targets the body about to be removed - a doomed commit, but dropping the focus is still required so the refresh repopulates the fields for whichever body the selection falls to.
    flushFocusedPropertyField();

    SSAtmoEnvPlanetary* p = planetary();
    if (!p) return;
    if (!p->removeBody(mSelectedBodyIndex)) return;

    // Back to the top of the list; refreshAll() clamps this to -1 itself if the last body just went.
    mSelectedBodyIndex = 0;
    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyName()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    std::string name = getChild<LLLineEditor>("body_name_editor")->getText();
    LLStringUtil::trim(name);
    // A blank name would leave an unreadable list row; keep the old name instead (the refresh below restores it in the editor too).
    if (!name.empty())
    {
        body->mName = name;
        // Theirs now, permanently - auto-naming skips it from here on. Even retyping the exact auto name counts: the intent expressed was "this name", not "whatever the ordering says".
        body->mNameCustom = true;
        // A custom-named planet's moons derive from it ("Tatooine.1") - re-run so they follow immediately, not on the next reorder.
        p->autoNameBodies();
    }

    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyShading()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body) return;

    body->mEmissive = getChild<LLUICtrl>("body_emissive_check")->getValue().asBoolean();
    body->mPhaseShaded = getChild<LLUICtrl>("body_phase_check")->getValue().asBoolean();

    // Emissive greys the phase control out, so the panel has to be refreshed rather than just the value written.
    refreshBodyFields();
}

void SSFloaterAtmoPlanetary::onCommitBodyScalars()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    const bool is_sun  = (body->mKind == SSAtmoEnvCelestialBody::SUN);
    const bool is_moon = (body->mKind == SSAtmoEnvCelestialBody::MOON);

    // All re-read on any one's commit - the untouched ones just read back the values the last refresh put there. Display units (AU/km/solar diameters, matching what refreshBodyFields() configured
    // for this kind) convert back to the stored metres here and nowhere else.
    body->mDiameterM = (F32)getChild<LLUICtrl>("body_diameter_spinner")->getValue().asReal()
                     * (is_sun ? SS_METRES_PER_SOLAR_DIAMETER : SS_METRES_PER_KM);
    body->mMassRelative = (F32)getChild<LLUICtrl>("body_mass_spinner")->getValue().asReal();
    // Orbit fields read back exactly when the spinners are enabled - the same ss_sun_orbit_editable() predicate refreshBodyFields() uses, so spinner edits round-trip with the canvas's phase drags
    // (which write these same fields). Only a ROOT senior sun's fields - the disabled display of genuinely unused values - are never read back, so a normalisation between refreshes can't be undone
    // by an unrelated commit.
    bool sun_pair_junior = false;
    if (!is_sun || ss_sun_orbit_editable(*p, mSelectedBodyIndex, sun_pair_junior))
    {
        body->mOrbitalRadius = (F32)getChild<LLUICtrl>("body_orbital_radius_spinner")->getValue().asReal()
                             * (is_moon ? SS_METRES_PER_KM : SS_METRES_PER_AU);
        body->mOrbitalInclinationDeg = (F32)getChild<LLUICtrl>("body_inclination_spinner")->getValue().asReal();
        body->mOrbitalPhaseDeg       = (F32)getChild<LLUICtrl>("body_phase_spinner")->getValue().asReal();
    }
    body->mAxialTiltDeg = (F32)getChild<LLUICtrl>("body_axial_tilt_spinner")->getValue().asReal();
    body->mLatitudeDeg = llclamp(
        (F32)getChild<LLUICtrl>("body_latitude_spinner")->getValue().asReal(), -90.f, 90.f);

    // A radius edit can reorder siblings, and the auto ordinals encode that ordering.
    p->autoNameBodies();

    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyStarType()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body || body->mKind != SSAtmoEnvCelestialBody::SUN) return;

    const S32 preset = getChild<LLComboBox>("body_star_type_combo")->getSelectedValue().asInteger();
    // "(Custom)" (or anything stale) does nothing beyond the refresh, which snaps the selection back to whatever the body actually is.
    if (preset >= 0 && preset < STAR_TYPE_PRESET_COUNT)
    {
        body->mDiameterM = STAR_TYPE_PRESETS[preset].mDiameterD * SS_METRES_PER_SOLAR_DIAMETER;
        body->mMassRelative = STAR_TYPE_PRESETS[preset].mMassM;
    }

    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyHome()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    if (getChild<LLUICtrl>("body_home_check")->getValue().asBoolean())
    {
        // Un-homes every other body (and clears this one's light-emitter flag) in one place - see setHomeBody().
        p->setHomeBody(mSelectedBodyIndex);
    }
    else
    {
        // No automatic reassignment: no home body at all is a legal state (homeBodyIndex() == -1, the sky simply has no computed arc), and silently electing a different body would be the bigger
        // surprise.
        body->mIsHome = false;
    }

    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyLight()
{
    SSAtmoEnvPlanetary* p = planetary();
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!p || !body) return;

    const bool want = getChild<LLUICtrl>("body_light_check")->getValue().asBoolean();
    // Re-checked at commit time rather than trusting the checkbox's enabled state - the cap or the home flag can have moved since the last refresh set it.
    if (want && !p->canSetLightEmitter(mSelectedBodyIndex))
    {
        refreshAll(); // snap the checkbox back
        return;
    }
    body->mIsLightEmitter = want;

    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyRing()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body) return;

    body->mHasRing = getChild<LLUICtrl>("body_ring_check")->getValue().asBoolean();
    body->mRingInnerRadius = (F32)getChild<LLUICtrl>("body_ring_inner_spinner")->getValue().asReal();
    body->mRingOuterRadius = (F32)getChild<LLUICtrl>("body_ring_outer_spinner")->getValue().asReal();
    body->mRingTexture = getChild<LLTextureCtrl>("body_ring_texture")->getValue().asUUID();

    refreshAll();
}

void SSFloaterAtmoPlanetary::onCommitBodyTexture()
{
    SSAtmoEnvCelestialBody* body = selectedBody();
    if (!body) return;

    body->mCustomTexture = getChild<LLTextureCtrl>("body_custom_texture")->getValue().asUUID();

    refreshAll();
}

// </SS:Nexii>
