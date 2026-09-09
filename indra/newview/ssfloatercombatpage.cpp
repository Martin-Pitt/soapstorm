/**
 * @file ssfloatercombatpage.cpp
 * @brief See ssfloatercombatpage.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloatercombatpage.h"

#include "sscombatviews.h"

#include "llbutton.h"
#include "llcommandhandler.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llurlregistry.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <map>

using namespace SSCombat;

namespace
{
    // The tool's own address for a link URL, or an empty string when the URL is not one of ours.
    std::string ss_address_from_url(const std::string& url)
    {
        static const std::string key = "/app/sscombat/";
        const size_t pos = url.find(key);
        if (pos == std::string::npos) return std::string();
        return "ss:" + url.substr(pos + key.length());
    }

    // A linked mention of one noun, for use inside generated prose.
    std::string ss_link(const NounRef& ref)
    {
        if (!ref.valid()) return std::string("unknown");
        return SSCombatUI::nounSlurl(ref);
    }

    // Link to a combatant.
    std::string ss_combatant_link(const LLUUID& id)
    {
        NounRef ref;
        ref.mType = NOUN_COMBATANT;
        ref.mId = id;
        return ss_link(ref);
    }

    // Blow state in words, never a coined one.
    std::string ss_blow_word(U8 state)
    {
        switch (state)
        {
        case BLOW_UNAMBIGUOUS:    return "one killing blow, unambiguous";
        case BLOW_AMBIGUOUS_PAIR: return "two hits too close to order apart, credit split";
        default:                  return "killing blow not determined";
        }
    }

    // Metres between the two measured positions of an event, or -1 when it carries none.
    F32 ss_event_distance(const Event& ev)
    {
        if (!ev.mHasPositions) return -1.f;
        return (F32)(ev.mSourcePos - ev.mTargetPos).length();
    }
}

// ----- SSUrlEntryCombat -----

// Matches secondlife:///app/sscombat/<type>/<id>[@t] anywhere inside a text widget.
SSUrlEntryCombat::SSUrlEntryCombat()
{
    mPattern = boost::regex("(hop|secondlife):///app/sscombat/[a-z_]+/[^ \t\r\n]+",
                            boost::regex::perl | boost::regex::icase);
    mMenuName = "menu_url_slapp.xml";
    mTooltip = "Open this Combat Log page";
}

// Renders the noun's own label rather than the address.
std::string SSUrlEntryCombat::getLabel(const std::string& url, const LLUrlLabelCallback& cb)
{
    const std::string address = ss_address_from_url(url);
    if (address.empty()) return url;

    const NounRef ref = NounRef::parse(address);
    if (!ref.valid()) return address;
    return ref.label();
}

// Says where the link goes, and that event addresses do not travel between officers.
std::string SSUrlEntryCombat::getTooltip(const std::string& string) const
{
    const std::string address = ss_address_from_url(string);
    if (address.empty()) return mTooltip;
    return address + "\nSession-local address - it will not resolve for anyone else.";
}

// ----- command handler -----

namespace
{
    // Runs the click on an ss: link; opens the page floater keyed by the address.
    class SSCombatPageHandler : public LLCommandHandler
    {
    public:
        SSCombatPageHandler() : LLCommandHandler("sscombat", UNTRUSTED_THROTTLE) {}

        bool handle(const LLSD& params, const LLSD& query_map, const std::string& grid, LLMediaCtrl* web) override
        {
            if (params.size() < 2) return false;
            SSFloaterCombatPage::openAddress("ss:" + params[0].asString() + "/" + params[1].asString());
            return true;
        }
    };
    SSCombatPageHandler sSSCombatPageHandler;
}

// ----- SSFloaterCombatPage -----

// Page shell; the noun arrives with the key or from the shared selection.
SSFloaterCombatPage::SSFloaterCombatPage(const LLSD& key)
:   LLFloater(key)
{
    for (S32 i = 0; i < 6; ++i) mLevelButtons[i] = NULL;
}

// Registers the link type once, in front of the catch-all secondlife: patterns.
void SSFloaterCombatPage::registerUrlEntry()
{
    static bool registered = false;
    if (registered) return;
    registered = true;
    LLUrlRegistry::instance().registerUrl(new SSUrlEntryCombat(), true);
}

// Opens or raises the page for one address.
void SSFloaterCombatPage::openAddress(const std::string& address)
{
    if (address.empty()) return;
    LLFloaterReg::showInstance("ss_combat_page", LLSD(address));
}

// Finds the widgets and wires the level bar.
bool SSFloaterCombatPage::postBuild()
{
    registerUrlEntry();

    mHeaderText = getChild<LLTextBox>("header_text");
    mBodyText = getChild<LLTextEditor>("body_text");
    mRelatedList = getChild<LLScrollListCtrl>("related_list");
    mReconstruct = getChild<LLButton>("reconstruct_button");

    static const char* names[6] =
        { "level_raw", "level_moment", "level_engagement", "level_sweep", "level_comparison", "level_session" };
    static const S8 levels[6] =
        { LEVEL_RAW, LEVEL_MOMENT, LEVEL_ENGAGEMENT, LEVEL_SWEEP, LEVEL_COMPARISON, LEVEL_SESSION };

    for (S32 i = 0; i < 6; ++i)
    {
        mLevelButtons[i] = getChild<LLButton>(names[i]);
        const S8 level = levels[i];
        mLevelButtons[i]->setCommitCallback([this, level](LLUICtrl*, const LLSD&) { onClickLevel(level); });
    }

    mReconstruct->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReconstruct(); });
    mRelatedList->setCommitOnSelectionChange(true);
    mRelatedList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectRelated(); });

    refreshPage();
    return true;
}

// Takes the noun from the key, falling back to whatever the officer last clicked.
void SSFloaterCombatPage::onOpen(const LLSD& key)
{
    const std::string address = key.asString();
    mRef = address.empty() ? SSCombatLog::instance().view().mSelection : NounRef::parse(address);
    refreshPage();
}

// Header, level bar, body and related list, all from the current noun.
void SSFloaterCombatPage::refreshPage()
{
    if (!mHeaderText) return;

    mUpdating = true;
    refreshPageBody();
    mUpdating = false;
}

// The body of refreshPage, run under the re-entry guard.
void SSFloaterCombatPage::refreshPageBody()
{
    if (!mRef.valid())
    {
        setTitle(std::string("Page"));
        mHeaderText->setValue(std::string("Nothing selected. Click a row, a mark or a link to open a page."));
        mBodyText->setValue(std::string());
        mRelatedList->deleteAllItems();
        mReconstruct->setVisible(false);
        refreshLevelBar();
        return;
    }

    const std::string label = mRef.label();
    setTitle(label);

    std::string when;
    if (mRef.mTime > 0.0) when = "  -  " + SSCombatUI::clockText(mRef.mTime);
    mHeaderText->setValue(label + when + "\n" + SSCombatUI::nounAddress(mRef));

    mBodyText->setValue(buildBody());

    mRelatedList->deleteAllItems();
    mReconstruct->setVisible(mRef.mType == NOUN_DEATH);

    switch (mRef.mType)
    {
    case NOUN_COMBATANT:  buildCombatantRows(); break;
    case NOUN_DEATH:      buildDeathRows(); break;
    case NOUN_EQUIPMENT:
    case NOUN_FAMILY:     buildFamilyRows(); break;
    case NOUN_TEAM:       buildTeamRows(); break;
    case NOUN_ENGAGEMENT: buildEngagementRows(); break;
    default:
        setColumns("Item", "Value", "Detail", "More");
        addRow("no related rows for this noun yet", "", "", "");
        break;
    }

    refreshLevelBar();
}

// The level row rule: enabled only where the noun means something at that level.
void SSFloaterCombatPage::refreshLevelBar()
{
    const U8 type = mRef.mType;
    const bool isEvent = (type == NOUN_DEATH || type == NOUN_DAMAGE);
    const bool hasTime = isEvent || type == NOUN_MOMENT || type == NOUN_LOGLINE ||
                         type == NOUN_LIFE || type == NOUN_ENGAGEMENT;

    const bool enabled[6] =
    {
        /* Raw */        isEvent || type == NOUN_LOGLINE,
        /* Moment */     hasTime,
        /* Engagement */ hasTime || type == NOUN_COMBATANT || type == NOUN_TEAM,
        /* Sweep */      type == NOUN_DEATH || type == NOUN_SIGHTLINE || type == NOUN_COMBATANT ||
                         type == NOUN_TEAM || type == NOUN_FAMILY,
        /* Comparison */ type == NOUN_COMBATANT || type == NOUN_TEAM || type == NOUN_FAMILY ||
                         type == NOUN_EQUIPMENT,
        /* Session */    mRef.valid()
    };

    for (S32 i = 0; i < 6; ++i)
    {
        if (mLevelButtons[i]) mLevelButtons[i]->setEnabled(enabled[i]);
    }
}

// Prose first: what this noun is, in sentences, with every mention a link.
std::string SSFloaterCombatPage::buildBody() const
{
    const SSCombatLog& log = SSCombatLog::instance();
    std::string body;

    switch (mRef.mType)
    {
    case NOUN_COMBATANT:
    {
        const LLUUID id = mRef.mId;
        const TeamAssignment ta = log.team(id);
        const std::vector<Life>& lives = log.lives(id);

        S32 kills = 0, deaths = 0;
        F32 dealt = 0.f, taken = 0.f;
        for (const Event& ev : log.events())
        {
            if (ev.mOwner == id) { dealt += ev.mDamage; if (ev.mKind == EVENT_DEATH) ++kills; }
            if (ev.mTarget == id) { taken += ev.mDamage; if (ev.mKind == EVENT_DEATH) ++deaths; }
        }

        body += SSCombatUI::nameOf(id) + " fought on " +
                (ta.mTeam < 0 ? std::string("no team the tool could resolve")
                              : log.teamName(ta.mTeam) + " (" + SSCombatUI::confidenceWord(ta.mConfidence) + ")") +
                ".\n\n";
        body += llformat("%d lives, %d kills, %d deaths, %.0f damage dealt and %.0f taken.\n",
                         (S32)lives.size(), kills, deaths, dealt, taken);
        body += "Damage totals are raw log damage, not health - health is not observable.\n\n";
        if (ta.mTeam >= 0)
        {
            NounRef teamRef;
            teamRef.mType = NOUN_TEAM;
            teamRef.mIndex = (U32)ta.mTeam;
            body += "Team: " + ss_link(teamRef) + "\n";
        }
        break;
    }
    case NOUN_DEATH:
    case NOUN_DAMAGE:
    {
        const Event* ev = log.event(mRef.mIndex);
        if (!ev) { body = "That event is no longer held."; break; }

        const F32 distance = ss_event_distance(*ev);
        const std::string family = SSCombatUI::familyOf(ev->mSource, ev->mRezzer);

        body += SSCombatUI::nameOf(ev->mTarget) +
                (ev->mKind == EVENT_DAMAGE ? " was hit by " : " was killed by ") +
                SSCombatUI::nameOf(ev->mOwner) + " at " + SSCombatUI::clockText(ev->mTime);
        if (distance >= 0.f) body += llformat(", %.1f m away", distance);
        if (!family.empty()) body += ", with " + family;
        body += llformat(" (%s, damage type %d).\n", SSCombatUI::deliveryWord(ev->mDelivery).c_str(), (S32)ev->mType);

        if (ev->mInitial > 0.f && std::fabs(ev->mInitial - ev->mDamage) > 0.01f)
        {
            body += llformat("The hit landed for %.1f, adjusted from %.1f by %u adjustment script(s).\n",
                             ev->mDamage, ev->mInitial, ev->mModsCount);
        }
        else
        {
            body += llformat("The hit landed for %.1f, unadjusted.\n", ev->mDamage);
        }

        body += llformat("Timestamp uncertainty %.2f s; source %s.\n",
                         ev->mUncertainty,
                         ev->mTrust == TRUST_SIM ? "simulator" :
                         (ev->mTrust == TRUST_FOREIGN ? "foreign script" : "synthetic"));

        if (ev->mKind == EVENT_DEATH)
        {
            const Attribution attr = log.attribution(ev->mId);
            body += "Attribution: " + ss_blow_word(attr.mBlowState) +
                    llformat(", window %.0f s, %d contributor(s).\n", attr.mWindow, (S32)attr.mShares.size());
        }

        body += "\nAttacker: " + ss_combatant_link(ev->mOwner) + "\n";
        body += "Target: " + ss_combatant_link(ev->mTarget) + "\n";

        NounRef lineRef;
        lineRef.mType = NOUN_LOGLINE;
        lineRef.mIndex = ev->mRawLine;
        body += "Log line: " + ss_link(lineRef) + "\n";
        break;
    }
    case NOUN_TEAM:
    {
        const S8 team = (S8)mRef.mIndex;
        S32 members = 0;
        for (const LLUUID& id : log.combatants())
        {
            if (SSCombatUI::teamOf(id) == team) ++members;
        }

        F32 outward = 0.f, inward = 0.f;
        for (const Event& ev : log.events())
        {
            if (SSCombatUI::teamOf(ev.mOwner) != team) continue;
            if (SSCombatUI::teamOf(ev.mTarget) == team) inward += ev.mDamage;
            else outward += ev.mDamage;
        }

        body += log.teamName(team) + llformat(" - %d combatants.\n\n", members);
        if (gSavedSettings.getBOOL("SSCombatLogShowStrain"))
        {
            const F32 total = outward + inward;
            body += llformat("internal fire %d%% (%.0f of %.0f damage landed on its own members)\n",
                             total > 0.f ? (S32)((inward / total) * 100.f + 0.5f) : 0, inward, total);
            body += "Friendly fire has ordinary causes; this is a count, not a finding.\n";
        }
        break;
    }
    case NOUN_EQUIPMENT:
    case NOUN_FAMILY:
    {
        body += mRef.label() + "\n\nAn equipment family is the maker prefix plus the name stem, version stripped.\n"
                "Hits, kills, users and the distance-at-kill spread are in the table below.\n";
        break;
    }
    case NOUN_ENGAGEMENT:
    {
        const std::vector<Engagement>& list = log.engagements();
        for (const Engagement& eng : list)
        {
            if (eng.mId != mRef.mIndex) continue;
            body += llformat("%s ran from %s to %s, %d combatants, %d events, radius %.0f m.\n",
                             eng.mLabel.empty() ? "This engagement" : eng.mLabel.c_str(),
                             SSCombatUI::shortClockText(eng.mStart).c_str(),
                             SSCombatUI::shortClockText(eng.mEnd).c_str(),
                             (S32)eng.mCombatants.size(), (S32)eng.mEvents.size(), eng.mRadius);
            body += "\nCombatants:\n";
            for (const LLUUID& id : eng.mCombatants) body += "  " + ss_combatant_link(id) + "\n";
            break;
        }
        if (body.empty()) body = "That engagement is no longer held.";
        break;
    }
    case NOUN_LOGLINE:
    {
        if (mRef.mIndex >= log.rawLines().size()) { body = "That log line is no longer held."; break; }
        const RawLine& line = log.rawLines()[mRef.mIndex];
        body += llformat("received %s, simulator frame %u, %u event(s), salvage mask 0x%02x.\n\n",
                         SSCombatUI::clockText(line.mReceived).c_str(), line.mFrame,
                         line.mEventCount, (U32)line.mSalvageMask);
        body += std::string(log.rawText(line));
        body += "\n";
        break;
    }
    case NOUN_SESSION:
    default:
        body += "Session " + SSCombatUI::durationText(log.sessionEnd() - log.sessionStart()) +
                llformat(", %d events, %d log lines, %d engagements.\n",
                         (S32)log.events().size(), (S32)log.rawLines().size(), (S32)log.engagements().size());
        break;
    }

    return body;
}

// ----- related lists -----

// Renames the four generic columns for the noun on show.
void SSFloaterCombatPage::setColumns(const std::string& a, const std::string& b,
                                     const std::string& c, const std::string& d)
{
    mRelatedList->setColumnLabel("col_a", a);
    mRelatedList->setColumnLabel("col_b", b);
    mRelatedList->setColumnLabel("col_c", c);
    mRelatedList->setColumnLabel("col_d", d);
}

// One row; an address makes the row a link to another page.
void SSFloaterCombatPage::addRow(const std::string& a, const std::string& b, const std::string& c,
                                 const std::string& d, const std::string& address)
{
    LLSD row;
    row["id"] = address;
    row["columns"][0]["column"] = "col_a";
    row["columns"][0]["value"] = a;
    row["columns"][1]["column"] = "col_b";
    row["columns"][1]["value"] = b;
    row["columns"][2]["column"] = "col_c";
    row["columns"][2]["value"] = c;
    row["columns"][3]["column"] = "col_d";
    row["columns"][3]["value"] = d;
    mRelatedList->addElement(row);
}

// Lives, kills, deaths, the equipment families used, and the streak gaps between them.
void SSFloaterCombatPage::buildCombatantRows()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const LLUUID id = mRef.mId;

    setColumns("Row", "When", "Value", "Detail");

    const std::vector<Life>& lives = log.lives(id);
    F64 longestLife = 0.0;
    for (size_t i = 0; i < lives.size(); ++i)
    {
        const Life& life = lives[i];
        const F64 length = llmax(0.0, life.mEnd - life.mStart);
        longestLife = llmax(longestLife, length);

        NounRef ref;
        ref.mType = NOUN_LIFE;
        ref.mId = id;
        ref.mIndex = (U32)i;
        ref.mTime = life.mStart;

        addRow(llformat("life %d", (S32)i + 1),
               SSCombatUI::shortClockText(life.mStart),
               SSCombatUI::durationText(length),
               life.mDeathEvent ? (life.mTeleportSeen ? "ended in a death, teleport seen"
                                                      : "ended in a death, no teleport seen")
                                : "still running",
               SSCombatUI::nounAddress(ref));
    }

    std::vector<F64> killTimes, deathTimes;
    std::map<std::string, std::pair<S32, S32> > families;   // family -> (hits, kills)

    for (const Event& ev : log.events())
    {
        if (ev.mOwner == id)
        {
            const std::string family = SSCombatUI::familyOf(ev.mSource, ev.mRezzer);
            std::pair<S32, S32>& counts = families[family.empty() ? std::string("UNATTRIBUTED") : family];
            ++counts.first;
            if (ev.mKind == EVENT_DEATH) { ++counts.second; killTimes.push_back(ev.mTime); }
        }
        if (ev.mTarget == id && ev.mKind == EVENT_DEATH) deathTimes.push_back(ev.mTime);
    }

    for (const auto& entry : families)
    {
        addRow("family: " + entry.first, "", llformat("%d hits", entry.second.first),
               llformat("%d kills", entry.second.second));
    }

    F64 longestKillGap = 0.0;
    for (size_t i = 1; i < killTimes.size(); ++i)
    {
        longestKillGap = llmax(longestKillGap, killTimes[i] - killTimes[i - 1]);
    }
    F64 longestDeathGap = 0.0;
    for (size_t i = 1; i < deathTimes.size(); ++i)
    {
        longestDeathGap = llmax(longestDeathGap, deathTimes[i] - deathTimes[i - 1]);
    }

    addRow("kills", "", llformat("%d", (S32)killTimes.size()),
           killTimes.size() > 1 ? "longest gap between kills " + SSCombatUI::durationText(longestKillGap)
                                : "not enough kills to have a gap");
    addRow("deaths", "", llformat("%d", (S32)deathTimes.size()),
           deathTimes.size() > 1 ? "longest gap between deaths " + SSCombatUI::durationText(longestDeathGap)
                                 : "not enough deaths to have a gap");
    addRow("longest life", "", SSCombatUI::durationText(longestLife),
           llformat("over %d lives", (S32)lives.size()));
}

// Attribution shares and the blow state; the Reconstruct button sits under them.
void SSFloaterCombatPage::buildDeathRows()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const Event* death = log.event(mRef.mIndex);
    if (!death) return;

    setColumns("Contributor", "Damage", "Credit", "Detail");

    const Attribution attr = log.attribution(death->mId);
    for (const VolleyShare& share : attr.mShares)
    {
        NounRef ref;
        ref.mType = NOUN_COMBATANT;
        ref.mId = share.mAttacker;

        addRow(SSCombatUI::nameOf(share.mAttacker),
               llformat("%.1f", share.mDamage),
               llformat("%.0f%%", share.mCredit * 100.f),
               SSCombatUI::familyOf(share.mRezzer, share.mRezzer),
               SSCombatUI::nounAddress(ref));
    }
    if (attr.mShares.empty()) addRow("no attributable damage in the window", "", "", "");

    addRow("blow state", "", ss_blow_word(attr.mBlowState),
           llformat("window %.0f s", attr.mWindow));
}

// Hits, kills, users and the distance at kill, wherever positions exist.
void SSFloaterCombatPage::buildFamilyRows()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const std::string family = mRef.label();

    setColumns("Row", "Value", "Detail", "More");

    S32 hits = 0, kills = 0, withPositions = 0;
    F32 distanceSum = 0.f, distanceMin = 1e9f, distanceMax = 0.f;
    std::map<LLUUID, S32> users;

    for (const Event& ev : log.events())
    {
        const std::string evFamily = SSCombatUI::familyOf(ev.mSource, ev.mRezzer);
        if (evFamily.empty() || evFamily != family) continue;

        ++hits;
        ++users[ev.mOwner];
        if (ev.mKind != EVENT_DEATH) continue;
        ++kills;

        const F32 distance = ss_event_distance(ev);
        if (distance < 0.f) continue;
        ++withPositions;
        distanceSum += distance;
        distanceMin = llmin(distanceMin, distance);
        distanceMax = llmax(distanceMax, distance);
    }

    addRow("hits", llformat("%d", hits), "", "");
    addRow("kills", llformat("%d", kills), "", "");
    addRow("users", llformat("%d", (S32)users.size()), "", "");

    if (withPositions > 0)
    {
        addRow("distance at kill", llformat("%.0f m mean", distanceSum / (F32)withPositions),
               llformat("%.0f - %.0f m", distanceMin, distanceMax),
               llformat("measured on %d of %d kills", withPositions, kills));
    }
    else
    {
        addRow("distance at kill", "not available", "no death carried positions", "");
    }

    for (const auto& entry : users)
    {
        NounRef ref;
        ref.mType = NOUN_COMBATANT;
        ref.mId = entry.first;
        addRow("user: " + SSCombatUI::nameOf(entry.first), llformat("%d hits", entry.second), "", "",
               SSCombatUI::nounAddress(ref));
    }
}

// Members with their kills and deaths; internal fire only when the officer switched that on.
void SSFloaterCombatPage::buildTeamRows()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const S8 team = (S8)mRef.mIndex;

    setColumns("Member", "Kills", "Deaths", "Confidence");

    for (const LLUUID& id : log.combatants())
    {
        const TeamAssignment ta = log.team(id);
        if (ta.mTeam != team) continue;

        S32 kills = 0, deaths = 0;
        for (const Event& ev : log.events())
        {
            if (ev.mKind != EVENT_DEATH) continue;
            if (ev.mOwner == id) ++kills;
            if (ev.mTarget == id) ++deaths;
        }

        NounRef ref;
        ref.mType = NOUN_COMBATANT;
        ref.mId = id;
        addRow(SSCombatUI::nameOf(id), llformat("%d", kills), llformat("%d", deaths),
               SSCombatUI::confidenceWord(ta.mConfidence), SSCombatUI::nounAddress(ref));
    }
}

// The events inside an engagement, as a plain timeline.
void SSFloaterCombatPage::buildEngagementRows()
{
    const SSCombatLog& log = SSCombatLog::instance();

    setColumns("Time", "Kind", "Attacker", "Target");

    for (const Engagement& eng : log.engagements())
    {
        if (eng.mId != mRef.mIndex) continue;
        for (U32 id : eng.mEvents)
        {
            const Event* ev = log.event(id);
            if (!ev) continue;

            NounRef ref;
            ref.mType = (ev->mKind == EVENT_DAMAGE) ? NOUN_DAMAGE : NOUN_DEATH;
            ref.mIndex = ev->mId;
            ref.mTime = ev->mTime;

            addRow(SSCombatUI::clockText(ev->mTime),
                   ev->mKind == EVENT_DAMAGE ? "damage" : "death",
                   SSCombatUI::nameOf(ev->mOwner),
                   SSCombatUI::nameOf(ev->mTarget),
                   SSCombatUI::nounAddress(ref));
        }
        break;
    }
}

// ----- callbacks -----

// A level button moves the shared view and raises the main floater on it.
void SSFloaterCombatPage::onClickLevel(S8 level)
{
    if (!mRef.valid()) return;

    SSCombatLog& log = SSCombatLog::instance();
    log.select(mRef, mRef.mTime > 0.0);
    log.setLevel(level);
    LLFloaterReg::showInstance("ss_combat_log");
}

// Watches this death happen again, ghosted, where it happened.
void SSFloaterCombatPage::onClickReconstruct()
{
    if (mRef.mType != NOUN_DEATH) return;
    SSCombatLog::instance().enterReconstruction(mRef.mIndex);
}

// A related row with an address opens that page.
void SSFloaterCombatPage::onSelectRelated()
{
    if (mUpdating) return;

    const LLScrollListItem* item = mRelatedList->getFirstSelected();
    if (!item) return;

    const std::string address = item->getValue().asString();
    if (address.empty()) return;
    openAddress(address);
}
