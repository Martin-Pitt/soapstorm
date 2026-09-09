/**
 * @file sscombatfeedline.h
 * @brief The one chat-log-style line for a Combat 2.0 event, shared by the Events floater's list and the
 *        Combatant floater's "Recent events" pane so the two never drift apart. Design: doc/combat_log_ux.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATFEEDLINE_H
#define SS_COMBATFEEDLINE_H

#include "sscombatlog.h"
#include "sscombaticons.h"

#include "llformat.h"
#include "llstring.h"

#include <cmath>
#include <string>
#include <string_view>

namespace SSCombat
{
    // How close an adjusted damage value has to be to its initial value before we call it unadjusted.
    constexpr F32 FEED_ADJUST_EPSILON = 0.05f;

    // mm:ss since the session started, clamped to zero so a slightly-early sample never prints a minus sign.
    inline std::string sessionClock(F64 t)
    {
        F64 rel = t - SSCombatLog::instance().sessionStart();
        if (rel < 0.0)
        {
            rel = 0.0;
        }
        const S32 total = (S32)(rel + 0.0005);
        return llformat("%02d:%02d", total / 60, total % 60);
    }

    // NounRef for one event, typed by its kind; DEATH covers DEATH/OBJECT_DEATH/CUSTOM alike since there is no
    // separate noun type for them (matches SSCombatLog::select()'s expectations for a session-local event).
    inline NounRef eventRef(const Event& ev)
    {
        NounRef ref;
        ref.mType = (ev.mKind == EVENT_DAMAGE) ? NOUN_DAMAGE : NOUN_DEATH;
        ref.mIndex = ev.mId;
        ref.mTime = ev.mTime;
        return ref;
    }

    // UI image name for an event's row icon; CUSTOM events get none.
    inline std::string kindIcon(const Event& ev)
    {
        switch (ev.mKind)
        {
        case EVENT_DAMAGE:       return SSCombatIcons::forType(ev.mType);
        case EVENT_DEATH:        return SSCombatIcons::forType(SSCombatIcons::ICON_DEATH);
        case EVENT_OBJECT_DEATH: return SSCombatIcons::forType(SSCombatIcons::ICON_OBJECT_DEATH);
        default:                 return std::string();
        }
    }

    // Family name of the rezzer's or source's equipment, "unknown" when neither is known.
    inline std::string weaponFamilyFor(const Event& ev)
    {
        const SSCombatLog& log = SSCombatLog::instance();
        if (const Equipment* eq = log.equipmentFor(ev.mRezzer); eq && !eq->mFamily.empty())
        {
            return eq->mFamily;
        }
        if (const Equipment* eq = log.equipmentFor(ev.mSource); eq && !eq->mFamily.empty())
        {
            return eq->mFamily;
        }
        return "unknown";
    }

    // The single kill-feed line for one event: "14:32  Cadmus killed Vex with [Ash] bolt-caster (58 m)" for a
    // death, "14:31  Rook hit Ilse for 25 piercing with [Ash] Soulseeker Repeater" for a hit (adjusted hits read
    // "for 20 (25 before armour)"), "X destroyed <object name>" for an object death, and "<reporter>: <first 80
    // chars>" for a foreign/custom line.
    inline std::string killFeedLine(const Event& ev)
    {
        const SSCombatLog& log = SSCombatLog::instance();
        const std::string time = sessionClock(ev.mTime);

        switch (ev.mKind)
        {
        case EVENT_DEATH:
        {
            const std::string weapon = weaponFamilyFor(ev);
            std::string line = llformat("%s  %s killed %s", time.c_str(),
                log.displayName(ev.mOwner).c_str(), log.displayName(ev.mTarget).c_str());
            if (weapon != "unknown")
            {
                line += " with " + weapon;
            }
            if (ev.mHasPositions)
            {
                line += llformat(" (%.0f m)", (ev.mTargetPos - ev.mSourcePos).length());
            }
            return line;
        }
        case EVENT_OBJECT_DEATH:
            return llformat("%s  %s destroyed %s", time.c_str(),
                log.displayName(ev.mOwner).c_str(), log.displayName(ev.mTarget).c_str());
        case EVENT_DAMAGE:
        {
            const std::string weapon = weaponFamilyFor(ev);
            std::string type_name(SSCombatIcons::typeName(ev.mType));
            LLStringUtil::toLower(type_name);

            std::string amount;
            if (fabsf(ev.mInitial - ev.mDamage) > FEED_ADJUST_EPSILON)
            {
                amount = llformat("%.0f (%.0f before armour)", ev.mDamage, ev.mInitial);
            }
            else
            {
                amount = llformat("%.0f", ev.mDamage);
            }

            std::string line = llformat("%s  %s hit %s for %s %s", time.c_str(),
                log.displayName(ev.mOwner).c_str(), log.displayName(ev.mTarget).c_str(), amount.c_str(), type_name.c_str());
            if (weapon != "unknown")
            {
                line += " with " + weapon;
            }
            return line;
        }
        default: // EVENT_CUSTOM: a foreign line the parser could not type; show who reported it and a snippet.
        {
            std::string text;
            const std::vector<RawLine>& raw = log.rawLines();
            if (ev.mRawLine < raw.size())
            {
                const std::string_view slice = log.rawText(raw[ev.mRawLine]);
                text.assign(slice.substr(0, slice.size() < 80 ? slice.size() : 80));
            }
            return llformat("%s  %s: %s", time.c_str(), log.displayName(ev.mReporter).c_str(), text.c_str());
        }
        }
    }
}

#endif // SS_COMBATFEEDLINE_H
