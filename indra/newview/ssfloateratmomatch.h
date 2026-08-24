/**
 * @file ssfloateratmomatch.h
 * @brief Atmo Magic: "Match sky from photo" - pick an image, draw a sample
 *        line up it, watch the sky walk toward it.
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

#ifndef SS_FLOATERATMOMATCH_H
#define SS_FLOATERATMOMATCH_H

// <SS:Nexii> Atmo Magic sky matching

#include "llfloater.h"
#include "lluictrl.h"
#include "llviewertexture.h"
#include "ssatmomatch.h"

#include <functional>
#include <vector>

//-----------------------------------------------------------------------------
// The photo, with the sample line drawn on it.
//
// A custom widget for the same reason the orbit canvas is one: it owns no
// data, draws from an accessor, and turns a drag into two numbers. Here that
// is the column being sampled and how far up it the sky is taken from.
//-----------------------------------------------------------------------------
class SSPhotoViewCtrl : public LLUICtrl
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

    // The texture to draw, plus the aspect of the PHOTO it came from.
    //
    // Those differ: a texture has to have power-of-two sides, and a
    // photograph does not, so the raw image is squashed into the nearest
    // legal size on the way to the GPU. Drawing it back into a rect of the
    // original aspect undoes exactly that squash - which is why the aspect
    // has to be carried separately rather than read off the texture.
    void setImage(LLPointer<LLViewerTexture> image, F32 photo_aspect)
    {
        mImage = image;
        mPhotoAspect = (photo_aspect > 0.01f) ? photo_aspect : 1.f;
    }
    bool hasImage() const { return mImage.notNull(); }

    // The line, in 0..1 of the image: x across, y from the BOTTOM. mY0 is
    // the horizon end, mY1 the sky end - the drag decides which is which by
    // where it started.
    F32 lineX() const { return mLineX; }
    F32 lineY0() const { return mY0; }
    F32 lineY1() const { return mY1; }
    bool hasLine() const { return mHasLine; }

    void setOnLineChanged(std::function<void()> cb) { mOnChanged = cb; }

protected:
    friend class LLUICtrlFactory;
    SSPhotoViewCtrl(const Params& p);

private:
    // Where the image is actually drawn inside the widget, letterboxed to
    // keep its aspect. Everything else works in these coordinates.
    void imageRect(S32& out_left, S32& out_bottom, S32& out_w, S32& out_h) const;

    LLPointer<LLViewerTexture> mImage;
    F32 mPhotoAspect = 1.f;

    F32 mLineX = 0.5f;
    F32 mY0 = 0.15f;
    F32 mY1 = 0.75f;
    bool mHasLine = false;
    bool mDragging = false;

    std::function<void()> mOnChanged;
};

class SSFloaterAtmoMatch : public LLFloater
{
public:
    SSFloaterAtmoMatch(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    void setTrack(S32 index) { mTrackIndex = index; }

private:
    void onClickBrowse();
    void onClickMatch();
    void onClickStop();
    void onClickRevert();

    // Reads the photo along the sample line and turns it into the elevation
    // /colour pairs the fitter wants. Averages a few pixels across the line
    // so a single noisy pixel cannot steer the fit.
    bool buildSamples(std::vector<SSAtmoMatch::Sample>& out) const;

    // Hands the current sample line to the matcher as a preview, so the
    // in-world markers track it while nothing is running.
    void pushPreview();

    void refreshStatus();

    SSPhotoViewCtrl* mPhoto = nullptr;
    LLPointer<class LLImageRaw> mRaw;   // kept for sampling; the texture is for drawing
    std::string mFilename;

    S32 mTrackIndex = 0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOMATCH_H
