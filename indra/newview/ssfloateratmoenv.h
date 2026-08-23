/**
 * @file ssfloateratmoenv.h
 * @brief Atmo Magic floater: create/load/save/revert, a vertical altitude
 *        rail for track selection, and per-track editing tabs (Track,
 *        Weather so far - Atmosphere & Lighting / Clouds / Planetary still
 *        to come). See doc/atmo_magic_environment.md.
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

#ifndef SS_FLOATERATMOENV_H
#define SS_FLOATERATMOENV_H

// <SS:Nexii> Atmo Magic floater

#include "llfloater.h"
#include "ssatmoenvkeyframe.h"

#include <functional>
#include <vector>

class SSFloaterAtmoEnv : public LLFloater
{
public:
    SSFloaterAtmoEnv(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void onVisibilityChange(bool new_visibility) override;
    void draw() override;

    // Dropping a notecard anywhere on the floater loads it
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    // hasAsset() false: only the create button is meaningful, per the
    // no-implicit-default rule. True: the normal editing chrome.
    void refreshVisibility();

    void refreshStatus();

    void onClickCreateNew();
    void onClickLoad();
    void onClickSave();
    void onClickRevert();
    void onClickAddTrack();
    void onClickRemoveTrack();
    void onCommitName();

    //-------------------------------------------------------------------
    // Track altitude rail - see the header comment on the multi_slider in
    // floater_ss_atmo_env.xml. Ground is a fixed row below the slider, not
    // a slider thumb (see SSAtmoEnvTrack::mFloorZ); tracks 1..3 are thumbs
    // named "track1".."track3", added/removed to match asset.mTracks.size()
    // rather than a fixed set - v3 tracks are 1-4 total, not always four
    // the way EEP's sky tracks are.
    //-------------------------------------------------------------------

    // Full rebuild: clears and re-adds every slider thumb from the asset,
    // repositions every altitude label, refreshes the Ground button and
    // Add/Remove's enabled state. Called whenever the track *set* changes
    // (add/remove/load/revert) or on the periodic poll - never from a
    // thumb's own commit callback, which only needs to move one label, not
    // rebuild the whole rail out from under an active drag.
    void refreshTrackRail();

    // One thumb's live-drag commit: writes the dragged value straight into
    // that track's mFloorZ, selects that track, and repositions just that
    // one marker's labels - see refreshTrackRail()'s comment for why this
    // doesn't rebuild the slider set. This is also the *only* place the
    // rail drives selection; a plain mouse-down cannot, because
    // LLMultiSlider signals mouse-down before it resolves which thumb was
    // hit.
    void onCommitAltitudeSlider();

    // Drag finished. Reordering and rebuilding the rail has to wait until
    // here rather than happening per-commit: rebuilding calls addSlider(),
    // which sets LLMultiSlider's own mCurSlider to whatever it added last,
    // so doing it mid-drag hands the drag to a different thumb - the bug
    // where grabbing any marker ended up dragging the topmost one.
    void onMouseUpAltitudeSlider();

    void onClickGroundRow();

    // Positions track_alt_label_<slot> (1-based, matching both the slider
    // name "track<slot>" and mTracks[slot] directly - there's no thumb
    // reordering support yet, so a slot's position in the rail and its
    // index into mTracks are always the same number) against the slider's
    // current thumb position - the same rect math updateAltLabel() in
    // llpanelenvironment.cpp uses, minus its overlap-avoidance pass
    // (readjustAltLabels()) - deferred; two thumbs dragged very close can
    // overlap their labels for now.
    void refreshAltitudeLabel(S32 slot);

    // Y coordinate, in track_panel's space, of where the rail puts a marker
    // sitting at `value`. Reproduces LLMultiSlider::setSliderValue()'s own
    // thumb-centre arithmetic rather than approximating it, so a track
    // marker and the fixed Ground marker land on exactly the same scale -
    // Ground used to be placed by a hand-tuned offset and didn't quite
    // agree with where a real 0m thumb would have gone.
    S32 railCentreForValue(F32 value) const;

    // Moves a view so its vertical centre sits on `centre_y`, leaving its
    // height and horizontal placement alone.
    static void centreViewOn(LLView* view, S32 centre_y);

    void selectTrack(S32 index);

    // Which track is selected for the tab_container on the right (Track,
    // Weather, ...). Index 0 (Ground) is always selectable but never
    // removable.
    S32 mSelectedTrackIndex = 0;

    //-------------------------------------------------------------------
    // Track tab - name and day cycle for mSelectedTrackIndex. Not keyframed
    // (unlike the Weather cube and the Water plane) - these are structural,
    // plain per-track values. Altitude is deliberately absent: the rail is
    // where a track's floor is set, and the band it covers is what the rail
    // draws, so neither needs a numeric twin here.
    //-------------------------------------------------------------------

    void refreshTrackTab();
    void onCommitTrackName();
    void onCommitDayCycle();

    //-------------------------------------------------------------------
    // Water tab - mSelectedTrackIndex's water plane. Every field except
    // the enable checkbox is keyframed, so most of it rides the same row
    // machinery the Weather cube does; the non-scalar controls (colour
    // swatch, texture picker, two xy_vectors) each get their own typed row
    // list rather than one heterogeneous list, since what "read the widget"
    // and "write the widget" mean differs per type.
    //-------------------------------------------------------------------

    void onCommitWaterEnabled();

    // mSelectedTrackIndex's Weather cube, one AE-style row per keyframable
    // field: label, slider, keyframe diamond, prev/next chevrons. Still no
    // curve/HOLD controls - see doc/atmo_magic_environment.md - but the row
    // mechanics (setValueAtHead's three-way rule, toggling a keyframe,
    // jumping between them) are the real thing. mField captures `this` and
    // reads mSelectedTrackIndex fresh on every call, both so switching
    // tracks needs no rebuild and so a raw pointer into
    // SSAtmoEnvAsset::mTracks can never dangle across an add/remove track
    // reallocating the vector.
    struct FloatRow
    {
        std::string mPrefix; // widget name prefix - "moisture" -> "moisture_slider", ...
        std::function<SSAtmoEnvKeyframed<F32>&()> mField;

        // Digits the ghost-keyframe labels print. The row's own spinner
        // carries its own decimal_digits from the XML; this only has to
        // agree with it so two readouts of the same number match.
        //
        // Units are deliberately not here: they belong in the parameter's
        // label ("Wind Speed (m/s)") rather than glued onto every number,
        // which is what stopped the numbers being right-aligned against
        // a column of their peers.
        bool mIntegerDisplay = false;
    };
    std::vector<FloatRow> mFloatRows;

    // The same idea for a keyframed field that isn't a plain scalar on a
    // slider. Only the diamond/chevron half is shared with FloatRow (see
    // the refreshKeyframeControls/toggleKeyframe/jumpKeyframe templates
    // below); reading and writing the control itself is per-type, so each
    // type gets its own small list and its own commit.
    template <typename T>
    struct KeyRow
    {
        std::string mPrefix; // also the control's own widget name, not just a prefix
        std::function<SSAtmoEnvKeyframed<T>&()> mField;
    };
    std::vector<KeyRow<LLColor3>>  mColorRows;
    std::vector<KeyRow<LLVector2>> mVectorRows;
    std::vector<KeyRow<LLUUID>>    mTextureRows;

    void refreshKeyframeProof();
    void onCommitPreviewTime();

    //-------------------------------------------------------------------
    // Ghost keyframes: hovering a parameter row draws that parameter's own
    // keyframes onto the shared preview scrubber, with the value each one
    // holds. The scrubber is one timeline shared by every row, so without
    // this there is nothing anywhere that says *where* a given parameter's
    // keyframes actually sit - you can only step between them blind with
    // the chevrons. Drawn rather than built from widgets because it is
    // transient, follows the mouse, and belongs to whichever row is under
    // it at the time.
    //-------------------------------------------------------------------

    struct GhostKeyframe
    {
        // Where the marker is drawn. For an eased or linear keyframe this
        // is simply where the keyframe is - the value only exists at that
        // instant, everything either side of it is a blend. A HOLD
        // keyframe is different: its value is in force across a whole span
        // until the next keyframe displaces it, so the marker is drawn at
        // the centre of that span instead. Putting it on the boundary
        // would suggest the value belongs to the instant rather than to
        // the stretch it actually governs.
        F64 mDrawPhase = 0.0;

        // The stretch this keyframe's value is in force over. Equal to
        // mDrawPhase for a non-HOLD keyframe, which governs no stretch.
        // The spans of a HOLD parameter's keyframes partition the whole
        // cycle exactly: the first runs from the start of the cycle, the
        // last runs to the end of it, and each one in between runs to the
        // next - so there are no unaccounted gaps and no overlaps.
        F64 mSpanStart = 0.0;
        F64 mSpanEnd = 0.0;
        bool mHold = false;

        std::string mLabel; // the value, already formatted for display
    };

    // True when the mouse is within the given row's horizontal band. Uses
    // the row's keyframe button for the band because every row type has
    // one, whatever its actual control is.
    bool rowHovered(const std::string& prefix) const;

    // Keyframes of whichever row is hovered, or false if none is. Walks
    // every row list, so a colour or wave-vector row ghosts exactly like a
    // slider row does.
    // out_labels comes back false for the whole-tab overview (hovering the
    // scrubber itself rather than one row): that gathers every animatable
    // parameter on the visible tab at once, where a value label per ghost
    // would be both unreadable and redundant - each parameter's own slider
    // already shows where it currently sits.
    bool collectHoveredKeyframes(std::vector<GhostKeyframe>& out, bool& out_labels) const;

    // True while the pointer is over the preview scrubber. Distinguished
    // from a row hover because it means "show me everything", not "show me
    // this one parameter".
    bool scrubberHovered() const;

    // Turns one parameter's keyframes into drawable ghosts. The span logic
    // is identical whatever the keyframed type is, so it lives here once
    // and each row list supplies only its own value formatter. A member
    // rather than a free function because GhostKeyframe is private.
    template <typename T, typename FormatFn>
    static void buildGhosts(const std::vector<SSAtmoEnvKeyframe<T>>& keyframes,
                            FormatFn format, std::vector<GhostKeyframe>& out);

    void drawKeyframeGhosts();

    // The other half of the same idea, on the other axis. The scrubber
    // ghosts say *when* a parameter has keyframes; these say *what values*
    // those keyframes hold, by marking each one at its own position along
    // that parameter's own slider. Between them a row can be read without
    // scrubbing: where its keyframes are, and what it does at each.
    //
    // Only scalar rows get these - a colour or an asset id has no position
    // along a scale to be marked at.
    void drawSliderValueGhosts();

    // Shared bodies every row's widget callbacks funnel into.
    void refreshFloatRow(const FloatRow& row, F64 phase);
    void commitFloatRow(const FloatRow& row);
    // The slider and the spinner edit the same value from opposite ends,
    // so each commits on its own rather than reading back through the other.
    void commitFloatRowSpinner(const FloatRow& row);
    void toggleFloatRowKeyframe(const FloatRow& row);
    void jumpFloatRowPrev(const FloatRow& row);
    void jumpFloatRowNext(const FloatRow& row);

    // The diamond/chevron half, shared by every row type. Defined in the
    // .cpp - only that translation unit ever instantiates them.
    template <typename T>
    void refreshKeyframeControls(const std::string& prefix, const SSAtmoEnvKeyframed<T>& field, F64 phase);
    template <typename T>
    void toggleKeyframe(SSAtmoEnvKeyframed<T>& field);
    template <typename T>
    void jumpKeyframe(const SSAtmoEnvKeyframed<T>& field, bool next);

    // Wires one keyframe cluster's three buttons. The bodies are identical
    // bar the field they close over, so every row type shares this rather
    // than repeating the same lambda triplet four times.
    template <typename T>
    void bindKeyframeButtons(const std::string& prefix, std::function<SSAtmoEnvKeyframed<T>&()> field);

    // Per-type control <-> field plumbing for the non-scalar water rows.
    void refreshColorRow(const KeyRow<LLColor3>& row, F64 phase);
    void commitColorRow(const KeyRow<LLColor3>& row);
    void refreshVectorRow(const KeyRow<LLVector2>& row, F64 phase);
    // The graphic and the two spinners edit the same vector from
    // opposite ends, so they commit separately rather than one
    // reading back through the other.
    void commitVectorRow(const KeyRow<LLVector2>& row);
    void commitVectorSpinners(const KeyRow<LLVector2>& row);
    void refreshTextureRow(const KeyRow<LLUUID>& row, F64 phase);
    void commitTextureRow(const KeyRow<LLUUID>& row);

    // Position in the selected track's day cycle as a fraction in [0, 1) -
    // the timeline head every keyframe operation above reads and writes
    // against, in the same unit keyframes themselves are stored in. Not
    // seconds: a head measured in seconds means the same head lands at a
    // different point in the cycle whenever day length changes, which is
    // the bug phase-based keyframes exist to eliminate.
    F64 mPreviewPhase = 0.0;

    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOENV_H
