/**
 * @file ssfloatercombatlog.h
 * @brief Combat Log main floater: status chrome, parameters, the six stage tabs, the ribbon and the transport
 *        bar. Design: doc/combat_log_ux.md 3.1-3.9.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_FLOATERCOMBATLOG_H
#define SS_FLOATERCOMBATLOG_H

#include "sscombatlog.h"

#include "llfloater.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

class LLButton;
class LLScrollListCtrl;
class LLTabContainer;
class LLTextBox;
class LLTextEditor;
class SSMapView;
class SSRibbonView;

// Menu handler for the Develop entries; live=true replays the scripted raid in real time.
void ss_combat_log_load_synthetic(const LLSD& live);

class SSFloaterCombatLog : public LLFloater
{
public:
    SSFloaterCombatLog(const LLSD& key);
    ~SSFloaterCombatLog();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;

private:
    // Store signals: data changed means the lists are stale, view changed means the drawing is stale.
    void onDataChanged();
    void onViewChanged();

    // Title status line, session span and the "not available" strip.
    void refreshChrome();
    // Selects the tab matching the current level (without re-entering) and refreshes only that panel.
    void refreshStage();
    // Transport readout, speed, live and follow states.
    void refreshTransport();

    void rebuildSession();
    void rebuildEngagement();
    void rebuildMoment();
    void rebuildRaw();

    // User picked a stage tab; maps it back onto SSCombatLog::setLevel unless we are syncing programmatically.
    void onCommitStageTab();

    void onCommitWindow();
    void onCommitLive();
    void onCommitSpeed();
    void onCommitFollow();

    void onClickRewind();
    void onClickStepBack();
    void onClickPlayPause();
    void onClickStepForward();
    void onClickForwardEnd();

    void onSelectEvent();
    void onSelectEngagementRow();
    void onSelectMomentRow();
    void onSelectRawRow();
    void onSelectTeamRow();
    void onCommitEventFilter();

    // Moves the cursor to the nearest event before or after it, honouring the filter-free event list.
    void stepEvent(S32 direction);
    // Selects a noun from a scroll list row whose id is an ss: address.
    void selectFromRow(LLScrollListCtrl* list, bool moveCursor);

    // Type/team filter state for the Session event list.
    std::string mEventKindFilter;
    S32         mEventTeamFilter = -2;

    LLTextBox*          mStatusText = NULL;
    LLTextBox*          mSpanText = NULL;
    LLTextBox*          mNotAvailableText = NULL;
    LLTextBox*          mTimeText = NULL;
    LLTextBox*          mHeadlineText = NULL;
    LLTextBox*          mRegionText = NULL;
    LLTextBox*          mEngagementHeader = NULL;
    LLTextEditor*       mMomentSummary = NULL;
    LLTextBox*          mEvidenceText = NULL;

    LLTabContainer*     mStageTabs = NULL;
    SSRibbonView*       mRibbon = NULL;
    SSMapView*          mMap = NULL;

    LLScrollListCtrl*   mTeamList = NULL;
    LLScrollListCtrl*   mEventList = NULL;
    LLScrollListCtrl*   mEngagementList = NULL;
    LLScrollListCtrl*   mMomentList = NULL;
    LLScrollListCtrl*   mRawList = NULL;

    LLTextEditor*       mRawTextView = NULL;

    boost::signals2::scoped_connection mDataConnection;
    boost::signals2::scoped_connection mViewConnection;

    bool                mListsDirty = true;
    bool                mFollowCursor = true;
    bool                mUpdating = false;      // guards the rebuild/select/notify loop
    bool                mSyncingTab = false;    // guards mStageTabs->selectTab so its commit does not re-enter setLevel
    S8                  mDrawnLevel = SSCombat::LEVEL_SESSION;
    S32                 mFilterTeams = -1;      // team count the filter combo was built for
    F64                 mDrawnCursor = -1.0;
    size_t              mDrawnEvents = 0;
};

#endif // SS_FLOATERCOMBATLOG_H
