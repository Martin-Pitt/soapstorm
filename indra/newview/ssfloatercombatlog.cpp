/**
 * @file ssfloatercombatlog.cpp
 * @brief See ssfloatercombatlog.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloatercombatlog.h"

#include "sscombatviews.h"
#include "ssfloatercombatpage.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llkeyboard.h"
#include "llscrolllistctrl.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>

using namespace SSCombat;

namespace
{
    // Hard cap on rows built per refresh; a raid is long and the officer is often peeking mid-fight.
    const S32 MAX_ROWS = 3000;

    // Holds a flag for the length of a rebuild, so a list's own selection callbacks cannot re-enter it.
    struct SSScopedFlag
    {
        bool& mFlag;
        explicit SSScopedFlag(bool& flag) : mFlag(flag) { mFlag = true; }
        ~SSScopedFlag() { mFlag = false; }
    };

    // Tab order, left to right: Session, Engagements, Moment, Sweep, Compare, Raw.
    S32 ss_tab_index_for_level(S8 level)
    {
        switch (level)
        {
        case LEVEL_SESSION:    return 0;
        case LEVEL_ENGAGEMENT: return 1;
        case LEVEL_MOMENT:     return 2;
        case LEVEL_SWEEP:      return 3;
        case LEVEL_COMPARISON: return 4;
        case LEVEL_RAW:        return 5;
        default:               return 0;
        }
    }

    // The inverse of ss_tab_index_for_level, for the tab container's commit callback.
    S8 ss_level_for_tab_index(S32 index)
    {
        static const S8 order[6] =
            { LEVEL_SESSION, LEVEL_ENGAGEMENT, LEVEL_MOMENT, LEVEL_SWEEP, LEVEL_COMPARISON, LEVEL_RAW };
        return order[llclamp(index, 0, 5)];
    }

    // Short kind word for a list column.
    std::string ss_kind_word(U8 kind)
    {
        switch (kind)
        {
        case EVENT_DAMAGE:       return "damage";
        case EVENT_DEATH:        return "death";
        case EVENT_OBJECT_DEATH: return "obj death";
        default:                 return "other";
        }
    }

    // Trust word for the Raw level.
    std::string ss_trust_word(U8 trust)
    {
        switch (trust)
        {
        case TRUST_SIM:       return "simulator";
        case TRUST_FOREIGN:   return "foreign";
        default:              return "synthetic";
        }
    }

    // NounRef for one event, typed by its kind.
    NounRef ss_event_ref(const Event& ev)
    {
        NounRef ref;
        ref.mType = (ev.mKind == EVENT_DAMAGE) ? NOUN_DAMAGE : NOUN_DEATH;
        ref.mIndex = ev.mId;
        ref.mTime = ev.mTime;
        return ref;
    }
}

// Loads the deterministic scripted raid; live replays it in real time through ingest.
void ss_combat_log_load_synthetic(const LLSD& live)
{
    SSCombatLog::instance().loadSynthetic(1, live.asBoolean());
    LLFloaterReg::showInstance("ss_combat_log");
}

// Floater shell; everything is wired in postBuild.
SSFloaterCombatLog::SSFloaterCombatLog(const LLSD& key)
:   LLFloater(key),
    mEventKindFilter("all")
{
}

// Connections are scoped, so nothing else has to be undone.
SSFloaterCombatLog::~SSFloaterCombatLog()
{
}

// Finds every widget, wires the callbacks and subscribes to the store's two signals.
bool SSFloaterCombatLog::postBuild()
{
    SSFloaterCombatPage::registerUrlEntry();

    mStatusText = getChild<LLTextBox>("status_text");
    mSpanText = getChild<LLTextBox>("span_text");
    mNotAvailableText = getChild<LLTextBox>("not_available_text");
    mTimeText = getChild<LLTextBox>("time_text");
    mHeadlineText = getChild<LLTextBox>("headline_text");
    mRegionText = getChild<LLTextBox>("region_text");
    mEngagementHeader = getChild<LLTextBox>("engagement_header");
    mEvidenceText = getChild<LLTextBox>("evidence_text");
    mMomentSummary = getChild<LLTextEditor>("moment_summary");
    mRawTextView = getChild<LLTextEditor>("raw_text");

    mRibbon = getChild<SSRibbonView>("ribbon");
    mMap = getChild<SSMapView>("engagement_map");

    mTeamList = getChild<LLScrollListCtrl>("team_list");
    mEventList = getChild<LLScrollListCtrl>("event_list");
    mEngagementList = getChild<LLScrollListCtrl>("engagement_list");
    mMomentList = getChild<LLScrollListCtrl>("moment_list");
    mRawList = getChild<LLScrollListCtrl>("raw_list");

    mStageTabs = getChild<LLTabContainer>("stage_tabs");
    mStageTabs->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitStageTab(); });

    getChild<LLUICtrl>("load_mock_button")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { ss_combat_log_load_synthetic(LLSD(false)); });
    getChild<LLUICtrl>("replay_mock_button")->setCommitCallback(
        [](LLUICtrl*, const LLSD&) { ss_combat_log_load_synthetic(LLSD(true)); });

    getChild<LLUICtrl>("window_slider")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitWindow(); });
    getChild<LLUICtrl>("live_check")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitLive(); });
    getChild<LLUICtrl>("follow_check")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitFollow(); });
    getChild<LLUICtrl>("speed_combo")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitSpeed(); });

    getChild<LLUICtrl>("rewind_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRewind(); });
    getChild<LLUICtrl>("step_back_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickStepBack(); });
    getChild<LLUICtrl>("play_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickPlayPause(); });
    getChild<LLUICtrl>("step_forward_button")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickStepForward(); });
    getChild<LLUICtrl>("forward_button_end")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickForwardEnd(); });

    getChild<LLUICtrl>("kind_filter")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitEventFilter(); });
    getChild<LLUICtrl>("team_filter")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitEventFilter(); });

    mTeamList->setCommitOnSelectionChange(true);
    mTeamList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectTeamRow(); });
    mEventList->setCommitOnSelectionChange(true);
    mEventList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectEvent(); });
    mEngagementList->setCommitOnSelectionChange(true);
    mEngagementList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectEngagementRow(); });
    mMomentList->setCommitOnSelectionChange(true);
    mMomentList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectMomentRow(); });
    mRawList->setCommitOnSelectionChange(true);
    mRawList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectRawRow(); });

    SSCombatLog& log = SSCombatLog::instance();
    mDataConnection = log.dataChangedSignal().connect([this]() { onDataChanged(); });
    mViewConnection = log.viewChangedSignal().connect([this]() { onViewChanged(); });

    getChild<LLUICtrl>("window_slider")->setValue(LLSD((F64)log.view().mWindow));
    getChild<LLUICtrl>("live_check")->setValue(LLSD(log.view().mLive));
    getChild<LLUICtrl>("follow_check")->setValue(LLSD(mFollowCursor));
    getChild<LLUICtrl>("speed_combo")->setValue(LLSD("1.0"));
    getChild<LLUICtrl>("kind_filter")->setValue(LLSD("all"));

    mListsDirty = true;
    refreshChrome();
    refreshStage();
    refreshTransport();
    return true;
}

// Nothing special the first time; the stage tabs make the level self-explanatory now.
void SSFloaterCombatLog::onOpen(const LLSD& key)
{
    mListsDirty = true;
    refreshChrome();
    refreshStage();
    refreshTransport();
}

// Marks the lists stale; the rebuild happens once in draw, not once per ingested line.
void SSFloaterCombatLog::onDataChanged()
{
    mListsDirty = true;
}

// The framing moved: redraw the cheap chrome, and rebuild a list only when the level actually changed.
void SSFloaterCombatLog::onViewChanged()
{
    if (mUpdating) return;

    const S8 level = SSCombatLog::instance().view().mLevel;

    refreshChrome();
    refreshTransport();

    if (level != mDrawnLevel) refreshStage();

    if (mRibbon) mRibbon->dirtyRect();
    if (mMap) mMap->dirtyRect();
}

// Per-frame: rebuilds stale lists once, and keeps the animated views out of the dirty-rect trap.
void SSFloaterCombatLog::draw()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const F64 cursor = log.view().mCursor;

    if (mListsDirty)
    {
        mListsDirty = false;
        refreshChrome();
        if (log.events().size() != mDrawnEvents)
        {
            mDrawnEvents = log.events().size();
            refreshStage();
        }
    }

    if (std::fabs(cursor - mDrawnCursor) > 0.02)
    {
        mDrawnCursor = cursor;
        refreshTransport();
        if (mRibbon) mRibbon->dirtyRect();
        if (mMap) mMap->dirtyRect();
        if (log.view().mLevel == LEVEL_MOMENT) rebuildMoment();
    }

    LLFloater::draw();
}

// The level keys live here and nowhere else, and only while this floater has focus.
bool SSFloaterCombatLog::handleKeyHere(KEY key, MASK mask)
{
    SSCombatLog& log = SSCombatLog::instance();

    if (key == KEY_ESCAPE && log.view().mReconstruct)
    {
        log.leaveReconstruction();
        return true;
    }
    if (mask == MASK_NONE && key == ']')
    {
        if (mStageTabs) mStageTabs->selectNextTab();
        return true;
    }
    if (mask == MASK_NONE && key == '[')
    {
        if (mStageTabs) mStageTabs->selectPrevTab();
        return true;
    }
    if (mask == MASK_NONE && key == ' ')
    {
        onClickPlayPause();
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}

// ----- chrome -----

// Title status line, session span, evidence column and the "not available" strip.
void SSFloaterCombatLog::refreshChrome()
{
    const SSCombatLog& log = SSCombatLog::instance();

    S32 deaths = 0;
    for (const Event& ev : log.events())
    {
        if (ev.mKind == EVENT_DEATH || ev.mKind == EVENT_OBJECT_DEATH) ++deaths;
    }

    const std::vector<LLUUID> combatants = log.combatants();
    const std::string source = log.isSynthetic() ? "synthetic raid"
                                                 : (log.isEnabled() ? "bridge" : "bridge off");

    mStatusText->setValue(llformat("Combat Log - %s - %s - %d combatants - %d present - %d deaths - %d log lines",
                                   log.isRecording() ? "RECORDING" : "idle",
                                   source.c_str(),
                                   (S32)combatants.size(),
                                   (S32)log.tracks().size(),
                                   deaths,
                                   (S32)log.rawLines().size()));

    const F64 start = log.sessionStart();
    const F64 end = log.sessionEnd();
    const NounRef& subject = log.view().mSubject;

    mSpanText->setValue(llformat("session %s - cursor %s - window %.0f s - level %s - about %s",
                                 SSCombatUI::durationText(end - start).c_str(),
                                 SSCombatUI::clockText(log.view().mCursor).c_str(),
                                 log.view().mWindow,
                                 SSCombatUI::levelName(log.view().mLevel).c_str(),
                                 subject.valid() ? subject.label().c_str() : "the whole session"));

    std::string missing;
    if (log.rawLines().empty()) missing += " log lines";
    if (log.tracks().empty()) missing += " combatant tracks";
    if (log.engagements().empty()) missing += " engagements";
    if (!log.regionSettings().mKnown) missing += " region settings";
    if (log.teamCount() <= 0) missing += " teams";
    mNotAvailableText->setValue(missing.empty() ? std::string("not available: nothing")
                                                : std::string("not available:") + missing);

    S32 samples = 0;
    for (const auto& entry : log.tracks()) samples += (S32)entry.second.mSamples.size();

    mEvidenceText->setValue(llformat(
        "EVIDENCE\n\nWhat this is built from:\n\n%d log lines\n%d typed events\n%d tracks, %d samples\n"
        "%d equipment records\n%d projectile flights\n%d engagements\n%d teams\n\nRegion settings: %s\n"
        "Source: %s\n\nEvery count on the stage prints over its denominator; a bare number is the one that\n"
        "ends up quoted in a report.",
        (S32)log.rawLines().size(), (S32)log.events().size(), (S32)log.tracks().size(), samples,
        (S32)log.equipment().size(), (S32)log.flights().size(), (S32)log.engagements().size(),
        log.teamCount(), log.regionSettings().mKnown ? "reported" : "not reported", source.c_str()));
}

// Selects the tab matching the current level, guarded so its own commit callback does not call setLevel
// again, then refreshes only that panel's contents.
void SSFloaterCombatLog::refreshStage()
{
    const S8 level = SSCombatLog::instance().view().mLevel;

    if (mStageTabs)
    {
        const S32 index = ss_tab_index_for_level(level);
        if (mStageTabs->getCurrentPanelIndex() != index)
        {
            mSyncingTab = true;
            mStageTabs->selectTab(index);
            mSyncingTab = false;
        }
    }
    mDrawnLevel = level;

    switch (level)
    {
    case LEVEL_RAW:        rebuildRaw(); break;
    case LEVEL_MOMENT:     rebuildMoment(); break;
    case LEVEL_ENGAGEMENT: rebuildEngagement(); break;
    case LEVEL_SESSION:    rebuildSession(); break;
    default:               break;
    }
}

// Transport readout and the states of live, speed and follow.
void SSFloaterCombatLog::refreshTransport()
{
    const SSCombatLog& log = SSCombatLog::instance();

    mTimeText->setValue(llformat("%s  of  %s   %s%s",
                                 SSCombatUI::clockText(log.view().mCursor).c_str(),
                                 SSCombatUI::clockText(log.sessionEnd()).c_str(),
                                 log.view().mLive ? "LIVE" : "held",
                                 log.view().mReconstruct ? "  reconstruction (Esc leaves)" : ""));

    getChild<LLButton>("play_button")->setLabel(std::string(log.view().mPlaying ? "Pause" : "Play"));
    getChild<LLUICtrl>("live_check")->setValue(LLSD(log.view().mLive));
    getChild<LLUICtrl>("window_slider")->setValue(LLSD((F64)log.view().mWindow));
}

// ----- stage panels -----

// Session: headline counts, teams, the region's own combat settings and the filtered event list.
void SSFloaterCombatLog::rebuildSession()
{
    SSScopedFlag guard(mUpdating);
    const SSCombatLog& log = SSCombatLog::instance();

    const std::vector<LLUUID> combatants = log.combatants();
    const S32 teams = llmax(0, log.teamCount());

    std::vector<S32> members((size_t)teams + 1, 0);
    std::vector<S32> militia((size_t)teams + 1, 0);
    std::vector<S32> kills((size_t)teams + 1, 0);
    std::vector<S32> teamDeaths((size_t)teams + 1, 0);

    for (const LLUUID& id : combatants)
    {
        const TeamAssignment ta = log.team(id);
        const size_t slot = (ta.mTeam < 0 || ta.mTeam >= teams) ? (size_t)teams : (size_t)ta.mTeam;
        ++members[slot];
        if (ta.mConfidence != CONF_FIRM) ++militia[slot];
    }

    S32 deaths = 0, attributed = 0;
    for (const Event& ev : log.events())
    {
        if (ev.mKind != EVENT_DEATH && ev.mKind != EVENT_OBJECT_DEATH) continue;
        ++deaths;

        const Attribution attr = log.attribution(ev.mId);
        if (attr.mBlow != 0) ++attributed;

        const S8 victimTeam = SSCombatUI::teamOf(ev.mTarget);
        const size_t victimSlot = (victimTeam < 0 || victimTeam >= teams) ? (size_t)teams : (size_t)victimTeam;
        ++teamDeaths[victimSlot];

        const S8 killerTeam = SSCombatUI::teamOf(ev.mOwner);
        const size_t killerSlot = (killerTeam < 0 || killerTeam >= teams) ? (size_t)teams : (size_t)killerTeam;
        ++kills[killerSlot];
    }

    std::string sides;
    for (S32 t = 0; t < teams; ++t)
    {
        sides += llformat("%s%s %d combatants (%d without a group tag)",
                          sides.empty() ? "" : ", ",
                          log.teamName((S8)t).c_str(), members[(size_t)t], militia[(size_t)t]);
    }
    if (members[(size_t)teams] > 0)
    {
        sides += llformat("%sside unresolved %d", sides.empty() ? "" : ", ", members[(size_t)teams]);
    }

    const S32 present = (S32)log.tracks().size();
    const S32 civilians = llmax(0, present - (S32)combatants.size());

    mHeadlineText->setValue(llformat(
        "%d present - %d combatants, %d civilians.  %s\n"
        "Deaths %d, attributed %d of %d.  Engagements %d.  Session %s.",
        present,
        (S32)combatants.size(), civilians, sides.c_str(),
        deaths, attributed, deaths, (S32)log.engagements().size(),
        SSCombatUI::durationText(log.sessionEnd() - log.sessionStart()).c_str()));

    // Teams box.
    mTeamList->deleteAllItems();
    for (S32 t = 0; t <= teams; ++t)
    {
        if (t == teams && members[(size_t)t] == 0) continue;

        NounRef ref;
        ref.mType = NOUN_TEAM;
        ref.mIndex = (U32)t;

        LLSD row;
        row["id"] = SSCombatUI::nounAddress(ref);
        row["columns"][0]["column"] = "team";
        row["columns"][0]["value"] = (t == teams) ? std::string("unresolved") : log.teamName((S8)t);
        row["columns"][1]["column"] = "members";
        row["columns"][1]["value"] = llformat("%d", members[(size_t)t]);
        row["columns"][2]["column"] = "kills";
        row["columns"][2]["value"] = llformat("%d", kills[(size_t)t]);
        row["columns"][3]["column"] = "deaths";
        row["columns"][3]["value"] = llformat("%d", teamDeaths[(size_t)t]);
        mTeamList->addElement(row);
    }

    // Region settings, read from the sim rather than inferred.
    const RegionSettings& rs = log.regionSettings();
    if (!rs.mKnown)
    {
        mRegionText->setValue(std::string(
            "REGION\nnot reported yet. The bridge reports llGetEnv on handshake and on every region change;\n"
            "until then every reading that depends on these values prints its own doubt."));
    }
    else
    {
        static const char* deathActions[4] = { "home", "parcel landing", "telehub", "no action" };
        mRegionText->setValue(llformat(
            "REGION\ndamage adjust %s - foreign messages %s\nthrottle %.0f - limit %.0f - restore health %s\n"
            "regen %.3f/s - invulnerable %.1f s\ndeath action: %s (%d) - agent limit %d",
            rs.mAllowDamageAdjust ? "on" : "off",
            rs.mRestrictCombatLog ? "BLOCKED" : "allowed",
            rs.mDamageThrottle, rs.mDamageLimit,
            rs.mRestoreHealth ? "on" : "off",
            rs.mHealthRegenRate, rs.mInvulnerabilityTime,
            deathActions[llclamp(rs.mDeathAction, 0, 3)], rs.mDeathAction, rs.mAgentLimit));
    }

    // Team filter items, rebuilt only when the team set itself changed.
    if (teams != mFilterTeams)
    {
        mFilterTeams = teams;
        LLComboBox* teamFilter = getChild<LLComboBox>("team_filter");
        teamFilter->removeall();
        teamFilter->add(std::string("All teams"), LLSD((S32)-2));
        for (S32 t = 0; t < teams; ++t) teamFilter->add(log.teamName((S8)t), LLSD(t));
        teamFilter->add(std::string("side unresolved"), LLSD((S32)-1));
        teamFilter->setValue(LLSD(mEventTeamFilter));
    }

    // Events.
    mEventList->deleteAllItems();
    S32 rows = 0;
    for (const Event& ev : log.events())
    {
        if (rows >= MAX_ROWS) break;

        if (mEventKindFilter == "damage" && ev.mKind != EVENT_DAMAGE) continue;
        if (mEventKindFilter == "death" && ev.mKind != EVENT_DEATH) continue;
        if (mEventKindFilter == "object" && ev.mKind != EVENT_OBJECT_DEATH) continue;
        if (mEventKindFilter == "custom" && ev.mKind != EVENT_CUSTOM) continue;
        if (mEventTeamFilter != -2 && SSCombatUI::teamOf(ev.mOwner) != (S8)mEventTeamFilter) continue;

        LLSD row;
        row["id"] = SSCombatUI::nounAddress(ss_event_ref(ev));
        row["columns"][0]["column"] = "time";
        row["columns"][0]["value"] = SSCombatUI::clockText(ev.mTime);
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"] = ss_kind_word(ev.mKind);
        row["columns"][2]["column"] = "attacker";
        row["columns"][2]["value"] = SSCombatUI::nameOf(ev.mOwner);
        row["columns"][3]["column"] = "target";
        row["columns"][3]["value"] = SSCombatUI::nameOf(ev.mTarget);
        row["columns"][4]["column"] = "family";
        row["columns"][4]["value"] = SSCombatUI::familyOf(ev.mSource, ev.mRezzer);
        row["columns"][5]["column"] = "damage";
        row["columns"][5]["value"] = llformat("%.1f", ev.mDamage);
        row["columns"][6]["column"] = "type";
        row["columns"][6]["value"] = llformat("%d %s", (S32)ev.mType, SSCombatUI::deliveryWord(ev.mDelivery).c_str());
        mEventList->addElement(row);
        ++rows;
    }
    if (rows == 0) mEventList->addCommentText(std::string("no events match this filter"));
}

// Engagement: the Timeline face beside the Map face, both about the same window.
void SSFloaterCombatLog::rebuildEngagement()
{
    SSScopedFlag guard(mUpdating);
    const SSCombatLog& log = SSCombatLog::instance();
    const std::vector<Engagement>& list = log.engagements();

    S32 index = -1;
    const NounRef& subject = log.view().mSubject;
    if (subject.mType == NOUN_ENGAGEMENT)
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].mId == subject.mIndex) { index = (S32)i; break; }
        }
    }
    if (index < 0) index = SSCombatUI::engagementAt(log.view().mCursor);
    if (mMap) mMap->setEngagement(index);

    mEngagementList->deleteAllItems();

    if (index < 0)
    {
        mEngagementHeader->setValue(std::string(
            "No engagement at the cursor. Zoom in from Session, or double-click an engagement band on the ribbon."));
        return;
    }

    const Engagement& eng = list[(size_t)index];

    F32 damage = 0.f;
    S32 deaths = 0;
    for (U32 id : eng.mEvents)
    {
        const Event* ev = log.event(id);
        if (!ev) continue;
        damage += ev->mDamage;
        if (ev->mKind == EVENT_DEATH || ev->mKind == EVENT_OBJECT_DEATH) ++deaths;
    }

    mEngagementHeader->setValue(llformat("%s   %s - %s   %d combatants   %d deaths   %.0f damage   radius %.0f m",
                                         eng.mLabel.empty() ? llformat("Engagement %d", (S32)eng.mId).c_str()
                                                            : eng.mLabel.c_str(),
                                         SSCombatUI::shortClockText(eng.mStart).c_str(),
                                         SSCombatUI::shortClockText(eng.mEnd).c_str(),
                                         (S32)eng.mCombatants.size(), deaths, damage, eng.mRadius));

    S32 rows = 0;
    for (U32 id : eng.mEvents)
    {
        if (rows >= MAX_ROWS) break;
        const Event* ev = log.event(id);
        if (!ev) continue;

        LLSD row;
        row["id"] = SSCombatUI::nounAddress(ss_event_ref(*ev));
        row["columns"][0]["column"] = "time";
        row["columns"][0]["value"] = SSCombatUI::clockText(ev->mTime);
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"] = ss_kind_word(ev->mKind);
        row["columns"][2]["column"] = "attacker";
        row["columns"][2]["value"] = SSCombatUI::nameOf(ev->mOwner);
        row["columns"][3]["column"] = "target";
        row["columns"][3]["value"] = SSCombatUI::nameOf(ev->mTarget);
        mEngagementList->addElement(row);
        ++rows;
    }
    if (rows == 0) mEngagementList->addCommentText(std::string("no events in this engagement"));
}

// Moment: everything within one second of the cursor, plus who is standing where.
void SSFloaterCombatLog::rebuildMoment()
{
    SSScopedFlag guard(mUpdating);
    const SSCombatLog& log = SSCombatLog::instance();
    const F64 cursor = log.view().mCursor;

    mMomentList->deleteAllItems();

    S32 rows = 0;
    for (const Event& ev : log.events())
    {
        if (ev.mTime < cursor - 1.0) continue;
        if (ev.mTime > cursor + 1.0) break;

        LLSD row;
        row["id"] = SSCombatUI::nounAddress(ss_event_ref(ev));
        row["columns"][0]["column"] = "time";
        row["columns"][0]["value"] = SSCombatUI::clockText(ev.mTime);
        row["columns"][1]["column"] = "kind";
        row["columns"][1]["value"] = ss_kind_word(ev.mKind);
        row["columns"][2]["column"] = "attacker";
        row["columns"][2]["value"] = SSCombatUI::nameOf(ev.mOwner);
        row["columns"][3]["column"] = "target";
        row["columns"][3]["value"] = SSCombatUI::nameOf(ev.mTarget);
        row["columns"][4]["column"] = "damage";
        row["columns"][4]["value"] = llformat("%.1f", ev.mDamage);
        row["columns"][5]["column"] = "delivery";
        row["columns"][5]["value"] = SSCombatUI::deliveryWord(ev.mDelivery);
        mMomentList->addElement(row);
        ++rows;
    }
    if (rows == 0) mMomentList->addCommentText(std::string("nothing within one second of the cursor"));

    std::string summary = llformat("MOMENT %s (cursor plus or minus one second)\n\n",
                                   SSCombatUI::clockText(cursor).c_str());

    S32 placed = 0;
    for (const auto& entry : log.tracks())
    {
        Sample sample;
        if (!log.sampleAt(entry.first, cursor, sample)) continue;

        const S8 team = SSCombatUI::teamOf(entry.first);
        summary += llformat("%s (%s%s) at %.0f, %.0f, %.0f%s%s\n",
                            SSCombatUI::nameOf(entry.first).c_str(),
                            team < 0 ? "side unresolved" : log.teamName(team).c_str(),
                            log.isCombatantAt(entry.first, cursor) ? ", combatant" : ", civilian",
                            sample.mPos.mV[VX], sample.mPos.mV[VY], sample.mPos.mV[VZ],
                            (sample.mFlags & FLAG_MOUSELOOK) ? ", mouselook" : "",
                            (sample.mFlags & FLAG_SITTING) ? ", seated" : "");
        ++placed;
    }
    if (placed == 0) summary += "no position samples cover this moment.\n";

    mMomentSummary->setValue(summary);
}

// Raw: the log lines exactly as they arrived, and the text of the selected one.
void SSFloaterCombatLog::rebuildRaw()
{
    SSScopedFlag guard(mUpdating);
    const SSCombatLog& log = SSCombatLog::instance();
    const std::vector<RawLine>& lines = log.rawLines();

    mRawList->deleteAllItems();

    const size_t first = (lines.size() > (size_t)MAX_ROWS) ? lines.size() - (size_t)MAX_ROWS : 0;
    for (size_t i = first; i < lines.size(); ++i)
    {
        const RawLine& line = lines[i];

        NounRef ref;
        ref.mType = NOUN_LOGLINE;
        ref.mIndex = (U32)i;

        LLSD row;
        row["id"] = SSCombatUI::nounAddress(ref);
        row["columns"][0]["column"] = "received";
        row["columns"][0]["value"] = SSCombatUI::clockText(line.mReceived);
        row["columns"][1]["column"] = "frame";
        row["columns"][1]["value"] = line.mFrame ? llformat("%u", line.mFrame) : std::string("-");
        row["columns"][2]["column"] = "trust";
        row["columns"][2]["value"] = ss_trust_word(line.mTrust);
        row["columns"][3]["column"] = "salvage";
        row["columns"][3]["value"] = line.mSalvageMask ? llformat("0x%02x", (U32)line.mSalvageMask)
                                                       : std::string("none");
        row["columns"][4]["column"] = "events";
        row["columns"][4]["value"] = llformat("%u", line.mEventCount);
        mRawList->addElement(row);
    }
    if (lines.empty()) mRawList->addCommentText(std::string("no log lines received yet"));

    mRawTextView->setValue(std::string("select a log line to read it exactly as it arrived"));
}

// ----- callbacks -----

// The officer picked a tab directly; maps it onto the level unless we are the ones who just moved it.
void SSFloaterCombatLog::onCommitStageTab()
{
    if (mSyncingTab || !mStageTabs) return;
    SSCombatLog::instance().setLevel(ss_level_for_tab_index(mStageTabs->getCurrentPanelIndex()));
}

// Window seconds, the one place the trail length is set.
void SSFloaterCombatLog::onCommitWindow()
{
    SSCombatLog& log = SSCombatLog::instance();
    log.view().mWindow = (F32)getChild<LLUICtrl>("window_slider")->getValue().asReal();
    log.notifyViewChanged();
}

// Live follows the newest data; holding it parks the cursor.
void SSFloaterCombatLog::onCommitLive()
{
    SSCombatLog& log = SSCombatLog::instance();
    log.view().mLive = getChild<LLUICtrl>("live_check")->getValue().asBoolean();
    if (log.view().mLive) log.view().mCursor = log.sessionEnd();
    log.notifyViewChanged();
}

// Playback speed.
void SSFloaterCombatLog::onCommitSpeed()
{
    SSCombatLog& log = SSCombatLog::instance();
    const F32 speed = (F32)getChild<LLUICtrl>("speed_combo")->getValue().asReal();
    log.view().mSpeed = (speed > 0.f) ? speed : 1.f;
    log.notifyViewChanged();
}

// Whether the stage and the overlay chase the cursor.
void SSFloaterCombatLog::onCommitFollow()
{
    mFollowCursor = getChild<LLUICtrl>("follow_check")->getValue().asBoolean();
}

// Back to the start of the session.
void SSFloaterCombatLog::onClickRewind()
{
    SSCombatLog& log = SSCombatLog::instance();
    log.view().mLive = false;
    log.view().mCursor = log.sessionStart();
    log.notifyViewChanged();
}

// Back one event.
void SSFloaterCombatLog::onClickStepBack()
{
    stepEvent(-1);
}

// Play or pause.
void SSFloaterCombatLog::onClickPlayPause()
{
    SSCombatLog& log = SSCombatLog::instance();
    log.view().mPlaying = !log.view().mPlaying;
    if (log.view().mPlaying) log.view().mLive = false;
    log.notifyViewChanged();
}

// Forward one event.
void SSFloaterCombatLog::onClickStepForward()
{
    stepEvent(1);
}

// Forward to the newest data.
void SSFloaterCombatLog::onClickForwardEnd()
{
    SSCombatLog& log = SSCombatLog::instance();
    log.view().mCursor = log.sessionEnd();
    log.notifyViewChanged();
}

// Moves the cursor to the neighbouring event in one direction.
void SSFloaterCombatLog::stepEvent(S32 direction)
{
    SSCombatLog& log = SSCombatLog::instance();
    const std::vector<Event>& events = log.events();
    if (events.empty()) return;

    const F64 cursor = log.view().mCursor;
    F64 target = cursor;

    if (direction > 0)
    {
        for (const Event& ev : events)
        {
            if (ev.mTime > cursor + 0.001) { target = ev.mTime; break; }
        }
    }
    else
    {
        for (size_t i = events.size(); i > 0; --i)
        {
            if (events[i - 1].mTime < cursor - 0.001) { target = events[i - 1].mTime; break; }
        }
    }

    log.view().mLive = false;
    log.view().mCursor = target;
    log.notifyViewChanged();
}

// Selects the noun whose address is the selected row's id.
void SSFloaterCombatLog::selectFromRow(LLScrollListCtrl* list, bool moveCursor)
{
    if (!list || mUpdating) return;
    const LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    const std::string address = item->getValue().asString();
    if (address.empty()) return;

    const NounRef ref = NounRef::parse(address);
    if (!ref.valid()) return;

    SSCombatLog::instance().select(ref, moveCursor);
}

// Session event row: select the noun and move the cursor to it.
void SSFloaterCombatLog::onSelectEvent()
{
    selectFromRow(mEventList, true);
}

// Timeline row inside an engagement.
void SSFloaterCombatLog::onSelectEngagementRow()
{
    selectFromRow(mEngagementList, true);
}

// Moment row; the cursor is already there, so this only changes the selection.
void SSFloaterCombatLog::onSelectMomentRow()
{
    selectFromRow(mMomentList, false);
}

// Team row opens the team as the selection.
void SSFloaterCombatLog::onSelectTeamRow()
{
    selectFromRow(mTeamList, false);
}

// Raw row: select the log line and show its verbatim text.
void SSFloaterCombatLog::onSelectRawRow()
{
    if (mUpdating) return;

    const SSCombatLog& log = SSCombatLog::instance();
    const LLScrollListItem* item = mRawList->getFirstSelected();
    if (!item)
    {
        mRawTextView->setValue(std::string("select a log line to read it exactly as it arrived"));
        return;
    }

    const NounRef ref = NounRef::parse(item->getValue().asString());
    if (ref.mType != NOUN_LOGLINE || ref.mIndex >= log.rawLines().size())
    {
        mRawTextView->setValue(std::string("that log line is no longer held"));
        return;
    }

    const RawLine& line = log.rawLines()[ref.mIndex];
    const std::string text(log.rawText(line));

    mRawTextView->setValue(llformat("received %s   frame %u   %s   salvage 0x%02x   %u events\n\n%s",
                                    SSCombatUI::clockText(line.mReceived).c_str(),
                                    line.mFrame, ss_trust_word(line.mTrust).c_str(),
                                    (U32)line.mSalvageMask, line.mEventCount, text.c_str()));

    SSCombatLog::instance().select(ref, false);
}

// Event kind and team filters for the Session list.
void SSFloaterCombatLog::onCommitEventFilter()
{
    mEventKindFilter = getChild<LLUICtrl>("kind_filter")->getValue().asString();
    mEventTeamFilter = getChild<LLUICtrl>("team_filter")->getValue().asInteger();
    rebuildSession();
}
