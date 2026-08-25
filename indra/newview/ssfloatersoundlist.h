/**
 * @file ssfloatersoundlist.h
 * @brief Atmo Magic: a list of sounds, as a compact control and the editor
 *        behind it.
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

#ifndef SS_FLOATERSOUNDLIST_H
#define SS_FLOATERSOUNDLIST_H

// <SS:Nexii> Atmo Magic sound lists

#include "ssassetlist.h"

#include "llfloater.h"
#include "lluictrl.h"

#include <functional>
#include <string>
#include <vector>

// Sound lists are ordered asset lists - see ssassetlist.h for the list
// itself, its two modes, the comma separated form the presets store, and the
// naming and natural-order helpers. All of that is shared with textures and
// none of it is about sound.
//
// What is left here is the part that is: previewing a list means PLAYING it,
// and playing has a duration, an order to go in, and something to stop.
typedef SSAssetList SSSoundList;
typedef SSAssetListMode SSSoundListMode;

// Play a sound so that it can be stopped again, and stop one so played.
//
// Ordinary triggerSound is fire and forget: it makes an audio source, names
// it with an id nobody keeps, and there is no way back to it. That is fine
// for a footstep and useless for a preview, where Stop has to mean silence
// rather than "no further entries" - which is all it could manage while the
// clip already sounding belonged to nobody.
//
// This fork's triggerSound takes the source id as an argument, so a caller
// can name its own source and find it again.
void ss_sound_play(const LLUUID& asset_id, const LLUUID& source_id);
void ss_sound_stop(const LLUUID& source_id);

// How long a sound is, in seconds, or -1 if that is not known yet.
//
// Not known YET is the normal case for anything just dropped in: length lives
// on the decoded buffer, so it exists only once the asset has been fetched
// and decoded. Callers show a placeholder and ask again later rather than
// blocking, and this kicks off the fetch so that later actually arrives.
F32 ss_sound_length(const LLUUID& id);

// The name of an inventory item pointing at this asset, or an empty string
// if the agent holds none.
//
// Empty is not an error and must not be treated as one. A sound plays from
// its asset id whether or not anybody owns an item for it, so a UUID typed
// in by hand, or one arriving in a preset from someone else, is perfectly
// playable and simply has no name here. This is for READING the list, not
// for deciding what may go in it.
std::string ss_asset_name(const LLUUID& id);

//-----------------------------------------------------------------------------
// The compact control: an inventory-asset-looking chip that says how many
// sounds are in the slot, with a play button beside it.
//
// A row of these is how a footstep grid has to be edited - 7 surfaces by 4
// actions is 28 slots, and 28 of anything larger does not fit on a panel.
// The chip carries only what is worth seeing at a glance, which is whether a
// slot is empty and roughly how full it is; the editor behind it carries the
// rest.
//-----------------------------------------------------------------------------
class SSSoundListCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        // Both are properties of the SLOT, so both belong in the markup that
        // declares the slot rather than in code that finds it afterwards. A
        // footstep grid is 28 of these and every one of them is random with
        // the same ceiling; saying so 28 times in XML is tedious, but saying
        // it 28 times in C++ is worse and easier to get out of step with the
        // panel it describes.
        Optional<std::string> mode;      // "random" (default) or "sequence"
        Optional<S32> max_sounds;        // 0 means no limit

        Params();
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

    // Accepts a sound dropped straight onto the chip, so filling an empty
    // slot does not need the editor opened at all.
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    // Stops any audition in progress: the list it was walking is no longer
    // the list.
    void setList(const SSSoundList& seq) { mList = seq; stopPlaying(); }
    const SSSoundList& getList() const { return mList; }

    // Set by whoever owns the slot, and only there.
    //
    // The slot's purpose decides this, not its contents: a footstep grid is
    // random because a step draws one sound, an ambient bed is sequenced
    // because a bed plays through. Neither is a preference, and offering it
    // as one in the editor invited an author to set a footstep slot to
    // "sequence" and get marching - a setting whose only use is to break the
    // thing it belongs to. It is declared in the markup beside the slot and
    // read from there.
    void setMode(SSSoundListMode mode) { mMode = mode; }
    SSSoundListMode getMode() const { return mMode; }

    // How many entries the slot will hold, or 0 for no limit. Enforced
    // wherever an entry can be added - the chip's own drop, and the editor -
    // so there is no way in that skips it.
    void setMaxSounds(S32 n) { mMaxSounds = llmax(0, n); }
    S32 getMaxSounds() const { return mMaxSounds; }
    bool isFull() const { return mMaxSounds > 0 && (S32)mList.size() >= mMaxSounds; }

    // What the editor titles itself with, so a slot in a grid can say which
    // one it is.
    void setSlotLabel(const std::string& label) { mSlotLabel = label; }

protected:
    friend class LLUICtrlFactory;
    SSSoundListCtrl(const Params& p);

private:
    void openEditor();
    LLRect playRect() const;

    // The chip plays its own slot, so it has to know whether it is doing so.
    void startPlaying();
    void stopPlaying();
    void advancePlayback();

    bool mPlaying = false;
    S32 mPlayIndex = -1;
    F64 mNextAt = 0.0;

    // The audio source this chip's previews are played through, so they can
    // be silenced. Generated once and reused: one chip is one voice, and
    // starting a new preview should cut off the last rather than pile on it.
    LLUUID mVoice;

    SSSoundList mList;
    SSSoundListMode mMode = SS_ASSET_RANDOM;
    S32 mMaxSounds = 0;
    std::string mSlotLabel;

    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    LLHandle<LLFloater> mEditorHandle;
    bool mHover = false;
    bool mHoverPlay = false;
};

//-----------------------------------------------------------------------------
// The list inside the editor.
//
// A custom view rather than a scroll list, because everything this has to do
// is the part a scroll list will not: a drop marker between two rows, a row
// that fades while it is being dragged out, and a drag that means "reorder"
// or "remove" depending only on where it ends up.
//-----------------------------------------------------------------------------
class SSSoundListRows : public LLUICtrl
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
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    void setList(const SSSoundList& seq) { mList = seq; mPlaying = -1; }
    const SSSoundList& getList() const { return mList; }

    void setOnChanged(std::function<void()> cb) { mOnChanged = cb; }

    void setMaxSounds(S32 n) { mMaxSounds = llmax(0, n); }
    bool isFull() const { return mMaxSounds > 0 && (S32)mList.size() >= mMaxSounds; }

    // Which entry is sounding, for the marker down the left.
    void setPlaying(S32 index) { mPlaying = index; scrollTo(index); }
    S32 getPlaying() const { return mPlaying; }

protected:
    friend class LLUICtrlFactory;
    SSSoundListRows(const Params& p);

private:
    S32 contentHeight() const;
    S32 maxScroll() const;
    void clampScroll();
    void scrollTo(S32 index);           // bring a row into view

    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    S32 rowAt(S32 y) const;             // which row a point is over, -1 if none
    S32 gapAt(S32 y) const;             // which gap between rows, 0..size()
    LLRect rowRect(S32 index) const;
    LLRect removeRect(S32 index) const;
    void changed();

    SSSoundList mList;
    S32 mPlaying = -1;

    // The row the cursor is over, and whether it is over that row's remove
    // button specifically. Both -1/false when the cursor is elsewhere.
    S32 mHoverRow = -1;
    bool mHoverRemove = false;

    // How far the list is scrolled, in pixels from the top of the content.
    S32 mScroll = 0;

    S32 mMaxSounds = 0;

    // The drag in progress, if any. mDragFrom is the row picked up; mDragTo
    // is the gap it would land in; mDragOut is whether letting go now would
    // throw it away instead.
    S32 mDragFrom = -1;
    S32 mDragTo = -1;
    bool mDragOut = false;

    // Where a dropped inventory sound would be inserted, or -1 when nothing
    // is hovering.
    S32 mDropGap = -1;

    // A multi-item drag arrives one call at a time, so where the batch began
    // and how much of it has landed have to be remembered between them.
    S32 mBatchStart = 0;
    S32 mBatchCount = 0;

    std::function<void()> mOnChanged;
};

//-----------------------------------------------------------------------------
// The editor.
//-----------------------------------------------------------------------------
class SSFloaterSoundList : public LLFloater
{
public:
    SSFloaterSoundList(const LLSD& key);
    ~SSFloaterSoundList();

    bool postBuild() override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // Opened against a control, which is where the result goes back to. The
    // control owns the value; this only edits a copy of it until OK.
    void editFor(SSSoundListCtrl* owner, const std::string& label);

    SSSoundListMode mode() const { return mMode; }

private:
    // Hands the edited list back to the control that opened this. Does NOT
    // close: closing is the caller's business, and confusing the two is what
    // made this recurse.
    void commitToOwner();
    void restoreOwner();

    void onCommitCsv();
    // One button, both jobs - see onClickPlay.
    void onClickPlay();
    void stopPlayback();
    void onClickOK();
    void onClickCancel();
    void advancePlayback();
    void refresh();

    SSSoundListRows* mList = nullptr;
    LLHandle<LLView> mOwnerHandle;

    // What the slot held on open, for Cancel. Only the CONTENTS are captured:
    // the mode cannot change here, so there is nothing about it to restore.
    SSSoundList mOriginal;

    // The owner's mode, read once and used for playback. Not edited.
    SSSoundListMode mMode = SS_ASSET_RANDOM;

    // Playback: which entry is sounding and when the next one is due. Timed
    // rather than driven by the audio engine finishing, because nothing
    // reports that - see ss_sound_length.
    //
    // Both modes run on the same clock. A sequence steps to the next entry
    // when the current one ends; a random slot picks another one. The
    // difference is only which entry comes next, not when.
    // Set by Cancel so the close that follows knows not to commit over the
    // restore it just did.
    bool mCancelled = false;

    bool mPlaying = false;
    S32 mPlayIndex = -1;
    F64 mNextAt = 0.0;

    // As on the chip - see SSSoundListCtrl::mVoice.
    LLUUID mVoice;
};

// </SS:Nexii>

#endif
