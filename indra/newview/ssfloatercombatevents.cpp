/**
 * @file ssfloatercombatevents.cpp
 * @brief Combat Log core: every DAMAGE and DEATH event in one sortable, filterable scroll list, with a lower
 *        pane that explains whichever row is selected.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloatercombatevents.h"

#include "sscombatfeedline.h"
#include "sscombaticons.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llformat.h"
#include "lllineeditor.h"
#include "llscrolllistcell.h"
#include "llscrolllistcolumn.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llstring.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

#include <cmath>

using namespace SSCombat;

namespace
{
    // The damage-type atlas is 128 px per cell (textures.xml); LLScrollListIcon::getHeight() reports that native
    // size regardless of setIconSize(), which would otherwise balloon every row in the list. SS_EVENT_ROW_HEIGHT
    // is forced back onto the list with LLScrollListCtrl::setLineHeight() after every batch of inserts, while
    // SS_EVENT_ICON_SIZE controls what actually gets drawn.
    constexpr S32 SS_EVENT_ICON_SIZE = 16;
    constexpr S32 SS_EVENT_ROW_HEIGHT = 20;
    // How close an adjusted damage value has to be to its initial value before we call it unadjusted.
    constexpr F32 SS_ADJUST_EPSILON = 0.05f;

    // "12.0" or, when a modification changed the number, "12.0 -> 8.0". Used by the related-hits pane, which
    // still shows both numbers in the old order; the kill-feed line (buildLine() below) leads with the final one.
    std::string ss_damage_text(F32 initial, F32 final_damage)
    {
        if (fabsf(initial - final_damage) > SS_ADJUST_EPSILON)
        {
            return llformat("%.1f -> %.1f", initial, final_damage);
        }
        return llformat("%.1f", final_damage);
    }

    // Casts the icon column of a freshly-added row down to its intended draw size (see SS_EVENT_ICON_SIZE above).
    void ss_shrink_icon_cell(LLScrollListItem* item, S32 icon_column)
    {
        if (!item || icon_column < 0)
        {
            return;
        }
        if (LLScrollListCell* cell = item->getColumn(icon_column))
        {
            static_cast<LLScrollListIcon*>(cell)->setIconSize(SS_EVENT_ICON_SIZE);
        }
    }
}

// Loads the deterministic scripted raid and shows this floater so the officer sees it land.
void ss_combat_log_load_synthetic(const LLSD& live)
{
    SSCombatLog::instance().loadSynthetic(1, live.asBoolean());
    LLFloaterReg::showInstance("ss_combat_events");
}

// Floater shell; everything is wired in postBuild.
SSFloaterCombatEvents::SSFloaterCombatEvents(const LLSD& key)
:   LLFloater(key)
{
}

// Finds every widget, wires the callbacks and subscribes to the store's two signals.
bool SSFloaterCombatEvents::postBuild()
{
    mFilterEdit = getChild<LLLineEditor>("filter_edit");
    mMineOnlyCheck = getChild<LLCheckBoxCtrl>("mine_only_check");
    mKindCombo = getChild<LLComboBox>("kind_combo");
    mCountText = getChild<LLTextBox>("count_text");
    mEventsList = getChild<LLScrollListCtrl>("events_list");
    mRelatedLabel = getChild<LLTextBox>("related_label");
    mRelatedList = getChild<LLScrollListCtrl>("related_list");
    mShowWorldBtn = getChild<LLButton>("show_world_btn");
    mReconstructBtn = getChild<LLButton>("reconstruct_btn");

    // XUI's keystroke_callback does not reliably reach here, so the filter edit is wired by hand (same
    // workaround as floater_settings_debug.xml).
    mFilterEdit->setKeystrokeCallback([this](LLLineEditor*, void*) { onFilterChanged(); }, nullptr);
    mMineOnlyCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFilterChanged(); });
    mKindCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFilterChanged(); });

    mEventsList->setCommitOnSelectionChange(true);
    mEventsList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectEvent(); });
    mEventsList->setDoubleClickCallback([this]() { onDoubleClickEvent(); });
    mEventsList->setLineHeight(SS_EVENT_ROW_HEIGHT);
    if (LLScrollListColumn* icon_col = mEventsList->getColumn("icon"))
    {
        mEventIconColumnIndex = icon_col->mIndex;
    }

    mShowWorldBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickShowInWorld(); });
    mReconstructBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReconstruct(); });
    getChild<LLButton>("load_mock_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickLoadMock(); });
    getChild<LLButton>("replay_mock_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReplayMock(); });

    setRelatedColumns(true);

    SSCombatLog& log = SSCombatLog::instance();
    mDataConnection = log.dataChangedSignal().connect([this]() { onDataChanged(); });
    mViewConnection = log.viewChangedSignal().connect([this]() { onViewChanged(); });

    rebuildEvents();
    rebuildRelated();
    refreshButtons();
    return true;
}

// Every frame: publish the viewport for the overlay and resolve the hover row, both cheap O(visible rows) polls
// since LLScrollListCtrl exposes no scroll or hover signal.
void SSFloaterCombatEvents::draw()
{
    publishVisibleViewport();
    updateHover();
    LLFloater::draw();
}

// Store data changed: fold in new events, or notice the store cleared and start over.
void SSFloaterCombatEvents::onDataChanged()
{
    if (SSCombatLog::instance().events().size() < mNextEventIndex)
    {
        rebuildEvents();
    }
    else
    {
        appendNewEvents();
    }
}

// The shared selection moved from elsewhere; mirror it into events_list without re-entering select().
void SSFloaterCombatEvents::onViewChanged()
{
    const SSCombat::View& view = SSCombatLog::instance().view();
    if (!view.mSelection.valid())
    {
        return;
    }

    const std::string want = view.mSelection.address();
    const LLScrollListItem* current = mEventsList->getFirstSelected();
    if (current && current->getValue().asString() == want)
    {
        return; // already showing it
    }

    if (mEventsList->setSelectedByValue(LLSD(want), true))
    {
        mEventsList->scrollToShowSelected();
        refreshButtons();
        rebuildRelated();
    }
}

// Any filter control changed: re-read all three and rescan.
void SSFloaterCombatEvents::onFilterChanged()
{
    mFilterText = mFilterEdit->getText();
    LLStringUtil::toLower(mFilterText);
    mMineOnly = mMineOnlyCheck->get();
    mKindFilter = mKindCombo->getValue().asString();
    rebuildEvents();
}

// The officer picked a row: tell the store, then refresh the lower pane and the buttons.
void SSFloaterCombatEvents::onSelectEvent()
{
    if (const LLScrollListItem* item = mEventsList->getFirstSelected())
    {
        const NounRef ref = NounRef::parse(item->getValue().asString());
        if (ref.valid())
        {
            SSCombatLog::instance().select(ref, true);
        }
    }
    refreshButtons();
    rebuildRelated();
}

// Double-clicking a death (or object death) jumps straight into reconstruction.
void SSFloaterCombatEvents::onDoubleClickEvent()
{
    const LLScrollListItem* item = mEventsList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const NounRef ref = NounRef::parse(item->getValue().asString());
    if (const Event* ev = SSCombatLog::instance().event(ref.mIndex))
    {
        if (ev->mKind == EVENT_DEATH || ev->mKind == EVENT_OBJECT_DEATH)
        {
            SSCombatLog::instance().enterReconstruction(ev->mId);
        }
    }
}

// Selects the current row and moves the cursor to it, same as clicking the row.
void SSFloaterCombatEvents::onClickShowInWorld()
{
    const LLScrollListItem* item = mEventsList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const NounRef ref = NounRef::parse(item->getValue().asString());
    if (ref.valid())
    {
        SSCombatLog::instance().select(ref, true);
    }
}

// Enters reconstruction for the selected death; refreshButtons() keeps this disabled otherwise.
void SSFloaterCombatEvents::onClickReconstruct()
{
    const LLScrollListItem* item = mEventsList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const NounRef ref = NounRef::parse(item->getValue().asString());
    if (const Event* ev = SSCombatLog::instance().event(ref.mIndex))
    {
        if (ev->mKind == EVENT_DEATH || ev->mKind == EVENT_OBJECT_DEATH)
        {
            SSCombatLog::instance().enterReconstruction(ev->mId);
        }
    }
}

// Stage 0 preview: load the scripted raid in bulk.
void SSFloaterCombatEvents::onClickLoadMock()
{
    ss_combat_log_load_synthetic(LLSD(false));
}

// Stage 0 preview: load the scripted raid and replay it live.
void SSFloaterCombatEvents::onClickReplayMock()
{
    ss_combat_log_load_synthetic(LLSD(true));
}

// Full rebuild: clears events_list and rescans every stored event against the current filter.
void SSFloaterCombatEvents::rebuildEvents()
{
    mEventsList->clearRows();
    mNextEventIndex = 0;
    mMatchedTotal = 0;
    appendNewEvents();
}

// Folds in only the events appended to the store since the last call, trims the visible cap, refreshes
// count_text, and auto-scrolls to the bottom only when the officer was already there (chat-log behaviour).
// Cheap even on a long session because it never rescans events already folded in.
void SSFloaterCombatEvents::appendNewEvents()
{
    const S32 visible_rows = llmax(1, mEventsList->getItemListRect().getHeight() / SS_EVENT_ROW_HEIGHT);
    const bool was_at_bottom = (mEventsList->getScrollPos() + visible_rows) >= mEventsList->getItemCount();

    const std::vector<Event>& events = SSCombatLog::instance().events();
    bool added_any = false;
    for (; mNextEventIndex < events.size(); ++mNextEventIndex)
    {
        const Event& ev = events[mNextEventIndex];
        if (!passesFilter(ev))
        {
            continue;
        }
        ++mMatchedTotal;
        addEventRow(ev);
        added_any = true;
    }

    while (mEventsList->getItemCount() > (S32)MAX_VISIBLE_ROWS)
    {
        // Oldest matching row is at the top as long as the list is still sorted by time; see the header
        // comment on MAX_VISIBLE_ROWS in ssfloatercombatevents.h for the caveat when the officer resorts it.
        mEventsList->deleteSingleItem(0);
    }
    mEventsList->setLineHeight(SS_EVENT_ROW_HEIGHT);

    if (added_any && was_at_bottom)
    {
        mEventsList->setScrollPos(mEventsList->getItemCount());
    }
    ++mListRevision;

    refreshCountText();
}

// One row for events_list: a kind icon and one kill-feed line, e.g. "14:32  Cadmus killed Vex with [Ash]
// bolt-caster (58 m)". Deaths draw bold so they stand out in the scroll.
void SSFloaterCombatEvents::addEventRow(const Event& ev)
{
    LLScrollListCell::Params cell;
    cell.font = LLFontGL::getFontSansSerif();

    LLScrollListItem::Params row;
    row.value = SSCombat::eventRef(ev).address();

    cell.column = "icon";
    cell.type = "icon";
    cell.value = SSCombat::kindIcon(ev);
    row.columns.add(cell);
    cell.type = "text";

    const bool is_death = (ev.mKind == EVENT_DEATH || ev.mKind == EVENT_OBJECT_DEATH);
    cell.column = "line";
    cell.value = buildLine(ev);
    cell.font = is_death ? LLFontGL::getFontSansSerifBold() : LLFontGL::getFontSansSerif();
    row.columns.add(cell);

    LLScrollListItem* item = mEventsList->addRow(row);
    ss_shrink_icon_cell(item, mEventIconColumnIndex);
}

// The single kill-feed line for one event; format depends on kind (ux spec, owner's chat-log request).
std::string SSFloaterCombatEvents::buildLine(const Event& ev) const
{
    return SSCombat::killFeedLine(ev);
}

// count_text: "N events", or "showing last 2000 of N" once the visible cap has kicked in.
void SSFloaterCombatEvents::refreshCountText()
{
    std::string text;
    if (mMatchedTotal > MAX_VISIBLE_ROWS)
    {
        text = llformat("showing last %u of %u", (U32)MAX_VISIBLE_ROWS, (U32)mMatchedTotal);
    }
    else
    {
        text = llformat("%u event%s", (U32)mMatchedTotal, mMatchedTotal == 1 ? "" : "s");
    }
    mCountText->setText(text);
}

// Family name of the rezzer's or source's equipment; "unknown" when neither is known.
std::string SSFloaterCombatEvents::weaponFamilyFor(const Event& ev) const
{
    return SSCombat::weaponFamilyFor(ev);
}

// Recomputes which event ids are on screen from getScrollPos() and the item list's rect against the fixed row
// height, and republishes them to the store for the overlay. Gated on a cheap key so the O(visible rows) work
// (and the vector churn in setVisibleEvents()) only happens on a frame that actually scrolled, resized, or
// changed the list's contents (mListRevision, bumped once per appendNewEvents() call).
void SSFloaterCombatEvents::publishVisibleViewport()
{
    const S32 scroll_pos = mEventsList->getScrollPos();
    const S32 item_count = mEventsList->getItemCount();
    const S32 rect_height = mEventsList->getItemListRect().getHeight();
    if (scroll_pos == mLastScrollPos && item_count == mLastItemCount
        && rect_height == mLastRectHeight && mListRevision == mLastRevision)
    {
        return;
    }
    mLastScrollPos = scroll_pos;
    mLastItemCount = item_count;
    mLastRectHeight = rect_height;
    mLastRevision = mListRevision;

    const S32 visible_rows = llmax(1, rect_height / SS_EVENT_ROW_HEIGHT);
    const S32 last = llmin(item_count, scroll_pos + visible_rows + 1); // +1 for a partially visible trailing row

    std::vector<U32> ids;
    ids.reserve((size_t)llmax(0, last - scroll_pos));
    for (S32 i = scroll_pos; i < last; ++i)
    {
        if (LLScrollListItem* item = mEventsList->getItemByIndex(i))
        {
            const NounRef ref = NounRef::parse(item->getValue().asString());
            if (ref.valid())
            {
                ids.push_back(ref.mIndex);
            }
        }
    }
    SSCombatLog::instance().setVisibleEvents(ids);
}

// Resolves the row under the pointer from LLScrollListCtrl's own hover highlight (it already tracks this for
// row shading, so no extra hit-testing is needed) and writes it straight into view().mHover; no signal fires.
void SSFloaterCombatEvents::updateHover()
{
    NounRef ref;
    const S32 hovered = mEventsList->getHighlightedItemInx();
    if (hovered >= 0)
    {
        if (LLScrollListItem* item = mEventsList->getItemByIndex(hovered))
        {
            ref = NounRef::parse(item->getValue().asString());
        }
    }
    if (ref == mLastHover)
    {
        return;
    }
    mLastHover = ref;
    SSCombatLog::instance().view().mHover = ref;
}

// Kind combo, "mine only" and the name/weapon filter, all ANDed together.
bool SSFloaterCombatEvents::passesFilter(const Event& ev) const
{
    if (mKindFilter == "deaths" && ev.mKind != EVENT_DEATH && ev.mKind != EVENT_OBJECT_DEATH)
    {
        return false;
    }
    if (mKindFilter == "damage" && ev.mKind != EVENT_DAMAGE)
    {
        return false;
    }

    if (mMineOnly)
    {
        const LLUUID& me = gAgent.getID();
        if (ev.mOwner != me && ev.mTarget != me)
        {
            return false;
        }
    }

    if (!mFilterText.empty())
    {
        const SSCombatLog& log = SSCombatLog::instance();
        std::string attacker = log.displayName(ev.mOwner);
        std::string target = log.displayName(ev.mTarget);
        std::string weapon = weaponFamilyFor(ev);
        LLStringUtil::toLower(attacker);
        LLStringUtil::toLower(target);
        LLStringUtil::toLower(weapon);
        if (attacker.find(mFilterText) == std::string::npos
            && target.find(mFilterText) == std::string::npos
            && weapon.find(mFilterText) == std::string::npos)
        {
            return false;
        }
    }
    return true;
}

// Reconstruct only makes sense for a death; Show in world needs any selection at all.
void SSFloaterCombatEvents::refreshButtons()
{
    const LLScrollListItem* item = mEventsList->getFirstSelected();
    mShowWorldBtn->setEnabled(item != nullptr);

    bool is_death = false;
    if (item)
    {
        const NounRef ref = NounRef::parse(item->getValue().asString());
        if (const Event* ev = SSCombatLog::instance().event(ref.mIndex))
        {
            is_death = (ev->mKind == EVENT_DEATH || ev->mKind == EVENT_OBJECT_DEATH);
        }
    }
    mReconstructBtn->setEnabled(is_death);
}

// Rebuilds the lower pane for whatever is selected in events_list: hits leading to a death, or the
// adjustment steps (and the death, if any) that followed a hit.
void SSFloaterCombatEvents::rebuildRelated()
{
    mRelatedList->clearRows();

    const LLScrollListItem* item = mEventsList->getFirstSelected();
    if (!item)
    {
        setRelatedColumns(true);
        mRelatedLabel->setText(std::string("Select a row to see what led to it"));
        return;
    }

    const SSCombatLog& log = SSCombatLog::instance();
    const NounRef ref = NounRef::parse(item->getValue().asString());
    const Event* ev = log.event(ref.mIndex);
    if (!ev)
    {
        setRelatedColumns(true);
        mRelatedLabel->setText(std::string("That event is no longer held"));
        return;
    }

    if (ev->mKind == EVENT_DEATH || ev->mKind == EVENT_OBJECT_DEATH)
    {
        mRelatedLabel->setText(std::string("Hits that led to this death"));
        setRelatedColumns(true);

        const Attribution attr = SSCombatLog::instance().attribution(ev->mId);
        const F64 from = ev->mTime - (F64)attr.mWindow;
        const std::vector<Event>& events = log.events();

        for (size_t i = log.eventIndexAt(from); i < events.size(); ++i)
        {
            const Event& hit = events[i];
            if (hit.mTime > ev->mTime)
            {
                break;
            }
            if (hit.mKind != EVENT_DAMAGE || hit.mTarget != ev->mTarget)
            {
                continue;
            }

            LLScrollListCell::Params cell;
            cell.font = LLFontGL::getFontSansSerif();
            LLScrollListItem::Params row;
            row.value = SSCombat::eventRef(hit).address();

            cell.column = "icon";
            cell.type = "icon";
            cell.value = SSCombatIcons::forType(hit.mType);
            row.columns.add(cell);
            cell.type = "text";

            cell.column = "tminus";
            cell.value = llformat("-%.1f s", ev->mTime - hit.mTime);
            row.columns.add(cell);

            cell.column = "attacker";
            cell.value = log.displayName(hit.mOwner);
            row.columns.add(cell);

            cell.column = "weapon";
            cell.value = weaponFamilyFor(hit);
            row.columns.add(cell);

            cell.column = "damage";
            cell.value = llformat("%.1f", hit.mDamage);
            row.columns.add(cell);

            cell.column = "type";
            cell.value = std::string(SSCombatIcons::typeName(hit.mType));
            row.columns.add(cell);

            cell.column = "adjusted";
            cell.value = (fabsf(hit.mInitial - hit.mDamage) > SS_ADJUST_EPSILON)
                ? llformat("%.1f -> %.1f", hit.mInitial, hit.mDamage) : std::string();
            row.columns.add(cell);

            LLScrollListItem* new_item = mRelatedList->addRow(row);
            ss_shrink_icon_cell(new_item, 0);
        }
    }
    else if (ev->mKind == EVENT_DAMAGE)
    {
        mRelatedLabel->setText(std::string("Adjustments for this hit"));
        setRelatedColumns(false);

        const std::vector<Modification>& mods = log.modifications();
        if (ev->mModsCount == 0)
        {
            mRelatedList->addCommentText(std::string("No adjustments"));
        }
        else
        {
            for (U32 m = 0; m < ev->mModsCount; ++m)
            {
                const Modification& mod = mods[ev->mModsBegin + m];
                const Equipment* eq = log.equipmentFor(mod.mTaskId);
                const std::string task = (eq && !eq->mName.empty()) ? eq->mName : mod.mTaskId.asString().substr(0, 8);

                LLScrollListCell::Params cell;
                cell.font = LLFontGL::getFontSansSerif();
                LLScrollListItem::Params row;
                row.value = llformat("mod:%u", ev->mModsBegin + m);

                cell.column = "task";
                cell.value = task;
                row.columns.add(cell);

                cell.column = "script";
                cell.value = mod.mScript;
                row.columns.add(cell);

                cell.column = "damage";
                cell.value = llformat("%.1f", mod.mNewDamage);
                row.columns.add(cell);

                mRelatedList->addRow(row);
            }
        }

        // The death this hit fed into, if one follows on the same target within the death-attribution window.
        static LLCachedControl<F32> window(gSavedSettings, "SSCombatLogDeathWindowSeconds", 20.f);
        const std::vector<Event>& events = log.events();
        for (size_t i = log.eventIndexAt(ev->mTime); i < events.size(); ++i)
        {
            const Event& later = events[i];
            if (later.mTime > ev->mTime + (F64)window)
            {
                break;
            }
            if ((later.mKind == EVENT_DEATH || later.mKind == EVENT_OBJECT_DEATH) && later.mTarget == ev->mTarget)
            {
                LLScrollListCell::Params cell;
                cell.font = LLFontGL::getFontSansSerifBold();
                LLScrollListItem::Params row;
                row.value = SSCombat::eventRef(later).address();

                cell.column = "task";
                cell.value = std::string("-> leads to death");
                row.columns.add(cell);
                cell.font = LLFontGL::getFontSansSerif();

                cell.column = "script";
                cell.value = log.displayName(later.mTarget);
                row.columns.add(cell);

                cell.column = "damage";
                cell.value = llformat("+%.1f s", later.mTime - ev->mTime);
                row.columns.add(cell);

                mRelatedList->addRow(row);
                break;
            }
        }
    }
    else
    {
        mRelatedLabel->setText(std::string("No detail for this event kind"));
    }

    mRelatedList->setLineHeight(SS_EVENT_ROW_HEIGHT);
}

// Swaps related_list between the seven-column hit-list layout and the three-column adjustment-list layout;
// a no-op when it is already showing the requested layout.
void SSFloaterCombatEvents::setRelatedColumns(bool hitsMode)
{
    if (mRelatedList->getNumColumns() > 0 && mRelatedHitsMode == hitsMode)
    {
        return;
    }
    mRelatedHitsMode = hitsMode;

    mRelatedList->clearRows();
    mRelatedList->clearColumns();

    LLScrollListColumn::Params col;
    if (hitsMode)
    {
        col.name = "icon";       col.header.label = "";         col.width.pixel_width.set(20, true);  mRelatedList->addColumn(col);
        col.name = "tminus";     col.header.label = "T-";       col.width.pixel_width.set(60, true);  mRelatedList->addColumn(col);
        col.name = "attacker";   col.header.label = "Attacker"; col.width.pixel_width.set(120, true); mRelatedList->addColumn(col);
        col.name = "weapon";     col.header.label = "Weapon";   col.width.pixel_width.set(100, true); mRelatedList->addColumn(col);
        col.name = "damage";     col.header.label = "Damage";   col.width.pixel_width.set(65, true);  mRelatedList->addColumn(col);
        col.name = "type";       col.header.label = "Type";     col.width.pixel_width.set(85, true);  mRelatedList->addColumn(col);
        col.name = "adjusted";   col.header.label = "Adjusted"; col.width.pixel_width.set(100, true); mRelatedList->addColumn(col);
    }
    else
    {
        col.name = "task";       col.header.label = "Task";        col.width.pixel_width.set(170, true); mRelatedList->addColumn(col);
        col.name = "script";     col.header.label = "Script";      col.width.pixel_width.set(170, true); mRelatedList->addColumn(col);
        col.name = "damage";     col.header.label = "New damage";  col.width.pixel_width.set(100, true); mRelatedList->addColumn(col);
    }
}
