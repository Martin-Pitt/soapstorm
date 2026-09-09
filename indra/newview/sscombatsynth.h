/**
 * @file sscombatsynth.h
 * @brief Deterministic synthetic combat session for the Stage 0 preview. Produces the exact wire strings
 *        script 2 will send and feeds them through SSCombatLog's public ingest, so the parsers are exercised.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATSYNTH_H
#define SS_COMBATSYNTH_H

#include "sscombatlog.h"

#include <string>
#include <unordered_map>
#include <vector>

// A scripted ~20 minute raid around the agent's current position, built once into a time-ordered queue of
// wire messages and then either dispatched all at once (bulk) or drip-fed by idle() (live replay).
// Every number comes from a seeded LCG, so the same seed gives the same raid and screenshots reproduce.
class SSCombatSynth
{
public:
    SSCombatSynth(SSCombatLog& log, U32 seed, bool live);
    ~SSCombatSynth();

    // Session length in seconds; SSCombatLog uses it to place a bulk session in the recent past.
    F64 duration() const { return mDuration; }
    // Dispatches the whole queue with the receive times it was built with.
    void runBulk();
    // Dispatches everything whose receive time has arrived; a no-op in bulk mode.
    void idle(F64 now);
    bool done() const { return mNext >= mQueue.size(); }

private:
    enum EMsgKind : U8 { MSG_SETTINGS = 0, MSG_EQUIP, MSG_TRACK, MSG_SAMPLE, MSG_FLIGHT, MSG_RELAY };

    // One queued wire message or deferred store call; mIndex points into the matching pool.
    struct Msg
    {
        F64         mAt = 0.0;      // session-relative receive time
        U8          mKind = MSG_RELAY;
        U32         mIndex = 0;
        std::string mText;
    };

    // One synthetic fighter.
    struct Person
    {
        LLUUID      mId;
        std::string mName;
        S8          mSide = 0;      // 0 side A, 1 side B, 2 the militia straggler
        U8          mWeapon = 0;    // EWeapon
        LLUUID      mWeaponId;      // the attached weapon or the HUD
        LLVector3   mPos, mVel, mSpawn;
        F32         mYaw = 0.f;
        F32         mLateral = 0.f; // per-person offset across the front, so a side is a line not a point
        F32         mHealth = 100.f;
        U16         mFlags = 0;
        bool        mAlive = true;
        F64         mRespawnAt = 0.0;
        F64         mLastShot = 0.0;
        S32         mSeat = -1;     // crew seat on the vehicle, -1 when on foot
    };

    void buildScenario();
    void step(F64 t, U32 frame);
    void fire(Person& shooter, Person& victim, F64 t, U32 frame);
    void applyDamage(Person& shooter, Person& victim, F32 raw, S16 type, const LLUUID& source,
                     const LLUUID& rezzer, F64 t, U32 frame, bool splash);
    void kill(Person& shooter, Person& victim, const LLUUID& source, const LLUUID& rezzer,
              S16 type, F32 damage, F64 t, U32 frame, bool teleport);
    void runCues(F64 t, U32 frame);
    void flushEvents(F64 t, U32 frame);
    void emitTick(F64 t, U32 frame);
    void queueSample(const LLUUID& id, bool isAgent, const SSCombat::Sample& s, F64 at);
    void dispatch(const Msg& msg);

    // Deterministic noise; std::random_device would make screenshots unrepeatable.
    U32 next();
    F32 frand();
    F32 range(F32 lo, F32 hi);
    LLUUID makeId();

    // The damage type for one shot: the weapon's usual type almost every time, with an occasional neighbour so
    // a raid's numbers are not perfectly monochrome (owner request 2026-09-09).
    S16 damageTypeFor(U8 weapon);

    // Simulator frame counter with a dilation dip in the middle of the fight.
    U32 frameAt(F64 t) const;
    // Ground height under a point, memoised on a 4 m grid; the officer's own height where no land is loaded.
    F32 ground(const LLVector3& p) const;
    LLVector3 clampToRegion(const LLVector3& p) const;
    Person* nearestEnemy(const Person& from, F32 maxRange);

    SSCombatLog&                mLog;
    U32                         mState = 1;
    bool                        mLive = true;
    F64                         mDuration = 1200.0;
    size_t                      mNext = 0;

    std::vector<Msg>                                mQueue;
    std::vector<SSCombat::Sample>                   mSamplePool;
    std::vector<LLUUID>                             mSampleIds;
    std::vector<U8>                                 mSampleIsAgent;
    std::vector<SSCombat::Equipment>                mEquipPool;
    std::vector<SSCombat::Flight>                   mFlightPool;

    std::vector<Person>         mPeople;
    std::vector<std::string>    mPendingEvents;     // JSON objects awaiting their batch
    std::vector<LLUUID>         mTrackKeys;         // index table as the script would build it
    std::vector<LLVector3>      mLastSent;          // last position put on the wire, for the delta encoding
    std::vector<U16>            mLastFlags;

    mutable std::unordered_map<U32, F32> mGroundCache;  // land height per 4 m cell, built once during the raid

    LLVector3                   mBase;              // the officer's position, the scenario is built around it
    LLVector3                   mSpawnA, mSpawnB;
    LLUUID                      mVehicle, mHud, mExperienceOwner, mMortar;
    U32                         mSeq = 0;
    U32                         mCue = 0;
    bool                        mKeysSent = false;
    bool                        mTruncateNext = false;  // the next batch is cut, as llOwnerSay would cut it
};

#endif // SS_COMBATSYNTH_H
