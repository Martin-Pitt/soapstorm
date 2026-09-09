/**
 * @file ssfloatercombatevents.h
 * @brief Combat Log core: every DAMAGE and DEATH event in one sortable, filterable scroll list, with a lower
 *        pane that explains whichever row is selected. Design: doc/combat_log_ux.md, doc/combat_log_analysis.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_FLOATERCOMBATEVENTS_H
#define SS_FLOATERCOMBATEVENTS_H

#include "sscombatlog.h"

#include "llfloater.h"

#include <boost/signals2.hpp>
#include <string>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;

// Menu handler for the Develop entries and the SSCombat.LoadSynthetic commit function; live=true replays the
// scripted raid in real time through ingest. Loads the raid and shows this floater so the officer sees it land.
void ss_combat_log_load_synthetic(const LLSD& live);

class SSFloaterCombatEvents : public LLFloater
{
public:
    SSFloaterCombatEvents(const LLSD& key);
    ~SSFloaterCombatEvents() override = default;

    bool postBuild() override;
    // Polls the list's scroll position and hover row every frame; see publishVisibleViewport()/updateHover().
    void draw() override;

private:
    // Store signals: new events to fold in, or the shared selection moved from elsewhere.
    void onDataChanged();
    void onViewChanged();

    // Any filter control changed; throws away the list and rescans everything.
    void onFilterChanged();
    // The officer picked a row; tells the store and refreshes the lower pane and buttons.
    void onSelectEvent();
    // Double-click a death row jumps straight into reconstruction.
    void onDoubleClickEvent();

    void onClickShowInWorld();
    void onClickReconstruct();
    void onClickLoadMock();
    void onClickReplayMock();

    // Full rebuild: clears the list and rescans every stored event against the current filter.
    void rebuildEvents();
    // Incremental update: folds in only the events appended since the last call; detects a store clear.
    void appendNewEvents();
    // One row for events_list, honouring the newest-2000 visible cap.
    void addEventRow(const SSCombat::Event& ev);
    // The single kill-feed line for one event, e.g. "14:32  Cadmus killed Vex with [Ash] bolt-caster (58 m)".
    std::string buildLine(const SSCombat::Event& ev) const;
    // Updates count_text with the matched/total tally.
    void refreshCountText();
    // Rebuilds the lower pane for whatever is selected in events_list, or clears it when nothing is.
    void rebuildRelated();
    // Reconstruct only makes sense for a death; Show in world needs any selection at all.
    void refreshButtons();
    // Swaps related_list's columns between the hit-list layout and the adjustment-list layout.
    void setRelatedColumns(bool hitsMode);

    // Current filter settings, cached from the widgets so passesFilter() need not touch them per event.
    bool passesFilter(const SSCombat::Event& ev) const;
    // Family name of the rezzer's or source's equipment, "unknown" when neither is known.
    std::string weaponFamilyFor(const SSCombat::Event& ev) const;

    // Recomputes the visible row range from getScrollPos()/getItemListRect() and republishes it to the store,
    // but only when the scroll pos, item count, list height or content revision actually changed.
    void publishVisibleViewport();
    // Resolves the row under the pointer from LLScrollListCtrl's own hover highlight and writes view().mHover.
    void updateHover();

    LLLineEditor*       mFilterEdit = nullptr;
    LLCheckBoxCtrl*     mMineOnlyCheck = nullptr;
    LLComboBox*         mKindCombo = nullptr;
    LLTextBox*          mCountText = nullptr;
    LLScrollListCtrl*   mEventsList = nullptr;
    LLTextBox*          mRelatedLabel = nullptr;
    LLScrollListCtrl*   mRelatedList = nullptr;
    LLButton*           mShowWorldBtn = nullptr;
    LLButton*           mReconstructBtn = nullptr;

    boost::signals2::scoped_connection mDataConnection;
    boost::signals2::scoped_connection mViewConnection;

    std::string mFilterText;           // lower-cased name/weapon filter
    std::string mKindFilter = "all";   // "all", "deaths" or "damage"
    bool        mMineOnly = false;

    size_t      mNextEventIndex = 0;   // how many of SSCombatLog::events() are already folded in
    size_t      mMatchedTotal = 0;     // how many of those pass the current filter
    bool        mRelatedHitsMode = true; // which column layout related_list currently has
    S32         mEventIconColumnIndex = -1; // events_list's "icon" column, cached so appending stays cheap at scale

    // Viewport-publish and hover-poll state, refreshed once per draw(); see publishVisibleViewport()/updateHover().
    S32                 mLastScrollPos = -1;
    S32                 mLastItemCount = -1;
    S32                 mLastRectHeight = -1;
    U32                 mListRevision = 0;      // bumped once per appendNewEvents() call
    U32                 mLastRevision = ~0u;
    SSCombat::NounRef   mLastHover;

    static constexpr size_t MAX_VISIBLE_ROWS = 2000;
};

#endif // SS_FLOATERCOMBATEVENTS_H
