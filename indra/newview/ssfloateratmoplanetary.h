/**
 * @file ssfloateratmoplanetary.h
 * @brief Atmo Magic planetary system designer: a dedicated sub-floater for
 *        one track's celestial bodies, opened from the main floater's
 *        Planetary tab. A scene-graph list plus the selected body's fields
 *        on the left, and an angled top-down orbital map on the right -
 *        bodies sit on display rings around their parent, selectable by
 *        clicking them (or their ring), draggable along the ring to set
 *        orbital phase. Bound sun pairs drag as a unit's swing, and an
 *        outer pair's centre is itself a drag handle - see
 *        SSOrbitViewCtrl::handleMouseDown. See
 *        doc/atmo_magic_environment.md's Planetary tab.
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

#ifndef SS_FLOATERATMOPLANETARY_H
#define SS_FLOATERATMOPLANETARY_H

// <SS:Nexii> Atmo Magic planetary system designer

#include "llfloater.h"
#include "lluictrl.h"

#include <functional>
#include <vector>

struct SSAtmoEnvCelestialBody;
struct SSAtmoEnvPlanetary;

//-----------------------------------------------------------------------------
// The orbit canvas: a custom widget (registered as "ss_orbit_view", the same way llxyvector.cpp registers "xy_vector") that draws the system as an angled top-down diagram and handles click-select
// and drag-along-ring. It owns no data: a fresh accessor into SSAtmoEnvManager is consulted on every draw and every mouse event, so a track or body vanishing between frames degrades to drawing
// nothing rather than dereferencing into a shrunken vector - the same rule the floater's own guards follow.
//-----------------------------------------------------------------------------

class SSOrbitViewCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params();
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    // Clears every hover state (pair-centre handle, body/ring highlight) so none of it can linger after the cursor leaves the canvas.
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    // Mousewheel zooms; resetting is the floater's corner button.
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;

    // Steps the zoom exponentially (positive = in), clamped - shared by the wheel and the floater's corner +/- buttons. Zoom scales ring radii and body positions about the canvas centre only; fonts
    // and body discs are diagram glyphs and keep their size.
    void zoomBy(S32 steps);
    // Back to 1.0 - the layout is fit-to-canvas by construction there, so "reset" and "fit the whole system" are the same view. Wired to the corner cluster's middle button.
    void resetZoom() { mZoom = 1.f; }

    // Null when there is nothing to draw (no asset, stale track index) - checked fresh on every use, never cached across frames.
    void setPlanetaryAccessor(std::function<SSAtmoEnvPlanetary*()> accessor) { mPlanetary = accessor; }

    // Selection is owned by the floater (it also drives the list and the field panel); the canvas only reflects it and reports clicks.
    void setSelectedIndex(S32 index) { mSelectedIndex = index; }
    void setSelectCallback(std::function<void(S32)> cb) { mOnSelect = cb; }
    // Fired continuously while any of the three drags runs (ring, pair swing, pair centre) - the drag has already written the edited body's mOrbitalPhaseDeg by the time this fires, so the callback
    // only needs to refresh whatever displays it.
    void setDragCallback(std::function<void()> cb) { mOnDrag = cb; }

protected:
    friend class LLUICtrlFactory;
    SSOrbitViewCtrl(const Params& p);

private:
    // Where one body landed on the canvas this frame. Display-space only - ring radii here are normalised per level (see computeLayout), not the authored orbital radii, which span metres to AU and
    // would put every moon inside its planet's pixel at any one zoom.
    struct Placement
    {
        S32 mIndex = -1;
        bool mResolved = false;
        F32 mAnchorX = 0.f;    // centre of this body's display ring
        F32 mAnchorY = 0.f;
        F32 mRingRadius = 0.f; // 0 = a root, drawn at its anchor with no ring
        F32 mTiltRad = 0.f;    // ellipse rotation derived from inclination
        F32 mX = 0.f;          // body centre
        F32 mY = 0.f;
        F32 mDrawRadius = 4.f;
        // Bound sun pair bookkeeping, set on both members: the partner's index and the projected centre the pair swings about. A pair member's phase drag rotates about this centre, not its anchor
        // (which, for an orbiting unit's senior, is the inner unit's shifted centre - the reference its phase is measured from).
        S32 mPairPartner = -1;
        F32 mPairCentreX = 0.f;
        F32 mPairCentreY = 0.f;
        // Where this body's ring is drawn/hit. Equal to the anchor except for an orbiting sun unit's senior after the sun-group barycentric shift: its ring becomes the path its UNIT'S CENTRE travels
        // about the group barycenter, while the anchor stays on the inner unit's shifted centre for the drag inverse.
        F32 mRingCentreX = 0.f;
        F32 mRingCentreY = 0.f;
        // The mutual-orbit counterpart, carried by the orbiting senior: the inner unit's smaller antipodal path about the same centre (mRingRadius * m_outer / m_inner of it), plus the inner PAIR's
        // shifted centre as a second grab point editing the same phase +180. Zero/false everywhere else.
        F32 mCounterRingRadius = 0.f;
        F32 mCounterCentreX = 0.f;
        F32 mCounterCentreY = 0.f;
        bool mHasCounterHandle = false;
    };

    // Rebuilt from the asset on every draw and every mouse event - at this body count a layout pass is trivially cheap, and never caching it is what makes external edits (the floater's fields,
    // another track being loaded) show up without any invalidation plumbing.
    void computeLayout(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out) const;

    // The projection: x = cos(a)*R, y = sin(a)*R*TILT, then the whole
    // ellipse rotated by tilt_rad for that body's inclination.
    static void projectOnRing(F32 anchor_x, F32 anchor_y, F32 ring_radius, F32 tilt_rad,
                              F32 phase_deg, F32& out_x, F32& out_y);

    // Exact inverse of projectOnRing's angle: un-rotate the tilt, undo the vertical flattening, atan2 the remainder into 0..360. The cursor's distance from the anchor drops out entirely - only the
    // angle is editable, so every drag is radius-agnostic (and zoom-proof, since layout positions are already zoomed). Shared by all three drags: ring, pair swing and pair-centre - only the
    // anchor/tilt fed in differ.
    static F32 inversePhaseDeg(F32 anchor_x, F32 anchor_y, F32 tilt_rad, S32 x, S32 y);

    // True when index is one half of a valid bound sun pair (symmetric partner refs, both suns). Senior = lower index, junior = higher - the resolver's pairJunior() convention: the junior's orbital
    // fields hold the pair's separation and orientation, the senior's hold the pair-as-a-unit's own orbit around its anchor.
    static bool sunPairMembers(const SSAtmoEnvPlanetary& planetary, S32 index,
                               S32& out_senior, S32& out_junior);

    // Places both members of a bound pair about their shared centre, mirroring the resolver's placePair(): opposite sides, each at separation * other_mass / total, oriented by the junior's phase.
    static void placePairMembers(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out,
                                 S32 senior, S32 junior, F32 centre_x, F32 centre_y);

    // Nearest body within the body tolerance wins; otherwise the nearest ring within the (thinner) ring tolerance selects that ring's body. Returns -1 for a miss. out_on_body distinguishes the two
    // so a ring click selects without starting a phase drag.
    S32 hitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_on_body) const;

    // The pair-centre handles: an orbiting unit's own pair centre (the outer pair), and - via out_antipodal - the inner pair's shifted centre, which in a 3-4 sun system travels its own counter-ring
    // and edits the same phase +180. Returns the orbiting SENIOR's index (its phase is what a centre drag edits) or -1. A lone root pair's centre is the pinned unit anchor with no phase, so it never
    // hits.
    S32 handleHitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_antipodal) const;

    void drawRing(F32 centre_x, F32 centre_y, F32 radius, F32 tilt_rad, const LLColor4& color) const;

    std::function<SSAtmoEnvPlanetary*()> mPlanetary;
    std::function<void(S32)> mOnSelect;
    std::function<void()> mOnDrag;

    // What an in-progress drag edits: a body's own phase along its ring (planets, moons, the outer single sun), a bound pair's swing about its centre (either member grabbed - the junior's phase), or
    // an outer pair's centre along the senior's ring (the hover handle grabbed - the senior's phase).
    enum EDragMode { DRAG_NONE, DRAG_RING, DRAG_PAIR, DRAG_CENTRE };

    S32 mSelectedIndex = -1;
    S32 mDragIndex = -1;             // grabbed body (the orbiting SENIOR for a centre drag), -1 when idle
    EDragMode mDragMode = DRAG_NONE;
    bool mDragAntipodal = false;     // centre drag grabbed the inner (antipodal) handle
    // The scrollbar-thumb grab offset: the edited phase minus the cursor angle at mouse-down, added back to every subsequent cursor angle. A motionless click therefore commits a delta of exactly
    // zero, and a drag keeps the grab point under the cursor instead of snapping the body to it. Grabbing a pair SENIOR bakes its 180-degree opposition to the junior into this offset - no explicit
    // half-turn anywhere in the drag itself.
    F32 mDragOffsetDeg = 0.f;
    S32 mHoverHandleIndex = -1;      // orbiting senior whose centre handle the idle cursor is over
    bool mHoverHandleAntipodal = false;
    // Idle hover feedback for bodies and rings, filled by the same hit priorities mouse-down uses (handle > body > ring) so what lights up is exactly what a click would act on. -1 while nothing is
    // hovered, a drag runs, or the cursor has left the canvas.
    S32 mHoverIndex = -1;
    bool mHoverOnBody = false;
    F32 mZoom = 1.f;     // display zoom, clamped - see zoomBy()
};

//-----------------------------------------------------------------------------
// The designer floater. Opened with the track index as its LLSD key; the main floater retargets an open instance via setTrack() whenever its own track selection changes, so the two never silently
// edit different tracks.
//-----------------------------------------------------------------------------

class SSFloaterAtmoPlanetary : public LLFloater
{
public:
    SSFloaterAtmoPlanetary(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // Which track's planetary system to edit. Re-reads SSAtmoEnvManager fresh on every refresh rather than holding anything from mTracks, so a stale index (track removed, asset reloaded) shows the
    // "no system to edit" state instead of crashing.
    void setTrack(S32 index);

private:
    // The guards every handler leads with, as accessors: null when there is no asset or an index has gone stale - same pattern the main floater used while this lived on its Planetary tab.
    SSAtmoEnvPlanetary* planetary();
    SSAtmoEnvCelestialBody* selectedBody();

    // Everything: no-system state, title, list, fields, canvas selection.
    void refreshAll();

    // Selection is about to move (body switch, add/remove, track switch): if keyboard focus sits in one of the property fields, commit any typed value THERE FIRST - so it lands on the body it was
    // typed for - then drop the focus, so the refresh that follows repopulates every field instead of the hasFocus guards preserving stale text. Without this a focused spinner neither committed nor
    // updated across a body switch. No-op when focus is elsewhere (the body list keeps its focus for arrow-key browsing).
    void flushFocusedPropertyField();

    // Title carries the track being edited and the same unsaved-changes asterisk the main floater's title does - the manager's modified flag is asset-wide, so both floaters read the same answer.
    void refreshTitle();

    // Scene-graph list: children follow their parent, indented two spaces per depth, siblings in orbital-radius order (matching ring order and the auto-name ordinals, not mBodies order); a
    // parentless non-sun shows at root with an "(orphan)" suffix. Selection and scroll position survive the rebuild. The list's order is owned here - header sorting is disabled in the XML
    // (can_sort="false").
    void rebuildBodyList();

    // The selected body's field panel - the field set moved here from the main floater's Planetary tab, same widgets, same enable/validation rules (canSetLightEmitter, home/emitter exclusion, focus
    // guards).
    void refreshBodyFields();

    void onSelectBody();
    // S32 rather than SSAtmoEnvCelestialBody::EKind so this header needs only the forward declarations above, not the whole schema header.
    void onClickAddBody(S32 kind);
    void onClickRemoveBody();

    void onCommitBodyName();
    // One handler for the plain numeric fields (diameter, mass, radius, inclination, phase, tilt) - all re-read together on any one's commit, converting display units (AU/km/solar diameters, per
    // kind) back to the stored metres. The two per-body shading flags: whether the disc lights itself, and whether it is shaded as a sphere with a phase.
    void onCommitBodyShading();

    void onCommitBodyScalars();
    // The star-type preset dropdown: applies the preset's diameter+mass pair; the "(Custom)" entry deliberately does nothing.
    void onCommitBodyStarType();
    void onCommitBodyHome();
    void onCommitBodyLight();
    // Ring enable, both radii and the ring texture together - they are one feature, and the sub-controls grey out when the checkbox is off.
    void onCommitBodyRing();
    void onCommitBodyTexture();

    // Canvas callbacks: a click selecting a body, and a drag having just rewritten the dragged body's orbital phase.
    void onOrbitSelect(S32 index);
    void onOrbitDrag();

    S32 mTrackIndex = 0;

    // Which body the field panel edits; -1 once the track has no bodies. Clamped against the body vector on every refresh, same as the track index is against mTracks.
    S32 mSelectedBodyIndex = 0;

    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOPLANETARY_H
