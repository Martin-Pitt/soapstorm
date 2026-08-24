/**
 * @file ssfloateratmomatch.cpp
 * @brief Atmo Magic: "Match sky from photo". See the header.
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

#include "ssfloateratmomatch.h"

#include "ssatmoenvmanager.h"

#include "llbutton.h"
#include "llfilepicker.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llrender.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewertexturelist.h"

// <SS:Nexii> Atmo Magic sky matching

static LLDefaultChildRegistry::Register<SSPhotoViewCtrl> r_ss_photo_view("ss_photo_view");

//-----------------------------------------------------------------------------
// The photo widget
//-----------------------------------------------------------------------------

SSPhotoViewCtrl::Params::Params()
{
}

SSPhotoViewCtrl::SSPhotoViewCtrl(const Params& p) :
    LLUICtrl(p)
{
}

void SSPhotoViewCtrl::imageRect(S32& out_left, S32& out_bottom, S32& out_w, S32& out_h) const
{
    const LLRect& r = getLocalRect();
    out_left = 0;
    out_bottom = 0;
    out_w = r.getWidth();
    out_h = r.getHeight();

    if (mImage.isNull()) return;

    // Letterboxed rather than stretched: a sample line's position only
    // means anything against the picture's own proportions - and those are
    // the PHOTO's, not the power-of-two texture's. See setImage.
    const F32 want = mPhotoAspect;
    const F32 have = (F32)r.getWidth() / (F32)r.getHeight();

    if (have > want)
    {
        out_w = (S32)((F32)r.getHeight() * want);
        out_left = (r.getWidth() - out_w) / 2;
    }
    else
    {
        out_h = (S32)((F32)r.getWidth() / want);
        out_bottom = (r.getHeight() - out_h) / 2;
    }
}

void SSPhotoViewCtrl::draw()
{
    const LLRect& r = getLocalRect();
    gl_rect_2d(r, LLColor4(0.08f, 0.08f, 0.10f, 1.f));

    S32 ix, iy, iw, ih;
    imageRect(ix, iy, iw, ih);

    if (mImage.notNull())
    {
        gl_draw_scaled_image(ix, iy, iw, ih, mImage);
    }

    if (mHasLine && iw > 0 && ih > 0)
    {
        const S32 x = ix + (S32)(mLineX * (F32)iw);
        const S32 y0 = iy + (S32)(mY0 * (F32)ih);
        const S32 y1 = iy + (S32)(mY1 * (F32)ih);

        // The line itself, plus a tick at each end - the lower end is the
        // horizon, the upper the elevation the spinner names, and which is
        // which matters enough to mark.
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4f(1.f, 0.85f, 0.3f, 0.9f);
        gGL.begin(LLRender::LINES);
        gGL.vertex2i(x, y0);
        gGL.vertex2i(x, y1);
        gGL.vertex2i(x - 8, y0);
        gGL.vertex2i(x + 8, y0);
        gGL.vertex2i(x - 5, y1);
        gGL.vertex2i(x + 5, y1);
        gGL.end();
        gGL.flush();
    }

    LLUICtrl::draw();
}

bool SSPhotoViewCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mImage.isNull()) return LLUICtrl::handleMouseDown(x, y, mask);

    S32 ix, iy, iw, ih;
    imageRect(ix, iy, iw, ih);
    if (iw <= 0 || ih <= 0) return true;

    mLineX = llclamp((F32)(x - ix) / (F32)iw, 0.f, 1.f);
    mY0 = llclamp((F32)(y - iy) / (F32)ih, 0.f, 1.f);
    mY1 = mY0;
    mHasLine = true;
    mDragging = true;

    gFocusMgr.setMouseCapture(this);
    return true;
}

bool SSPhotoViewCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mDragging) return LLUICtrl::handleHover(x, y, mask);

    S32 ix, iy, iw, ih;
    imageRect(ix, iy, iw, ih);
    if (ih > 0)
    {
        mY1 = llclamp((F32)(y - iy) / (F32)ih, 0.f, 1.f);
    }
    return true;
}

bool SSPhotoViewCtrl::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!mDragging) return LLUICtrl::handleMouseUp(x, y, mask);

    mDragging = false;
    gFocusMgr.setMouseCapture(nullptr);

    // A click without a drag is not a line. Keeping the old one is kinder
    // than clearing it, since a stray click should not throw away a
    // carefully placed sample.
    if (fabsf(mY1 - mY0) < 0.02f)
    {
        mY1 = llmin(1.f, mY0 + 0.4f);
    }

    // Lower end is always the horizon end, whichever way the drag went.
    if (mY1 < mY0)
    {
        const F32 swap = mY0;
        mY0 = mY1;
        mY1 = swap;
    }

    if (mOnChanged) mOnChanged();
    return true;
}

//-----------------------------------------------------------------------------
// The floater
//-----------------------------------------------------------------------------

SSFloaterAtmoMatch::SSFloaterAtmoMatch(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoMatch::postBuild()
{
    mPhoto = getChild<SSPhotoViewCtrl>("photo_view");
    mPhoto->setOnLineChanged([this]() { pushPreview(); refreshStatus(); });

    getChild<LLUICtrl>("top_elevation_spinner")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { pushPreview(); });
    getChild<LLUICtrl>("browse_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickBrowse(); });
    getChild<LLUICtrl>("match_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickMatch(); });
    getChild<LLUICtrl>("stop_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickStop(); });
    getChild<LLUICtrl>("revert_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRevert(); });

    refreshStatus();
    return true;
}

void SSFloaterAtmoMatch::onOpen(const LLSD& key)
{
    mTrackIndex = key.asInteger();
    refreshStatus();
}

void SSFloaterAtmoMatch::pushPreview()
{
    // Keeps the in-world markers showing whatever the line currently says,
    // so moving it is immediately visible against the sky.
    std::vector<SSAtmoMatch::Sample> samples;
    if (buildSamples(samples))
    {
        SSAtmoMatch::getInstance()->setPreview(samples);
    }
    else
    {
        SSAtmoMatch::getInstance()->clearPreview();
    }
}

void SSFloaterAtmoMatch::onClose(bool app_quitting)
{
    // The markers belong to this floater being open, so they go with it.
    SSAtmoMatch::getInstance()->clearPreview();

    // Closing ACCEPTS, per the design: cancel is a button, and a fit you
    // watched settle and then closed the window on is one you wanted.
    if (SSAtmoMatch::getInstance()->isRunning())
    {
        SSAtmoMatch::getInstance()->accept();
    }
}

void SSFloaterAtmoMatch::draw()
{
    refreshStatus();
    LLFloater::draw();
}

void SSFloaterAtmoMatch::onClickBrowse()
{
    LLFilePicker& picker = LLFilePicker::instance();
    if (!picker.getOpenFile(LLFilePicker::FFLOAD_IMAGE)) return;

    mFilename = picker.getFirstFile();

    // Same decode path the local-texture preview uses: type from the
    // extension, load, decode to raw.
    const std::string ext = gDirUtilp->getExtension(mFilename);
    LLPointer<LLImageFormatted> formatted;
    if (ext == "png")
    {
        formatted = new LLImagePNG;
    }
    else if (ext == "jpg" || ext == "jpeg")
    {
        formatted = new LLImageJPEG;
    }

    if (formatted.isNull() || !formatted->load(mFilename))
    {
        mFilename.clear();
        refreshStatus();
        return;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw;
    if (!formatted->decode(raw, 0.f))
    {
        mFilename.clear();
        refreshStatus();
        return;
    }

    // The raw image is kept EXACTLY as decoded, because that is what the
    // sample line is read from - every sample is a pixel address in the
    // original photo.
    mRaw = raw;

    const F32 aspect = (raw->getHeight() > 0)
        ? (F32)raw->getWidth() / (F32)raw->getHeight() : 1.f;

    // The GPU copy is a different thing: textures must have power-of-two
    // sides, and a photograph never does, so this is a squashed copy at a
    // sane size. Squashing rather than padding keeps the whole picture
    // visible, and drawing it into a correctly-proportioned rect undoes the
    // squash - which is what the aspect above is for.
    //
    // Handing the decoded image straight to the GPU is what produced
    // "Trying to create a texture with incorrect dimensions!".
    LLPointer<LLImageRaw> gpu_copy = new LLImageRaw(
        raw->getWidth(), raw->getHeight(), raw->getComponents());
    gpu_copy->copy(raw);
    gpu_copy->expandToPowerOfTwo(1024, true);

    mPhoto->setImage(LLViewerTextureManager::getLocalTexture(gpu_copy.get(), false), aspect);
    pushPreview();
    refreshStatus();
}

bool SSFloaterAtmoMatch::buildSamples(std::vector<SSAtmoMatch::Sample>& out) const
{
    out.clear();
    if (mRaw.isNull() || !mPhoto || !mPhoto->hasLine()) return false;

    const S32 w = mRaw->getWidth();
    const S32 h = mRaw->getHeight();
    const S32 comps = mRaw->getComponents();
    if (w < 2 || h < 2 || comps < 3) return false;

    const F32 top_elev = (F32)getChild<LLUICtrl>("top_elevation_spinner")->getValue().asReal();

    static const S32 SAMPLE_COUNT = 12;
    static const S32 ACROSS = 3;        // pixels averaged either side of the line

    const U8* data = mRaw->getData();
    if (!data) return false;

    for (S32 i = 0; i < SAMPLE_COUNT; ++i)
    {
        const F32 t = (F32)i / (F32)(SAMPLE_COUNT - 1);

        // Up the line from the horizon end.
        //
        // No flip: an LLImageRaw's first scanline is the BOTTOM of the
        // picture, "per SecondLife conventions" as llpngwrapper.cpp puts
        // it, and the widget's line is measured from the bottom too. They
        // already agree. Converting between them as though the rows ran
        // top-down is what had every sample taken from the mirror image of
        // where the line was drawn - so a line up a sunset sampled the
        // ground.
        const F32 fy = mPhoto->lineY0() + t * (mPhoto->lineY1() - mPhoto->lineY0());
        const S32 y = llclamp((S32)(fy * (F32)(h - 1)), 0, h - 1);
        const S32 x = llclamp((S32)(mPhoto->lineX() * (F32)(w - 1)), 0, w - 1);

        F32 r = 0.f, g = 0.f, b = 0.f;
        S32 taken = 0;
        for (S32 dx = -ACROSS; dx <= ACROSS; ++dx)
        {
            const S32 sx = llclamp(x + dx, 0, w - 1);
            const U8* px = data + ((size_t)y * w + sx) * comps;
            r += (F32)px[0];
            g += (F32)px[1];
            b += (F32)px[2];
            ++taken;
        }

        SSAtmoMatch::Sample sample;
        sample.mElevationDeg = t * top_elev;
        sample.mColor.setVec(r / (taken * 255.f), g / (taken * 255.f), b / (taken * 255.f));
        out.push_back(sample);
    }

    return true;
}

void SSFloaterAtmoMatch::onClickMatch()
{
    std::vector<SSAtmoMatch::Sample> samples;
    if (!buildSamples(samples)) return;

    SSAtmoMatch::getInstance()->start(mTrackIndex, samples);
    refreshStatus();
}

void SSFloaterAtmoMatch::onClickStop()
{
    SSAtmoMatch::getInstance()->accept();
    refreshStatus();
}

void SSFloaterAtmoMatch::onClickRevert()
{
    SSAtmoMatch::getInstance()->cancel();
    refreshStatus();
}

void SSFloaterAtmoMatch::refreshStatus()
{
    SSAtmoMatch* match = SSAtmoMatch::getInstance();
    const bool running = match->isRunning();
    const bool ready = mRaw.notNull() && mPhoto && mPhoto->hasLine();

    getChild<LLUICtrl>("match_button")->setEnabled(ready && !running);
    getChild<LLUICtrl>("stop_button")->setEnabled(running);
    getChild<LLUICtrl>("revert_button")->setEnabled(!running && match->lastError() < 1.0e8f);

    std::string status;
    if (mRaw.isNull())
    {
        status = "Pick a photo, then drag a line up its sky from the horizon.";
    }
    else if (!ready)
    {
        status = "Drag a line up the sky, starting at the horizon.";
    }
    else if (running)
    {
        if (!match->waitReason().empty())
        {
            status = match->waitReason();
        }
        else
        {
            status = llformat("Matching... %d%%   difference %.4f",
                              (S32)(match->progress() * 100.f), match->lastError());
        }
    }
    else
    {
        // The markers are on screen exactly now, while nothing is running,
        // so this is where saying what they are belongs.
        status = "Markers in the world show the photo's colours at each height,"
                 " along the way the camera is facing - turn to put them over the"
                 " sky you mean, then press Match.";
        if (match->lastError() < 1.0e8f)
        {
            status += llformat("   Last difference %.4f", match->lastError());
        }
    }

    getChild<LLTextBox>("match_status")->setText(status);
}

// </SS:Nexii>
