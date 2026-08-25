/**
 * @file ssfloatertexturelist.h
 * @brief Atmo Magic: a list of textures, as a compact control and the editor
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

#ifndef SS_FLOATERTEXTURELIST_H
#define SS_FLOATERTEXTURELIST_H

// <SS:Nexii> Atmo Magic texture lists
// The same shape as the sound list, and for the same reasons - see ssassetlist.h, which holds everything the two have in common. What differs is entirely in how a row shows itself: a sound can only
// be described (a name, a length) and has to be played to be known, while a texture can just be LOOKED at. So there is no transport here at all. The swatch is the preview, and the rows are tall
// enough to carry one.

#include "ssassetlist.h"

#include "llfloater.h"
#include "lluictrl.h"
#include "llviewertexture.h"

#include <functional>
#include <string>

//-----------------------------------------------------------------------------
// The compact control: a chip showing the first texture and how many there are, standing in for the slot on a crowded panel.
//-----------------------------------------------------------------------------
class SSTextureListCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        // Both are properties of the slot, declared where the slot is - see SSSoundListCtrl::Params, which says the same about the same two.
        Optional<std::string> mode;      // "random" (default) or "sequence"
        Optional<S32> max_textures;      // 0 means no limit

        Params();
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    void setList(const SSAssetList& list) { mList = list; }
    const SSAssetList& getList() const { return mList; }

    void setMode(SSAssetListMode mode) { mMode = mode; }
    SSAssetListMode getMode() const { return mMode; }

    void setMaxTextures(S32 n) { mMaxTextures = llmax(0, n); }
    S32 getMaxTextures() const { return mMaxTextures; }
    bool isFull() const { return mMaxTextures > 0 && (S32)mList.size() >= mMaxTextures; }

    void setSlotLabel(const std::string& label) { mSlotLabel = label; }

protected:
    friend class LLUICtrlFactory;
    SSTextureListCtrl(const Params& p);

private:
    void openEditor();

    SSAssetList mList;
    SSAssetListMode mMode = SS_ASSET_RANDOM;
    S32 mMaxTextures = 0;
    std::string mSlotLabel;

    LLHandle<LLFloater> mEditorHandle;
    bool mHover = false;
};

//-----------------------------------------------------------------------------
// The rows inside the editor. Taller than the sound list's, because each carries a swatch and two lines of text rather than one line - the name, and the resolution under it.
//-----------------------------------------------------------------------------
class SSTextureListRows : public LLUICtrl
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
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    void setList(const SSAssetList& list) { mList = list; clampScroll(); }
    const SSAssetList& getList() const { return mList; }

    void setOnChanged(std::function<void()> cb) { mOnChanged = cb; }

    void setMaxTextures(S32 n) { mMaxTextures = llmax(0, n); }
    bool isFull() const { return mMaxTextures > 0 && (S32)mList.size() >= mMaxTextures; }

protected:
    friend class LLUICtrlFactory;
    SSTextureListRows(const Params& p);

private:
    S32 contentHeight() const;
    S32 maxScroll() const;
    void clampScroll();

    S32 rowAt(S32 y) const;
    S32 gapAt(S32 y) const;
    LLRect rowRect(S32 index) const;
    LLRect removeRect(S32 index) const;
    void changed();

    SSAssetList mList;

    S32 mScroll = 0;
    S32 mMaxTextures = 0;

    S32 mHoverRow = -1;
    bool mHoverRemove = false;

    S32 mDragFrom = -1;
    S32 mDragTo = -1;
    bool mDragOut = false;

    S32 mDropGap = -1;
    S32 mBatchStart = 0;
    S32 mBatchCount = 0;

    std::function<void()> mOnChanged;
};

//-----------------------------------------------------------------------------
// The editor.
//-----------------------------------------------------------------------------
class SSFloaterTextureList : public LLFloater
{
public:
    SSFloaterTextureList(const LLSD& key);

    bool postBuild() override;
    void onClose(bool app_quitting) override;

    void editFor(SSTextureListCtrl* owner, const std::string& label);

private:
    void commitToOwner();
    void restoreOwner();

    void onCommitCsv();
    void onClickOK();
    void onClickCancel();
    void refresh();

    SSTextureListRows* mRows = nullptr;
    LLHandle<LLView> mOwnerHandle;

    SSAssetList mOriginal;
    SSAssetListMode mMode = SS_ASSET_RANDOM;

    bool mCancelled = false;
};

// </SS:Nexii>

#endif
