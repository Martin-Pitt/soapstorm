/**
 * @file sscombatlog.h
 * @brief Combat Log store: Combat 2.0 events, combatant tracks, equipment, projectile flights, region combat
 *        settings, the shared playback/navigation View, and the Stage 0 analysis contract. Design and rationale:
 *        doc/combat_log_ux.md, doc/combat_log_analysis.md, doc/combat_log_glossary.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATLOG_H
#define SS_COMBATLOG_H

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"
#include "v4color.h"

#include <boost/signals2.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class LLViewerObject;

namespace SSCombat
{
    // Combat log event kinds; CUSTOM covers any foreign schema the parser could not type.
    enum EEventKind : U8 { EVENT_DAMAGE = 0, EVENT_DEATH, EVENT_OBJECT_DEATH, EVENT_CUSTOM };
    // Who produced the line: the simulator (COMBAT_LOG_ID), a foreign script (tagged by the bridge), or the synthetic generator.
    enum ETrust : U8 { TRUST_SIM = 0, TRUST_FOREIGN, TRUST_SYNTHETIC };
    // How the damage was delivered, inferred per hit (doc/combat_log_analysis.md 5.5).
    enum EDelivery : U8 { DELIVERY_UNKNOWN = 0, DELIVERY_HITSCAN, DELIVERY_PROJECTILE, DELIVERY_LOBBED, DELIVERY_AREA };
    // Where a track sample came from; BRIDGE samples are simulator ground truth and win ties.
    enum ESampleSource : U8 { SAMPLE_VIEWER = 0, SAMPLE_COARSE, SAMPLE_BRIDGE, SAMPLE_SYNTHETIC };
    // llGetAgentInfo-style flags plus viewer-derived ones.
    enum ESampleFlags : U16
    {
        FLAG_MOUSELOOK = 1 << 0, FLAG_SITTING = 1 << 1, FLAG_ON_OBJECT = 1 << 2, FLAG_FLYING = 1 << 3,
        FLAG_WALKING = 1 << 4, FLAG_IN_AIR = 1 << 5, FLAG_CROUCHING = 1 << 6, FLAG_TELEPORT = 1 << 7 /* sample follows an unexplained 8-24 m jump */
    };
    enum EEquipmentKind : U8 { EQUIP_UNKNOWN = 0, EQUIP_WEAPON, EQUIP_PROJECTILE, EQUIP_HUD, EQUIP_DEPLOYABLE, EQUIP_VEHICLE, EQUIP_MOUNT };
    // The one confidence ladder used everywhere in UI copy (glossary part 3).
    enum EConfidence : U8 { CONF_UNDETERMINED = 0, CONF_WEAK, CONF_LIKELY, CONF_FIRM };
    // Region types from the owner's registry schema.
    enum ERegionType : U8 { REGION_BASE = 0, REGION_CONTINENT, REGION_NEUTRAL };

    // One damage-adjustment step from the log's modifications[] array.
    struct Modification
    {
        LLUUID      mTaskId;
        std::string mScript;
        F32         mNewDamage = 0.f;
    };

    // One typed combat event. Times are viewer seconds (LLFrameTimer::getElapsedSeconds()); mFrame is the simulator frame stamp when known.
    struct Event
    {
        U32         mId = 0;
        F64         mTime = 0.0;            // best estimate of when the hit happened, viewer clock
        F64         mReceived = 0.0;        // when the line arrived
        U32         mFrame = 0;             // simulator frame number, 0 when unknown
        F32         mUncertainty = 1.f;     // seconds of doubt on mTime
        U8          mKind = EVENT_DAMAGE;
        U8          mTrust = TRUST_SIM;
        U8          mDelivery = DELIVERY_UNKNOWN;
        bool        mTargetIsAgent = true;
        bool        mHasPositions = false;  // DEATH carries positions, DAMAGE does not
        bool        mFromFinalDamage = false;
        S16         mType = 0;              // damage type; -1 impact
        F32         mDamage = 0.f;
        F32         mInitial = 0.f;         // before adjustment, equals mDamage when no modifications
        LLUUID      mOwner, mRezzer, mSource, mTarget;
        LLVector3   mSourcePos, mTargetPos; // region-local, valid when mHasPositions
        U32         mModsBegin = 0, mModsCount = 0; // slice of SSCombatLog::modifications()
        U32         mRawLine = 0;           // index into raw lines
        LLUUID      mReporter, mReporterOwner; // foreign lines only
    };

    // One position sample on a track. mTime is viewer seconds relative to the session start (F32 keeps 36 bytes).
    struct Sample
    {
        F32         mTime = 0.f;
        LLVector3   mPos;                   // region-local
        LLVector3   mVel;
        F32         mYaw = 0.f;             // radians, world Z
        U16         mFlags = 0;             // ESampleFlags
        U8          mSource = SAMPLE_VIEWER;
        U8          mQuality = 255;         // 0..255, COARSE samples are low
        LLUUID      mParent;                // root object when seated, null otherwise
    };

    struct Track
    {
        LLUUID              mId;
        bool                mIsAgent = true;
        std::vector<Sample> mSamples;       // sorted by mTime
    };

    // What is known about an object that appeared as source or rezzer (or a vehicle root). Partial by nature.
    struct Equipment
    {
        LLUUID      mId, mRezzer, mRoot, mCreator, mOwner, mGroup;
        std::string mName;                  // as seen
        std::string mFamily;                // maker prefix + stem, version stripped (analysis 5.13)
        S32         mAttachPoint = -1;      // -1 unknown, 0 not attached, 31..38 HUD
        U8          mKind = EQUIP_UNKNOWN;
        U8          mConfidence = CONF_UNDETERMINED;
        F64         mFirstSeen = 0.0, mLastSeen = 0.0;
        LLVector3   mLastPos;
        bool        mHasLastPos = false;
    };

    // A projectile flight seen by the viewer's ghost-projectile heuristic (strong secondary evidence, never primary).
    struct Flight
    {
        LLUUID      mObject;
        F64         mStart = 0.0, mEnd = 0.0;
        std::vector<std::pair<F32, LLVector3>> mPath; // (session seconds, region-local position)
        LLUUID      mShooter;               // inferred from the spawn point vs tracks
        U8          mShooterConfidence = CONF_UNDETERMINED;
        bool        mHitGeometry = false;
        U32         mDamageEvent = 0;       // 0 when no DAMAGE was matched
    };

    // Region combat settings reported through llGetEnv by the bridge (or the synthetic generator).
    struct RegionSettings
    {
        bool        mKnown = false;
        U32         mRegionFlags = 0;       // llGetRegionFlags(), reported beside the llGetEnv values
        bool        mAllowDamageAdjust = true;
        bool        mRestrictCombatLog = false;
        bool        mRestoreHealth = true;
        F32         mDamageThrottle = 0.f;
        F32         mDamageLimit = 0.f;
        F32         mHealthRegenRate = 1.f / 6.f;
        F32         mInvulnerabilityTime = 0.f;
        S32         mDeathAction = 0;       // 0 home, 1 parcel landing, 2 telehub, 3 no action
        S32         mAgentLimit = 0;
    };

    // One received line kept verbatim for the Raw level (doc/combat_log_ux.md 1.1).
    struct RawLine
    {
        F64         mReceived = 0.0;
        U32         mFrame = 0;
        U32         mOffset = 0, mLength = 0; // slice of SSCombatLog::rawArena()
        U8          mTrust = TRUST_SIM;
        U8          mSalvageMask = 0;       // bit i set when array element i was dropped by salvage
        U32         mFirstEvent = 0, mEventCount = 0;
    };

    // Owner's registry schema: armies {g, m?, c?, l?, n, s} and regions {n, t, g?, l?}.
    struct GroupInfo
    {
        LLUUID      mGroup, mMilitia, mCivilians, mLand;
        std::string mName, mShort;
    };
    struct RegionInfo
    {
        std::string mName;
        U8          mType = REGION_BASE;
        LLUUID      mGroup, mLand;
    };

    // Navigation levels; UI copy uses the plain names (Raw, Moment, Engagement, Sweep, Comparison, Session), never "rung".
    enum ELevel : S8 { LEVEL_RAW = -1, LEVEL_MOMENT = 0, LEVEL_ENGAGEMENT = 1, LEVEL_SWEEP = 2, LEVEL_COMPARISON = 3, LEVEL_SESSION = 4 };
    // Page/noun types; every one is a linkable page with backlinks.
    enum ENounType : U8
    {
        NOUN_NONE = 0, NOUN_SESSION, NOUN_ENGAGEMENT, NOUN_MOMENT, NOUN_LIFE, NOUN_COMBATANT, NOUN_TEAM, NOUN_SQUAD,
        NOUN_DEATH, NOUN_DAMAGE, NOUN_VOLLEY, NOUN_EQUIPMENT, NOUN_FAMILY, NOUN_OBJECT, NOUN_ADJUSTMENT, NOUN_ZONE,
        NOUN_SIGHTLINE, NOUN_CLAIM, NOUN_RULE, NOUN_SUMMARY, NOUN_CASE, NOUN_LOGLINE
    };

    // A reference to one noun instance: UUID-keyed nouns use mId, session-local ones use mIndex, time-keyed ones use mTime.
    struct NounRef
    {
        U8          mType = NOUN_NONE;
        LLUUID      mId;
        U32         mIndex = 0;
        F64         mTime = 0.0;
        bool        valid() const { return mType != NOUN_NONE; }
        bool        operator==(const NounRef& o) const { return mType == o.mType && mId == o.mId && mIndex == o.mIndex && mTime == o.mTime; }
        // ss:<type>/<id-or-index>[@t]
        std::string address() const;
        static NounRef parse(std::string_view address);
        // Human label for breadcrumbs and pick lists, e.g. "DEATH Vex <- Cadmus".
        std::string label() const;
    };

    // Shared playback and navigation state: the floater edits it, the overlay and reconstruction read it.
    struct View
    {
        F64         mCursor = 0.0;          // viewer seconds
        F32         mWindow = 20.f;         // seconds of trail behind the cursor
        bool        mLive = true;
        bool        mPlaying = false;
        F32         mSpeed = 1.f;
        S8          mLevel = LEVEL_SESSION;
        NounRef     mSubject;               // what the current level is about
        NounRef     mSelection;             // what the officer last clicked
        NounRef     mHover;                 // what the pointer is over right now (list row or world icon); transient, no signal
        bool        mReconstruct = false;   // reconstruction mode (ux 3.10)
        F64         mReconStart = 0.0, mReconEnd = 0.0;
        bool        mXray = false;          // overlay draws through walls
    };

    // ----- Stage 0 analysis contract (simple implementations in sscombatanalysis.cpp, replaced in Stage 2) -----

    // One life: from spawn (or first sight) to a DEATH, per combatant.
    struct Life
    {
        LLUUID      mCombatant;
        F64         mStart = 0.0, mEnd = 0.0;
        U32         mDeathEvent = 0;        // 0 while alive
        bool        mTeleportSeen = false;  // an unexplained jump followed the death
    };

    // Who gets credit for a death; ambiguous pairs split credit (analysis 5.6).
    enum EBlowState : U8 { BLOW_UNKNOWN = 0, BLOW_UNAMBIGUOUS, BLOW_AMBIGUOUS_PAIR };
    struct VolleyShare
    {
        LLUUID      mAttacker;
        LLUUID      mRezzer;
        F32         mDamage = 0.f;
        F32         mCredit = 0.f;          // 0..1
        U32         mFirstEvent = 0, mLastEvent = 0;
    };
    struct Attribution
    {
        U32                     mDeath = 0;
        U32                     mBlow = 0, mBlowAlt = 0;
        U8                      mBlowState = BLOW_UNKNOWN;
        F32                     mWindow = 20.f;
        std::vector<VolleyShare> mShares;   // ranked by damage
    };

    // A spatiotemporal cluster of fighting.
    struct Engagement
    {
        U32                 mId = 0;
        F64                 mStart = 0.0, mEnd = 0.0;
        LLVector3           mCentre;
        F32                 mRadius = 0.f;
        std::vector<U32>    mEvents;
        std::vector<LLUUID> mCombatants;
        std::string         mLabel;         // "Engagement 3", never a coined name
    };

    // Team assignment with confidence; -1 = unassigned.
    struct TeamAssignment
    {
        S8          mTeam = -1;
        U8          mConfidence = CONF_UNDETERMINED;
    };
}

class SSCombatLog : public LLSingleton<SSCombatLog>
{
    LLSINGLETON(SSCombatLog);
    ~SSCombatLog();

public:
    typedef boost::signals2::signal<void()> signal_t;

    // ----- lifecycle -----
    // Master setting SSCombatLogEnabled and a live bridge (or synthetic feed) make this true.
    bool isEnabled() const;
    bool isRecording() const { return mRecording; }
    void setRecording(bool on);
    // Drops every event, track, flight and raw line; keeps registry data and settings.
    void clear();
    // Per-frame tick from idle: advances playback, replays the synthetic feed, runs retention.
    void idle(F64 now);

    // ----- ingest (bridge relay strings; the synthetic generator produces the same strings) -----
    // The text after the "C2" prefix of a relayed owner-say: "<frame>|" then a JSON array/object, or 72 key chars then foreign text.
    void ingestRelay(std::string_view payload, F64 received);
    // The body of a GetTrackedAgents reply (TA|K|T lines) and the round-trip time of the request.
    void ingestTrackReply(std::string_view body, F64 received, F64 rtt);
    // The region settings block "RS|allow_damage_adjust,restrict_combat_log,damage_throttle,damage_limit,restore_health,health_regen_rate,invulnerability_time,death_action,agent_limit".
    void ingestRegionSettings(std::string_view body);
    // A position sample from the viewer's own object updates or coarse locations (Stage 1) or the generator.
    void addSample(const LLUUID& id, bool isAgent, const SSCombat::Sample& sample);
    // A projectile flight (Stage 1 from the ghost-projectile heuristic, Stage 0 from the generator).
    void addFlight(const SSCombat::Flight& flight);
    // Merge facts about an object into the equipment registry (any field may be unknown).
    void noteEquipment(const SSCombat::Equipment& facts);
    // Registry data (owner's slmc-data schema), optional.
    void setGroups(const std::vector<SSCombat::GroupInfo>& groups);
    void setRegions(const std::vector<SSCombat::RegionInfo>& regions);

    // ----- clocks -----
    // Records one (frame, viewer time) pair from a tick or a poll; the fit is piecewise linear.
    void noteFrameSample(U32 frame, F64 time);
    F64 frameToTime(U32 frame) const;
    U32 timeToFrame(F64 time) const;
    F64 sessionStart() const { return mSessionStart; }
    F64 sessionEnd() const;   // latest data time, or now when live
    F64 now() const;          // viewer seconds

    // ----- queries -----
    const std::vector<SSCombat::Event>& events() const { return mEvents; }
    const SSCombat::Event* event(U32 id) const;
    // Index of the first event with mTime >= t.
    size_t eventIndexAt(F64 t) const;
    const std::vector<SSCombat::Modification>& modifications() const { return mModifications; }
    const std::unordered_map<LLUUID, SSCombat::Track>& tracks() const { return mTracks; }
    const SSCombat::Track* track(const LLUUID& id) const;
    // Interpolated sample at time t; false when the track has no data within 5 s of t.
    bool sampleAt(const LLUUID& id, F64 t, SSCombat::Sample& out) const;
    const std::unordered_map<LLUUID, SSCombat::Equipment>& equipment() const { return mEquipment; }
    const SSCombat::Equipment* equipmentFor(const LLUUID& id) const;
    const std::vector<SSCombat::Flight>& flights() const { return mFlights; }
    const std::vector<SSCombat::RawLine>& rawLines() const { return mRawLines; }
    std::string_view rawText(const SSCombat::RawLine& line) const;
    const SSCombat::RegionSettings& regionSettings() const { return mRegionSettings; }
    const std::vector<SSCombat::GroupInfo>& groups() const { return mGroups; }
    const std::vector<SSCombat::RegionInfo>& regions() const { return mRegions; }
    // Display name for an avatar or object; synthetic sessions carry their own names.
    std::string displayName(const LLUUID& id) const;
    // True when the id is one of the synthetic session's avatars (so names/tracks are ours).
    bool isSynthetic() const { return mSynthetic; }

    // ----- Stage 0 analysis contract (sscombatanalysis.cpp) -----
    const std::vector<SSCombat::Engagement>& engagements() const;
    const std::vector<SSCombat::Life>& lives(const LLUUID& combatant) const;
    SSCombat::Attribution attribution(U32 deathEvent) const;
    SSCombat::TeamAssignment team(const LLUUID& combatant) const;
    S32 teamCount() const;
    std::string teamName(S8 team) const;
    LLColor4 teamColor(S8 team) const;
    // Active-combatant test from the owner's tracker: recent damage, or mouselook/seated on a damage-enabled parcel.
    bool isCombatantAt(const LLUUID& id, F64 t) const;
    std::vector<LLUUID> combatants() const;
    // Marks analysis caches dirty; called by ingest.
    void invalidateAnalysis();

    // ----- shared view and navigation -----
    SSCombat::View& view() { return mView; }
    const SSCombat::View& view() const { return mView; }
    // Push the current View before a move (ux 2.5); pop restores it.
    void pushView();
    bool popView();
    // Noun chain: what the officer has been reading about.
    void pushNoun(const SSCombat::NounRef& ref);
    bool popNoun();
    const std::vector<SSCombat::NounRef>& nounChain() const { return mNounChain; }
    // Select a noun (pushes View), optionally moving the cursor to its time.
    void select(const SSCombat::NounRef& ref, bool moveCursor);
    // The event ids the Events floater currently shows in its viewport; the overlay draws exactly these (floater writes, overlay reads).
    void setVisibleEvents(const std::vector<U32>& ids) { mVisibleEvents = ids; }
    const std::vector<U32>& visibleEvents() const { return mVisibleEvents; }
    void setLevel(S8 level);
    // Enter/leave reconstruction of a DEATH event (ux 3.10).
    void enterReconstruction(U32 deathEvent);
    void leaveReconstruction();

    // ----- synthetic session (Stage 0 preview) -----
    // Builds a deterministic scripted raid around the agent's position; live=true replays it in real time through ingest.
    void loadSynthetic(U32 seed, bool live);

    // Fired when data changed (throttled to once per frame) and when the View changed.
    signal_t& dataChangedSignal() { return mDataChanged; }
    signal_t& viewChangedSignal() { return mViewChanged; }
    void notifyViewChanged() { mViewChanged(); }

private:
    friend class SSCombatSynth;
    friend class SSCombatAnalysis;

    void appendEvent(SSCombat::Event& ev);
    void appendRawLine(std::string_view text, F64 received, U32 frame, U8 trust, U8 salvage, U32 firstEvent, U32 count);
    void dedupFinalDamage(SSCombat::Event& ev);
    void runRetention(F64 now);

    bool                                            mRecording = false;
    bool                                            mSynthetic = false;
    F64                                             mSessionStart = 0.0;
    F64                                             mLastDataTime = 0.0;
    F64                                             mLastIdle = 0.0;        // previous idle() stamp, for the playback dt
    F64                                             mLastRetention = 0.0;   // retention runs at most every 30 s
    bool                                            mDirty = false;
    U32                                             mNextEventId = 0;       // event ids are 1-based and never reused
    std::unordered_map<U32, U32>                    mEventIndex;            // event id -> index into mEvents
    std::vector<LLUUID>                             mTrackKeys;             // the tick reply's K| index table
    std::vector<SSCombat::Event>                    mEvents;
    std::vector<SSCombat::Modification>             mModifications;
    std::unordered_map<LLUUID, SSCombat::Track>     mTracks;
    std::unordered_map<LLUUID, SSCombat::Equipment> mEquipment;
    std::vector<SSCombat::Flight>                   mFlights;
    std::vector<SSCombat::RawLine>                  mRawLines;
    std::string                                     mRawArena;
    SSCombat::RegionSettings                        mRegionSettings;
    std::vector<SSCombat::GroupInfo>                mGroups;
    std::vector<SSCombat::RegionInfo>               mRegions;
    std::unordered_map<LLUUID, std::string>         mNames;
    std::vector<std::pair<U32, F64>>                mFrameFit;      // (frame, time), ascending
    std::vector<SSCombat::Event>                    mPendingFinal;  // final_damage records awaiting their log twin
    SSCombat::View                                  mView;
    std::vector<SSCombat::View>                     mViewChain;
    std::vector<SSCombat::NounRef>                  mNounChain;
    std::vector<U32>                                mVisibleEvents;
    signal_t                                        mDataChanged;
    signal_t                                        mViewChanged;
    class SSCombatAnalysis*                         mAnalysis = nullptr;
    class SSCombatSynth*                            mSynth = nullptr;
};

#endif // SS_COMBATLOG_H
