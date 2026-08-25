/**
 * @file ssfloatersoundlist.cpp
 * @brief Atmo Magic sound lists. See the header.
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

#include "ssfloatersoundlist.h"

#include "ssassetlist.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llbutton.h"
#include "llfloaterreg.h"
#include "lllocalcliprect.h"
#include "llinventory.h"
#include "lltooldraganddrop.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "lltooltip.h"
#include "llui.h"
#include "llrender.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llwindow.h"

// <SS:Nexii> Atmo Magic sound lists

namespace
{
    // Row metrics for the editor's list.
    const S32 ROW_H = 22;
    const S32 ROW_PAD = 2;
    const S32 ICON_W = 18;
    const S32 LEN_W = 52;
    const S32 REMOVE_W = 18;

    // The scrollbar's width, always reserved. Reserved even when the list is short enough not to need one, so that adding the entry that overflows does not shove every row's text sideways. A layout
    // that moves when content arrives is worse than one that gives up a few pixels it is not using.
    const S32 SCROLL_W = 6;

    // How far sideways a dragged row has to go before letting go throws it away rather than dropping it back in. Generous, because the cost of the two mistakes is not symmetric: dropping a row you
    // meant to keep loses work, while failing to remove one costs a second attempt. Far enough out that it cannot happen by accident on a list this narrow.
    const S32 DRAG_OUT_M = 46;

    // How long a sound is assumed to last when its length is not known. Playback has to schedule the next entry somehow, and an undecoded asset cannot say. Better to move on at a plausible interval
    // and keep going than to stall on the first entry - and by the second pass the buffer is usually there and the real length is used.
    const F32 UNKNOWN_LEN = 1.2f;
}

void ss_sound_play(const LLUUID& asset_id, const LLUUID& source_id)
{
    if (!gAudiop || asset_id.isNull()) return;

    // Whatever this voice was saying, it is not saying it any more. A preview that layered over its predecessor would be a chord, not an audition.
    ss_sound_stop(source_id);

    gAudiop->triggerSound(asset_id, gAgent.getID(), 1.f,
                          LLAudioEngine::AUDIO_TYPE_UI, LLVector3d::zero,
                          LLUUID::null, source_id);
}

void ss_sound_stop(const LLUUID& source_id)
{
    if (!gAudiop || source_id.isNull()) return;

    LLAudioSource* asp = gAudiop->findAudioSource(source_id);
    if (!asp) return;

    // Deleting the source is what silences it: ~LLAudioSource detaches its channel, which is the thing actually making noise (llaudioengine.cpp, "Stop playback of this sound"). Reaching in to do
    // that here would be doing the destructor's job through members it keeps protected, and getting the same result a call later.
    gAudiop->cleanupAudioSource(asp);
}

F32 ss_sound_length(const LLUUID& id)
{
    if (id.isNull() || !gAudiop) return -1.f;

    LLAudioData* data = gAudiop->getAudioData(id);
    if (!data) return -1.f;

    if (!data->hasDecodedData())
    {
        // Ask for it, and say we do not know yet. The fetch and decode are asynchronous, so the answer arrives in some later frame and the caller simply asks again.
        gAudiop->preloadSound(id);
        return -1.f;
    }

    // Decoded is not the same as loaded, and that gap is why only the sound being played had a length. preloadSound fetches the asset and gets it decoded, but the BUFFER - the thing that knows how
    // long it is - is only built when something needs to play it. So every row sat at "--" until it was the row sounding, at which point a buffer appeared for reasons nothing to do with the list.
    // Asking the engine to build one is the same call playback makes; it just does not wait to be asked.
    LLAudioBuffer* buffer = data->getBuffer();
    if (!buffer)
    {
        gAudiop->updateBufferForData(data, id);
        buffer = data->getBuffer();
    }
    if (!buffer) return -1.f;

    // Milliseconds, from the one place that can convert them. getLength() is PCM BYTES - it always was - so dividing it by a thousand gave a "length" out by however many bytes a second the clip
    // happens to be, which is how a ten second sound read as thousands. The sample rate and channel count needed to turn bytes into time are known only inside the audio backend, so the question is
    // asked there.
    const U32 ms = buffer->getLengthMS();
    return (ms > 0) ? (F32)ms / 1000.f : -1.f;
}

//-----------------------------------------------------------------------------
// The chip
//-----------------------------------------------------------------------------

static LLDefaultChildRegistry::Register<SSSoundListCtrl> r_ss_sound_list("ss_sound_list");

SSSoundListCtrl::Params::Params()
:   mode("mode", "random"),
    max_sounds("max_sounds", 0)
{
}

SSSoundListCtrl::SSSoundListCtrl(const Params& p)
:   LLUICtrl(p),
    mMode(ss_asset_mode_from_key(p.mode)),
    mMaxSounds(llmax(0, p.max_sounds()))
{
}

LLRect SSSoundListCtrl::playRect() const
{
    const LLRect& r = getLocalRect();
    return LLRect(r.mRight - 20, r.mTop - 2, r.mRight - 2, r.mBottom + 2);
}

void SSSoundListCtrl::draw()
{
    advancePlayback();

    const LLRect& r = getLocalRect();

    // An inventory item is a bordered slab with an icon and a label, and this reads as one on purpose: it holds assets, it takes a drop, and looking like the thing it behaves like saves explaining
    // it. The whole chip lifts under the cursor, and its border with it - a grid of these is a lot of small targets, and which one is live wants saying before it is clicked.
    gl_rect_2d(r, mHover ? LLColor4(0.19f, 0.20f, 0.25f, 1.f)
                         : LLColor4(0.13f, 0.13f, 0.16f, 1.f), true);
    gl_rect_2d(r, mHover ? LLColor4(0.55f, 0.58f, 0.68f, 1.f)
                         : LLColor4(0.35f, 0.35f, 0.40f, 1.f), false);

    LLUIImagePtr icon = LLUI::getUIImage("Inv_Sound");
    if (icon.notNull())
    {
        icon->draw(r.mLeft + 3, r.mBottom + (r.getHeight() - 16) / 2, 16, 16);
    }

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 count = (S32)mList.size();

    // Empty says so in words rather than as "0 Sounds", which reads as a quantity where what is meant is a state. Says as much as it has room for, and no less than the count. A footstep grid puts
    // twenty-eight of these across a panel, so most of them are narrow - and "3 Random Sounds" clipped to "3 Rand" is worse than "3", which is the part that was worth reading. The full wording
    // survives wherever there is room for it, and the tooltip has it either way.
    const S32 text_left = r.mLeft + 23;
    const S32 text_room = llmax(0, playRect().mLeft - 4 - text_left);

    std::string label;
    if (count == 0)
    {
        label = (text_room >= 40) ? "Empty" : "-";
    }
    else
    {
        const std::string full = llformat("%d %s Sound%s", count,
                                          (mMode == SS_ASSET_SEQUENCE) ? "Sequenced" : "Random",
                                          count == 1 ? "" : "s");
        label = (font->getWidth(full) <= text_room) ? full : llformat("%d", count);
    }

    font->renderUTF8(label, 0, (F32)text_left, (F32)r.getCenterY(),
                     count ? LLColor4::white : LLColor4(0.6f, 0.6f, 0.6f, 1.f),
                     LLFontGL::LEFT, LLFontGL::VCENTER,
                     LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                     text_room, NULL, true);

    // The play button, drawn rather than childed so the whole chip stays one widget in the XML - a grid of these is verbose enough already.
    const LLRect play = playRect();
    if (count > 0)
    {
        gl_rect_2d(play, mHoverPlay ? LLColor4(0.30f, 0.34f, 0.42f, 1.f)
                                    : LLColor4(0.20f, 0.22f, 0.28f, 1.f), true);

        // A speaker when idle, pause while sounding. A speaker rather than a play triangle because the button previews a sound, and that is what a speaker means; a triangle means "start this thing"
        // and belongs on a transport. Not Play_Off, which despite its name is a drawing of an old movie camera in this skin - worth knowing before reaching for it by name again.
        LLUIImagePtr icon = LLUI::getUIImage(mPlaying ? "Pause_Off" : "Audio_Off");
        if (icon.notNull())
        {
            icon->draw(play.mLeft + 1, play.mBottom + (play.getHeight() - 14) / 2, 14, 14);
        }
    }

    LLUICtrl::draw();
}

void SSSoundListCtrl::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = false;
    mHoverPlay = false;
}

bool SSSoundListCtrl::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = true;
    mHoverPlay = playRect().pointInRect(x, y);
    getWindow()->setCursor(UI_CURSOR_HAND);
    return true;
}

bool SSSoundListCtrl::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mList.empty() && playRect().pointInRect(x, y))
    {
        if (mPlaying)
        {
            stopPlaying();
            return true;
        }

        startPlaying();
        return true;
    }

    openEditor();
    return true;
}

void SSSoundListCtrl::startPlaying()
{
    if (mList.empty()) return;

    if (mVoice.isNull()) mVoice.generate();

    mPlaying = true;
    mPlayIndex = -1;
    mNextAt = 0.0;      // due immediately
}

void SSSoundListCtrl::stopPlaying()
{
    // Silences what is sounding as well as stopping what would follow. The preview is played through a source this control names, so there is something to call back - see ss_sound_play.
    ss_sound_stop(mVoice);

    mPlaying = false;
    mPlayIndex = -1;
}

void SSSoundListCtrl::advancePlayback()
{
    if (!mPlaying) return;

    if (mList.empty())
    {
        stopPlaying();
        return;
    }

    const F64 now = LLTimer::getElapsedSeconds();
    if (now < mNextAt) return;

    if (mMode == SS_ASSET_RANDOM)
    {
        // Sampled, not played through - the same reasoning as the editor's playback. What a random slot sounds like IS consecutive draws from it, so that is what auditioning it has to be.
        mPlayIndex = (S32)((size_t)(ll_frand() * (F32)mList.size()) % mList.size());
    }
    else
    {
        mPlayIndex++;
        if (mPlayIndex >= (S32)mList.size())
        {
            stopPlaying();
            return;
        }
    }

    const LLUUID& id = mList[mPlayIndex];
    ss_sound_play(id, mVoice);

    const F32 len = ss_sound_length(id);
    mNextAt = now + (F64)((len > 0.f) ? len : UNKNOWN_LEN);
}

void SSSoundListCtrl::openEditor()
{
    SSFloaterSoundList* floater =
        LLFloaterReg::getTypedInstance<SSFloaterSoundList>("ss_sound_list");
    if (!floater) return;

    floater->editFor(this, mSlotLabel);
    floater->openFloater();
    mEditorHandle = floater->getHandle();
}

bool SSSoundListCtrl::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                       EDragAndDropType cargo_type, void* cargo_data,
                                       EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_SOUND)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    // A full slot refuses the drop outright rather than accepting it and quietly discarding it - the cursor says no before the mouse is let go, which is the only moment the answer is useful.
    if (isFull())
    {
        *accept = ACCEPT_NO;
        tooltip_msg = llformat("This slot holds at most %d sounds", mMaxSounds);
        return true;
    }

    *accept = ACCEPT_YES_SINGLE;

    if (drop)
    {
        // Appended, not replacing: a slot is a set, and dropping onto a full one plainly means "and this as well". A batch appends in cargo order for free, one call at a time.
        mList.push_back(item->getAssetUUID());
        if (gAudiop) gAudiop->preloadSound(item->getAssetUUID());

        // A commit, like any other control's - this is an edit the owner made through the UI, and nothing about it being a drop makes it less of one. Going out through a private callback instead is
        // how dropping onto a slot managed to change a preset without the editor noticing it had been changed.
        onCommit();
    }

    return true;
}

//-----------------------------------------------------------------------------
// The list
//-----------------------------------------------------------------------------

static LLDefaultChildRegistry::Register<SSSoundListRows> r_ss_sound_list_rows("ss_sound_list_rows");

SSSoundListRows::Params::Params()
{
}

SSSoundListRows::SSSoundListRows(const Params& p)
:   LLUICtrl(p)
{
}

S32 SSSoundListRows::contentHeight() const
{
    return (S32)mList.size() * (ROW_H + ROW_PAD);
}

S32 SSSoundListRows::maxScroll() const
{
    return llmax(0, contentHeight() - getLocalRect().getHeight());
}

void SSSoundListRows::clampScroll()
{
    mScroll = llclamp(mScroll, 0, maxScroll());
}

void SSSoundListRows::scrollTo(S32 index)
{
    if (index < 0 || index >= (S32)mList.size()) return;

    // Only moves when the row is actually out of sight. Centring it every time would make the list jump under playback even while the whole thing is already visible.
    const S32 top = index * (ROW_H + ROW_PAD);
    const S32 bottom = top + ROW_H;
    const S32 view_h = getLocalRect().getHeight();

    if (top < mScroll) mScroll = top;
    else if (bottom > mScroll + view_h) mScroll = bottom - view_h;

    clampScroll();
}

LLRect SSSoundListRows::rowRect(S32 index) const
{
    const LLRect& r = getLocalRect();
    const S32 top = r.mTop + mScroll - index * (ROW_H + ROW_PAD);
    return LLRect(r.mLeft, top, r.mRight - SCROLL_W, top - ROW_H);
}

LLRect SSSoundListRows::removeRect(S32 index) const
{
    const LLRect row = rowRect(index);
    return LLRect(row.mRight - REMOVE_W - 4, row.mTop - 2,
                  row.mRight - 4, row.mBottom + 2);
}

S32 SSSoundListRows::rowAt(S32 y) const
{
    const LLRect& r = getLocalRect();
    const S32 index = (r.mTop + mScroll - y) / (ROW_H + ROW_PAD);
    return (index >= 0 && index < (S32)mList.size()) ? index : -1;
}

S32 SSSoundListRows::gapAt(S32 y) const
{
    // Which gap a point falls in, counting from the top. Rounded to the NEAREST boundary rather than the row containing the point, so the marker lands where the eye expects when hovering near an
    // edge.
    const LLRect& r = getLocalRect();
    const S32 gap = (S32)((F32)(r.mTop + mScroll - y) / (F32)(ROW_H + ROW_PAD) + 0.5f);
    return llclamp(gap, 0, (S32)mList.size());
}

void SSSoundListRows::changed()
{
    if (mOnChanged) mOnChanged();
}

void SSSoundListRows::draw()
{
    const LLRect& r = getLocalRect();
    gl_rect_2d(r, LLColor4(0.10f, 0.10f, 0.12f, 1.f), true);

    LLFontGL* font = LLFontGL::getFontSansSerifSmall();

    // Rows outside the view are skipped rather than drawn and clipped: a long list is mostly off screen, and this is redrawn every frame.
    LLLocalClipRect clip(r);

    for (S32 i = 0; i < (S32)mList.size(); ++i)
    {
        const LLRect row = rowRect(i);
        if (row.mBottom > r.mTop) continue;     // above the view
        if (row.mTop < r.mBottom) break;        // below it, and so is the rest

        const bool dragging_this = (mDragFrom == i);

        // A row being dragged out of the list is drawn faded, so letting go is a decision made with the answer already visible rather than one found out afterwards.
        const F32 alpha = (dragging_this && mDragOut) ? 0.3f
                        : (dragging_this ? 0.65f : 1.f);

        // Hovered rows lift slightly. Enough to follow the cursor down a long list without the list itself looking busy.
        const bool hovered = (i == mHoverRow) && (mDragFrom < 0);
        const LLColor4 row_col = hovered ? LLColor4(0.23f, 0.24f, 0.30f, alpha)
                                         : LLColor4(0.16f, 0.16f, 0.20f, alpha);
        gl_rect_2d(row, row_col, true);

        // The marker down the left: which entry is sounding now. One marker for the whole list, drawn on whichever row that is - the gutter is reserved on every row so the text does not shift
        // sideways as it moves.
        if (i == mPlaying)
        {
            // A speaker again, and deliberately not a triangle: a triangle on a row reads as a button that would play THAT row, which is not what this is. It is a marker saying this one is sounding
            // now.
            LLUIImagePtr icon = LLUI::getUIImage("Audio_Off");
            if (icon.notNull())
            {
                icon->draw(row.mLeft + 2, row.mBottom + (ROW_H - 14) / 2, 14, 14);
            }
        }

        // The item's name where there is one, the raw id where there is not. A list of eight footstep variants is unreadable as eight UUIDs - they differ in the middle, where nobody is looking. The
        // name is what an author chose and is the only thing that distinguishes them at a glance. Falling back to the id rather than to nothing keeps a hand-entered or shared-preset entry
        // identifiable; it is playable either way (see ss_asset_name).
        const std::string name = ss_asset_name(mList[i]);
        const bool named = !name.empty();
        const std::string row_text = named ? name : mList[i].asString();

        // The unnamed ones are dimmed rather than flagged. Not owning the item is unremarkable and perfectly workable, so it wants to look like a quieter row, not a warning.
        const LLColor4 text_col = named
            ? LLColor4(0.88f, 0.88f, 0.92f, alpha)
            : LLColor4(0.62f, 0.62f, 0.68f, alpha);

        const S32 text_left = row.mLeft + ICON_W + 6;
        const S32 text_right = row.mRight - REMOVE_W - LEN_W - 12;
        font->renderUTF8(row_text, 0, (F32)text_left, (F32)row.getCenterY(),
                         text_col, LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX,
                         llmax(0, text_right - text_left), NULL, true);

        // Length, once it is knowable - see ss_sound_length.
        const F32 secs = ss_sound_length(mList[i]);
        const std::string len_text = (secs >= 0.f) ? llformat("%.2fs", secs)
                                                   : std::string("--");
        font->renderUTF8(len_text, 0, row.mRight - REMOVE_W - 10, row.getCenterY(),
                         LLColor4(0.65f, 0.65f, 0.7f, alpha),
                         LLFontGL::RIGHT, LLFontGL::VCENTER);

        // The remove button only reddens under the cursor. Sitting there in warning colour on every row makes a list of eight look like eight problems.
        const LLRect x_rect = removeRect(i);
        const bool x_hot = hovered && mHoverRemove;
        if (x_hot)
        {
            gl_rect_2d(x_rect, LLColor4(0.45f, 0.20f, 0.20f, alpha), true);
        }
        font->renderUTF8("x", 0, x_rect.getCenterX(), x_rect.getCenterY(),
                         x_hot ? LLColor4(1.f, 0.85f, 0.85f, alpha)
                               : LLColor4(0.62f, 0.55f, 0.55f, alpha),
                         LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    // The scrollbar, when there is more list than window.
    if (maxScroll() > 0)
    {
        const S32 view_h = r.getHeight();
        const S32 track_x = r.mRight - SCROLL_W;

        gl_rect_2d(LLRect(track_x, r.mTop, r.mRight, r.mBottom),
                   LLColor4(0.06f, 0.06f, 0.08f, 1.f), true);

        const F32 span = (F32)view_h / (F32)contentHeight();
        const S32 thumb_h = llmax(20, (S32)(span * (F32)view_h));
        const F32 pos = (F32)mScroll / (F32)maxScroll();
        const S32 thumb_top = r.mTop - (S32)(pos * (F32)(view_h - thumb_h));

        gl_rect_2d(LLRect(track_x + 1, thumb_top, r.mRight - 1, thumb_top - thumb_h),
                   LLColor4(0.34f, 0.36f, 0.42f, 1.f), true);
    }

    // The drop marker: a line in the gap the entry would land in, whether it is arriving from inventory or being moved within the list.
    const S32 marker = (mDropGap >= 0) ? mDropGap : (mDragFrom >= 0 ? mDragTo : -1);
    if (marker >= 0 && !mDragOut)
    {
        const S32 y = r.mTop + mScroll - marker * (ROW_H + ROW_PAD) + ROW_PAD / 2;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4f(0.4f, 0.75f, 1.f, 0.95f);
        gGL.begin(LLRender::LINES);
        gGL.vertex2i(r.mLeft + 2, y);
        gGL.vertex2i(r.mRight - SCROLL_W - 2, y);
        gGL.end();
        gGL.flush();
    }

    LLUICtrl::draw();
}

bool SSSoundListRows::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row < 0) return LLUICtrl::handleMouseDown(x, y, mask);

    if (removeRect(row).pointInRect(x, y))
    {
        mList.erase(mList.begin() + row);
        if (mPlaying >= (S32)mList.size()) mPlaying = -1;
        changed();
        return true;
    }

    mDragFrom = row;
    mDragTo = row;
    mDragOut = false;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool SSSoundListRows::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (maxScroll() <= 0) return false;   // nothing to scroll; let it pass on

    mScroll += clicks * (ROW_H + ROW_PAD);
    clampScroll();
    return true;
}

bool SSSoundListRows::handleToolTip(S32 x, S32 y, MASK mask)
{
    // The id, for the row showing a name - copying one out or checking it against a preset is the only reason to want it, and neither is worth a column.
    const S32 row = rowAt(y);
    if (row >= 0 && row < (S32)mList.size())
    {
        LLToolTipMgr::instance().show(mList[row].asString());
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

void SSSoundListRows::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHoverRow = -1;
    mHoverRemove = false;
}

bool SSSoundListRows::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragFrom < 0 || !hasMouseCapture())
    {
        // Which row the cursor is on, and whether it is on the remove button - the two things the user needs told back before clicking, since one of them deletes an entry.
        mHoverRow = rowAt(y);
        mHoverRemove = (mHoverRow >= 0) && removeRect(mHoverRow).pointInRect(x, y);
        getWindow()->setCursor(mHoverRow >= 0 ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
        return LLUICtrl::handleHover(x, y, mask);
    }

    mHoverRow = -1;
    mHoverRemove = false;

    const LLRect& r = getLocalRect();

    // Far enough out to the side, or off the view entirely, and letting go means removing rather than moving.
    mDragOut = (x < r.mLeft - DRAG_OUT_M) || (x > r.mRight + DRAG_OUT_M)
            || (y > r.mTop + DRAG_OUT_M) || (y < r.mBottom - DRAG_OUT_M);

    mDragTo = gapAt(y);
    getWindow()->setCursor(mDragOut ? UI_CURSOR_NOLOCKED : UI_CURSOR_ARROW);
    return true;
}

bool SSSoundListRows::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragFrom < 0 || !hasMouseCapture()) return LLUICtrl::handleMouseUp(x, y, mask);

    gFocusMgr.setMouseCapture(NULL);

    const S32 from = mDragFrom;
    const S32 to = mDragTo;
    const bool out = mDragOut;

    mDragFrom = -1;
    mDragTo = -1;
    mDragOut = false;

    if (out)
    {
        mList.erase(mList.begin() + from);
    }
    else if (to != from && to != from + 1)
    {
        // Lift then insert, and the insertion point shifts down by one when the row came from above it - the list is one shorter by then.
        const LLUUID id = mList[from];
        mList.erase(mList.begin() + from);
        mList.insert(mList.begin() + (to > from ? to - 1 : to), id);
    }
    else
    {
        return true;    // dropped where it started
    }

    if (mPlaying >= (S32)mList.size()) mPlaying = -1;
    clampScroll();
    changed();
    return true;
}

bool SSSoundListRows::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                       EDragAndDropType cargo_type, void* cargo_data,
                                       EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_SOUND)
    {
        mDropGap = -1;
        *accept = ACCEPT_NO;
        return false;
    }

    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item)
    {
        mDropGap = -1;
        *accept = ACCEPT_NO;
        return false;
    }

    if (isFull())
    {
        mDropGap = -1;
        *accept = ACCEPT_NO;
        tooltip_msg = llformat("This slot holds at most %d sounds", mMaxSounds);
        return true;
    }

    *accept = ACCEPT_YES_SINGLE;
    mDropGap = gapAt(y);

    if (drop)
    {
        LLToolDragAndDrop& dnd = LLToolDragAndDrop::instance();
        const S32 index = dnd.getCargoIndex();
        const S32 count = (S32)dnd.getCargoCount();

        // A batch arrives one call per item. The first fixes where it starts; the rest go in after it, so the run lands in the order it was dragged rather than every item on the same gap - which
        // would stack them up backwards.
        if (index <= 0)
        {
            mBatchStart = llclamp(mDropGap, 0, (S32)mList.size());
            mBatchCount = 0;
        }

        const LLUUID id = item->getAssetUUID();
        const S32 at = llclamp(mBatchStart + mBatchCount, 0, (S32)mList.size());
        mList.insert(mList.begin() + at, id);
        mBatchCount++;

        if (gAudiop) gAudiop->preloadSound(id);

        // Sorted once the whole batch has landed. Only the run just dropped, never the list around it: an author who has arranged a sequence by hand should not have it rearranged because they added
        // something to it. And only on a MULTI drop, since sorting one item is a rearrangement of nothing. Dropping a numbered set in one gesture is the case this is for - inventory hands them over
        // in whatever order the selection was built, which is rarely the order they are named.
        if (index + 1 >= count && mBatchCount > 1)
        {
            std::sort(mList.begin() + mBatchStart,
                      mList.begin() + mBatchStart + mBatchCount,
                      [](const LLUUID& x, const LLUUID& y)
                      {
                          // Named entries sort by name, and anything without one falls back to its id so the order is at least stable rather than undefined.
                          const std::string nx = ss_asset_name(x);
                          const std::string ny = ss_asset_name(y);
                          return ss_natural_less(nx.empty() ? x.asString() : nx,
                                                 ny.empty() ? y.asString() : ny);
                      });
        }

        mDropGap = -1;
        changed();
    }

    return true;
}

//-----------------------------------------------------------------------------
// The editor
//-----------------------------------------------------------------------------

SSFloaterSoundList::SSFloaterSoundList(const LLSD& key)
:   LLFloater(key)
{
}

SSFloaterSoundList::~SSFloaterSoundList()
{
}

bool SSFloaterSoundList::postBuild()
{
    mList = getChild<SSSoundListRows>("sound_list");

    getChild<LLUICtrl>("play_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickPlay(); });
    getChild<LLUICtrl>("ok_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickOK(); });
    getChild<LLUICtrl>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCancel(); });

    getChild<LLUICtrl>("csv_editor")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitCsv(); });

    mList->setOnChanged([this]() { refresh(); });
    return true;
}

void SSFloaterSoundList::editFor(SSSoundListCtrl* owner, const std::string& label)
{
    if (!owner) return;

    mOwnerHandle = owner->getHandle();
    mOriginal = owner->getList();
    mMode = owner->getMode();
    mCancelled = false;
    mList->setList(mOriginal);
    mList->setMaxSounds(owner->getMaxSounds());

    // Stated, not offered. An author opening a footstep slot should be able to see why the list behaves the way it does without being invited to change it into something that cannot work.
    getChild<LLTextBox>("mode_text")->setText(std::string(
        (mMode == SS_ASSET_SEQUENCE)
            ? "Plays through in order"
            : "Plays one at random each time"));

    setTitle(label.empty() ? std::string("Sounds") : ("Sounds - " + label));

    stopPlayback();
    refresh();
}

void SSFloaterSoundList::onClose(bool app_quitting)
{
    // Closing the window is not a decision either way, so it keeps whatever the list currently holds - the same as OK. Cancel is the button for changing your mind, and it says so. Committing
    // DIRECTLY rather than by calling the OK handler. OK closes, and closing calls this - so routing one through the other was infinite recursion and a stack overflow on the first click of either
    // button. The two share the commit, not the close. Skipped entirely after Cancel, which has already put the original back: committing here would immediately overwrite the restore with the very
    // edits the user asked to discard.
    stopPlayback();

    if (!mCancelled)
    {
        commitToOwner();
    }
    mCancelled = false;
}

void SSFloaterSoundList::commitToOwner()
{
    SSSoundListCtrl* owner = dynamic_cast<SSSoundListCtrl*>(mOwnerHandle.get());
    if (owner && mList)
    {
        // The list only. The mode came from the owner and was never editable here, so writing it back would be handing it its own value.
        owner->setList(mList->getList());
        owner->onCommit();
    }
}

void SSFloaterSoundList::restoreOwner()
{
    SSSoundListCtrl* owner = dynamic_cast<SSSoundListCtrl*>(mOwnerHandle.get());
    if (owner)
    {
        owner->setList(mOriginal);
        owner->onCommit();
    }
}

void SSFloaterSoundList::onCommitCsv()
{
    // The text is the authority when it is edited, so the list is rebuilt from it wholesale. This exists because the list above can only show what it can identify, and a UUID is valid whether or not
    // anything here recognises it - one pasted from a notecard, one out of somebody else's preset, one for an asset the agent does not hold. Without a way in as text those are unreachable through
    // this window, and the preset format is a comma separated string anyway, so the field is showing the file's own view of the slot rather than inventing a second one.
    const std::string csv = getChild<LLUICtrl>("csv_editor")->getValue().asString();
    if (mList)
    {
        mList->setList(ss_asset_list_parse(csv));
    }

    // Whatever was playing referred to entries that may no longer exist.
    stopPlayback();
    refresh();
}

void SSFloaterSoundList::refresh()
{
    const S32 count = mList ? (S32)mList->getList().size() : 0;
    // Kept in step with the list, except while it has focus - rewriting text under a caret mid-edit is the same rule the name fields follow.
    LLUICtrl* csv = getChild<LLUICtrl>("csv_editor");
    if (mList && !csv->hasFocus())
    {
        csv->setValue(ss_asset_list_str(mList->getList()));
    }

    const S32 cap = mList ? (mList->isFull() ? count : 0) : 0;
    getChild<LLTextBox>("count_text")->setText(
        cap > 0 ? llformat("%d sounds (full)", count)
                : llformat("%d sound%s", count, count == 1 ? "" : "s"));

    // Enabled whenever there is something to do: play a list that has entries, or stop one that is running.
    getChild<LLUICtrl>("play_button")->setEnabled(count > 0 || mPlaying);
    getChild<LLButton>("play_button")->setLabel(std::string(mPlaying ? "Stop" : "Play"));
}

void SSFloaterSoundList::onClickPlay()
{
    // The one control for both, because they are one decision. A separate Stop spends a second button on a state the first one already knows, and leaves it sitting there disabled most of the time
    // saying nothing. A button that reads Play or Stop reports what is happening and offers the only thing worth doing about it, in the same place.
    if (mPlaying)
    {
        stopPlayback();
        return;
    }

    if (!mList || mList->getList().empty()) return;

    if (mVoice.isNull()) mVoice.generate();

    mPlaying = true;
    mPlayIndex = -1;
    mNextAt = 0.0;      // due immediately
    refresh();
}

void SSFloaterSoundList::stopPlayback()
{
    ss_sound_stop(mVoice);

    mPlaying = false;
    mPlayIndex = -1;
    if (mList) mList->setPlaying(-1);
    refresh();
}

void SSFloaterSoundList::advancePlayback()
{
    if (!mPlaying || !mList) return;

    const SSSoundList& seq = mList->getList();
    if (seq.empty())
    {
        stopPlayback();
        return;
    }

    const F64 now = LLTimer::getElapsedSeconds();
    if (now < mNextAt) return;

    if (mMode == SS_ASSET_RANDOM)
    {
        // A random list is not played through, it is SAMPLED - which is the only way to hear what it will sound like in use, where consecutive triggers are what an ear is judging. Playing it in
        // order would audition an arrangement that never happens.
        mPlayIndex = (S32)((size_t)(ll_frand() * (F32)seq.size()) % seq.size());
    }
    else
    {
        mPlayIndex++;
        if (mPlayIndex >= (S32)seq.size())
        {
            // One pass, not a loop. Something that will not stop on its own is a nuisance to audition.
            stopPlayback();
            return;
        }
    }

    const LLUUID& id = seq[mPlayIndex];
    ss_sound_play(id, mVoice);

    const F32 len = ss_sound_length(id);
    mNextAt = now + (F64)((len > 0.f) ? len : UNKNOWN_LEN);

    mList->setPlaying(mPlayIndex);
}

void SSFloaterSoundList::draw()
{
    advancePlayback();
    LLFloater::draw();
}

void SSFloaterSoundList::onClickOK()
{
    commitToOwner();
    closeFloater();
}

void SSFloaterSoundList::onClickCancel()
{
    restoreOwner();

    // Told before closing, because the close is what would otherwise commit straight over this.
    mCancelled = true;
    closeFloater();
}

// </SS:Nexii>
