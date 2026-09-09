/**
 * @file sscombatviews.cpp
 * @brief See sscombatviews.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatviews.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>

using namespace SSCombat;

namespace
{
    const F64 BUCKET_SECONDS = 10.0;
    const S32 RAIL_ROWS = 6;

    const LLColor4 COL_PANEL(0.11f, 0.12f, 0.15f, 1.f);
    const LLColor4 COL_CELL(0.15f, 0.16f, 0.20f, 1.f);
    const LLColor4 COL_CELL_ON(0.24f, 0.28f, 0.36f, 1.f);
    const LLColor4 COL_EDGE(0.33f, 0.35f, 0.42f, 1.f);
    const LLColor4 COL_TEXT(0.88f, 0.89f, 0.92f, 1.f);
    const LLColor4 COL_DIM(0.60f, 0.62f, 0.68f, 1.f);
    const LLColor4 COL_GREY(0.40f, 0.41f, 0.45f, 1.f);
    const LLColor4 COL_CURSOR(1.f, 0.85f, 0.30f, 1.f);
    const LLColor4 COL_BAND(0.22f, 0.30f, 0.38f, 1.f);
    const LLColor4 COL_QUALITY(0.30f, 0.62f, 0.42f, 1.f);
    const LLColor4 COL_QUALITY_LOW(0.55f, 0.42f, 0.22f, 1.f);

    // The one small font every custom Combat Log view draws with.
    LLFontGL* ss_font()
    {
        return LLFontGL::getFontSansSerifSmall();
    }

    // True when a noun type is what a level is naturally about (used to find a back-anchor in the noun chain).
    bool ss_natural_at(S8 level, U8 type)
    {
        switch (level)
        {
        case LEVEL_RAW:        return type == NOUN_LOGLINE;
        case LEVEL_MOMENT:     return type == NOUN_MOMENT || type == NOUN_DEATH || type == NOUN_DAMAGE;
        case LEVEL_ENGAGEMENT: return type == NOUN_ENGAGEMENT || type == NOUN_LIFE;
        case LEVEL_SWEEP:      return type == NOUN_SIGHTLINE || type == NOUN_CLAIM;
        case LEVEL_COMPARISON: return type == NOUN_TEAM || type == NOUN_COMBATANT || type == NOUN_FAMILY;
        case LEVEL_SESSION:    return type == NOUN_SESSION;
        default:               return false;
        }
    }

    // The death event nearest a time, or nullptr.
    const Event* ss_nearest_death(F64 t)
    {
        const SSCombatLog& log = SSCombatLog::instance();
        const Event* best = NULL;
        F64 bestGap = 1e30;
        for (const Event& ev : log.events())
        {
            if (ev.mKind != EVENT_DEATH) continue;
            const F64 gap = std::fabs(ev.mTime - t);
            if (gap < bestGap) { bestGap = gap; best = &ev; }
        }
        return best;
    }

    // Draws text clipped to a width, appending "..." when it does not fit.
    void ss_draw_text(const std::string& text, S32 x, S32 y, S32 maxWidth, const LLColor4& color,
                      LLFontGL::VAlign valign = LLFontGL::BASELINE)
    {
        if (maxWidth <= 8) return;
        ss_font()->renderUTF8(text, 0, (F32)x, (F32)y, color, LLFontGL::LEFT, valign,
                              LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, maxWidth, NULL, true);
    }
}

// ----- SSCombatUI helpers -----

// Plain name of a level; never "rung" and never a number.
std::string SSCombatUI::levelName(S8 level)
{
    switch (level)
    {
    case LEVEL_RAW:        return "Raw";
    case LEVEL_MOMENT:     return "Moment";
    case LEVEL_ENGAGEMENT: return "Engagement";
    case LEVEL_SWEEP:      return "Sweep";
    case LEVEL_COMPARISON: return "Comparison";
    case LEVEL_SESSION:    return "Session";
    default:               return "Session";
    }
}

// One step out, toward the abstract.
S8 SSCombatUI::levelOut(S8 level)
{
    return (S8)llmin((S32)level + 1, (S32)LEVEL_SESSION);
}

// One step in, toward the concrete.
S8 SSCombatUI::levelIn(S8 level)
{
    return (S8)llmax((S32)level - 1, (S32)LEVEL_RAW);
}

// Start of the drawn session span.
F64 SSCombatUI::spanStart()
{
    return SSCombatLog::instance().sessionStart();
}

// End of the drawn session span; follows now while live.
F64 SSCombatUI::spanEnd()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const F64 start = log.sessionStart();
    const F64 end = log.sessionEnd();
    return (end > start + 1.0) ? end : start + 1.0;
}

// Index of the engagement containing t, or -1.
S32 SSCombatUI::engagementAt(F64 t)
{
    const std::vector<Engagement>& list = SSCombatLog::instance().engagements();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (t >= list[i].mStart && t <= list[i].mEnd) return (S32)i;
    }
    return -1;
}

// The instance a zoom-in would land on: the view chain's anchor when one exists, else the example.
bool SSCombatUI::landingFor(S8 level, NounRef& out, bool& back)
{
    const SSCombatLog& log = SSCombatLog::instance();

    const std::vector<NounRef>& chain = log.nounChain();
    for (size_t i = chain.size(); i > 0; --i)
    {
        if (ss_natural_at(level, chain[i - 1].mType))
        {
            out = chain[i - 1];
            back = true;
            return true;
        }
    }

    back = false;
    const F64 cursor = log.view().mCursor;

    switch (level)
    {
    case LEVEL_RAW:
    {
        if (log.rawLines().empty()) return false;
        U32 line = (U32)(log.rawLines().size() - 1);
        const NounRef& sel = log.view().mSelection;
        if (sel.mType == NOUN_DEATH || sel.mType == NOUN_DAMAGE)
        {
            if (const Event* ev = log.event(sel.mIndex)) line = ev->mRawLine;
        }
        out = NounRef();
        out.mType = NOUN_LOGLINE;
        out.mIndex = line;
        return true;
    }
    case LEVEL_MOMENT:
    {
        out = NounRef();
        out.mType = NOUN_MOMENT;
        out.mTime = cursor;
        if (const Event* ev = ss_nearest_death(cursor))
        {
            out.mType = NOUN_DEATH;
            out.mIndex = ev->mId;
            out.mTime = ev->mTime;
        }
        return true;
    }
    case LEVEL_ENGAGEMENT:
    {
        const std::vector<Engagement>& list = log.engagements();
        if (list.empty()) return false;
        S32 pick = engagementAt(cursor);
        if (pick < 0)
        {
            size_t best = 0;
            for (size_t i = 1; i < list.size(); ++i)
            {
                if (list[i].mEvents.size() > list[best].mEvents.size()) best = i;
            }
            pick = (S32)best;
        }
        out = NounRef();
        out.mType = NOUN_ENGAGEMENT;
        out.mIndex = list[pick].mId;
        out.mTime = list[pick].mStart;
        return true;
    }
    case LEVEL_SWEEP:
    {
        const Event* ev = ss_nearest_death(cursor);
        if (!ev) return false;
        out = NounRef();
        out.mType = NOUN_SIGHTLINE;
        out.mIndex = ev->mId;
        out.mTime = ev->mTime;
        return true;
    }
    case LEVEL_COMPARISON:
    {
        if (log.teamCount() <= 0) return false;
        out = NounRef();
        out.mType = NOUN_TEAM;
        out.mIndex = 0;
        return true;
    }
    default:
        out = NounRef();
        out.mType = NOUN_SESSION;
        return true;
    }
}

// The rail's landing line, or the sentence saying what would define the level.
std::string SSCombatUI::landingText(S8 level)
{
    NounRef ref;
    bool back = false;
    if (!landingFor(level, ref, back))
    {
        switch (level)
        {
        case LEVEL_RAW:        return "not available: no log lines received yet";
        case LEVEL_ENGAGEMENT: return "not available: no engagement found yet";
        case LEVEL_SWEEP:      return "not available: no death to sweep yet";
        case LEVEL_COMPARISON: return "not available: no teams resolved yet";
        default:               return "not available";
        }
    }
    return "lands on: " + ref.label() + (back ? "  (back)" : "  (example)");
}

// Session-relative clock with tenths.
std::string SSCombatUI::clockText(F64 t)
{
    F64 rel = t - SSCombatLog::instance().sessionStart();
    if (rel < 0.0) rel = 0.0;
    const S32 minutes = (S32)(rel / 60.0);
    const F64 seconds = rel - minutes * 60.0;
    return llformat("%02d:%04.1f", minutes, seconds);
}

// Session-relative clock without tenths.
std::string SSCombatUI::shortClockText(F64 t)
{
    F64 rel = t - SSCombatLog::instance().sessionStart();
    if (rel < 0.0) rel = 0.0;
    const S32 minutes = (S32)(rel / 60.0);
    const S32 seconds = (S32)(rel - minutes * 60.0);
    return llformat("%02d:%02d", minutes, seconds);
}

// Duration in the officer's own shorthand.
std::string SSCombatUI::durationText(F64 seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const S32 total = (S32)seconds;
    const S32 hours = total / 3600;
    const S32 mins = (total % 3600) / 60;
    const S32 secs = total % 60;
    if (hours > 0) return llformat("%dh%02dm", hours, mins);
    if (mins > 0)  return llformat("%dm%02ds", mins, secs);
    return llformat("%ds", secs);
}

// The tool's own address string.
std::string SSCombatUI::nounAddress(const NounRef& ref)
{
    return ref.address();
}

// The clickable form of an address, matched by SSUrlEntryCombat.
std::string SSCombatUI::nounSlurl(const NounRef& ref)
{
    std::string address = ref.address();
    if (address.compare(0, 3, "ss:") == 0) address = address.substr(3);
    return "secondlife:///app/sscombat/" + address;
}

// Team colour with a readable fallback for unassigned.
LLColor4 SSCombatUI::teamTint(S8 team)
{
    if (team < 0) return LLColor4(0.62f, 0.62f, 0.66f, 1.f);
    return SSCombatLog::instance().teamColor(team);
}

// Team of a combatant, -1 when unassigned.
S8 SSCombatUI::teamOf(const LLUUID& id)
{
    return SSCombatLog::instance().team(id).mTeam;
}

// Display name, falling back to a short key.
std::string SSCombatUI::nameOf(const LLUUID& id)
{
    if (id.isNull()) return "unknown";
    const std::string name = SSCombatLog::instance().displayName(id);
    if (!name.empty()) return name;
    return id.asString().substr(0, 8);
}

// The one confidence ladder, four words and never a fifth.
std::string SSCombatUI::confidenceWord(U8 confidence)
{
    switch (confidence)
    {
    case CONF_FIRM:   return "FIRM";
    case CONF_LIKELY: return "LIKELY";
    case CONF_WEAK:   return "WEAK";
    default:          return "UNDETERMINED";
    }
}

// Delivery class word, the owner's own vocabulary.
std::string SSCombatUI::deliveryWord(U8 delivery)
{
    switch (delivery)
    {
    case DELIVERY_HITSCAN:    return "hitscan";
    case DELIVERY_PROJECTILE: return "projectile";
    case DELIVERY_LOBBED:     return "lobbed";
    case DELIVERY_AREA:       return "area";
    default:                  return "unknown";
    }
}

// Family of the equipment behind a hit, preferring the source object over its rezzer.
std::string SSCombatUI::familyOf(const LLUUID& source, const LLUUID& rezzer)
{
    const SSCombatLog& log = SSCombatLog::instance();
    if (const Equipment* eq = log.equipmentFor(source))
    {
        if (!eq->mFamily.empty()) return eq->mFamily;
        if (!eq->mName.empty()) return eq->mName;
    }
    if (const Equipment* eq = log.equipmentFor(rezzer))
    {
        if (!eq->mFamily.empty()) return eq->mFamily;
        if (!eq->mName.empty()) return eq->mName;
    }
    return std::string();
}

// ----- SSLevelRail -----

static LLDefaultChildRegistry::Register<SSLevelRail> r_ss_level_rail("ss_level_rail");
static LLDefaultChildRegistry::Register<SSRibbonView> r_ss_ribbon_view("ss_ribbon_view");
static LLDefaultChildRegistry::Register<SSMapView> r_ss_map_view("ss_map_view");

// Level rail control; all state lives in the shared View.
SSLevelRail::SSLevelRail(const Params& p)
:   LLUICtrl(p)
{
}

// Level held by a row, abstract at top and ground at bottom.
S8 SSLevelRail::levelForRow(S32 row)
{
    static const S8 order[RAIL_ROWS] =
        { LEVEL_SESSION, LEVEL_COMPARISON, LEVEL_SWEEP, LEVEL_ENGAGEMENT, LEVEL_MOMENT, LEVEL_RAW };
    return order[llclamp(row, 0, RAIL_ROWS - 1)];
}

// Even sixth of the control, top down.
LLRect SSLevelRail::cellRect(S32 row) const
{
    const LLRect& r = getLocalRect();
    const S32 h = r.getHeight() / RAIL_ROWS;
    const S32 top = r.mTop - row * h;
    return LLRect(r.mLeft, top, r.mRight, top - h);
}

// Row under a local y, or -1.
S32 SSLevelRail::rowAt(S32 y) const
{
    const LLRect& r = getLocalRect();
    const S32 h = r.getHeight() / RAIL_ROWS;
    if (h <= 0) return -1;
    const S32 row = (r.mTop - y) / h;
    return (row >= 0 && row < RAIL_ROWS) ? row : -1;
}

// Draws six cells with plain names, the current one highlighted, and the landing under it.
void SSLevelRail::draw()
{
    const LLRect& full = getLocalRect();
    gl_rect_2d(full, COL_PANEL, true);

    const SSCombatLog& log = SSCombatLog::instance();
    const S8 current = log.view().mLevel;
    const S8 landingLevel = SSCombatUI::levelIn(current);

    LLFontGL* font = ss_font();
    const S32 lineH = font->getLineHeight();

    for (S32 row = 0; row < RAIL_ROWS; ++row)
    {
        const S8 level = levelForRow(row);
        const LLRect cell = cellRect(row);
        const bool isCurrent = (level == current);

        gl_rect_2d(cell.mLeft + 1, cell.mTop - 1, cell.mRight - 1, cell.mBottom + 1,
                   isCurrent ? COL_CELL_ON : COL_CELL, true);
        gl_rect_2d(cell.mLeft + 1, cell.mTop - 1, cell.mRight - 1, cell.mBottom + 1, COL_EDGE, false);

        NounRef ref;
        bool back = false;
        const bool defined = SSCombatUI::landingFor(level, ref, back);

        const S32 textX = cell.mLeft + 6;
        const S32 textW = cell.getWidth() - 12;
        S32 y = cell.mTop - 4 - lineH;

        ss_draw_text(SSCombatUI::levelName(level), textX, y,
                     textW, defined ? (isCurrent ? COL_TEXT : COL_DIM) : COL_GREY);
        y -= lineH + 1;

        if (isCurrent && cell.getHeight() > 2 * lineH + 8)
        {
            const NounRef& subject = log.view().mSubject;
            const std::string sub = subject.valid() ? subject.label() : std::string("the whole session");
            ss_draw_text(sub, textX, y, textW, COL_DIM);
            y -= lineH + 1;
        }

        if (level == landingLevel && level != current && cell.getHeight() > 2 * lineH + 8)
        {
            ss_draw_text(SSCombatUI::landingText(level), textX, y, textW, COL_CURSOR);
        }
    }

    LLUICtrl::draw();
}

// Clicking a cell moves to that level.
bool SSLevelRail::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row < 0) return LLUICtrl::handleMouseDown(x, y, mask);

    SSCombatLog::instance().setLevel(levelForRow(row));
    return true;
}

// One sentence per level, so the vocabulary is learnable by hovering.
bool SSLevelRail::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 row = rowAt(y);
    if (row < 0) return LLUICtrl::handleToolTip(x, y, mask);

    std::string tip;
    switch (levelForRow(row))
    {
    case LEVEL_RAW:        tip = "Raw - the log lines behind this, exactly as they arrived."; break;
    case LEVEL_MOMENT:     tip = "Moment - everything within one second of the cursor."; break;
    case LEVEL_ENGAGEMENT: tip = "Engagement - one cluster of fighting, in time and in space."; break;
    case LEVEL_SWEEP:      tip = "Sweep - one reading run across one parameter."; break;
    case LEVEL_COMPARISON: tip = "Comparison - a set of combatants, teams or families side by side."; break;
    default:               tip = "Session - the whole raid: counts, teams, region settings and events."; break;
    }
    tip += "\n" + SSCombatUI::landingText(levelForRow(row));
    LLToolTipMgr::instance().show(tip);
    return true;
}

// ----- SSRibbonView -----

// Ribbon control; the spine that makes vertical movement legible.
SSRibbonView::SSRibbonView(const Params& p)
:   LLUICtrl(p)
{
}

// Lane rectangles, laid out top down; optional lanes take no room when their setting is off.
SSRibbonView::Lanes SSRibbonView::laneLayout() const
{
    const LLRect& r = getLocalRect();
    const bool escalation = gSavedSettings.getBOOL("SSCombatLogEscalationLane");
    const bool presence = gSavedSettings.getBOOL("SSCombatLogPresenceLane");

    Lanes lanes;
    S32 y = r.mTop - 1;

    lanes.mDeathsTop = y;
    y -= 10;
    lanes.mDeathsBottom = y;

    const S32 fixed = 8 + (escalation ? 18 : 0) + (presence ? 14 : 0) + 7;
    const S32 damageH = llmax(12, (y - r.mBottom) - fixed - 2);

    lanes.mDamageTop = y;
    y -= damageH;
    lanes.mDamageBottom = y;

    lanes.mBandsTop = y;
    y -= 8;
    lanes.mBandsBottom = y;

    if (escalation)
    {
        lanes.mEscalationTop = y;
        y -= 18;
        lanes.mEscalationBottom = y;
    }
    if (presence)
    {
        lanes.mPresenceTop = y;
        y -= 14;
        lanes.mPresenceBottom = y;
    }

    lanes.mQualityTop = y;
    y -= 7;
    lanes.mQualityBottom = llmax(r.mBottom, y);
    return lanes;
}

// Pixel x for a session time.
S32 SSRibbonView::timeToX(F64 t) const
{
    const LLRect& r = getLocalRect();
    const F64 start = SSCombatUI::spanStart();
    const F64 end = SSCombatUI::spanEnd();
    const F64 f = (t - start) / llmax(0.001, end - start);
    return r.mLeft + (S32)(llclamp(f, 0.0, 1.0) * (F64)(r.getWidth() - 1));
}

// Session time under a pixel x.
F64 SSRibbonView::xToTime(S32 x) const
{
    const LLRect& r = getLocalRect();
    const F64 start = SSCombatUI::spanStart();
    const F64 end = SSCombatUI::spanEnd();
    const F64 f = (F64)(x - r.mLeft) / (F64)llmax(1, r.getWidth() - 1);
    return start + llclamp(f, 0.0, 1.0) * (end - start);
}

// Per-team damage per 10 s bucket and per-bucket track coverage; rebuilt only when the data grew.
void SSRibbonView::rebuildBuckets()
{
    const SSCombatLog& log = SSCombatLog::instance();
    const F64 start = SSCombatUI::spanStart();
    const F64 end = SSCombatUI::spanEnd();

    if (log.events().size() == mBuiltEvents && std::fabs(end - mBuiltEnd) < BUCKET_SECONDS) return;
    mBuiltEvents = log.events().size();
    mBuiltEnd = end;

    const S32 buckets = llmax(1, (S32)((end - start) / BUCKET_SECONDS) + 1);
    const S32 teams = llmax(1, log.teamCount());

    mDamage.assign((size_t)teams + 1, std::vector<F32>((size_t)buckets, 0.f));
    mCoverage.assign((size_t)buckets, 0.f);
    mDamagePeak = 1.f;

    for (const Event& ev : log.events())
    {
        if (ev.mKind != EVENT_DAMAGE && ev.mKind != EVENT_DEATH) continue;
        const S32 b = llclamp((S32)((ev.mTime - start) / BUCKET_SECONDS), 0, buckets - 1);
        const S8 team = SSCombatUI::teamOf(ev.mOwner);
        const size_t slot = (team < 0 || team >= teams) ? (size_t)teams : (size_t)team;
        mDamage[slot][(size_t)b] += ev.mDamage;
    }

    for (S32 b = 0; b < buckets; ++b)
    {
        F32 total = 0.f;
        for (size_t s = 0; s < mDamage.size(); ++s) total += mDamage[s][(size_t)b];
        mDamagePeak = llmax(mDamagePeak, total);
    }

    const std::unordered_map<LLUUID, Track>& tracks = log.tracks();
    if (!tracks.empty())
    {
        std::vector<S32> seen((size_t)buckets, 0);
        for (const auto& entry : tracks)
        {
            std::vector<char> hit((size_t)buckets, 0);
            for (const Sample& s : entry.second.mSamples)
            {
                const S32 b = llclamp((S32)((F64)s.mTime / BUCKET_SECONDS), 0, buckets - 1);
                hit[(size_t)b] = 1;
            }
            for (S32 b = 0; b < buckets; ++b) seen[(size_t)b] += hit[(size_t)b];
        }
        const F32 denom = (F32)tracks.size();
        for (S32 b = 0; b < buckets; ++b) mCoverage[(size_t)b] = (F32)seen[(size_t)b] / denom;
    }
}

// Draws the constant strip: deaths, damage, engagements, the pref-gated lanes, quality and the cursor.
void SSRibbonView::draw()
{
    const LLRect& r = getLocalRect();
    gl_rect_2d(r, COL_PANEL, true);
    gl_rect_2d(r, COL_EDGE, false);

    rebuildBuckets();

    const SSCombatLog& log = SSCombatLog::instance();
    const Lanes lanes = laneLayout();
    const F64 start = SSCombatUI::spanStart();
    const F64 end = SSCombatUI::spanEnd();
    const S32 buckets = (S32)mCoverage.size();

    LLLocalClipRect clip(r);

    // Engagement bands.
    for (const Engagement& eng : log.engagements())
    {
        const S32 x0 = timeToX(eng.mStart);
        const S32 x1 = llmax(x0 + 1, timeToX(eng.mEnd));
        gl_rect_2d(x0, lanes.mBandsTop - 1, x1, lanes.mBandsBottom + 1, COL_BAND, true);
    }

    // Stacked per-team damage rate, one column per 10 s bucket.
    const S32 damageH = lanes.mDamageTop - lanes.mDamageBottom;
    if (buckets > 0 && damageH > 2)
    {
        const F32 colW = (F32)r.getWidth() / (F32)buckets;
        for (S32 b = 0; b < buckets; ++b)
        {
            S32 x0 = r.mLeft + (S32)(colW * b);
            S32 x1 = llmax(x0 + 1, r.mLeft + (S32)(colW * (b + 1)) - 1);
            S32 y = lanes.mDamageBottom;
            for (size_t s = 0; s < mDamage.size(); ++s)
            {
                const F32 value = mDamage[s][(size_t)b];
                if (value <= 0.f) continue;
                const S32 h = llmax(1, (S32)((value / mDamagePeak) * (F32)damageH));
                const S8 team = (s + 1 == mDamage.size()) ? (S8)-1 : (S8)s;
                gl_rect_2d(x0, llmin(lanes.mDamageTop, y + h), x1, y, SSCombatUI::teamTint(team), true);
                y += h;
            }
        }
    }

    // Death ticks, coloured by the victim's team.
    for (const Event& ev : log.events())
    {
        if (ev.mKind != EVENT_DEATH && ev.mKind != EVENT_OBJECT_DEATH) continue;
        const S32 x = timeToX(ev.mTime);
        gl_rect_2d(x, lanes.mDeathsTop, x + 1, lanes.mDeathsBottom,
                   SSCombatUI::teamTint(SSCombatUI::teamOf(ev.mTarget)), true);
    }

    // Escalation lane, drawn only when the officer switched it on.
    if (lanes.mEscalationTop != lanes.mEscalationBottom)
    {
        ss_draw_text("escalation lane: no classified first-use yet",
                     r.mLeft + 4, lanes.mEscalationBottom + 4, r.getWidth() - 8, COL_GREY);
        gl_line_2d(r.mLeft, lanes.mEscalationBottom, r.mRight, lanes.mEscalationBottom, COL_EDGE);
    }

    // Presence lane, drawn only when the officer switched it on.
    if (lanes.mPresenceTop != lanes.mPresenceBottom && buckets > 0)
    {
        const S32 h = lanes.mPresenceTop - lanes.mPresenceBottom;
        const F32 colW = (F32)r.getWidth() / (F32)buckets;
        for (S32 b = 0; b < buckets; ++b)
        {
            const S32 x0 = r.mLeft + (S32)(colW * b);
            const S32 x1 = llmax(x0 + 1, r.mLeft + (S32)(colW * (b + 1)) - 1);
            const S32 bar = llmax(1, (S32)(mCoverage[(size_t)b] * (F32)h));
            gl_rect_2d(x0, lanes.mPresenceBottom + bar, x1, lanes.mPresenceBottom, COL_DIM, true);
        }
    }

    // Track-quality row, hatched dark below 60 % coverage.
    if (buckets > 0)
    {
        const F32 colW = (F32)r.getWidth() / (F32)buckets;
        for (S32 b = 0; b < buckets; ++b)
        {
            const S32 x0 = r.mLeft + (S32)(colW * b);
            const S32 x1 = llmax(x0 + 1, r.mLeft + (S32)(colW * (b + 1)) - 1);
            const bool low = mCoverage[(size_t)b] < 0.6f;
            gl_rect_2d(x0, lanes.mQualityTop, x1, lanes.mQualityBottom,
                       low ? COL_QUALITY_LOW : COL_QUALITY, true);
        }
    }

    // Cursor line, full height.
    const S32 cx = timeToX(log.view().mCursor);
    gl_line_2d(cx, r.mTop, cx, r.mBottom, COL_CURSOR);

    if (end - start > 1.5)
    {
        ss_draw_text(SSCombatUI::shortClockText(start), r.mLeft + 3, r.mBottom + 1, 60, COL_GREY);
    }

    LLUICtrl::draw();
}

// Drag scrubs the cursor; the ribbon never changes level by dragging.
bool SSRibbonView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mDragging = true;
    gFocusMgr.setMouseCapture(this);
    scrubTo(x);
    return true;
}

// Continues a scrub while the mouse is held.
bool SSRibbonView::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging && hasMouseCapture())
    {
        scrubTo(x);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

// Ends a scrub.
bool SSRibbonView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(NULL);
        return true;
    }
    return LLUICtrl::handleMouseUp(x, y, mask);
}

// Double-clicking an engagement band selects it and moves to the Engagement level.
bool SSRibbonView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    const Lanes lanes = laneLayout();
    if (y > lanes.mBandsTop || y < lanes.mBandsBottom) return LLUICtrl::handleDoubleClick(x, y, mask);

    const F64 t = xToTime(x);
    const S32 index = SSCombatUI::engagementAt(t);
    if (index < 0) return true;

    const Engagement& eng = SSCombatLog::instance().engagements()[(size_t)index];
    NounRef ref;
    ref.mType = NOUN_ENGAGEMENT;
    ref.mIndex = eng.mId;
    ref.mTime = eng.mStart;

    SSCombatLog::instance().select(ref, true);
    SSCombatLog::instance().setLevel(LEVEL_ENGAGEMENT);
    return true;
}

// Names the lane under the pointer and the time under it.
bool SSRibbonView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const Lanes lanes = laneLayout();
    const F64 t = xToTime(x);
    std::string tip = SSCombatUI::shortClockText(t) + " - ";

    if (y <= lanes.mDeathsTop && y >= lanes.mDeathsBottom)          tip += "deaths, coloured by the victim's team";
    else if (y <= lanes.mDamageTop && y >= lanes.mDamageBottom)     tip += "damage rate per team, 10 second buckets";
    else if (y <= lanes.mBandsTop && y >= lanes.mBandsBottom)       tip += "engagements; double-click one to open it";
    else if (y <= lanes.mQualityTop && y >= lanes.mQualityBottom)   tip += "track coverage; dark below 60 percent";
    else                                                            tip += "drag to scrub";

    LLToolTipMgr::instance().show(tip);
    return true;
}

// Moves the shared cursor to the time under x and leaves live mode.
void SSRibbonView::scrubTo(S32 x)
{
    SSCombatLog& log = SSCombatLog::instance();
    log.view().mCursor = xToTime(x);
    log.view().mLive = false;
    log.notifyViewChanged();
    dirtyRect();
}

// ----- SSMapView -----

// Overhead renderer; the Map face's Stage 0 twin of the world overlay.
SSMapView::SSMapView(const Params& p)
:   LLUICtrl(p)
{
}

// Window and scale that fit the drawn positions into the rect.
bool SSMapView::computeBounds(F64& start, F64& end, F32& minX, F32& minY, F32& scale) const
{
    const SSCombatLog& log = SSCombatLog::instance();

    start = SSCombatUI::spanStart();
    end = SSCombatUI::spanEnd();
    if (mEngagement >= 0 && mEngagement < (S32)log.engagements().size())
    {
        const Engagement& eng = log.engagements()[(size_t)mEngagement];
        start = eng.mStart;
        end = eng.mEnd;
    }

    F32 maxX = -1e9f, maxY = -1e9f;
    minX = 1e9f;
    minY = 1e9f;
    bool any = false;

    const F64 base = log.sessionStart();
    for (const auto& entry : log.tracks())
    {
        for (const Sample& s : entry.second.mSamples)
        {
            const F64 t = base + (F64)s.mTime;
            if (t < start || t > end) continue;
            minX = llmin(minX, s.mPos.mV[VX]); maxX = llmax(maxX, s.mPos.mV[VX]);
            minY = llmin(minY, s.mPos.mV[VY]); maxY = llmax(maxY, s.mPos.mV[VY]);
            any = true;
        }
    }
    for (const Event& ev : log.events())
    {
        if (!ev.mHasPositions || ev.mTime < start || ev.mTime > end) continue;
        minX = llmin(minX, ev.mTargetPos.mV[VX]); maxX = llmax(maxX, ev.mTargetPos.mV[VX]);
        minY = llmin(minY, ev.mTargetPos.mV[VY]); maxY = llmax(maxY, ev.mTargetPos.mV[VY]);
        any = true;
    }
    if (!any) return false;

    const F32 spanX = llmax(8.f, maxX - minX);
    const F32 spanY = llmax(8.f, maxY - minY);
    const LLRect& r = getLocalRect();
    scale = llmin((F32)(r.getWidth() - 16) / spanX, (F32)(r.getHeight() - 16) / spanY);
    minX -= 8.f / llmax(0.001f, scale);
    minY -= 8.f / llmax(0.001f, scale);
    return true;
}

// Draws team-coloured trails, death crosses and the cursor positions, north up.
void SSMapView::draw()
{
    const LLRect& r = getLocalRect();
    gl_rect_2d(r, COL_PANEL, true);
    gl_rect_2d(r, COL_EDGE, false);

    mMarks.clear();

    F64 start = 0.0, end = 0.0;
    F32 minX = 0.f, minY = 0.f, scale = 1.f;
    if (!computeBounds(start, end, minX, minY, scale))
    {
        ss_draw_text("no positions in this window - tracks arrive with the bridge",
                     r.mLeft + 8, r.getCenterY(), r.getWidth() - 16, COL_GREY);
        LLUICtrl::draw();
        return;
    }

    const SSCombatLog& log = SSCombatLog::instance();
    const F64 base = log.sessionStart();
    LLLocalClipRect clip(r);

    // Trails, one polyline per track.
    for (const auto& entry : log.tracks())
    {
        const LLColor4 tint = SSCombatUI::teamTint(SSCombatUI::teamOf(entry.first));
        S32 lastX = 0, lastY = 0;
        bool have = false;
        for (const Sample& s : entry.second.mSamples)
        {
            const F64 t = base + (F64)s.mTime;
            if (t < start || t > end) { have = false; continue; }
            const S32 px = r.mLeft + (S32)((s.mPos.mV[VX] - minX) * scale);
            const S32 py = r.mBottom + (S32)((s.mPos.mV[VY] - minY) * scale);
            if (have) gl_line_2d(lastX, lastY, px, py, tint);
            lastX = px; lastY = py; have = true;
        }
        if (have)
        {
            gl_rect_2d(lastX - 2, lastY + 2, lastX + 2, lastY - 2, tint, true);
            Mark mark;
            mark.mRect = LLRect(lastX - 4, lastY + 4, lastX + 4, lastY - 4);
            mark.mRef.mType = NOUN_COMBATANT;
            mark.mRef.mId = entry.first;
            mark.mLabel = SSCombatUI::nameOf(entry.first);
            mMarks.push_back(mark);
        }
    }

    // Death crosses, measured positions only.
    for (const Event& ev : log.events())
    {
        if (ev.mKind != EVENT_DEATH && ev.mKind != EVENT_OBJECT_DEATH) continue;
        if (!ev.mHasPositions || ev.mTime < start || ev.mTime > end) continue;

        const S32 px = r.mLeft + (S32)((ev.mTargetPos.mV[VX] - minX) * scale);
        const S32 py = r.mBottom + (S32)((ev.mTargetPos.mV[VY] - minY) * scale);
        const LLColor4 tint = SSCombatUI::teamTint(SSCombatUI::teamOf(ev.mTarget));
        gl_line_2d(px - 4, py - 4, px + 4, py + 4, tint);
        gl_line_2d(px - 4, py + 4, px + 4, py - 4, tint);

        Mark mark;
        mark.mRect = LLRect(px - 5, py + 5, px + 5, py - 5);
        mark.mRef.mType = NOUN_DEATH;
        mark.mRef.mIndex = ev.mId;
        mark.mRef.mTime = ev.mTime;
        mark.mLabel = SSCombatUI::nameOf(ev.mTarget) + " <- " + SSCombatUI::nameOf(ev.mOwner);
        mMarks.push_back(mark);
    }

    // Where everyone is at the cursor.
    const F64 cursor = log.view().mCursor;
    if (cursor >= start && cursor <= end)
    {
        for (const auto& entry : log.tracks())
        {
            Sample s;
            if (!log.sampleAt(entry.first, cursor, s)) continue;
            const S32 px = r.mLeft + (S32)((s.mPos.mV[VX] - minX) * scale);
            const S32 py = r.mBottom + (S32)((s.mPos.mV[VY] - minY) * scale);
            gl_rect_2d(px - 3, py + 3, px + 3, py - 3, COL_CURSOR, false);
            ss_draw_text(SSCombatUI::nameOf(entry.first), px + 6, py - 4, 90, COL_DIM);
        }
    }

    ss_draw_text(llformat("north up - %.0f m across - %s to %s",
                          (F32)(r.getWidth() - 16) / llmax(0.001f, scale),
                          SSCombatUI::shortClockText(start).c_str(),
                          SSCombatUI::shortClockText(end).c_str()),
                 r.mLeft + 5, r.mBottom + 4, r.getWidth() - 10, COL_GREY);

    LLUICtrl::draw();
}

// Clicking a mark selects that noun and moves the cursor to it.
bool SSMapView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    for (size_t i = mMarks.size(); i > 0; --i)
    {
        const Mark& mark = mMarks[i - 1];
        if (!mark.mRect.pointInRect(x, y)) continue;
        SSCombatLog::instance().select(mark.mRef, mark.mRef.mTime > 0.0);
        return true;
    }
    return LLUICtrl::handleMouseDown(x, y, mask);
}

// Names the mark under the pointer.
bool SSMapView::handleToolTip(S32 x, S32 y, MASK mask)
{
    for (size_t i = mMarks.size(); i > 0; --i)
    {
        const Mark& mark = mMarks[i - 1];
        if (!mark.mRect.pointInRect(x, y)) continue;
        LLToolTipMgr::instance().show(mark.mLabel);
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}
