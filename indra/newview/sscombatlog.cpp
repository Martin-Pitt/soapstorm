/**
 * @file sscombatlog.cpp
 * @brief Combat Log store: relay/tick/settings parsers, the frame->time fit, tracks, equipment, retention,
 *        and the shared playback View. Design and rationale: doc/combat_log_ux.md, doc/combat_log_analysis.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatlog.h"

#include "sscombatanalysis.h"
#include "sscombatsynth.h"

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llformat.h"
#include "llframetimer.h"
#include "llsdjson.h"
#include "llviewercontrol.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace SSCombat;

// ---------------------------------------------------------------------------
// Small text helpers. The wire is ASCII, comma/pipe separated, and deliberately
// tolerant: a missing field is "unknown", never a parse failure.
// ---------------------------------------------------------------------------

namespace
{
    // Trims ASCII whitespace from both ends of a view.
    std::string_view ss_trim(std::string_view v)
    {
        size_t a = 0, b = v.size();
        while (a < b && (v[a] == ' ' || v[a] == '\t' || v[a] == '\r' || v[a] == '\n')) ++a;
        while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t' || v[b - 1] == '\r' || v[b - 1] == '\n')) --b;
        return v.substr(a, b - a);
    }

    // Reads a double out of a view; empty or junk reads as zero.
    F64 ss_f64(std::string_view v)
    {
        std::string s(ss_trim(v));
        if (s.empty()) return 0.0;
        return atof(s.c_str());
    }

    // Reads a signed integer out of a view.
    S32 ss_s32(std::string_view v)
    {
        std::string s(ss_trim(v));
        if (s.empty()) return 0;
        return (S32)strtol(s.c_str(), nullptr, 10);
    }

    // Reads an unsigned integer out of a view.
    U32 ss_u32(std::string_view v)
    {
        std::string s(ss_trim(v));
        if (s.empty()) return 0;
        return (U32)strtoul(s.c_str(), nullptr, 10);
    }

    // Splits a view on one separator; empty fields are preserved so positional formats stay aligned.
    void ss_split(std::string_view v, char sep, std::vector<std::string_view>& out)
    {
        out.clear();
        size_t start = 0;
        while (true)
        {
            size_t hit = v.find(sep, start);
            if (hit == std::string_view::npos)
            {
                out.push_back(v.substr(start));
                return;
            }
            out.push_back(v.substr(start, hit - start));
            start = hit + 1;
        }
    }

    // Parses a 36-character UUID; false leaves the output null.
    bool ss_uuid(std::string_view v, LLUUID& out)
    {
        std::string s(ss_trim(v));
        out.setNull();
        if (s.size() != 36) return false;
        return out.set(s, false);
    }

    // Pulls a UUID out of an LLSD field whatever shape the JSON used.
    LLUUID ss_sd_uuid(const LLSD& sd, const char* key)
    {
        LLUUID id;
        if (!sd.has(key)) return id;
        const LLSD& v = sd[key];
        if (v.type() == LLSD::TypeUUID) return v.asUUID();
        std::string s = v.asString();
        if (s.size() == 36) id.set(s, false);
        return id;
    }

    // Parses an LSL vector or rotation cast to string, "<x, y, z>" or "<x, y, z, s>"; the fourth is ignored.
    // Everything the bridge puts inside a llList2Json array arrives in this shape, six decimals and spaces.
    bool ss_lsl_vec(std::string_view text, LLVector3& out)
    {
        std::string_view v = ss_trim(text);
        if (v.size() < 5) return false;
        const size_t a = v.find('<');
        const size_t b = v.rfind('>');
        if (a == std::string_view::npos || b == std::string_view::npos || b <= a) return false;
        std::vector<std::string_view> parts;
        ss_split(v.substr(a + 1, b - a - 1), ',', parts);
        if (parts.size() < 3) return false;
        out.mV[VX] = (F32)ss_f64(parts[0]);
        out.mV[VY] = (F32)ss_f64(parts[1]);
        out.mV[VZ] = (F32)ss_f64(parts[2]);
        return true;
    }

    // Reads a number out of an LLSD value that may be a JSON number or the string llGetEnv handed the script.
    F64 ss_num(const LLSD& v, F64 fallback)
    {
        if (v.isUndefined()) return fallback;
        if (v.type() == LLSD::TypeString)
        {
            const std::string s = v.asString();
            if (s.empty()) return fallback; // llGetEnv answers "" for a key the region does not report
            return atof(s.c_str());
        }
        if (v.type() == LLSD::TypeBoolean) return v.asBoolean() ? 1.0 : 0.0;
        return v.asReal();
    }

    // The same by key.
    F64 ss_sd_num(const LLSD& sd, const char* key, F64 fallback)
    {
        if (!sd.has(key)) return fallback;
        return ss_num(sd[key], fallback);
    }

    // Pulls a position out of an LLSD field: either a JSON array or an LSL "<x, y, z>" string.
    bool ss_sd_vec(const LLSD& sd, const char* key, LLVector3& out)
    {
        if (!sd.has(key)) return false;
        const LLSD& v = sd[key];
        if (v.isArray() && v.size() >= 3)
        {
            out.mV[VX] = (F32)v[0].asReal();
            out.mV[VY] = (F32)v[1].asReal();
            out.mV[VZ] = (F32)v[2].asReal();
            return true;
        }
        return ss_lsl_vec(v.asString(), out);
    }

    // Collects complete top-level {...} elements out of a possibly truncated JSON array; sets a mask bit per dropped element.
    void ss_salvage(std::string_view text, std::vector<std::string>& out, U8& mask)
    {
        bool in_str = false, esc = false;
        S32 depth = 0, index = 0;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (in_str)
            {
                if (esc)              esc = false;
                else if (c == '\\')   esc = true;
                else if (c == '"')    in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{')
            {
                if (depth == 0) start = i;
                ++depth;
            }
            else if (c == '}')
            {
                if (depth > 0 && --depth == 0)
                {
                    out.push_back(std::string(text.substr(start, i - start + 1)));
                    ++index;
                }
            }
        }
        if (depth != 0 && index < 8)
        {
            mask |= (U8)(1 << index); // the trailing element never closed - llOwnerSay cut it off
        }
    }

    // Lowercases ASCII in place.
    void ss_lower(std::string& s)
    {
        for (char& c : s)
        {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
    }

    // True when the token is a version-looking run of digits and dots, with or without a leading v.
    bool ss_is_version_token(std::string_view t)
    {
        if (t.empty()) return false;
        size_t i = 0;
        if (t[0] == 'v' || t[0] == 'V') ++i;
        if (i >= t.size()) return true; // a bare "v" left over from "v 1.04"
        bool digit = false;
        for (; i < t.size(); ++i)
        {
            const char c = t[i];
            if (c >= '0' && c <= '9') digit = true;
            else if (c != '.' && c != '-' && c != '_') return false;
        }
        return digit;
    }

    // Interpolates two angles the short way round.
    F32 ss_lerp_angle(F32 a, F32 b, F32 s)
    {
        F32 d = b - a;
        while (d > F_PI)  d -= F_TWO_PI;
        while (d < -F_PI) d += F_TWO_PI;
        return a + d * s;
    }

    static const std::vector<Life> sNoLives;
}

// ---------------------------------------------------------------------------
// NounRef
// ---------------------------------------------------------------------------

namespace
{
    // The address grammar's type tokens, indexed by ENounType.
    const char* const SS_NOUN_TOKEN[] =
    {
        "none", "session", "engagement", "moment", "life", "combatant", "team", "squad",
        "death", "damage", "volley", "equipment", "family", "object", "adjustment", "zone",
        "sightline", "claim", "rule", "summary", "case", "logline"
    };
    constexpr U8 SS_NOUN_COUNT = (U8)(sizeof(SS_NOUN_TOKEN) / sizeof(SS_NOUN_TOKEN[0]));
}

// Renders the address form ss:<type>/<id-or-index>[@t].
std::string NounRef::address() const
{
    if (mType == NOUN_NONE || mType >= SS_NOUN_COUNT)
    {
        return "ss:none/0";
    }
    std::string out("ss:");
    out += SS_NOUN_TOKEN[mType];
    out += "/";
    out += mId.isNull() ? llformat("%u", mIndex) : mId.asString();
    if (mTime != 0.0)
    {
        out += llformat("@%.3f", mTime);
    }
    return out;
}

// Parses the address form; an unrecognised string yields an invalid ref.
NounRef NounRef::parse(std::string_view address)
{
    NounRef ref;
    std::string_view v = ss_trim(address);
    if (v.size() > 3 && v.substr(0, 3) == "ss:")
    {
        v = v.substr(3);
    }
    const size_t slash = v.find('/');
    if (slash == std::string_view::npos)
    {
        return ref;
    }
    const std::string_view type = v.substr(0, slash);
    std::string_view rest = v.substr(slash + 1);
    const size_t at = rest.find('@');
    if (at != std::string_view::npos)
    {
        ref.mTime = ss_f64(rest.substr(at + 1));
        rest = rest.substr(0, at);
    }
    const size_t query = rest.find('?');
    if (query != std::string_view::npos)
    {
        rest = rest.substr(0, query);
    }
    for (U8 i = 1; i < SS_NOUN_COUNT; ++i)
    {
        if (type == SS_NOUN_TOKEN[i])
        {
            ref.mType = i;
            break;
        }
    }
    if (ref.mType == NOUN_NONE)
    {
        return ref;
    }
    if (!ss_uuid(rest, ref.mId))
    {
        ref.mIndex = ss_u32(rest);
    }
    return ref;
}

// Human label for breadcrumbs and pick lists; asks the store for names when it exists.
std::string NounRef::label() const
{
    if (mType == NOUN_NONE || mType >= SS_NOUN_COUNT)
    {
        return "(nothing)";
    }
    if (!SSCombatLog::instanceExists())
    {
        return std::string(SS_NOUN_TOKEN[mType]);
    }
    const SSCombatLog& log = SSCombatLog::instance();
    switch (mType)
    {
        case NOUN_SESSION:
            return "Session";
        case NOUN_ENGAGEMENT:
            return llformat("Engagement %u", mIndex);
        case NOUN_MOMENT:
            return llformat("Moment %.1fs", mTime - log.sessionStart());
        case NOUN_COMBATANT:
            return log.displayName(mId);
        case NOUN_TEAM:
            return log.teamName((S8)mIndex);
        case NOUN_SQUAD:
            return llformat("Squad %u", mIndex);
        case NOUN_LIFE:
            return llformat("%s life %u", log.displayName(mId).c_str(), mIndex);
        case NOUN_DEATH:
        case NOUN_DAMAGE:
        {
            const Event* ev = log.event(mIndex);
            if (!ev)
            {
                return mType == NOUN_DEATH ? "DEATH" : "DAMAGE";
            }
            const char* kind = (ev->mKind == EVENT_DEATH) ? "DEATH"
                             : (ev->mKind == EVENT_OBJECT_DEATH) ? "OBJECT_DEATH"
                             : (ev->mKind == EVENT_CUSTOM) ? "CUSTOM" : "DAMAGE";
            return llformat("%s %s <- %s", kind, log.displayName(ev->mTarget).c_str(),
                            log.displayName(ev->mOwner).c_str());
        }
        case NOUN_VOLLEY:
            return llformat("Volley %u", mIndex);
        case NOUN_EQUIPMENT:
        case NOUN_OBJECT:
            return log.displayName(mId);
        case NOUN_FAMILY:
        {
            const Equipment* eq = log.equipmentFor(mId);
            return (eq && !eq->mFamily.empty()) ? eq->mFamily : std::string("Family");
        }
        case NOUN_ADJUSTMENT:
            return llformat("Adjustment %u", mIndex);
        case NOUN_LOGLINE:
            return llformat("Record %u", mIndex);
        default:
            break;
    }
    return std::string(SS_NOUN_TOKEN[mType]);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Builds the store, its analysis half and the initial View.
SSCombatLog::SSCombatLog()
{
    static LLCachedControl<F32> trail(gSavedSettings, "SSCombatLogTrailSeconds", 20.f);
    mView.mWindow = llmax(1.f, (F32)trail);
    mAnalysis = new SSCombatAnalysis(*this);
    mLastIdle = now();
    mLastRetention = mLastIdle;
}

// Tears down the analysis and any synthetic feed.
SSCombatLog::~SSCombatLog()
{
    delete mSynth;
    mSynth = nullptr;
    delete mAnalysis;
    mAnalysis = nullptr;
}

// Recording needs the master setting; a loaded synthetic session is always "on" so Stage 0 previews without one.
bool SSCombatLog::isEnabled() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSCombatLogEnabled", false);
    if (mSynthetic)
    {
        return true;
    }
    return enabled;
}

// Arms or disarms ingest; the session clock starts on the first data either way.
void SSCombatLog::setRecording(bool on)
{
    if (mRecording == on)
    {
        return;
    }
    mRecording = on;
    mDirty = true;
}

// Drops every event, track, flight and raw line; registry data and region settings survive.
void SSCombatLog::clear()
{
    delete mSynth;
    mSynth = nullptr;
    mSynthetic = false;
    mSessionStart = 0.0;
    mLastDataTime = 0.0;
    mEvents.clear();
    mEventIndex.clear();
    mNextEventId = 0;
    mModifications.clear();
    mTracks.clear();
    mEquipment.clear();
    mFlights.clear();
    mRawLines.clear();
    mRawArena.clear();
    mNames.clear();
    mFrameFit.clear();
    mPendingFinal.clear();
    mTrackKeys.clear();
    mViewChain.clear();
    mNounChain.clear();
    mView = View();
    static LLCachedControl<F32> trail(gSavedSettings, "SSCombatLogTrailSeconds", 20.f);
    mView.mWindow = llmax(1.f, (F32)trail);
    invalidateAnalysis();
    mDirty = true;
    mDataChanged();
    mViewChanged();
}

// Per-frame tick: synthetic replay, playback cursor, retention, and one throttled dataChanged.
void SSCombatLog::idle(F64 nowsec)
{
    F64 dt = nowsec - mLastIdle;
    if (dt < 0.0 || dt > 1.0)
    {
        dt = 0.0; // a stall must not fast-forward the replay
    }
    mLastIdle = nowsec;

    if (mSynth)
    {
        mSynth->idle(nowsec);
    }

    if (mView.mLive)
    {
        mView.mCursor = sessionEnd();
    }
    else if (mView.mPlaying)
    {
        mView.mCursor += dt * (F64)mView.mSpeed;
        if (mView.mReconstruct && mView.mCursor >= mView.mReconEnd)
        {
            mView.mCursor = mView.mReconEnd;
            mView.mPlaying = false;
        }
        const F64 end = sessionEnd();
        if (mView.mCursor > end)
        {
            mView.mCursor = end;
            mView.mPlaying = false;
        }
    }

    if (nowsec - mLastRetention >= 30.0)
    {
        mLastRetention = nowsec;
        runRetention(nowsec);
    }

    if (mDirty)
    {
        mDirty = false;
        mDataChanged();
    }
}

// ---------------------------------------------------------------------------
// Ingest - relay
// ---------------------------------------------------------------------------

// Files one [name, pos, attachPoint, group, creator] details slice into the equipment registry.
// base is 1 for rezzerDetails, whose element 0 is the rezzer's own rezzer key rather than a name.
static void ss_note_detail_equipment(SSCombatLog& log, const LLUUID& id, const LLSD& details, S32 base,
                                     const LLUUID& owner, const LLUUID& rezzer, F64 seen)
{
    if (id.isNull())
    {
        return;
    }
    Equipment eq;
    eq.mId = id;
    eq.mOwner = owner;
    eq.mRezzer = rezzer;
    eq.mFirstSeen = seen;
    eq.mLastSeen = seen;
    eq.mConfidence = CONF_LIKELY; // the script measured this, it is not an inference
    if (details.isArray() && details.size() >= (size_t)(base + 5))
    {
        eq.mName = details[base].asString();
        LLVector3 pos;
        if (ss_lsl_vec(details[base + 1].asString(), pos))
        {
            eq.mLastPos = pos;
            eq.mHasLastPos = true;
        }
        eq.mAttachPoint = (S32)ss_num(details[base + 2], -1.0);
        const std::string group = details[base + 3].asString();
        const std::string creator = details[base + 4].asString();
        if (group.size() == 36)   eq.mGroup.set(group, false);
        if (creator.size() == 36) eq.mCreator.set(creator, false);
    }
    // Attach point decides the class: 31-38 is a HUD, any other point is worn gear, and an unattached
    // object that something rezzed is a projectile until better evidence arrives.
    if (eq.mAttachPoint >= 31 && eq.mAttachPoint <= 38)   eq.mKind = EQUIP_HUD;
    else if (eq.mAttachPoint > 0)                         eq.mKind = EQUIP_WEAPON;
    else if (eq.mAttachPoint == 0 && !rezzer.isNull())    eq.mKind = EQUIP_PROJECTILE;
    log.noteEquipment(eq);
}

// Maps the bridge's FINAL_DAMAGE object: the wearer's own incoming hit, faster and richer than the log.
static bool ss_final_damage_from_llsd(SSCombatLog& log, const LLSD& sd, Event& ev)
{
    ev.mKind = EVENT_DAMAGE;
    ev.mFromFinalDamage = true;
    ev.mTargetIsAgent = true;
    ev.mSource = ss_sd_uuid(sd, "source");
    ev.mRezzer = ss_sd_uuid(sd, "rezzer");
    ev.mOwner  = ss_sd_uuid(sd, "owner");
    // final_damage only ever fires in the damaged avatar's own attachments, so the target is the wearer.
    ev.mTarget = gAgent.getID();
    ev.mDamage = (F32)ss_sd_num(sd, "damage", 0.0);
    ev.mInitial = (F32)ss_sd_num(sd, "original", (F64)ev.mDamage);
    ev.mType = (S16)ss_sd_num(sd, "type", 0.0);

    const LLSD& source_details = sd["sourceDetails"];
    const LLSD& rezzer_details = sd["rezzerDetails"];
    const LLSD& root_details   = sd["rootDetails"];

    bool have_source = false;
    if (source_details.isArray() && source_details.size() >= 2)
    {
        have_source = ss_lsl_vec(source_details[1].asString(), ev.mSourcePos);
    }
    const bool have_target = ss_sd_vec(sd, "targetPos", ev.mTargetPos);
    ev.mHasPositions = have_source && have_target;

    // Everything the script looked up on the way is a fact about an object; keep all of it.
    const F64 seen = ev.mReceived;
    LLUUID root = ss_sd_uuid(sd, "root");
    if (root.isNull() && rezzer_details.isArray() && rezzer_details.size() >= 1)
    {
        const std::string key = rezzer_details[0].asString();
        if (key.size() == 36) root.set(key, false);
    }
    ss_note_detail_equipment(log, ev.mSource, source_details, 0, ev.mOwner, ev.mRezzer, seen);
    ss_note_detail_equipment(log, ev.mRezzer, rezzer_details, 1, ev.mOwner, root, seen);
    ss_note_detail_equipment(log, root, root_details, 0, ev.mOwner, LLUUID::null, seen);
    return true;
}

// Maps one parsed log object onto an Event; returns false when the object carries no usable schema.
static bool ss_event_from_llsd(SSCombatLog& log, const LLSD& sd, Event& ev)
{
    if (!sd.isMap())
    {
        return false;
    }
    const std::string kind = sd.has("event") ? sd["event"].asString() : std::string();
    if (kind == "FINAL_DAMAGE")
    {
        return ss_final_damage_from_llsd(log, sd, ev);
    }
    if (kind == "DAMAGE")            ev.mKind = EVENT_DAMAGE;
    else if (kind == "DEATH")        ev.mKind = EVENT_DEATH;
    else if (kind == "OBJECT_DEATH") ev.mKind = EVENT_OBJECT_DEATH;
    else                             ev.mKind = EVENT_CUSTOM;

    ev.mDamage  = (F32)sd["damage"].asReal();
    ev.mInitial = sd.has("initial") ? (F32)sd["initial"].asReal() : ev.mDamage;
    ev.mType    = (S16)sd["type"].asInteger();
    ev.mSource  = ss_sd_uuid(sd, "source");
    ev.mRezzer  = ss_sd_uuid(sd, "rezzer");
    ev.mOwner   = ss_sd_uuid(sd, "owner");
    ev.mTarget  = ss_sd_uuid(sd, "target");

    LLVector3 sp, tp;
    const bool have_src = ss_sd_vec(sd, "source_pos", sp);
    const bool have_tgt = ss_sd_vec(sd, "target_pos", tp);
    if (have_src) ev.mSourcePos = sp;
    if (have_tgt) ev.mTargetPos = tp;
    ev.mHasPositions = have_src && have_tgt;

    if (sd.has("agent"))
    {
        ev.mTargetIsAgent = sd["agent"].asInteger() != 0;
    }
    if (sd.has("fd"))
    {
        ev.mFromFinalDamage = sd["fd"].asInteger() != 0;
    }
    if (sd.has("t"))
    {
        // Owner verdict: the per-event stamp is the simulator's frame counter, not script seconds.
        const U32 stamp = (U32)sd["t"].asInteger();
        if (stamp != 0)
        {
            ev.mFrame = stamp;
        }
    }
    // A foreign object with no recognisable field at all is still a record, but not a typed one.
    if (ev.mKind == EVENT_CUSTOM && ev.mOwner.isNull() && ev.mTarget.isNull() && ev.mSource.isNull())
    {
        return sd.has("event");
    }
    return true;
}

// Copies the modifications[] array into the shared side table and records the slice on the event.
static void ss_take_modifications(const LLSD& sd, Event& ev, std::vector<Modification>& table)
{
    if (!sd.has("modifications"))
    {
        return;
    }
    const LLSD& mods = sd["modifications"];
    if (!mods.isArray())
    {
        return;
    }
    const U32 begin = (U32)table.size();
    U32 count = 0;
    for (LLSD::array_const_iterator it = mods.beginArray(); it != mods.endArray(); ++it)
    {
        const LLSD& entry = *it;
        const LLUUID task = ss_sd_uuid(entry, "task_id");
        if (entry.has("events") && entry["events"].isArray())
        {
            const LLSD& evs = entry["events"];
            for (LLSD::array_const_iterator jt = evs.beginArray(); jt != evs.endArray(); ++jt)
            {
                Modification m;
                m.mTaskId = task;
                m.mScript = (*jt).has("script") ? (*jt)["script"].asString() : std::string();
                m.mNewDamage = (F32)(*jt)["new_damage"].asReal();
                table.push_back(m);
                ++count;
            }
        }
        else
        {
            Modification m;
            m.mTaskId = task;
            m.mScript = entry.has("script") ? entry["script"].asString() : std::string();
            m.mNewDamage = (F32)entry["new_damage"].asReal();
            table.push_back(m);
            ++count;
        }
    }
    if (count)
    {
        ev.mModsBegin = begin;
        ev.mModsCount = count;
    }
}

// The text after the "C2" prefix: "<frame>|" then a sim JSON batch, or 72 key chars then foreign text.
void SSCombatLog::ingestRelay(std::string_view payload, F64 received)
{
    if (payload.empty())
    {
        return;
    }
    if (mSessionStart <= 0.0)
    {
        mSessionStart = received;
    }

    const std::string_view whole = payload;

    // The bridge says the region's combat settings as a plain owner-say, "C2RS|{...}", with no frame stamp.
    if (ss_trim(payload).size() >= 3 && ss_trim(payload).substr(0, 3) == "RS|")
    {
        ingestRegionSettings(ss_trim(payload));
        appendRawLine(whole, received, 0, mSynthetic ? (U8)TRUST_SYNTHETIC : (U8)TRUST_SIM, 0, 0, 0);
        mDirty = true;
        return;
    }

    U32 frame = 0;
    std::string_view rest = payload;
    const size_t bar = payload.find('|');
    if (bar != std::string_view::npos && bar > 0 && bar <= 12)
    {
        const std::string_view head = payload.substr(0, bar);
        bool numeric = true;
        for (char c : head)
        {
            if (c < '0' || c > '9') { numeric = false; break; }
        }
        if (numeric)
        {
            frame = ss_u32(head);
            rest = payload.substr(bar + 1);
        }
    }
    if (frame == 0)
    {
        LL_WARNS("CombatLog") << "relay line with no frame stamp, falling back to receive time" << LL_ENDL;
    }

    U8 trust = TRUST_SIM;
    LLUUID reporter, reporter_owner;
    std::string_view body = ss_trim(rest);
    if (!body.empty() && body[0] != '[' && body[0] != '{')
    {
        // Script 2 tags a non-system message with <sender 36><owner 36> before the payload.
        if (body.size() >= 72 && ss_uuid(body.substr(0, 36), reporter) && ss_uuid(body.substr(36, 36), reporter_owner))
        {
            body = ss_trim(body.substr(72));
        }
        else
        {
            reporter.setNull();
            reporter_owner.setNull();
        }
        trust = TRUST_FOREIGN;
    }
    if (mSynthetic && trust == TRUST_SIM)
    {
        trust = TRUST_SYNTHETIC; // a foreign synthetic line stays FOREIGN so the Record level reads honestly
    }

    const U32 raw_index = (U32)mRawLines.size();
    const U32 first_id  = mNextEventId + 1;
    U32       appended  = 0;
    U8        salvage   = 0;

    std::vector<std::string> objects;
    bool typed = false;

    if (!body.empty() && (body[0] == '[' || body[0] == '{'))
    {
        boost::system::error_code ec;
        boost::json::value jv = boost::json::parse(std::string(body), ec);
        if (!ec)
        {
            typed = true;
            LLSD sd = LlsdFromJson(jv);
            if (sd.isArray())
            {
                for (LLSD::array_const_iterator it = sd.beginArray(); it != sd.endArray(); ++it)
                {
                    Event ev;
                    ev.mTrust = trust;
                    ev.mFrame = frame;
                    ev.mReceived = received;
                    ev.mRawLine = raw_index;
                    ev.mReporter = reporter;
                    ev.mReporterOwner = reporter_owner;
                    if (ss_event_from_llsd(*this, *it, ev))
                    {
                        ss_take_modifications(*it, ev, mModifications);
                        appendEvent(ev);
                        if (ev.mId >= first_id) ++appended;
                    }
                }
            }
            else if (sd.isMap())
            {
                Event ev;
                ev.mTrust = trust;
                ev.mFrame = frame;
                ev.mReceived = received;
                ev.mRawLine = raw_index;
                ev.mReporter = reporter;
                ev.mReporterOwner = reporter_owner;
                if (ss_event_from_llsd(*this, sd, ev))
                {
                    ss_take_modifications(sd, ev, mModifications);
                    appendEvent(ev);
                    if (ev.mId >= first_id) ++appended;
                }
            }
        }
        else if (body[0] == '[')
        {
            // llOwnerSay truncates at 1024 bytes, so a batch can lose its tail. Keep the complete elements.
            ss_salvage(body, objects, salvage);
            LL_WARNS("CombatLog") << "truncated combat batch, salvaged " << objects.size()
                                  << " element(s), mask " << (S32)salvage << LL_ENDL;
            U8 index = 0;
            for (const std::string& text : objects)
            {
                boost::system::error_code ec2;
                boost::json::value one = boost::json::parse(text, ec2);
                if (ec2)
                {
                    if (index < 8) salvage |= (U8)(1 << index);
                    ++index;
                    continue;
                }
                typed = true;
                Event ev;
                ev.mTrust = trust;
                ev.mFrame = frame;
                ev.mReceived = received;
                ev.mRawLine = raw_index;
                ev.mReporter = reporter;
                ev.mReporterOwner = reporter_owner;
                LLSD sd = LlsdFromJson(one);
                if (ss_event_from_llsd(*this, sd, ev))
                {
                    ss_take_modifications(sd, ev, mModifications);
                    appendEvent(ev);
                    if (ev.mId >= first_id) ++appended;
                }
                ++index;
            }
        }
        else
        {
            LL_WARNS("CombatLog") << "unparseable combat object dropped" << LL_ENDL;
        }
    }

    if (!typed)
    {
        // Foreign text in the region's own dialect: kept verbatim as a CUSTOM event so it can still be read.
        Event ev;
        ev.mKind = EVENT_CUSTOM;
        ev.mTrust = (trust == TRUST_SIM) ? (U8)TRUST_FOREIGN : trust;
        ev.mFrame = frame;
        ev.mReceived = received;
        ev.mRawLine = raw_index;
        ev.mReporter = reporter;
        ev.mReporterOwner = reporter_owner;
        ev.mOwner = reporter_owner;
        ev.mSource = reporter;
        ev.mTargetIsAgent = false;
        // A foreign line often names its subject first: "<verb>|<uuid>|..." parses that much for free.
        std::vector<std::string_view> parts;
        ss_split(body, '|', parts);
        for (const std::string_view& p : parts)
        {
            LLUUID id;
            if (ss_uuid(p, id))
            {
                ev.mTarget = id;
                break;
            }
        }
        appendEvent(ev);
        if (ev.mId >= first_id) ++appended;
    }

    appendRawLine(whole, received, frame, trust, salvage, appended ? first_id : 0, appended);
    invalidateAnalysis();
    mDirty = true;
}

// Stores one received line verbatim in the raw arena for the Record level.
void SSCombatLog::appendRawLine(std::string_view text, F64 received, U32 frame, U8 trust, U8 salvage,
                                U32 firstEvent, U32 count)
{
    RawLine line;
    line.mReceived = received;
    line.mFrame = frame;
    line.mOffset = (U32)mRawArena.size();
    line.mLength = (U32)text.size();
    line.mTrust = trust;
    line.mSalvageMask = salvage;
    line.mFirstEvent = firstEvent;
    line.mEventCount = count;
    mRawArena.append(text.data(), text.size());
    mRawLines.push_back(line);
}

// ---------------------------------------------------------------------------
// Ingest - tracking ticks
// ---------------------------------------------------------------------------

// The body of a GetTrackedAgents reply: TA header, K index table, T sample lines.
void SSCombatLog::ingestTrackReply(std::string_view body, F64 received, F64 rtt)
{
    if (body.empty())
    {
        return;
    }
    if (mSessionStart <= 0.0)
    {
        mSessionStart = received;
    }
    // The reply describes the simulator's state half a round trip ago.
    const F64 stamped = received - rtt * 0.5;

    std::vector<std::string_view> lines;
    ss_split(body, '\n', lines);

    std::vector<std::string_view> fields;
    std::vector<std::string_view> entries;
    std::vector<std::string_view> nums;

    for (const std::string_view& raw : lines)
    {
        const std::string_view line = ss_trim(raw);
        if (line.empty())
        {
            continue;
        }
        if (line.size() >= 3 && line.substr(0, 3) == "TA|")
        {
            // The header carries the frame counter that anchors the whole fit, so it is read first.
            ss_split(line.substr(3), '|', fields);
            if (!fields.empty())
            {
                const U32 head_frame = ss_u32(fields[0]);
                if (head_frame)
                {
                    noteFrameSample(head_frame, stamped);
                }
                if (fields.size() >= 4)
                {
                    const U32 dropped = ss_u32(fields[3]);
                    if (dropped)
                    {
                        LL_WARNS("CombatLog") << "tracking gap: " << dropped << " tick(s) dropped by the script" << LL_ENDL;
                    }
                }
            }
            continue;
        }
        if (line.size() < 2 || line[1] != '|')
        {
            LL_WARNS("CombatLog") << "tick reply line with no type prefix, ignored" << LL_ENDL;
            continue;
        }
        const char type = line[0];
        const std::string_view rest = line.substr(2);

        if (type == 'K')
        {
            ss_split(rest, ',', entries);
            for (const std::string_view& e : entries)
            {
                const size_t eq = e.find('=');
                if (eq == std::string_view::npos) continue;
                const U32 idx = ss_u32(e.substr(0, eq));
                LLUUID id;
                if (!ss_uuid(e.substr(eq + 1), id))
                {
                    LL_WARNS("CombatLog") << "tick index table entry with a bad key, ignored" << LL_ENDL;
                    continue;
                }
                if (idx >= 4096)
                {
                    LL_WARNS("CombatLog") << "tick index " << idx << " out of range, ignored" << LL_ENDL;
                    continue;
                }
                if (mTrackKeys.size() <= idx)
                {
                    mTrackKeys.resize(idx + 1);
                }
                mTrackKeys[idx] = id;
            }
            continue;
        }

        if (type == 'T')
        {
            ss_split(rest, '|', fields);
            if (fields.size() < 3)
            {
                LL_WARNS("CombatLog") << "short T line, ignored" << LL_ENDL;
                continue;
            }
            const U32 frame = ss_u32(fields[1]);
            const F64 t = frame ? frameToTime(frame) : stamped;
            ss_split(fields[2], ';', entries);
            for (const std::string_view& e : entries)
            {
                const std::string_view entry = ss_trim(e);
                if (entry.empty()) continue;
                const size_t colon = entry.find(':');
                if (colon == std::string_view::npos) continue;
                const U32 idx = ss_u32(entry.substr(0, colon));
                if (idx >= mTrackKeys.size() || mTrackKeys[idx].isNull())
                {
                    LL_WARNS("CombatLog") << "tick sample for unknown index " << idx << ", ignored" << LL_ENDL;
                    continue;
                }
                ss_split(entry.substr(colon + 1), ',', nums);
                if (nums.size() < 3)
                {
                    continue; // a sample without a position says nothing
                }
                Sample s;
                s.mTime = (F32)(t - mSessionStart);
                s.mPos.mV[VX] = (F32)ss_f64(nums[0]);
                s.mPos.mV[VY] = (F32)ss_f64(nums[1]);
                s.mPos.mV[VZ] = (F32)ss_f64(nums[2]);
                if (nums.size() > 3) s.mYaw = (F32)(ss_f64(nums[3]) * DEG_TO_RAD);
                if (nums.size() > 6)
                {
                    s.mVel.mV[VX] = (F32)ss_f64(nums[4]);
                    s.mVel.mV[VY] = (F32)ss_f64(nums[5]);
                    s.mVel.mV[VZ] = (F32)ss_f64(nums[6]);
                }
                if (nums.size() > 7) s.mFlags = (U16)ss_u32(nums[7]);
                if (nums.size() > 8) ss_uuid(nums[8], s.mParent);
                s.mSource = SAMPLE_BRIDGE;
                s.mQuality = 240;
                addSample(mTrackKeys[idx], true, s);
            }
            continue;
        }

        LL_WARNS("CombatLog") << "unknown tick line type '" << type << "', ignored" << LL_ENDL;
    }

    invalidateAnalysis();
    mDirty = true;
}

// The region settings block "RS|" followed by the nine values in the header's order.
void SSCombatLog::ingestRegionSettings(std::string_view body)
{
    std::string_view rest = ss_trim(body);
    if (rest.size() >= 3 && rest.substr(0, 3) == "RS|")
    {
        rest = rest.substr(3);
    }
    RegionSettings rs;
    rs.mKnown = true;

    if (!rest.empty() && rest[0] == '{')
    {
        // The bridge's own shape: llGetRegionFlags() as a JSON number, every llGetEnv value as a string.
        boost::system::error_code ec;
        boost::json::value jv = boost::json::parse(std::string(rest), ec);
        if (ec)
        {
            LL_WARNS("CombatLog") << "region settings object did not parse, ignored" << LL_ENDL;
            return;
        }
        const LLSD sd = LlsdFromJson(jv);
        if (!sd.isMap())
        {
            LL_WARNS("CombatLog") << "region settings payload is not an object, ignored" << LL_ENDL;
            return;
        }
        // llGetRegionFlags is an LSL integer, so a flag above bit 30 arrives negative; the S64 hop keeps it.
        rs.mRegionFlags        = (U32)(S64)ss_sd_num(sd, "flags", 0.0);
        rs.mAllowDamageAdjust  = ss_sd_num(sd, "allow_damage_adjust", 1.0) != 0.0;
        rs.mRestrictCombatLog  = ss_sd_num(sd, "restrict_combat_log", 0.0) != 0.0;
        rs.mDamageThrottle     = (F32)ss_sd_num(sd, "damage_throttle", 0.0);
        rs.mDamageLimit        = (F32)ss_sd_num(sd, "damage_limit", 0.0);
        rs.mRestoreHealth      = ss_sd_num(sd, "restore_health", 1.0) != 0.0;
        rs.mHealthRegenRate    = (F32)ss_sd_num(sd, "health_regen_rate", (F64)(1.f / 6.f));
        rs.mInvulnerabilityTime = (F32)ss_sd_num(sd, "invulnerability_time", 0.0);
        rs.mDeathAction        = (S32)ss_sd_num(sd, "death_action", 0.0);
        rs.mAgentLimit         = (S32)ss_sd_num(sd, "agent_limit", 0.0);
        mRegionSettings = rs;
        mDirty = true;
        return;
    }

    // The comma-separated form, kept so a simpler script or a test fixture still works.
    std::vector<std::string_view> f;
    ss_split(rest, ',', f);
    if (f.size() < 9)
    {
        LL_WARNS("CombatLog") << "region settings block has " << f.size() << " field(s), expected 9" << LL_ENDL;
    }
    if (f.size() > 0) rs.mAllowDamageAdjust   = ss_s32(f[0]) != 0;
    if (f.size() > 1) rs.mRestrictCombatLog   = ss_s32(f[1]) != 0;
    if (f.size() > 2) rs.mDamageThrottle      = (F32)ss_f64(f[2]);
    if (f.size() > 3) rs.mDamageLimit         = (F32)ss_f64(f[3]);
    if (f.size() > 4) rs.mRestoreHealth       = ss_s32(f[4]) != 0;
    if (f.size() > 5) rs.mHealthRegenRate     = (F32)ss_f64(f[5]);
    if (f.size() > 6) rs.mInvulnerabilityTime = (F32)ss_f64(f[6]);
    if (f.size() > 7) rs.mDeathAction         = ss_s32(f[7]);
    if (f.size() > 8) rs.mAgentLimit          = ss_s32(f[8]);
    mRegionSettings = rs;
    mDirty = true;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Decides how a hit was delivered from source/rezzer identity, damage type and the equipment registry (analysis 5.5).
static U8 ss_classify_delivery(const SSCombatLog& log, const Event& ev)
{
    if (ev.mKind != EVENT_DAMAGE && ev.mKind != EVENT_DEATH)
    {
        return DELIVERY_UNKNOWN;
    }
    if (ev.mType == 102 || ev.mType == 103)
    {
        return DELIVERY_AREA; // the region's own splash types
    }
    if (!ev.mSource.isNull() && (ev.mSource == ev.mRezzer || ev.mSource == ev.mOwner))
    {
        return DELIVERY_HITSCAN; // attached, or the weapon is its own rezzer
    }
    if (const Equipment* src = log.equipmentFor(ev.mSource))
    {
        if (src->mKind == EQUIP_PROJECTILE) return DELIVERY_PROJECTILE;
        if (src->mKind == EQUIP_DEPLOYABLE) return DELIVERY_LOBBED;
        if (src->mAttachPoint > 0)          return DELIVERY_HITSCAN;
    }
    if (const Equipment* rez = log.equipmentFor(ev.mRezzer))
    {
        if (rez->mKind == EQUIP_HUD)        return DELIVERY_LOBBED;
        if (rez->mAttachPoint > 0)          return DELIVERY_PROJECTILE;
    }
    return DELIVERY_UNKNOWN;
}

// Stamps the event on the simulator clock, runs the final_damage merge, then inserts it in time order.
void SSCombatLog::appendEvent(Event& ev)
{
    if (mSessionStart <= 0.0)
    {
        mSessionStart = ev.mReceived > 0.0 ? ev.mReceived : now();
    }

    if (ev.mFrame != 0 && !mFrameFit.empty())
    {
        ev.mTime = frameToTime(ev.mFrame);
        // One frame at the local slope is the honest quantum; fd records are better than that.
        const F64 next = frameToTime(ev.mFrame + 1);
        ev.mUncertainty = ev.mFromFinalDamage ? 0.05f : (F32)llmax(0.02, llmin(0.5, next - ev.mTime));
    }
    else
    {
        if (ev.mFrame != 0 && mFrameFit.empty())
        {
            // Nothing has mapped a frame yet. Anchor the counter to this line at the nominal 45 frames/s
            // so a burst arriving before the first tick still orders correctly among itself.
            noteFrameSample(ev.mFrame, ev.mReceived - 0.5);
        }
        ev.mTime = ev.mReceived - 0.5;
        ev.mUncertainty = 1.f;
    }

    if (ev.mDelivery == DELIVERY_UNKNOWN)
    {
        ev.mDelivery = ss_classify_delivery(*this, ev);
    }

    ev.mId = 0;
    dedupFinalDamage(ev);
    if (ev.mId != 0)
    {
        return; // merged into the fd record that arrived first
    }

    ev.mId = ++mNextEventId;

    if (ev.mFromFinalDamage)
    {
        Event key = ev;
        key.mModsCount = 0;
        mPendingFinal.push_back(key);
    }

    size_t index;
    if (mEvents.empty() || mEvents.back().mTime <= ev.mTime)
    {
        index = mEvents.size();
        mEvents.push_back(ev);
        mEventIndex[ev.mId] = (U32)index;
    }
    else
    {
        // Out-of-order arrival: keep the vector sorted by time and rebuild the id map.
        auto it = std::upper_bound(mEvents.begin(), mEvents.end(), ev.mTime,
                                   [](F64 t, const Event& e) { return t < e.mTime; });
        index = (size_t)(it - mEvents.begin());
        mEvents.insert(it, ev);
        mEventIndex.clear();
        for (size_t i = 0; i < mEvents.size(); ++i)
        {
            mEventIndex[mEvents[i].mId] = (U32)i;
        }
    }

    if (ev.mTime > mLastDataTime)
    {
        mLastDataTime = ev.mTime;
    }

    // One source hitting two or more distinct targets inside a second is area damage however it was classified.
    if (index < mEvents.size() && mEvents[index].mKind == EVENT_DAMAGE && !mEvents[index].mSource.isNull())
    {
        const LLUUID source = mEvents[index].mSource;
        const F64    when   = mEvents[index].mTime;
        std::vector<size_t> group;
        std::vector<LLUUID> targets;
        const size_t first = index > 64 ? index - 64 : 0;
        for (size_t i = first; i <= index; ++i)
        {
            const Event& e = mEvents[i];
            if (e.mSource != source || std::fabs(e.mTime - when) > 1.0)
            {
                continue;
            }
            group.push_back(i);
            if (std::find(targets.begin(), targets.end(), e.mTarget) == targets.end())
            {
                targets.push_back(e.mTarget);
            }
        }
        if (targets.size() >= 2)
        {
            for (size_t i : group)
            {
                mEvents[i].mDelivery = DELIVERY_AREA;
            }
        }
    }
    mDirty = true;
}

// Matches a log DAMAGE against a final_damage record already in the store; on a match the event is folded in.
void SSCombatLog::dedupFinalDamage(Event& ev)
{
    // Retire records past the merge window whichever way this call goes.
    const F64 cutoff = ev.mTime - 2.5;
    mPendingFinal.erase(std::remove_if(mPendingFinal.begin(), mPendingFinal.end(),
                                       [cutoff](const Event& p) { return p.mTime < cutoff; }),
                        mPendingFinal.end());

    if (ev.mFromFinalDamage || ev.mKind != EVENT_DAMAGE)
    {
        return;
    }
    for (const Event& pending : mPendingFinal)
    {
        if (pending.mSource != ev.mSource || pending.mOwner != ev.mOwner || pending.mType != ev.mType)
        {
            continue;
        }
        if (!ev.mTarget.isNull() && pending.mTarget != ev.mTarget)
        {
            continue; // fd records are always about the wearer; a hit on somebody else is a different event
        }
        if (std::fabs(pending.mDamage - ev.mDamage) > 0.01f)
        {
            continue;
        }
        if (std::fabs(pending.mTime - ev.mTime) > 2.5)
        {
            continue;
        }
        auto it = mEventIndex.find(pending.mId);
        if (it == mEventIndex.end() || it->second >= mEvents.size())
        {
            continue;
        }
        Event& kept = mEvents[it->second];
        // The fd record has the better stamp and the positions; the log line brings the adjustments.
        if (ev.mModsCount && !kept.mModsCount)
        {
            kept.mModsBegin = ev.mModsBegin;
            kept.mModsCount = ev.mModsCount;
            kept.mInitial = ev.mInitial;
        }
        if (kept.mFrame == 0)
        {
            kept.mFrame = ev.mFrame;
        }
        kept.mTrust = TRUST_SIM; // the simulator confirmed it
        ev.mId = kept.mId;
        mDirty = true;
        return;
    }
}

// Returns the event with this id, or null once retention has dropped it.
const Event* SSCombatLog::event(U32 id) const
{
    if (id == 0)
    {
        return nullptr;
    }
    auto it = mEventIndex.find(id);
    if (it == mEventIndex.end() || it->second >= mEvents.size())
    {
        return nullptr;
    }
    return &mEvents[it->second];
}

// Index of the first event at or after t.
size_t SSCombatLog::eventIndexAt(F64 t) const
{
    auto it = std::lower_bound(mEvents.begin(), mEvents.end(), t,
                               [](const Event& e, F64 v) { return e.mTime < v; });
    return (size_t)(it - mEvents.begin());
}

// ---------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------

// Adds one position sample, decimating still viewer samples and keeping the track sorted.
void SSCombatLog::addSample(const LLUUID& id, bool isAgent, const Sample& sample)
{
    if (id.isNull())
    {
        return;
    }
    if (mSessionStart <= 0.0)
    {
        mSessionStart = now() - (F64)sample.mTime;
    }
    Track& track = mTracks[id];
    track.mId = id;
    track.mIsAgent = isAgent;

    if (!track.mSamples.empty() && sample.mSource == SAMPLE_VIEWER)
    {
        const Sample& last = track.mSamples.back();
        const F32 dt = sample.mTime - last.mTime;
        if (dt >= 0.f && dt < 1.f
            && last.mFlags == sample.mFlags
            && last.mParent == sample.mParent
            && (sample.mPos - last.mPos).lengthSquared() < 0.0001f
            && sample.mVel.lengthSquared() < 0.0001f
            && last.mVel.lengthSquared() < 0.0001f)
        {
            return; // a standing avatar does not need 10 samples a second
        }
    }

    if (track.mSamples.empty() || track.mSamples.back().mTime <= sample.mTime)
    {
        track.mSamples.push_back(sample);
    }
    else
    {
        auto it = std::upper_bound(track.mSamples.begin(), track.mSamples.end(), sample.mTime,
                                   [](F32 t, const Sample& s) { return t < s.mTime; });
        track.mSamples.insert(it, sample);
    }

    const F64 abs_time = mSessionStart + (F64)sample.mTime;
    if (abs_time > mLastDataTime)
    {
        mLastDataTime = abs_time;
    }
    mDirty = true;
}

// The track for an id, or null.
const Track* SSCombatLog::track(const LLUUID& id) const
{
    auto it = mTracks.find(id);
    return it == mTracks.end() ? nullptr : &it->second;
}

// Interpolated position at t: a nearby bridge sample wins, else Hermite, else linear; a gap over 5 s is unknown.
bool SSCombatLog::sampleAt(const LLUUID& id, F64 t, Sample& out) const
{
    const Track* track_p = track(id);
    if (!track_p || track_p->mSamples.empty())
    {
        return false;
    }
    const std::vector<Sample>& s = track_p->mSamples;
    const F32 st = (F32)(t - mSessionStart);

    auto it = std::lower_bound(s.begin(), s.end(), st,
                               [](const Sample& a, F32 v) { return a.mTime < v; });
    const size_t hi = (size_t)(it - s.begin());
    const size_t lo = hi ? hi - 1 : 0;

    // A bridge sample inside a quarter second is ground truth; take it as it stands.
    const size_t scan_first = lo > 2 ? lo - 2 : 0;
    const size_t scan_last  = llmin(s.size() - 1, hi + 2);
    for (size_t i = scan_first; i <= scan_last; ++i)
    {
        if (s[i].mSource == SAMPLE_BRIDGE && std::fabs(s[i].mTime - st) <= 0.25f)
        {
            out = s[i];
            return true;
        }
    }

    if (hi == 0)
    {
        if (s.front().mTime - st > 5.f) return false;
        out = s.front();
        return true;
    }
    if (hi >= s.size())
    {
        if (st - s.back().mTime > 5.f) return false;
        out = s.back();
        return true;
    }

    const Sample& a = s[lo];
    const Sample& b = s[hi];
    const F32 span = b.mTime - a.mTime;
    if (span > 5.f)
    {
        return false; // the track went dark across this moment
    }
    const F32 u = span > 0.0001f ? (st - a.mTime) / span : 0.f;

    out = (u < 0.5f) ? a : b;
    out.mTime = st;
    out.mQuality = llmin(a.mQuality, b.mQuality);

    const bool hermite = (st - a.mTime) <= 2.f && (b.mTime - st) <= 2.f
                       && (a.mVel.lengthSquared() > 0.f || b.mVel.lengthSquared() > 0.f);
    if (hermite)
    {
        const F32 h00 = 2.f * u * u * u - 3.f * u * u + 1.f;
        const F32 h10 = u * u * u - 2.f * u * u + u;
        const F32 h01 = -2.f * u * u * u + 3.f * u * u;
        const F32 h11 = u * u * u - u * u;
        out.mPos = a.mPos * h00 + a.mVel * (span * h10) + b.mPos * h01 + b.mVel * (span * h11);
    }
    else
    {
        out.mPos = a.mPos + (b.mPos - a.mPos) * u;
    }
    out.mVel = a.mVel + (b.mVel - a.mVel) * u;
    out.mYaw = ss_lerp_angle(a.mYaw, b.mYaw, u);
    return true;
}

// Records a projectile flight; the store owns it from here.
void SSCombatLog::addFlight(const Flight& flight)
{
    mFlights.push_back(flight);
    if (flight.mEnd > mLastDataTime)
    {
        mLastDataTime = flight.mEnd;
    }
    invalidateAnalysis();
    mDirty = true;
}

// ---------------------------------------------------------------------------
// Equipment
// ---------------------------------------------------------------------------

// Derives the officer-facing family key: maker prefix in brackets plus the stem, version stripped, lowercased.
static std::string ss_family_key(const std::string& name)
{
    std::string prefix, stem(name);
    // Leading "[Ash]" style maker prefix.
    size_t a = stem.find_first_not_of(" \t");
    if (a != std::string::npos && stem[a] == '[')
    {
        const size_t close = stem.find(']', a);
        if (close != std::string::npos)
        {
            prefix = stem.substr(a, close - a + 1);
            stem = stem.substr(close + 1);
        }
    }
    // Separators the community writes between prefix and name.
    size_t b = stem.find_first_not_of(" \t-_");
    stem = (b == std::string::npos) ? std::string() : stem.substr(b);

    // Strip trailing version tokens and any bare trailing numbers, twice so "v 1.04" goes as one.
    for (S32 pass = 0; pass < 3; ++pass)
    {
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t' || stem.back() == '-'
                                 || stem.back() == '_' || stem.back() == '.'))
        {
            stem.pop_back();
        }
        const size_t space = stem.find_last_of(" \t");
        if (space == std::string::npos)
        {
            break;
        }
        if (!ss_is_version_token(std::string_view(stem).substr(space + 1)))
        {
            break;
        }
        stem.erase(space);
    }

    std::string family = prefix;
    if (!family.empty() && !stem.empty())
    {
        family += " ";
    }
    family += stem;
    ss_lower(family);
    return family;
}

// Merges partial facts about an object into the registry; a known field is never overwritten with an unknown one.
void SSCombatLog::noteEquipment(const Equipment& facts)
{
    if (facts.mId.isNull())
    {
        return;
    }
    Equipment& eq = mEquipment[facts.mId];
    const bool fresh = eq.mId.isNull();
    eq.mId = facts.mId;
    if (eq.mRezzer.isNull())  eq.mRezzer = facts.mRezzer;
    if (eq.mRoot.isNull())    eq.mRoot = facts.mRoot;
    if (eq.mCreator.isNull()) eq.mCreator = facts.mCreator;
    if (eq.mOwner.isNull())   eq.mOwner = facts.mOwner;
    if (eq.mGroup.isNull())   eq.mGroup = facts.mGroup;
    if (!facts.mName.empty() && facts.mName != eq.mName)
    {
        eq.mName = facts.mName;
        eq.mFamily = ss_family_key(eq.mName);
    }
    if (eq.mAttachPoint < 0 && facts.mAttachPoint >= 0)
    {
        eq.mAttachPoint = facts.mAttachPoint;
    }
    if (eq.mKind == EQUIP_UNKNOWN)
    {
        eq.mKind = facts.mKind;
    }
    if (facts.mConfidence > eq.mConfidence)
    {
        eq.mConfidence = facts.mConfidence;
    }
    const F64 seen = facts.mFirstSeen > 0.0 ? facts.mFirstSeen : now();
    if (fresh || eq.mFirstSeen <= 0.0 || seen < eq.mFirstSeen)
    {
        eq.mFirstSeen = seen;
    }
    const F64 last = facts.mLastSeen > 0.0 ? facts.mLastSeen : seen;
    if (last > eq.mLastSeen)
    {
        eq.mLastSeen = last;
    }
    if (facts.mHasLastPos)
    {
        eq.mLastPos = facts.mLastPos;
        eq.mHasLastPos = true;
    }
    mDirty = true;
}

// The registry entry for an object, or null.
const Equipment* SSCombatLog::equipmentFor(const LLUUID& id) const
{
    auto it = mEquipment.find(id);
    return it == mEquipment.end() ? nullptr : &it->second;
}

// Replaces the army registry.
void SSCombatLog::setGroups(const std::vector<GroupInfo>& groups)
{
    mGroups = groups;
    invalidateAnalysis();
    mDirty = true;
}

// Replaces the region registry.
void SSCombatLog::setRegions(const std::vector<RegionInfo>& regions)
{
    mRegions = regions;
    mDirty = true;
}

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------

// Adds one (frame, viewer time) pair, collapsing the previous point when the fit stays straight through it.
void SSCombatLog::noteFrameSample(U32 frame, F64 time)
{
    if (frame == 0 || time <= 0.0)
    {
        return;
    }
    if (!mFrameFit.empty())
    {
        if (frame <= mFrameFit.back().first)
        {
            return; // repeats and out-of-order polls say nothing new
        }
        if (mFrameFit.size() >= 2)
        {
            const std::pair<U32, F64>& prev = mFrameFit[mFrameFit.size() - 2];
            const std::pair<U32, F64>& last = mFrameFit.back();
            const F64 span = (F64)(frame - prev.first);
            if (span > 0.0)
            {
                const F64 slope = (time - prev.second) / span;
                const F64 pred = prev.second + slope * (F64)(last.first - prev.first);
                if (std::fabs(pred - last.second) < 0.022)
                {
                    mFrameFit.pop_back(); // no break here: one straight segment covers all three
                }
            }
        }
    }
    mFrameFit.emplace_back(frame, time);
    if (mFrameFit.size() > 4096)
    {
        mFrameFit.erase(mFrameFit.begin(), mFrameFit.begin() + 1024);
    }
}

// Maps a simulator frame onto viewer seconds; outside the fit it extrapolates at the nearest segment's slope.
F64 SSCombatLog::frameToTime(U32 frame) const
{
    if (mFrameFit.empty())
    {
        return 0.0;
    }
    const F64 nominal = 1.0 / 45.0;
    if (mFrameFit.size() == 1)
    {
        return mFrameFit[0].second + (F64)((S64)frame - (S64)mFrameFit[0].first) * nominal;
    }
    if (frame <= mFrameFit.front().first)
    {
        const std::pair<U32, F64>& a = mFrameFit[0];
        const std::pair<U32, F64>& b = mFrameFit[1];
        const F64 slope = (b.first > a.first) ? (b.second - a.second) / (F64)(b.first - a.first) : nominal;
        return a.second + (F64)((S64)frame - (S64)a.first) * slope;
    }
    if (frame >= mFrameFit.back().first)
    {
        const std::pair<U32, F64>& a = mFrameFit[mFrameFit.size() - 2];
        const std::pair<U32, F64>& b = mFrameFit.back();
        const F64 slope = (b.first > a.first) ? (b.second - a.second) / (F64)(b.first - a.first) : nominal;
        return b.second + (F64)((S64)frame - (S64)b.first) * slope;
    }
    auto it = std::lower_bound(mFrameFit.begin(), mFrameFit.end(), frame,
                               [](const std::pair<U32, F64>& p, U32 f) { return p.first < f; });
    const size_t hi = (size_t)(it - mFrameFit.begin());
    const std::pair<U32, F64>& a = mFrameFit[hi ? hi - 1 : 0];
    const std::pair<U32, F64>& b = mFrameFit[hi];
    if (b.first == a.first)
    {
        return b.second;
    }
    const F64 u = (F64)(frame - a.first) / (F64)(b.first - a.first);
    return a.second + (b.second - a.second) * u;
}

// Inverse of frameToTime over the same piecewise fit.
U32 SSCombatLog::timeToFrame(F64 time) const
{
    if (mFrameFit.empty())
    {
        return 0;
    }
    const F64 nominal = 1.0 / 45.0;
    if (mFrameFit.size() == 1 || time <= mFrameFit.front().second)
    {
        const std::pair<U32, F64>& a = mFrameFit.front();
        F64 slope = nominal;
        if (mFrameFit.size() > 1 && mFrameFit[1].first > a.first)
        {
            slope = (mFrameFit[1].second - a.second) / (F64)(mFrameFit[1].first - a.first);
        }
        if (slope <= 0.0) slope = nominal;
        const F64 f = (F64)a.first + (time - a.second) / slope;
        return f <= 0.0 ? 0 : (U32)f;
    }
    if (time >= mFrameFit.back().second)
    {
        const std::pair<U32, F64>& a = mFrameFit[mFrameFit.size() - 2];
        const std::pair<U32, F64>& b = mFrameFit.back();
        F64 slope = (b.first > a.first) ? (b.second - a.second) / (F64)(b.first - a.first) : nominal;
        if (slope <= 0.0) slope = nominal;
        return (U32)((F64)b.first + (time - b.second) / slope);
    }
    auto it = std::lower_bound(mFrameFit.begin(), mFrameFit.end(), time,
                               [](const std::pair<U32, F64>& p, F64 t) { return p.second < t; });
    const size_t hi = (size_t)(it - mFrameFit.begin());
    const std::pair<U32, F64>& a = mFrameFit[hi ? hi - 1 : 0];
    const std::pair<U32, F64>& b = mFrameFit[hi];
    if (b.second <= a.second)
    {
        return b.first;
    }
    const F64 u = (time - a.second) / (b.second - a.second);
    return (U32)((F64)a.first + u * (F64)(b.first - a.first));
}

// The latest data time, or the wall clock while the View is live.
F64 SSCombatLog::sessionEnd() const
{
    if (mView.mLive)
    {
        return llmax(mLastDataTime, now());
    }
    return mLastDataTime > 0.0 ? mLastDataTime : mSessionStart;
}

// The viewer clock every time in this store is on.
F64 SSCombatLog::now() const
{
    return (F64)LLFrameTimer::getElapsedSeconds();
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

// Drops events, samples, flights and raw lines older than the retention window; runs at most every 30 s.
void SSCombatLog::runRetention(F64 nowsec)
{
    static LLCachedControl<S32> minutes(gSavedSettings, "SSCombatLogRetentionMinutes", 180);
    const S32 window = llmax(1, (S32)minutes);
    const F64 cutoff = nowsec - (F64)window * 60.0;
    if (cutoff <= mSessionStart)
    {
        return;
    }

    bool changed = false;

    const size_t drop = eventIndexAt(cutoff);
    if (drop)
    {
        mEvents.erase(mEvents.begin(), mEvents.begin() + (std::ptrdiff_t)drop);
        mEventIndex.clear();
        for (size_t i = 0; i < mEvents.size(); ++i)
        {
            mEventIndex[mEvents[i].mId] = (U32)i;
        }
        changed = true;
    }

    const F32 sample_cutoff = (F32)(cutoff - mSessionStart);
    for (auto it = mTracks.begin(); it != mTracks.end(); )
    {
        std::vector<Sample>& s = it->second.mSamples;
        auto first = std::lower_bound(s.begin(), s.end(), sample_cutoff,
                                      [](const Sample& a, F32 v) { return a.mTime < v; });
        if (first != s.begin())
        {
            s.erase(s.begin(), first);
            changed = true;
        }
        if (s.empty())
        {
            it = mTracks.erase(it);
        }
        else
        {
            ++it;
        }
    }

    const size_t flights_before = mFlights.size();
    mFlights.erase(std::remove_if(mFlights.begin(), mFlights.end(),
                                  [cutoff](const Flight& f) { return f.mEnd < cutoff; }),
                   mFlights.end());
    changed = changed || mFlights.size() != flights_before;

    size_t raw_drop = 0;
    while (raw_drop < mRawLines.size() && mRawLines[raw_drop].mReceived < cutoff)
    {
        ++raw_drop;
    }
    if (raw_drop)
    {
        // Compacting the arena keeps a long session from growing without bound.
        std::string arena;
        arena.reserve(mRawArena.size());
        std::vector<RawLine> kept;
        kept.reserve(mRawLines.size() - raw_drop);
        for (size_t i = raw_drop; i < mRawLines.size(); ++i)
        {
            RawLine line = mRawLines[i];
            const std::string_view text = rawText(line);
            line.mOffset = (U32)arena.size();
            line.mLength = (U32)text.size();
            arena.append(text.data(), text.size());
            kept.push_back(line);
        }
        mRawArena.swap(arena);
        mRawLines.swap(kept);
        changed = true;
    }

    if (changed)
    {
        invalidateAnalysis();
        mDirty = true;
    }
}

// The verbatim text of one received line.
std::string_view SSCombatLog::rawText(const RawLine& line) const
{
    if ((size_t)line.mOffset + (size_t)line.mLength > mRawArena.size())
    {
        return std::string_view();
    }
    return std::string_view(mRawArena).substr(line.mOffset, line.mLength);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// Synthetic names first, then a cached display name, then the object's own name, then a short key.
std::string SSCombatLog::displayName(const LLUUID& id) const
{
    if (id.isNull())
    {
        return "(nobody)";
    }
    auto it = mNames.find(id);
    if (it != mNames.end())
    {
        return it->second;
    }
    LLAvatarName av;
    if (LLAvatarNameCache::get(id, &av))
    {
        const std::string name = av.getDisplayName();
        if (!name.empty())
        {
            return name;
        }
    }
    auto eq = mEquipment.find(id);
    if (eq != mEquipment.end() && !eq->second.mName.empty())
    {
        return eq->second.mName;
    }
    return id.asString().substr(0, 8);
}

// ---------------------------------------------------------------------------
// Analysis pass-through (sscombatanalysis.cpp)
// ---------------------------------------------------------------------------

// Spatiotemporal clusters of fighting.
const std::vector<Engagement>& SSCombatLog::engagements() const
{
    return mAnalysis->engagements();
}

// Every life this combatant lived in the session.
const std::vector<Life>& SSCombatLog::lives(const LLUUID& combatant) const
{
    return mAnalysis ? mAnalysis->lives(combatant) : sNoLives;
}

// Who gets credit for one death.
Attribution SSCombatLog::attribution(U32 deathEvent) const
{
    return mAnalysis->attribution(deathEvent);
}

// Which side this combatant is on, with confidence.
TeamAssignment SSCombatLog::team(const LLUUID& combatant) const
{
    return mAnalysis->team(combatant);
}

// How many sides the solve found.
S32 SSCombatLog::teamCount() const
{
    return mAnalysis->teamCount();
}

// The label for a side.
std::string SSCombatLog::teamName(S8 team_id) const
{
    return mAnalysis->teamName(team_id);
}

// The colour for a side.
LLColor4 SSCombatLog::teamColor(S8 team_id) const
{
    return mAnalysis->teamColor(team_id);
}

// The owner's own active-combatant test at one moment.
bool SSCombatLog::isCombatantAt(const LLUUID& id, F64 t) const
{
    return mAnalysis->isCombatantAt(id, t);
}

// Everyone who fought.
std::vector<LLUUID> SSCombatLog::combatants() const
{
    return mAnalysis->combatants();
}

// Marks every derived cache dirty.
void SSCombatLog::invalidateAnalysis()
{
    if (mAnalysis)
    {
        mAnalysis->invalidate();
    }
}

// ---------------------------------------------------------------------------
// View and navigation
// ---------------------------------------------------------------------------

// Pushes the current View so a move can be undone (ux 2.5).
void SSCombatLog::pushView()
{
    mViewChain.push_back(mView);
    if (mViewChain.size() > 64)
    {
        mViewChain.erase(mViewChain.begin());
    }
}

// Restores the last pushed View.
bool SSCombatLog::popView()
{
    if (mViewChain.empty())
    {
        return false;
    }
    mView = mViewChain.back();
    mViewChain.pop_back();
    mViewChanged();
    return true;
}

// Appends to the noun chain, collapsing an immediate repeat.
void SSCombatLog::pushNoun(const NounRef& ref)
{
    if (!ref.valid())
    {
        return;
    }
    if (!mNounChain.empty() && mNounChain.back() == ref)
    {
        return;
    }
    mNounChain.push_back(ref);
    if (mNounChain.size() > 64)
    {
        mNounChain.erase(mNounChain.begin());
    }
}

// Steps back one noun.
bool SSCombatLog::popNoun()
{
    if (mNounChain.empty())
    {
        return false;
    }
    mNounChain.pop_back();
    mViewChanged();
    return true;
}

// Selects a noun: pushes the View, records the noun, and optionally moves the cursor to its time.
void SSCombatLog::select(const NounRef& ref, bool moveCursor)
{
    if (!ref.valid())
    {
        return;
    }
    pushView();
    mView.mSelection = ref;
    switch (ref.mType)
    {
        case NOUN_SESSION:
        case NOUN_ENGAGEMENT:
        case NOUN_MOMENT:
        case NOUN_LIFE:
        case NOUN_COMBATANT:
        case NOUN_TEAM:
        case NOUN_SQUAD:
            mView.mSubject = ref;
            break;
        default:
            break;
    }
    pushNoun(ref);
    if (moveCursor && ref.mTime > 0.0)
    {
        mView.mCursor = ref.mTime;
        mView.mLive = false;
        mView.mPlaying = false;
    }
    mViewChanged();
}

// Changes level, keeping the subject; pushes the View first so the rail's back arrow works.
void SSCombatLog::setLevel(S8 level)
{
    if (mView.mLevel == level)
    {
        return;
    }
    pushView();
    mView.mLevel = level;
    mViewChanged();
}

// Enters the ghosted replay of one death (ux 3.10).
void SSCombatLog::enterReconstruction(U32 deathEvent)
{
    const Event* ev = event(deathEvent);
    if (!ev)
    {
        LL_WARNS("CombatLog") << "reconstruction asked for event " << deathEvent << " which is gone" << LL_ENDL;
        return;
    }
    static LLCachedControl<F32> before(gSavedSettings, "SSCombatLogReconBefore", 6.f);
    static LLCachedControl<F32> after(gSavedSettings, "SSCombatLogReconAfter", 2.f);
    pushView();
    mView.mReconstruct = true;
    mView.mReconStart = ev->mTime - (F64)llmax(0.5f, (F32)before);
    mView.mReconEnd   = ev->mTime + (F64)llmax(0.f, (F32)after);
    mView.mCursor = mView.mReconStart;
    mView.mLive = false;
    mView.mPlaying = true;
    mView.mLevel = LEVEL_MOMENT;

    NounRef ref;
    ref.mType = NOUN_DEATH;
    ref.mIndex = deathEvent;
    ref.mTime = ev->mTime;
    ref.mId = ev->mTarget;
    mView.mSubject = ref;
    mView.mSelection = ref;
    pushNoun(ref);
    mViewChanged();
}

// Leaves reconstruction and stops the replay where it stands.
void SSCombatLog::leaveReconstruction()
{
    if (!mView.mReconstruct)
    {
        return;
    }
    mView.mReconstruct = false;
    mView.mPlaying = false;
    mViewChanged();
}

// ---------------------------------------------------------------------------
// Synthetic session
// ---------------------------------------------------------------------------

// Builds the deterministic scripted raid; live replays it in real time through the same ingest path.
void SSCombatLog::loadSynthetic(U32 seed, bool live)
{
    clear();
    mSynthetic = true;
    mRecording = true;

    SSCombatSynth* synth = new SSCombatSynth(*this, seed, live);
    mSessionStart = live ? now() : (now() - synth->duration());
    mSynth = synth;

    if (live)
    {
        mView.mLive = true;
        mView.mPlaying = false;
        mView.mCursor = mSessionStart;
        synth->idle(now());
    }
    else
    {
        synth->runBulk();
        mView.mLive = false;
        mView.mPlaying = false;
        mView.mCursor = mSessionStart;
    }
    mView.mLevel = LEVEL_SESSION;

    NounRef ref;
    ref.mType = NOUN_SESSION;
    mView.mSubject = ref;

    invalidateAnalysis();
    mDirty = true;
    mDataChanged();
    mViewChanged();
}
