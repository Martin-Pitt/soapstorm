/**
 * @file ssfloatercombatcombatant.cpp
 * @brief Combat Log: the Combatant page, the first "details" floater. Multi-instance, keyed by the avatar's
 *        UUID string, so the officer can have several combatants open side by side.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloatercombatcombatant.h"

#include "sscombatfeedline.h"
#include "sscombaticons.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfontgl.h"
#include "llformat.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llstring.h"
#include "lltextbox.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace SSCombat;

namespace
{
    // Same row height/icon size convention as the Events floater's kill feed (sscombatfeedline.h note).
    constexpr S32 SS_COMBATANT_ICON_SIZE = 16;
    constexpr S32 SS_COMBATANT_ROW_HEIGHT = 20;
    // How far ±60 s reaches; "whole session" swaps this in instead (see windowSeconds()).
    constexpr F32 SS_COMBATANT_DEFAULT_WINDOW = 60.f;
    constexpr F32 SS_COMBATANT_WHOLE_SESSION_WINDOW = 1.0e9f;

    // Word for one of the four confidence bands, per the glossary's fixed ladder (never a fifth word).
    const char* ss_confidence_word(U8 confidence)
    {
        switch (confidence)
        {
        case CONF_FIRM:   return "firm";
        case CONF_LIKELY: return "likely";
        case CONF_WEAK:   return "weak";
        default:          return "undetermined";
        }
    }

    // Shrinks a freshly-added row's icon cell to the draw size (the atlas cells are 128 px natively).
    void ss_shrink_icon_cell(LLScrollListItem* item, S32 icon_column = 0)
    {
        if (!item)
        {
            return;
        }
        if (LLScrollListCell* cell = item->getColumn(icon_column))
        {
            static_cast<LLScrollListIcon*>(cell)->setIconSize(SS_COMBATANT_ICON_SIZE);
        }
    }

    // Per-family tally for the equipment list: hits, kills, first/last used, and which damage type is most common.
    struct SSFamilyRow
    {
        std::string                    mFamily;
        U32                             mHits = 0;
        U32                             mKills = 0;
        F64                             mFirst = 0.0;
        F64                             mLast = 0.0;
        std::unordered_map<S16, U32>   mTypeCounts;
    };
}

// Floater shell; everything is wired in postBuild.
SSFloaterCombatCombatant::SSFloaterCombatCombatant(const LLSD& key)
:   LLFloater(key)
{
}

// Finds every widget, wires the callbacks, captures the reference time and subscribes to the store's signals.
bool SSFloaterCombatCombatant::postBuild()
{
    mId = LLUUID(getKey().asString());

    mSideText = getChild<LLTextBox>("side_text");
    mStatusText = getChild<LLTextBox>("status_text");
    mStatsText = getChild<LLTextBox>("stats_text");
    mReferenceText = getChild<LLTextBox>("reference_text");
    mWholeSessionCheck = getChild<LLCheckBoxCtrl>("whole_session_check");
    mEquipmentList = getChild<LLScrollListCtrl>("equipment_list");
    mRecentList = getChild<LLScrollListCtrl>("recent_events_list");
    mReconstructBtn = getChild<LLButton>("reconstruct_btn");

    mWholeSessionCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onWholeSessionChanged(); });
    mRecentList->setCommitOnSelectionChange(true);
    mRecentList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectRecentEvent(); });
    mReconstructBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReconstruct(); });

    SSCombatLog& log = SSCombatLog::instance();
    mReferenceTime = log.view().mSelection.mTime;
    mLastSelection = log.view().mSelection;
    mReferenceText->setText(llformat("Around %s", sessionClock(mReferenceTime).c_str()));

    mDataConnection = log.dataChangedSignal().connect([this]() { onDataChanged(); });
    mViewConnection = log.viewChangedSignal().connect([this]() { onViewChanged(); });

    refreshAll();
    return true;
}

// Store data changed: refresh everything, but at most once a second while the session is replaying live.
void SSFloaterCombatCombatant::onDataChanged()
{
    const SSCombatLog& log = SSCombatLog::instance();
    if (log.view().mLive && (log.now() - mLastRefresh) < 1.0)
    {
        return;
    }
    mLastRefresh = log.now();
    refreshAll();
}

// The shared View moved. A *new* selection while "whole session" is off re-centres the reference time and
// rebuilds both lists; anything else (most often just the cursor) only touches the cheap status line, throttled
// the same way as onDataChanged() so cursor scrubbing during live playback cannot storm full rescans.
void SSFloaterCombatCombatant::onViewChanged()
{
    const SSCombat::View& view = SSCombatLog::instance().view();
    const bool new_selection = view.mSelection.valid() && !(view.mSelection == mLastSelection);
    mLastSelection = view.mSelection;

    if (new_selection && !mWholeSessionCheck->get())
    {
        mReferenceTime = view.mSelection.mTime;
        mReferenceText->setText(llformat("Around %s", sessionClock(mReferenceTime).c_str()));
        refreshAll();
        return;
    }

    const SSCombatLog& log = SSCombatLog::instance();
    if (log.view().mLive && (log.now() - mLastRefresh) < 1.0)
    {
        return;
    }
    mLastRefresh = log.now();
    refreshHeader();
}

// "Whole session" toggled: the window changed, so both lists need rebuilding.
void SSFloaterCombatCombatant::onWholeSessionChanged()
{
    rebuildEquipment();
    rebuildRecentEvents();
}

// Clicked a row in recent_events_list: tell the store, same as the Events floater's onClickShowInWorld.
void SSFloaterCombatCombatant::onSelectRecentEvent()
{
    if (const LLScrollListItem* item = mRecentList->getFirstSelected())
    {
        const NounRef ref = NounRef::parse(item->getValue().asString());
        if (ref.valid())
        {
            SSCombatLog::instance().select(ref, true);
        }
    }
}

// Reconstructs the most recent DEATH where this combatant was the target; refreshButtons() disables the button
// otherwise.
void SSFloaterCombatCombatant::onClickReconstruct()
{
    if (const Event* death = findLastDeath())
    {
        SSCombatLog::instance().enterReconstruction(death->mId);
    }
}

// Name (also the floater title), side with its confidence word, status at the cursor, and the session tallies.
void SSFloaterCombatCombatant::refreshHeader()
{
    const SSCombatLog& log = SSCombatLog::instance();
    setTitle(log.displayName(mId));

    const TeamAssignment side = log.team(mId);
    mSideText->setText(llformat("Side: %s (%s)", log.teamName(side.mTeam).c_str(), ss_confidence_word(side.mConfidence)));
    mSideText->setColor(log.teamColor(side.mTeam));

    // Alive/dead: the latest life whose start is at or before the cursor tells us; a life with mDeathEvent == 0
    // is still open. Combatant/civilian is the owner's own active-combatant test.
    const F64 cursor = log.view().mCursor;
    bool alive = true;
    for (const Life& life : log.lives(mId))
    {
        if (life.mStart <= cursor)
        {
            alive = (life.mDeathEvent == 0) || (cursor < life.mEnd);
        }
    }
    const bool is_combatant = log.isCombatantAt(mId, cursor);
    mStatusText->setText(llformat("Status: %s - %s", alive ? "alive" : "dead", is_combatant ? "combatant" : "civilian"));

    // Session tallies: owner == id counts as dealt (a kill or a hit dealt), target == id counts as taken.
    U32 kills = 0, deaths = 0;
    F32 dealt = 0.f, taken = 0.f;
    for (const Event& ev : log.events())
    {
        if (ev.mKind == EVENT_DEATH || ev.mKind == EVENT_OBJECT_DEATH)
        {
            if (ev.mOwner == mId) { ++kills; }
            if (ev.mTarget == mId) { ++deaths; }
        }
        else if (ev.mKind == EVENT_DAMAGE)
        {
            if (ev.mOwner == mId) { dealt += ev.mDamage; }
            if (ev.mTarget == mId) { taken += ev.mDamage; }
        }
    }
    mStatsText->setText(llformat("Kills %u   Deaths %u   Damage dealt %.0f   Damage taken %.0f", kills, deaths, dealt, taken));
}

// One row per equipment family this combatant fired with inside the current window, ordered so the family
// closest to the reference time (by its nearest hit) leads.
void SSFloaterCombatCombatant::rebuildEquipment()
{
    mEquipmentList->clearRows();

    const SSCombatLog& log = SSCombatLog::instance();
    const F64 from = mReferenceTime - (F64)windowSeconds();
    const F64 to = mReferenceTime + (F64)windowSeconds();

    std::vector<SSFamilyRow> rows;
    auto find_row = [&rows](const std::string& family) -> SSFamilyRow&
    {
        for (SSFamilyRow& row : rows)
        {
            if (row.mFamily == family)
            {
                return row;
            }
        }
        rows.push_back(SSFamilyRow());
        rows.back().mFamily = family;
        rows.back().mFirst = 1.0e18; // huge sentinel so the first real hit always wins the min() below
        return rows.back();
    };

    for (const Event& ev : log.events())
    {
        if (ev.mKind != EVENT_DAMAGE || ev.mOwner != mId || ev.mTime < from || ev.mTime > to)
        {
            continue;
        }
        SSFamilyRow& row = find_row(weaponFamilyFor(ev));
        ++row.mHits;
        ++row.mTypeCounts[ev.mType];
        row.mFirst = llmin(row.mFirst, ev.mTime);
        row.mLast = llmax(row.mLast, ev.mTime);
    }

    // Credit a kill to whichever family dealt the attributed blow, per the store's own attribution, not a guess.
    for (const Event& ev : log.events())
    {
        if ((ev.mKind != EVENT_DEATH && ev.mKind != EVENT_OBJECT_DEATH) || ev.mTime < from || ev.mTime > to)
        {
            continue;
        }
        const Attribution attr = log.attribution(ev.mId);
        const Event* blow = (attr.mBlow != 0) ? log.event(attr.mBlow) : nullptr;
        if (blow && blow->mOwner == mId)
        {
            find_row(weaponFamilyFor(*blow)).mKills++;
        }
    }

    std::sort(rows.begin(), rows.end(), [this](const SSFamilyRow& a, const SSFamilyRow& b)
    {
        const F64 da = llmin(fabs(a.mFirst - mReferenceTime), fabs(a.mLast - mReferenceTime));
        const F64 db = llmin(fabs(b.mFirst - mReferenceTime), fabs(b.mLast - mReferenceTime));
        return da < db;
    });

    for (const SSFamilyRow& row : rows)
    {
        S16 top_type = 0;
        U32 top_count = 0;
        for (const auto& entry : row.mTypeCounts)
        {
            if (entry.second > top_count)
            {
                top_count = entry.second;
                top_type = entry.first;
            }
        }

        LLScrollListCell::Params cell;
        cell.font = LLFontGL::getFontSansSerif();
        LLScrollListItem::Params item;

        cell.column = "icon"; cell.type = "icon"; cell.value = SSCombatIcons::forType(top_type); item.columns.add(cell);
        cell.type = "text";
        cell.column = "family"; cell.value = row.mFamily; item.columns.add(cell);
        cell.column = "hits";   cell.value = llformat("%u", row.mHits); item.columns.add(cell);
        cell.column = "kills";  cell.value = llformat("%u", row.mKills); item.columns.add(cell);
        cell.column = "first";  cell.value = sessionClock(row.mFirst); item.columns.add(cell);
        cell.column = "last";   cell.value = sessionClock(row.mLast); item.columns.add(cell);

        ss_shrink_icon_cell(mEquipmentList->addRow(item));
    }
    mEquipmentList->setLineHeight(SS_COMBATANT_ROW_HEIGHT); // icon cells re-grow the line on insert; force it back
}

// This combatant's events (as attacker or target) inside the current window, oldest first, in the same
// kill-feed format as the Events floater.
void SSFloaterCombatCombatant::rebuildRecentEvents()
{
    mRecentList->clearRows();

    const SSCombatLog& log = SSCombatLog::instance();
    const F64 from = mReferenceTime - (F64)windowSeconds();
    const F64 to = mReferenceTime + (F64)windowSeconds();

    const std::vector<Event>& events = log.events();
    for (size_t i = log.eventIndexAt(from); i < events.size(); ++i)
    {
        const Event& ev = events[i];
        if (ev.mTime > to)
        {
            break;
        }
        if (ev.mOwner != mId && ev.mTarget != mId)
        {
            continue;
        }

        LLScrollListCell::Params cell;
        cell.font = LLFontGL::getFontSansSerif();
        LLScrollListItem::Params row;
        row.value = eventRef(ev).address();

        const bool is_death = (ev.mKind == EVENT_DEATH || ev.mKind == EVENT_OBJECT_DEATH);
        cell.column = "icon"; cell.type = "icon"; cell.value = kindIcon(ev); row.columns.add(cell);
        cell.type = "text";
        cell.column = "line"; cell.value = killFeedLine(ev);
        cell.font = is_death ? LLFontGL::getFontSansSerifBold() : LLFontGL::getFontSansSerif();
        row.columns.add(cell);

        ss_shrink_icon_cell(mRecentList->addRow(row));
    }
    mRecentList->setLineHeight(SS_COMBATANT_ROW_HEIGHT); // icon cells re-grow the line on insert; force it back
}

// Reconstruct only makes sense once this combatant has died at least once.
void SSFloaterCombatCombatant::refreshButtons()
{
    mReconstructBtn->setEnabled(findLastDeath() != nullptr);
}

// Runs all four rebuilds; called from both store signals and the whole-session toggle.
void SSFloaterCombatCombatant::refreshAll()
{
    refreshHeader();
    rebuildEquipment();
    rebuildRecentEvents();
    refreshButtons();
}

// ±60 s, or the whole session when whole_session_check is ticked (a window wide enough to swallow any real
// session, so the equipment/recent-events scans need no special-casing).
F32 SSFloaterCombatCombatant::windowSeconds() const
{
    return mWholeSessionCheck->get() ? SS_COMBATANT_WHOLE_SESSION_WINDOW : SS_COMBATANT_DEFAULT_WINDOW;
}

// The most recent DEATH with this combatant as the target, or null.
const Event* SSFloaterCombatCombatant::findLastDeath() const
{
    const std::vector<Event>& events = SSCombatLog::instance().events();
    for (auto it = events.rbegin(); it != events.rend(); ++it)
    {
        if (it->mKind == EVENT_DEATH && it->mTarget == mId)
        {
            return &(*it);
        }
    }
    return nullptr;
}
