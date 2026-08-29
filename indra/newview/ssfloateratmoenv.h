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

    void refreshTrackTab();
    void onCommitTrackName();
    void onCommitDayCycle();

    void onCommitWaterEnabled();

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
