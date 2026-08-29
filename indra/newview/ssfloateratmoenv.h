/**
 * @file ssfloateratmoenv.h
 * @brief Atmo Magic: environment editor floater.
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

#include "llfloater.h"
#include "ssatmoenvasset.h" // <SS:Nexii> SS_ATMOENV_REGION_CEILING, for the rail's track-mode range
#include "ssatmoenvkeyframe.h"

#include <functional>
#include <string>
#include <utility>
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

    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void handleSettingsDrop(const LLInventoryItem* item);
    void refreshVisibility();

    void refreshStatus();

    void onClickCreateNew();
    void onClickLoad();
    void onClickSave();
    void onClickRevert();

    void onClickUnload();
    void onClickAddTrack();
    void onClickRemoveTrack();
    void onCommitName();

    void refreshTrackRail();

    void repositionRailMarkers();

    void onCommitAltitudeSlider();

    void onMouseUpAltitudeSlider();

    void onClickGroundRow();

    void onClickWeatherInfluence();

    void refreshAltitudeLabel(S32 slot);

    S32 railCentreForValue(F32 value) const;

    static void centreViewOn(LLView* view, S32 centre_y);

    void selectTrack(S32 index);

    S32 mSelectedTrackIndex = 0;

    // <SS:Nexii> The altitude rail runs in two modes rather than there being two rails. Track mode
    // is the region scale with a marker per track, the way it has always been. Selecting any other
    // tab switches to layer mode: the scale fits the selected track's own contents and the markers
    // become the things stacked inside it - water plane, main deck, optional under deck - with
    // Space and the Dome pinned above as fixed anchors excluded from the fit.
    //
    // One widget with two modes because the track markers already carry two meanings (altitude and
    // tab selection) and hanging a third kind of object off them would overload the same control
    // again. A fixed scale cannot serve both a coastal build inside 100m and a sky archipelago
    // spanning 10km, and a piecewise one is worse - the same drag would mean 64m at one end and
    // 800m at the other. See doc/atmo_magic_env_ui.md.
    enum class ERailMode { TRACK, LAYER };

    // Marker slots in layer mode, in the order the rail lists them.
    static const S32 LAYER_NONE  = -1;
    static const S32 LAYER_WATER = 0;
    static const S32 LAYER_MAIN  = 1;
    static const S32 LAYER_UNDER = 2;
    static const S32 LAYER_COUNT = 3;

    void refreshRailMode();
    void refreshLayerRail();
    void railRangeForTrack(F32& out_min, F32& out_max) const;

    // The higher of the track's floor and its water plane: what precipitation lands on.
    F32 weatherReferenceSurface() const;
    // LAYER_MAIN or LAYER_UNDER, honouring the track's authored override; LAYER_NONE if neither
    // deck sits above the reference surface.
    S32 weatherDeliveringDeck() const;
    F32 layerAltitude(S32 layer) const;
    bool layerPresent(S32 layer) const;

    void selectLayer(S32 layer);
    void onClickLayerMarker(S32 layer);
    void onClickAddDeck();
    void onClickRemoveDeck();
    void onCommitWeatherSource();
    void refreshWeatherSource();

    // <SS:Nexii> The precipitation combo lists two tiers: the shipped derivation vocabulary, read
    // once from the XUI so the panel stays the single place it is written down, and whatever types
    // this environment carries of its own. Rebuilt whenever the environment's set changes.
    void refreshPrecipitationTypes();
    void onClickNewPrecipType();
    void onClickEditPrecipTypes();

    std::vector<std::pair<std::string, std::string>> mBuiltinPrecipItems;

    void drawWeatherBracket();

    ERailMode mRailMode = ERailMode::TRACK;
    S32 mSelectedLayer = LAYER_NONE;

    // The rail's live value range, and where it is heading. Interpolated in draw() so the mode
    // switch reads as diving into the selected track rather than as the widget swapping contents.
    F32 mRailMin = 0.f;
    F32 mRailMax = SS_ATMOENV_REGION_CEILING;
    F32 mRailMinFrom = 0.f;
    F32 mRailMaxFrom = SS_ATMOENV_REGION_CEILING;
    F32 mRailMinTo = 0.f;
    F32 mRailMaxTo = SS_ATMOENV_REGION_CEILING;
    bool mRailZooming = false;
    F64 mRailZoomStart = 0.0;

    void refreshTrackTab();
    void onCommitTrackName();

    // <SS:Nexii> Seeds the selected track from a world archetype - see ssAtmoEnvTemplates().
    void onClickApplyTemplate();
    void onCommitDayCycle();

    void onCommitWaterEnabled();

    void onCommitUnderEnabled();
    void onCommitUnderAuto();

    bool waterRowsInactive() const;

    void refreshWaterRows();

    void onCommitGustAuto();
    void onCommitLightningAuto();
    void onCommitLightningFlags();
    void refreshLightningRows();
    void refreshAutoRows();
    void onCommitCloudAuto();
    void onCommitDomeAuto();
    void onCommitHorizonClip();

    bool rowAutoOwned(const std::string& prefix) const;

    void refreshPlanetaryScales();

    void onCommitPlanetaryScales();

    void onClickOpenPlanetaryDesigner();

    struct FloatRow
    {
        std::string mPrefix;
        std::function<SSAtmoEnvKeyframed<F32>&()> mField;

        bool mIntegerDisplay = false;

        F32 mScale = 1.f;
    };
    std::vector<FloatRow> mFloatRows;

    template <typename T>
    struct KeyRow
    {
        std::string mPrefix;
        std::function<SSAtmoEnvKeyframed<T>&()> mField;

        F32 mScale = 1.f;
    };
    std::vector<KeyRow<LLColor3>>  mColorRows;
    std::vector<KeyRow<LLVector2>> mVectorRows;
    std::vector<KeyRow<LLUUID>>    mTextureRows;
    std::vector<KeyRow<std::string>> mStringRows;

    void refreshPreview();
    void onCommitPreviewTime();

    struct GhostKeyframe
    {
        F64 mDrawPhase = 0.0;

        F64 mSpanStart = 0.0;
        F64 mSpanEnd = 0.0;
        bool mHold = false;

        std::string mLabel;
    };

    bool rowHovered(const std::string& prefix) const;

    bool collectHoveredKeyframes(std::vector<GhostKeyframe>& out, bool& out_labels) const;

    bool scrubberHovered() const;

    template <typename T, typename FormatFn>
    static void buildGhosts(const std::vector<SSAtmoEnvKeyframe<T>>& keyframes,
                            FormatFn format, std::vector<GhostKeyframe>& out);

    bool scrubberGeometry(LLRect& out_rect, S32& out_left_edge, S32& out_travel) const;

    void drawRiseSetMarkers();

    void drawKeyframeGhosts();

    void drawSliderValueGhosts();

    void refreshFloatRow(const FloatRow& row, F64 phase);
    void commitFloatRow(const FloatRow& row);
    void commitFloatRowSpinner(const FloatRow& row);
    void toggleFloatRowKeyframe(const FloatRow& row);
    void jumpFloatRowPrev(const FloatRow& row);
    void jumpFloatRowNext(const FloatRow& row);

    template <typename T>
    void refreshKeyframeControls(const std::string& prefix, const SSAtmoEnvKeyframed<T>& field, F64 phase);
    template <typename T>
    void toggleKeyframe(SSAtmoEnvKeyframed<T>& field);
    template <typename T>
    void jumpKeyframe(const SSAtmoEnvKeyframed<T>& field, bool next);

    template <typename T>
    void bindKeyframeButtons(const std::string& prefix, std::function<SSAtmoEnvKeyframed<T>&()> field);

    void refreshColorRow(const KeyRow<LLColor3>& row, F64 phase);
    void commitColorRow(const KeyRow<LLColor3>& row);
    void refreshVectorRow(const KeyRow<LLVector2>& row, F64 phase);
    void commitVectorRow(const KeyRow<LLVector2>& row);
    void commitVectorSpinners(const KeyRow<LLVector2>& row);
    void refreshTextureRow(const KeyRow<LLUUID>& row, F64 phase);
    void commitTextureRow(const KeyRow<LLUUID>& row);
    void refreshStringRow(const KeyRow<std::string>& row, F64 phase);
    void commitStringRow(const KeyRow<std::string>& row);

    F64 mPreviewPhase = 0.0;

    bool mPreviewPlaying = false;
    F64 mPreviewPlayLast = 0.0;
    void onClickPreviewPlay();
    void advancePreviewPlayback();

    F64 mLastPoll = 0.0;
};

#endif
