/**
 * @file ssfloateratmoenv.h
 * @brief Atmo Magic floater: create/load/save/revert, a vertical altitude
 *        rail for track selection, and per-track editing tabs (Track,
 *        Weather, Water, Clouds, Atmosphere & Lighting, Planetary - the
 *        last opening the SSFloaterAtmoPlanetary designer for its bodies).
 *        See doc/atmo_magic_environment.md.
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

class LLInventoryItem;

class SSFloaterAtmoEnv : public LLFloater
{
public:
    SSFloaterAtmoEnv(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void onVisibilityChange(bool new_visibility) override;
    void draw() override;

    // The rail's markers are POSITIONED IN C++ against the slider's own geometry, so a resize has to reposition them. Doing it only on the periodic poll was not enough: that poll deliberately skips
    // while any widget holds mouse capture, and dragging a resize handle IS mouse capture - so through the whole drag the Ground marker sat wherever its follows="bottom" put it while the thumbs
    // moved with the rail, and the two drifted apart.
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

    // Dropping a notecard anywhere on the floater loads it; dropping a full-perm EEP sky settings item (only once an asset is loaded) stamps that sky's look as keyframes at the current preview head
    // - see handleSettingsDrop.
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    // The DAD_SETTINGS drop path: full-perm gate, fetch, sky-only type check, then addKeyframesFromSky at mPreviewPhase into the selected track's atmosphere + cloud dome. The fetch is async, so the
    // apply half guards against the floater having closed and the asset having been replaced or reshaped in the meantime.
    void handleSettingsDrop(const LLInventoryItem* item);
    // hasAsset() false: only the create button is meaningful, per the no-implicit-default rule. True: the normal editing chrome.
    void refreshVisibility();

    void refreshStatus();

    void onClickCreateNew();
    void onClickLoad();
    void onClickSave();
    void onClickRevert();

    // Drops the environment entirely, falling back to EEP - distinct from Revert, which restores the last saved state of this one.
    void onClickUnload();
    void onClickAddTrack();
    void onClickRemoveTrack();
    void onCommitName();

    //-------------------------------------------------------------------
    // Track altitude rail - see the header comment on the multi_slider in floater_ss_atmo_env.xml. Ground is a fixed row below the slider, not a slider thumb (see SSAtmoEnvTrack::mFloorZ); tracks
    // 1..3 are thumbs named "track1".."track3", added/removed to match asset.mTracks.size() rather than a fixed set - v3 tracks are 1-4 total, not always four the way EEP's sky tracks are.
    //-------------------------------------------------------------------

    // Full rebuild: clears and re-adds every slider thumb from the asset, repositions every altitude label, refreshes the Ground button and Add/Remove's enabled state. Called whenever the track
    // *set* changes (add/remove/load/revert) or on the periodic poll - never from a thumb's own commit callback, which only needs to move one label, not rebuild the whole rail out from under an
    // active drag.
    void refreshTrackRail();

    // The positioning half of refreshTrackRail, without the rebuild: where Ground sits and where each track's labels sit, both derived from the rail's current geometry. Safe to call as often as the
    // layout changes, which is what reshape() does - rebuilding the slider set that often would hand an in-progress drag to a different thumb (see refreshTrackRail's own note).
    void repositionRailMarkers();

    // One thumb's live-drag commit: writes the dragged value straight into that track's mFloorZ, selects that track, and repositions just that one marker's labels - see refreshTrackRail()'s comment
    // for why this doesn't rebuild the slider set. This is also the *only* place the rail drives selection; a plain mouse-down cannot, because LLMultiSlider signals mouse-down before it resolves
    // which thumb was hit.
    void onCommitAltitudeSlider();

    // Drag finished. Reordering and rebuilding the rail has to wait until here rather than happening per-commit: rebuilding calls addSlider(), which sets LLMultiSlider's own mCurSlider to whatever
    // it added last, so doing it mid-drag hands the drag to a different thumb - the bug where grabbing any marker ended up dragging the topmost one.
    void onMouseUpAltitudeSlider();

    void onClickGroundRow();

    // Opens the shared Weather Influence editor on the selected track - wired to the identical button on each tab weather can reach.
    void onClickWeatherInfluence();

    // Positions track_alt_label_<slot> (1-based, matching both the slider name "track<slot>" and mTracks[slot] directly - there's no thumb reordering support yet, so a slot's position in the rail
    // and its index into mTracks are always the same number) against the slider's current thumb position - the same rect math updateAltLabel() in llpanelenvironment.cpp uses, minus its
    // overlap-avoidance pass (readjustAltLabels()) - deferred; two thumbs dragged very close can overlap their labels for now.
    void refreshAltitudeLabel(S32 slot);

    // Y coordinate, in track_panel's space, of where the rail puts a marker sitting at `value`. Reproduces LLMultiSlider::setSliderValue()'s own thumb-centre arithmetic rather than approximating it,
    // so a track marker and the fixed Ground marker land on exactly the same scale - Ground used to be placed by a hand-tuned offset and didn't quite agree with where a real 0m thumb would have
    // gone.
    S32 railCentreForValue(F32 value) const;

    // Moves a view so its vertical centre sits on `centre_y`, leaving its height and horizontal placement alone.
    static void centreViewOn(LLView* view, S32 centre_y);

    void selectTrack(S32 index);

    // Which track is selected for the tab_container on the right (Track, Weather, ...). Index 0 (Ground) is always selectable but never removable.
    S32 mSelectedTrackIndex = 0;

    //-------------------------------------------------------------------
    // Track tab - name and day cycle for mSelectedTrackIndex. Not keyframed (unlike the Weather cube and the Water plane) - these are structural, plain per-track values. Altitude is deliberately
    // absent: the rail is where a track's floor is set, and the band it covers is what the rail draws, so neither needs a numeric twin here.
    //-------------------------------------------------------------------

    void refreshTrackTab();
    void onCommitTrackName();
    void onCommitDayCycle();

    //-------------------------------------------------------------------
    // Water tab - mSelectedTrackIndex's water plane. Every field except the enable checkbox is keyframed, so most of it rides the same row machinery the Weather cube does; the non-scalar controls
    // (colour swatch, texture picker, two xy_vectors) each get their own typed row list rather than one heterogeneous list, since what "read the widget" and "write the widget" mean differs per type.
    //-------------------------------------------------------------------

    void onCommitWaterEnabled();

    // True when the selected track has no water plane, which makes every water parameter meaningless: the whole tab below the checkbox greys out and stops ghosting keyframes, exactly as an
    // Auto-owned row does. The values stay in the asset untouched - turning water back on restores the plane the author had, rather than a default one.
    bool waterRowsInactive() const;

    // Greys (or restores) every "water_" row's controls and keyframe cluster to match waterRowsInactive(). Runs from the same pass refreshAutoRows() does, and for the same reason: the generic row
    // refresh re-enables controls by keyframe count, so this verdict has to be applied after it.
    void refreshWaterRows();

    // Weather tab's structural toggles - like the water enable, plain bools rather than keyframe rows. When an Auto is on its manual rows grey out (sliders/spinners disabled) rather than hide: the
    // derived behaviour still exists, those rows just aren't the author of it.
    void onCommitGustAuto();
    void onCommitLightningAuto();
    void onCommitLightningFlags();
    void refreshLightningRows();
    // One pass over every Auto-owned row group (gust, lightning, clouds): enables/disables the manual controls - slider, spinner, AND the keyframe diamond, since inserting a keyframe into a field
    // Auto is ignoring only parks confusion for later - and, while Auto owns a group, writes the values Auto computed for the current preview instant into the greyed controls, so they read as a live
    // preview of what Auto decided rather than a stale parked override. Chevrons stay live: they navigate the preview head, which is never wrong to do. Runs at the end of refreshPreview() so the
    // computed values track the scrub; the checkbox values themselves refresh with the structural state in refreshTrackTab().
    void refreshAutoRows();
    void onCommitCloudAuto();

    // Whether `prefix` names a row an Auto flag currently owns. Auto-owned rows suppress their keyframe ghosts entirely (scrubber overlay and slider value marks both): the parked keyframes do
    // nothing while Auto drives the field, and drawing them beside a computed readout that says something different reads as a contradiction, not information. The precipitation override row is
    // deliberately NOT in this set even while it sits on "Auto" - its keyframes are what schedule when Auto vs a forced type applies across the cycle, so they are never inert.
    bool rowAutoOwned(const std::string& prefix) const;

    //-------------------------------------------------------------------
    // Planetary tab - mSelectedTrackIndex's two distance-scale dials plus the button that opens the System Designer sub-floater (SSFloaterAtmoPlanetary), which is where the celestial bodies
    // themselves are edited now. Nothing here is keyframed: a solar system is structural, like a water plane existing, not something to tween mid-cycle. See panel_ss_atmo_env_planetary.xml.
    //-------------------------------------------------------------------

    // The two per-track distance-scale dials' display refresh - the tab's only remaining per-track state now the body editing lives in the designer.
    void refreshPlanetaryScales();

    // Slider+spinner pairs sharing one handler the same way the day-cycle rows do.
    void onCommitPlanetaryScales();

    // Opens (or retargets) the designer for mSelectedTrackIndex.
    void onClickOpenPlanetaryDesigner();

    // mSelectedTrackIndex's Weather cube, one AE-style row per keyframable field: label, slider, keyframe diamond, prev/next chevrons. Still no curve/HOLD controls - see
    // doc/atmo_magic_environment.md - but the row mechanics (setValueAtHead's three-way rule, toggling a keyframe, jumping between them) are the real thing. mField captures `this` and reads
    // mSelectedTrackIndex fresh on every call, both so switching tracks needs no rebuild and so a raw pointer into SSAtmoEnvAsset::mTracks can never dangle across an add/remove track reallocating
    // the vector.
    struct FloatRow
    {
        std::string mPrefix; // widget name prefix - "moisture" -> "moisture_slider", ...
        std::function<SSAtmoEnvKeyframed<F32>&()> mField;

        // Digits the ghost-keyframe labels print. The row's own spinner carries its own decimal_digits from the XML; this only has to agree with it so two readouts of the same number match. Units
        // are deliberately not here: they belong in the parameter's label ("Wind Speed (m/s)") rather than glued onto every number, which is what stopped the numbers being right-aligned against a
        // column of their peers.
        bool mIntegerDisplay = false;

        // Stored value per displayed unit, for the fields EEP's own editor does not show raw - the same idea as KeyRow::mScale, and for the same reason: the schema keeps LLSettingsSky's units so
        // seeding from a sky and writing back to one are both the identity, while the controls read the way the EEP editor's do. Only density multiplier needs it today (llpaneleditsky.cpp's
        // SLIDER_SCALE_DENSITY_MULTIPLIER), and it needs it badly: a thousandth. Left at 1 the control is not merely differently labelled, it is unusable - the settings range that does anything is
        // about 1e-5 to 5e-4, so every sky worth having lives in the bottom thirtieth of one percent of the slider and one press of the spinner steps clean over the whole of it.
        F32 mScale = 1.f;
    };
    std::vector<FloatRow> mFloatRows;

    // The same idea for a keyframed field that isn't a plain scalar on a slider. Only the diamond/chevron half is shared with FloatRow (see the refreshKeyframeControls/toggleKeyframe/jumpKeyframe
    // templates below); reading and writing the control itself is per-type, so each type gets its own small list and its own commit.
    template <typename T>
    struct KeyRow
    {
        std::string mPrefix; // also the control's own widget name, not just a prefix
        std::function<SSAtmoEnvKeyframed<T>&()> mField;

        // Display scale, for the colours EEP stores outside 0..1. Several sky colours are HDR in the settings: blue horizon and blue density run to 2, sunlight and ambient to 3. A colour swatch is a
        // 0..255 control, so EEP's own editor divides by these on the way in and multiplies on the way out (SLIDER_SCALE_* in llpaneleditsky.cpp), and anything showing the stored value raw reports a
        // white sunlight as <765, 765, 765>. Storage stays in EEP's own space - the applier hands these straight to the setters - so this is a display convention and nothing more.
        F32 mScale = 1.f;
    };
    std::vector<KeyRow<LLColor3>>  mColorRows;
    std::vector<KeyRow<LLVector2>> mVectorRows;
    std::vector<KeyRow<LLUUID>>    mTextureRows;
    // A dropdown over a keyframed string (the precipitation override) - holds across a keyframe rather than blending, since the value type's lerp does; see ss_atmoenv_lerp<std::string>.
    std::vector<KeyRow<std::string>> mStringRows;

    void refreshPreview();
    void onCommitPreviewTime();

    //-------------------------------------------------------------------
    // Ghost keyframes: hovering a parameter row draws that parameter's own keyframes onto the shared preview scrubber, with the value each one holds. The scrubber is one timeline shared by every
    // row, so without this there is nothing anywhere that says *where* a given parameter's keyframes actually sit - you can only step between them blind with the chevrons. Drawn rather than built
    // from widgets because it is transient, follows the mouse, and belongs to whichever row is under it at the time.
    //-------------------------------------------------------------------

    struct GhostKeyframe
    {
        // Where the marker is drawn. For an eased or linear keyframe this is simply where the keyframe is - the value only exists at that instant, everything either side of it is a blend. A HOLD
        // keyframe is different: its value is in force across a whole span until the next keyframe displaces it, so the marker is drawn at the centre of that span instead. Putting it on the boundary
        // would suggest the value belongs to the instant rather than to the stretch it actually governs.
        F64 mDrawPhase = 0.0;

        // The stretch this keyframe's value is in force over. Equal to mDrawPhase for a non-HOLD keyframe, which governs no stretch. The spans of a HOLD parameter's keyframes partition the whole
        // cycle exactly: the first runs from the start of the cycle, the last runs to the end of it, and each one in between runs to the next - so there are no unaccounted gaps and no overlaps.
        F64 mSpanStart = 0.0;
        F64 mSpanEnd = 0.0;
        bool mHold = false;

        std::string mLabel; // the value, already formatted for display
    };

    // True when the mouse is within the given row's horizontal band. Uses the row's keyframe button for the band because every row type has one, whatever its actual control is.
    bool rowHovered(const std::string& prefix) const;

    // Keyframes of whichever row is hovered, or false if none is. Walks every row list, so a colour or wave-vector row ghosts exactly like a slider row does. out_labels comes back false for the
    // whole-tab overview (hovering the scrubber itself rather than one row): that gathers every animatable parameter on the visible tab at once, where a value label per ghost would be both
    // unreadable and redundant - each parameter's own slider already shows where it currently sits.
    bool collectHoveredKeyframes(std::vector<GhostKeyframe>& out, bool& out_labels) const;

    // True while the pointer is over the preview scrubber. Distinguished from a row hover because it means "show me everything", not "show me this one parameter".
    bool scrubberHovered() const;

    // Turns one parameter's keyframes into drawable ghosts. The span logic is identical whatever the keyframed type is, so it lives here once and each row list supplies only its own value formatter.
    // A member rather than a free function because GhostKeyframe is private.
    template <typename T, typename FormatFn>
    static void buildGhosts(const std::vector<SSAtmoEnvKeyframe<T>>& keyframes,
                            FormatFn format, std::vector<GhostKeyframe>& out);

    // Rect, left travel origin and travel width of the preview scrubber, in the space the post-children overlays draw in. False when the scrubber is missing, hidden, or too narrow to place anything
    // on.
    bool scrubberGeometry(LLRect& out_rect, S32& out_left_edge, S32& out_travel) const;

    // Where the selected track's sun and moon cross the horizon, drawn on the scrubber as up/down triangles: full-size warm amber for the sun, small moonlit blue-white for the moon. Unlike the
    // ghosts these are always on - they annotate the timeline itself, the way tick marks do, rather than answering a hover.
    void drawRiseSetMarkers();

    void drawKeyframeGhosts();

    // The other half of the same idea, on the other axis. The scrubber ghosts say *when* a parameter has keyframes; these say *what values* those keyframes hold, by marking each one at its own
    // position along that parameter's own slider. Between them a row can be read without scrubbing: where its keyframes are, and what it does at each. Only scalar rows get these - a colour or an
    // asset id has no position along a scale to be marked at.
    void drawSliderValueGhosts();

    // Shared bodies every row's widget callbacks funnel into.
    void refreshFloatRow(const FloatRow& row, F64 phase);
    void commitFloatRow(const FloatRow& row);
    // The slider and the spinner edit the same value from opposite ends, so each commits on its own rather than reading back through the other.
    void commitFloatRowSpinner(const FloatRow& row);
    void toggleFloatRowKeyframe(const FloatRow& row);
    void jumpFloatRowPrev(const FloatRow& row);
    void jumpFloatRowNext(const FloatRow& row);

    // The diamond/chevron half, shared by every row type. Defined in the .cpp - only that translation unit ever instantiates them.
    template <typename T>
    void refreshKeyframeControls(const std::string& prefix, const SSAtmoEnvKeyframed<T>& field, F64 phase);
    template <typename T>
    void toggleKeyframe(SSAtmoEnvKeyframed<T>& field);
    template <typename T>
    void jumpKeyframe(const SSAtmoEnvKeyframed<T>& field, bool next);

    // Wires one keyframe cluster's three buttons. The bodies are identical bar the field they close over, so every row type shares this rather than repeating the same lambda triplet four times.
    template <typename T>
    void bindKeyframeButtons(const std::string& prefix, std::function<SSAtmoEnvKeyframed<T>&()> field);

    // Per-type control <-> field plumbing for the non-scalar water rows.
    void refreshColorRow(const KeyRow<LLColor3>& row, F64 phase);
    void commitColorRow(const KeyRow<LLColor3>& row);
    void refreshVectorRow(const KeyRow<LLVector2>& row, F64 phase);
    // The graphic and the two spinners edit the same vector from opposite ends, so they commit separately rather than one reading back through the other.
    void commitVectorRow(const KeyRow<LLVector2>& row);
    void commitVectorSpinners(const KeyRow<LLVector2>& row);
    void refreshTextureRow(const KeyRow<LLUUID>& row, F64 phase);
    void commitTextureRow(const KeyRow<LLUUID>& row);
    void refreshStringRow(const KeyRow<std::string>& row, F64 phase);
    void commitStringRow(const KeyRow<std::string>& row);

    // Position in the selected track's day cycle as a fraction in [0, 1) - the timeline head every keyframe operation above reads and writes against, in the same unit keyframes themselves are stored
    // in. Not seconds: a head measured in seconds means the same head lands at a different point in the cycle whenever day length changes, which is the bug phase-based keyframes exist to eliminate.
    F64 mPreviewPhase = 0.0;

    // Whether the head is running, and when it was last advanced. Real elapsed time rather than a per-frame increment, so the lap takes the same minute however the frame rate is doing.
    bool mPreviewPlaying = false;
    F64 mPreviewPlayLast = 0.0;
    void onClickPreviewPlay();
    void advancePreviewPlayback();

    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOENV_H
