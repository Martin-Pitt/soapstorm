/**
 * @file ssfloatercombatcombatant.h
 * @brief Combat Log: the Combatant page, the first "details" floater. Multi-instance, keyed by the avatar's
 *        UUID string, so the officer can have several combatants open side by side. Design: doc/combat_log_ux.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_FLOATERCOMBATCOMBATANT_H
#define SS_FLOATERCOMBATCOMBATANT_H

#include "sscombatlog.h"

#include "llfloater.h"

#include <boost/signals2.hpp>
#include <string>

class LLButton;
class LLCheckBoxCtrl;
class LLScrollListCtrl;
class LLTextBox;

class SSFloaterCombatCombatant : public LLFloater
{
public:
    SSFloaterCombatCombatant(const LLSD& key);
    ~SSFloaterCombatCombatant() override = default;

    bool postBuild() override;

private:
    // Store signals: new events (throttled to once a second while live) and the shared View moving.
    void onDataChanged();
    void onViewChanged();

    // "Whole session" toggled: widen or narrow the window and rebuild both lists.
    void onWholeSessionChanged();
    // Clicked a row in recent_events_list: tells the store, same as the Events floater.
    void onSelectRecentEvent();
    // Reconstructs the most recent DEATH where this combatant was the target.
    void onClickReconstruct();

    // Name, side, status-at-cursor and the session kill/death/damage tallies.
    void refreshHeader();
    // One row per equipment family this combatant fired with, ordered by proximity to the reference time.
    void rebuildEquipment();
    // This combatant's events (as attacker or target) inside the current window, oldest first.
    void rebuildRecentEvents();
    // Enables reconstruct_btn only when a death exists.
    void refreshButtons();
    // Runs all four rebuilds; called from both store signals and the whole-session toggle.
    void refreshAll();

    // ±60 s, or the whole session when whole_session_check is ticked.
    F32 windowSeconds() const;
    // The most recent DEATH with this combatant as the target, or null.
    const SSCombat::Event* findLastDeath() const;

    LLUUID mId;                        // the combatant this page is about

    LLTextBox*          mSideText = nullptr;
    LLTextBox*          mStatusText = nullptr;
    LLTextBox*          mStatsText = nullptr;
    LLTextBox*          mReferenceText = nullptr;
    LLCheckBoxCtrl*     mWholeSessionCheck = nullptr;
    LLScrollListCtrl*   mEquipmentList = nullptr;
    LLScrollListCtrl*   mRecentList = nullptr;
    LLButton*           mReconstructBtn = nullptr;

    boost::signals2::scoped_connection mDataConnection;
    boost::signals2::scoped_connection mViewConnection;

    F64                 mReferenceTime = 0.0;   // view().mSelection.mTime when the floater was opened
    SSCombat::NounRef   mLastSelection;         // to notice a *new* selection while whole-session is off
    F64                 mLastRefresh = 0.0;     // throttles onDataChanged() to once a second while live
};

#endif // SS_FLOATERCOMBATCOMBATANT_H
