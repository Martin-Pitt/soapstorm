/**
 * @file sscombatsynth.cpp
 * @brief The Stage 0 synthetic raid. Everything the scenario produces goes out as the wire strings script 2
 *        will send and comes back in through SSCombatLog's public ingest, so this is also the parser's test.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatsynth.h"

#include "sscombatanalysis.h"

#include "llagent.h"
#include "llformat.h"
#include "llworld.h"

#include <algorithm>
#include <cmath>

using namespace SSCombat;

namespace
{
    // One phase of the raid. Advance is how far each side has pushed from its own spawn toward the other's,
    // 0 = at home, 1 = standing on the enemy spawn. Keeping the whole shape here makes a beat one line to tweak.
    struct Beat
    {
        F32         mStart, mEnd;
        const char* mName;
        F32         mAdvanceA, mAdvanceB;
        F32         mFireRate;      // multiplier on every weapon's rate of fire
    };

    const Beat SS_BEATS[] =
    {
        {    0.f,   60.f, "staging",  0.02f, 0.02f, 0.02f },   // milling about at the spawn hub
        {   60.f,  300.f, "push",     0.55f, 0.12f, 1.00f },   // side A advances, side B holds
        {  300.f,  420.f, "wave one", 0.45f, 0.30f, 1.15f },   // first respawn wave arrives
        {  420.f,  720.f, "grind",    0.52f, 0.42f, 1.00f },   // the dilation dip sits inside this beat
        {  720.f,  840.f, "wave two", 0.35f, 0.62f, 1.10f },   // second respawn wave, momentum turns
        {  840.f, 1080.f, "collapse", 0.10f, 0.82f, 0.90f },   // side A retreats
        { 1080.f, 1200.f, "mop up",   0.05f, 0.58f, 0.30f }
    };
    constexpr size_t SS_BEAT_COUNT = sizeof(SS_BEATS) / sizeof(SS_BEATS[0]);

    enum EWeapon : U8 { W_RIFLE = 0, W_BOLT, W_MORTAR, W_GRENADE };

    // Rate of fire, reach and bite per weapon, indexed by EWeapon.
    const F32 SS_COOLDOWN[] = { 1.6f,  2.2f,  9.0f, 13.0f };
    const F32 SS_RANGE[]    = { 90.f,  70.f, 130.f,  32.f };
    const F32 SS_DMG_LO[]   = {  9.f,  14.f,  26.f,  22.f };
    const F32 SS_DMG_HI[]   = { 17.f,  23.f,  44.f,  38.f };

    // Damage type per weapon (fscombathitmarker.cpp's DAMAGE_TYPES table): the type the weapon actually implies
    // almost every hit, with an occasional neighbour so a raid's numbers are not perfectly monochrome (owner
    // request 2026-09-09: rifle -> piercing, grenade -> explosive; the mortar shell was already explosive, the
    // grenade was wrongly wired to crushing).
    struct WeaponDamage { S16 mUsual, mOccasional; F32 mOccasionalChance; };
    const WeaponDamage SS_DAMAGE_TYPE[] =
    {
        /* W_RIFLE   */ {   8, 12, 0.15f },   // piercing, sometimes slashing
        /* W_BOLT    */ {   8,  0, 0.10f },   // piercing, sometimes generic
        /* W_MORTAR  */ { 102,  2, 0.10f },   // explosive, sometimes bludgeoning
        /* W_GRENADE */ { 102,  2, 0.10f },   // explosive, sometimes bludgeoning
    };

    // The scripted moments the officer is meant to find.
    enum ECue : U8
    {
        CUE_FD_PAIR = 0, CUE_TRUNCATE, CUE_AMBIGUOUS, CUE_FLASH, CUE_THROUGH_WALL, CUE_SITHACK, CUE_DEATH_NO_TP
    };
    struct Cue { F32 mAt; U8 mWhat; };
    const Cue SS_CUES[] =
    {
        { 182.f, CUE_FD_PAIR },
        { 268.f, CUE_TRUNCATE },
        { 431.f, CUE_AMBIGUOUS },
        { 548.f, CUE_FLASH },
        { 664.f, CUE_THROUGH_WALL },
        { 795.f, CUE_SITHACK },
        { 912.f, CUE_DEATH_NO_TP }
    };
    constexpr size_t SS_CUE_COUNT = sizeof(SS_CUES) / sizeof(SS_CUES[0]);

    // Invented names. None of these is a real person; the maker prefixes are the community's, the names are not.
    const char* const SS_NAMES_A[] =
    {
        "Cadmus Vale", "Iri Sundmark", "Orrin Blackwell", "Tessa Cray",
        "Halden Roe", "Nessa Quill", "Ferro Duquesne", "Brant Mallow"
    };
    const char* const SS_NAMES_B[] =
    {
        "Vex Orlow", "Sable Nix", "Corin Wraye", "Juno Ferrer",
        "Dax Helvig", "Mira Solane", "Anselm Roque"
    };
    const char* const SS_NAME_MILITIA = "Rook Tam";

    constexpr size_t SS_SIDE_A = 8;
    constexpr size_t SS_SIDE_B = 7;
    constexpr size_t SS_PEOPLE = SS_SIDE_A + SS_SIDE_B + 1;
    constexpr size_t SS_ARMOUR_PERSON = 3;      // Tessa Cray wears the 20 % reduction script
    constexpr size_t SS_CREW_A = 6;             // the two side A members riding the APC
    constexpr size_t SS_CREW_B = 7;
    constexpr F32    SS_SPAWN_SHIELD = 15.f;    // the region experience makes this ring immune
    constexpr F32    SS_HEALTH = 160.f;         // one life's worth of hit points
    constexpr F32    SS_HIT_CHANCE = 0.16f;     // only hits reach the combat log, so most windows produce nothing
    constexpr F32    SS_TICK = 0.5f;            // 2 Hz, as the wire spec's default hz

    // The relay's own lag: chat is slower than the HTTP tick, which is why the fit comes from ticks.
    constexpr F64 SS_RELAY_LAG = 0.32;
    constexpr F64 SS_TICK_LAG  = 0.12;
    constexpr F64 SS_TICK_RTT  = 0.24;

    // The militia straggler fights alongside side A without wearing its tag, so he is not its enemy.
    bool ss_hostile(S8 a, S8 b)
    {
        if (a == b) return false;
        if ((a == 0 && b == 2) || (a == 2 && b == 0)) return false;
        return true;
    }

    // An LSL vector as the script would serialise it (the LSL (string) cast puts a space after each comma).
    std::string ss_lsl_vec(const LLVector3& v)
    {
        return llformat("<%.3f, %.3f, %.3f>", v.mV[VX], v.mV[VY], v.mV[VZ]);
    }

    // An LSL rotation as the script would serialise it.
    std::string ss_lsl_rot(F32 x, F32 y, F32 z, F32 s)
    {
        return llformat("<%.3f, %.3f, %.3f, %.3f>", x, y, z, s);
    }

    // Dispatch order for messages sharing a receive time: settings and equipment first, then ticks, then events.
    U8 ss_rank(U8 kind)
    {
        return kind;
    }
}

// Builds the whole scenario up front; nothing is ingested until runBulk or idle.
SSCombatSynth::SSCombatSynth(SSCombatLog& log, U32 seed, bool live)
:   mLog(log),
    mState(seed ? seed : 1u),
    mLive(live)
{
    buildScenario();
}

// Nothing owned outside the pools.
SSCombatSynth::~SSCombatSynth()
{
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

// One step of a plain LCG; no std::random_device, so the same seed is the same raid every time.
U32 SSCombatSynth::next()
{
    mState = mState * 1664525u + 1013904223u;
    return mState;
}

// Uniform in [0,1).
F32 SSCombatSynth::frand()
{
    return (F32)(next() >> 8) / (F32)(1u << 24);
}

// Uniform in [lo,hi).
F32 SSCombatSynth::range(F32 lo, F32 hi)
{
    return lo + (hi - lo) * frand();
}

// The damage type for one shot from this weapon: its usual type almost every time, occasionally the neighbour
// SS_DAMAGE_TYPE lists beside it, so a raid's own numbers show a little of the variety a real one would.
S16 SSCombatSynth::damageTypeFor(U8 weapon)
{
    const WeaponDamage& d = SS_DAMAGE_TYPE[weapon];
    return (frand() < d.mOccasionalChance) ? d.mOccasional : d.mUsual;
}

// A UUID drawn from the same stream, so keys are reproducible too.
LLUUID SSCombatSynth::makeId()
{
    LLUUID id;
    for (S32 i = 0; i < UUID_BYTES; ++i)
    {
        id.mData[i] = (U8)(next() >> 24);
    }
    return id;
}

// ---------------------------------------------------------------------------
// Geometry and the simulator clock
// ---------------------------------------------------------------------------

// 45 frames a second, dropping to 30 for two minutes in the middle so the fit has a real break to find.
U32 SSCombatSynth::frameAt(F64 t) const
{
    const F64 dip_start = 480.0, dip_end = 600.0;
    F64 f;
    if (t <= dip_start)     f = 45.0 * t;
    else if (t <= dip_end)  f = 45.0 * dip_start + 30.0 * (t - dip_start);
    else                    f = 45.0 * dip_start + 30.0 * (dip_end - dip_start) + 45.0 * (t - dip_end);
    return 4000000u + (U32)f;
}

// Land height under a point, memoised per 4 m cell: the build asks this tens of thousands of times.
F32 SSCombatSynth::ground(const LLVector3& p) const
{
    const U32 ix = (U32)llclamp((S32)(p.mV[VX] / 4.f), 0, 63);
    const U32 iy = (U32)llclamp((S32)(p.mV[VY] / 4.f), 0, 63);
    const U32 key = (ix << 8) | iy;
    auto it = mGroundCache.find(key);
    if (it != mGroundCache.end())
    {
        return it->second;
    }
    F32 h = mBase.mV[VZ] - 1.f;
    if (LLWorld::instanceExists())
    {
        const F32 land = LLWorld::instance().resolveLandHeightAgent(p);
        if (land > 0.5f && land < 4000.f)
        {
            h = land;
        }
    }
    mGroundCache[key] = h;
    return h;
}

// Keeps everything inside the region and off the ground.
LLVector3 SSCombatSynth::clampToRegion(const LLVector3& p) const
{
    LLVector3 out = p;
    out.mV[VX] = llclamp(out.mV[VX], 8.f, 248.f);
    out.mV[VY] = llclamp(out.mV[VY], 8.f, 248.f);
    out.mV[VZ] = ground(out) + 1.f;
    return out;
}

// The closest living fighter on another side within reach.
SSCombatSynth::Person* SSCombatSynth::nearestEnemy(const Person& from, F32 maxRange)
{
    Person* best = nullptr;
    F32 best_dist = maxRange;
    for (Person& p : mPeople)
    {
        if (!p.mAlive || p.mId == from.mId || !ss_hostile(p.mSide, from.mSide))
        {
            continue;
        }
        const F32 d = (p.mPos - from.mPos).length();
        if (d < best_dist)
        {
            best_dist = d;
            best = &p;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Scenario
// ---------------------------------------------------------------------------

// Places the raid around the officer, names everybody, registers the equipment, then runs the whole
// simulation into the message queue. One function so the beats above are the only thing to edit.
void SSCombatSynth::buildScenario()
{
    mBase = gAgent.getPositionAgent();
    mBase.mV[VX] = llclamp(mBase.mV[VX], 70.f, 186.f);
    mBase.mV[VY] = llclamp(mBase.mV[VY], 70.f, 186.f);

    mSpawnA = clampToRegion(mBase + LLVector3(-58.f, -32.f, 0.f));
    mSpawnB = clampToRegion(mBase + LLVector3( 58.f,  32.f, 0.f));

    mExperienceOwner = makeId();
    mVehicle = makeId();
    mHud = makeId();

    // The army registry, in the owner's own schema, so the team pages can name a side.
    std::vector<GroupInfo> armies(2);
    armies[0].mGroup = makeId();
    armies[0].mName = "Ashguard";
    armies[0].mShort = "[Ash]";
    armies[1].mGroup = makeId();
    armies[1].mName = "Exiles";
    armies[1].mShort = "[Ex]";
    mLog.setGroups(armies);

    std::vector<RegionInfo> regions(1);
    regions[0].mName = "Synthetic";
    regions[0].mType = REGION_NEUTRAL;
    mLog.setRegions(regions);

    if (mLog.mAnalysis)
    {
        mLog.mAnalysis->clearTeamHints();
    }

    // --- people -----------------------------------------------------------
    mPeople.reserve(SS_PEOPLE);
    for (size_t i = 0; i < SS_PEOPLE; ++i)
    {
        Person p;
        p.mId = makeId();
        if (i < SS_SIDE_A)
        {
            p.mSide = 0;
            p.mName = SS_NAMES_A[i];
            p.mSpawn = mSpawnA;
            p.mWeapon = (i == 5) ? W_MORTAR : W_RIFLE;
        }
        else if (i < SS_SIDE_A + SS_SIDE_B)
        {
            p.mSide = 1;
            p.mName = SS_NAMES_B[i - SS_SIDE_A];
            p.mSpawn = mSpawnB;
            const size_t j = i - SS_SIDE_A;
            p.mWeapon = (j < 4) ? W_BOLT : (j == 6 ? W_GRENADE : W_RIFLE);
        }
        else
        {
            // The militia straggler fights with side A but wears nobody's tag and buys his own gear.
            p.mSide = 2;
            p.mName = SS_NAME_MILITIA;
            p.mSpawn = mSpawnA;
            p.mWeapon = W_RIFLE;
        }
        p.mHealth = SS_HEALTH;
        p.mLateral = range(-22.f, 22.f);
        p.mPos = clampToRegion(p.mSpawn + LLVector3(range(-8.f, 8.f), range(-8.f, 8.f), 0.f));
        p.mYaw = range(0.f, F_TWO_PI);
        p.mWeaponId = (p.mWeapon == W_GRENADE) ? mHud : makeId();
        if (i == SS_CREW_A) p.mSeat = 0;
        if (i == SS_CREW_B) p.mSeat = 1;
        mPeople.push_back(p);

        mLog.mNames[p.mId] = p.mName;
        if (p.mSide < 2 && mLog.mAnalysis)
        {
            mLog.mAnalysis->setTeamHint(p.mId, p.mSide);
        }
        mTrackKeys.push_back(p.mId);
    }
    mLastSent.assign(mTrackKeys.size(), LLVector3(-9999.f, -9999.f, -9999.f));
    mLastFlags.assign(mTrackKeys.size(), 0xFFFF);

    mLog.mNames[mVehicle] = "Wolfhound APC";

    // --- region combat settings, once -------------------------------------
    {
        Msg m;
        m.mAt = 0.01;
        m.mKind = MSG_SETTINGS;
        // allow_damage_adjust, restrict_combat_log, damage_throttle, damage_limit, restore_health,
        // health_regen_rate, invulnerability_time, death_action, agent_limit
        m.mText = "RS|1,0,0,0,1,0.166667,3,1,40";
        mQueue.push_back(m);
    }

    // --- equipment registry ----------------------------------------------
    auto note = [this](const LLUUID& id, const LLUUID& owner, const LLUUID& group, const std::string& name,
                       U8 kind, S32 attach, F64 at)
    {
        Equipment eq;
        eq.mId = id;
        eq.mOwner = owner;
        eq.mGroup = group;
        eq.mName = name;
        eq.mKind = kind;
        eq.mAttachPoint = attach;
        eq.mConfidence = CONF_FIRM;
        eq.mFirstSeen = at;
        eq.mLastSeen = at;
        Msg m;
        m.mAt = at;
        m.mKind = MSG_EQUIP;
        m.mIndex = (U32)mEquipPool.size();
        mEquipPool.push_back(eq);
        mQueue.push_back(m);
    };

    for (size_t i = 0; i < mPeople.size(); ++i)
    {
        const Person& p = mPeople[i];
        const LLUUID group = (p.mSide == 0) ? armies[0].mGroup : (p.mSide == 1 ? armies[1].mGroup : LLUUID::null);
        std::string name;
        U8 kind = EQUIP_WEAPON;
        S32 attach = 6; // right hand
        switch (p.mWeapon)
        {
            case W_RIFLE:
                name = (p.mSide == 0) ? "[Ash] - Kestrel Rifle v1.2.3"
                     : (p.mSide == 1) ? "[Ex] PP19 Bizon v 1.04"
                                      : "Surplus Carbine 3.1";
                break;
            case W_BOLT:
                name = "[Ex] Kagrenac bolt-caster v 1.04";
                break;
            case W_MORTAR:
                name = "[Ash] Fieldpiece Mortar v2.0";
                break;
            case W_GRENADE:
            default:
                name = "[Ex] Basket Grenade HUD v1.1";
                kind = EQUIP_HUD;
                attach = 31; // HUD centre, which is what makes the grenade a lobbed delivery
                break;
        }
        if (p.mWeapon == W_MORTAR)
        {
            mMortar = p.mWeaponId;
        }
        note(p.mWeaponId, p.mId, group, name, kind, attach, 0.02);
    }
    note(mVehicle, mPeople[SS_CREW_A].mId, armies[0].mGroup, "[Ash] Wolfhound APC v3.0.1", EQUIP_VEHICLE, 0, 0.02);
    // The region's own experience attachment: the legitimate reading of every immunity below.
    note(mExperienceOwner, mExperienceOwner, LLUUID::null, "Region Safe Zone Experience", EQUIP_DEPLOYABLE, 0, 0.02);

    // --- the raid ---------------------------------------------------------
    const S32 ticks = (S32)(mDuration / (F64)SS_TICK);
    for (S32 k = 0; k <= ticks; ++k)
    {
        const F64 t = (F64)k * (F64)SS_TICK;
        const U32 frame = frameAt(t);
        runCues(t, frame);
        step(t, frame);
        emitTick(t, frame);
        flushEvents(t, frame);
    }

    // Dispatch order: settings and equipment, then ticks, then samples, flights and finally the chat relay.
    std::stable_sort(mQueue.begin(), mQueue.end(), [](const Msg& a, const Msg& b)
    {
        if (a.mAt != b.mAt) return a.mAt < b.mAt;
        return ss_rank(a.mKind) < ss_rank(b.mKind);
    });
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

// Moves everybody for one 0.5 s tick and lets anyone with a target and a cooled weapon take a shot.
void SSCombatSynth::step(F64 t, U32 frame)
{
    const Beat* beat = &SS_BEATS[SS_BEAT_COUNT - 1];
    for (size_t i = 0; i < SS_BEAT_COUNT; ++i)
    {
        if (t >= (F64)SS_BEATS[i].mStart && t < (F64)SS_BEATS[i].mEnd)
        {
            beat = &SS_BEATS[i];
            break;
        }
    }

    // The line each side is trying to hold, as a point between the two spawns.
    const LLVector3 axis = mSpawnB - mSpawnA;
    LLVector3 across(-axis.mV[VY], axis.mV[VX], 0.f);
    if (across.length() > 0.001f)
    {
        across.normalize();
    }

    for (Person& p : mPeople)
    {
        if (!p.mAlive)
        {
            if (t >= p.mRespawnAt)
            {
                p.mAlive = true;
                p.mHealth = SS_HEALTH;
                p.mPos = clampToRegion(p.mSpawn + LLVector3(range(-6.f, 6.f), range(-6.f, 6.f), 0.f));
                p.mVel.clear();
                // The true respawn is its own instant jump, one tick and no interpolation, same as the
                // death_action landing above; the store's own detector would also catch this, but the
                // generator is honest about what it just did.
                p.mFlags |= FLAG_TELEPORT;
            }
            else
            {
                p.mVel.clear();
                continue;
            }
        }

        const F32 advance = (p.mSide == 1) ? beat->mAdvanceB : beat->mAdvanceA;
        LLVector3 target = (p.mSide == 1)
                         ? mSpawnB + (mSpawnA - mSpawnB) * advance
                         : mSpawnA + (mSpawnB - mSpawnA) * advance;
        target += across * p.mLateral;
        target += LLVector3(range(-4.f, 4.f), range(-4.f, 4.f), 0.f);
        target = clampToRegion(target);

        LLVector3 dir = target - p.mPos;
        dir.mV[VZ] = 0.f;
        const F32 dist = dir.length();
        if (dist > 1.2f)
        {
            dir.normalize();
            const F32 speed = range(2.4f, 4.2f);
            p.mVel = dir * speed;
            p.mPos = clampToRegion(p.mPos + p.mVel * SS_TICK);
            p.mYaw = atan2f(dir.mV[VY], dir.mV[VX]);
        }
        else
        {
            p.mVel *= 0.3f;
            p.mYaw += range(-0.4f, 0.4f);
        }

        // Flags: mouselook while recently firing, walking while moving, crew stay seated on the APC.
        U16 flags = (U16)(p.mFlags & FLAG_TELEPORT); // a pending teleport survives one tick
        if (p.mVel.length() > 0.6f)   flags |= FLAG_WALKING;
        if (t - p.mLastShot < 3.0)    flags |= FLAG_MOUSELOOK;
        if (p.mSeat >= 0)             flags |= (U16)(FLAG_SITTING | FLAG_ON_OBJECT);
        p.mFlags = flags;
    }

    // The APC carries its crew; the root is the vehicle and the crew ride its position (analysis 5.13).
    LLVector3 apc = mSpawnA + (mSpawnB - mSpawnA) * (beat->mAdvanceA * 0.9f);
    apc += across * 12.f;
    apc = clampToRegion(apc);
    for (Person& p : mPeople)
    {
        if (p.mSeat >= 0)
        {
            p.mPos = apc + LLVector3(p.mSeat == 0 ? 1.1f : -1.1f, 0.f, 1.2f);
            p.mVel.clear();
        }
    }
    {
        Sample s;
        s.mTime = (F32)(t + 0.25);
        s.mPos = apc;
        s.mYaw = atan2f((mSpawnB - mSpawnA).mV[VY], (mSpawnB - mSpawnA).mV[VX]);
        s.mSource = SAMPLE_VIEWER;
        s.mQuality = 200;
        queueSample(mVehicle, false, s, t + 0.25);
    }

    // Shooting.
    for (size_t i = 0; i < mPeople.size(); ++i)
    {
        Person& p = mPeople[i];
        if (!p.mAlive)
        {
            continue; // crew do keep shooting; only the dead stop
        }
        const F32 cooldown = SS_COOLDOWN[p.mWeapon] / llmax(0.05f, beat->mFireRate);
        if (t - p.mLastShot < (F64)cooldown)
        {
            continue;
        }
        Person* victim = nearestEnemy(p, SS_RANGE[p.mWeapon]);
        if (!victim)
        {
            continue;
        }
        p.mLastShot = t; // the shot was taken whether or not it landed
        if (frand() > SS_HIT_CHANCE)
        {
            continue; // most shots miss, and a miss never reaches the combat log at all
        }
        fire(p, *victim, t, frame);
    }
}

// One shot: the wire records differ per weapon, which is exactly what the delivery classifier reads.
void SSCombatSynth::fire(Person& shooter, Person& victim, F64 t, U32 frame)
{
    shooter.mLastShot = t;
    shooter.mFlags |= FLAG_MOUSELOOK;

    const F32 raw = range(SS_DMG_LO[shooter.mWeapon], SS_DMG_HI[shooter.mWeapon]);
    const S16 type = damageTypeFor(shooter.mWeapon);

    switch (shooter.mWeapon)
    {
        case W_RIFLE:
        {
            // Attached and its own rezzer: hitscan, no flight, no travel window.
            applyDamage(shooter, victim, raw, type, shooter.mWeaponId, shooter.mWeaponId, t, frame, false);
            break;
        }
        case W_BOLT:
        {
            // A bolt is rezzed, flies, and is gone; the viewer's ghost-projectile heuristic would see it.
            const LLUUID bolt = makeId();
            Equipment eq;
            eq.mId = bolt;
            eq.mOwner = shooter.mId;
            eq.mRezzer = shooter.mWeaponId;
            eq.mName = "[Ex] Kagrenac bolt-caster bolt v1.04";
            eq.mKind = EQUIP_PROJECTILE;
            eq.mAttachPoint = 0;
            eq.mConfidence = CONF_LIKELY;
            eq.mFirstSeen = t;
            eq.mLastSeen = t + 1.0;
            {
                Msg m;
                m.mAt = t;
                m.mKind = MSG_EQUIP;
                m.mIndex = (U32)mEquipPool.size();
                mEquipPool.push_back(eq);
                mQueue.push_back(m);
            }

            LLVector3 from = shooter.mPos + LLVector3(0.f, 0.f, 1.4f);
            LLVector3 to = victim.mPos + LLVector3(0.f, 0.f, 1.0f);
            // Havok's own physics speed limit is ~200 m/s (llSetKeyframedMotion/physics step ceiling), which
            // is about as fast as a scripted, tracked projectile can move; the bolt used to travel at a mere
            // 120 m/s and, worse, land its damage at the muzzle instant t rather than the impact instant the
            // flight above actually reaches -- the victim was recorded dead while their own death's evidence
            // still showed the bolt mid-air (owner bug report 2026-09-09).
            const F32 SS_BOLT_SPEED = 200.f;
            const F32 travel = llmax(0.01f, (to - from).length() / SS_BOLT_SPEED);
            const F64 impact_t = t + (F64)travel;
            const U32 impact_frame = frameAt(impact_t);
            Flight f;
            f.mObject = bolt;
            f.mStart = t;
            f.mEnd = impact_t;
            f.mShooter = shooter.mId;
            f.mShooterConfidence = CONF_LIKELY;
            for (S32 s = 0; s <= 5; ++s)
            {
                const F32 u = (F32)s / 5.f;
                f.mPath.push_back(std::make_pair((F32)(t + (F64)(travel * u)), from + (to - from) * u));
            }
            {
                Msg m;
                m.mAt = impact_t;
                m.mKind = MSG_FLIGHT;
                m.mIndex = (U32)mFlightPool.size();
                mFlightPool.push_back(f);
                mQueue.push_back(m);
            }
            applyDamage(shooter, victim, raw, type, bolt, shooter.mWeaponId, impact_t, impact_frame, false);
            break;
        }
        case W_MORTAR:
        {
            // Lobbed, and it hits everyone near the impact: one source, several targets in the same second.
            const LLUUID shell = makeId();
            applyDamage(shooter, victim, raw, type, shell, shooter.mWeaponId, t, frame, true);
            for (Person& other : mPeople)
            {
                if (!other.mAlive || other.mId == victim.mId || !ss_hostile(other.mSide, shooter.mSide))
                {
                    continue;
                }
                const F32 d = (other.mPos - victim.mPos).length();
                if (d <= 7.f)
                {
                    applyDamage(shooter, other, raw * (1.f - d / 9.f), type, shell, shooter.mWeaponId, t, frame, true);
                }
            }
            break;
        }
        case W_GRENADE:
        default:
        {
            // Rezzed by a HUD attachment, which is what makes it lobbed rather than hitscan.
            const LLUUID nade = makeId();
            applyDamage(shooter, victim, raw, type, nade, mHud, t, frame, true);
            for (Person& other : mPeople)
            {
                if (!other.mAlive || other.mId == victim.mId || !ss_hostile(other.mSide, shooter.mSide))
                {
                    continue;
                }
                if ((other.mPos - victim.mPos).length() <= 5.f)
                {
                    applyDamage(shooter, other, raw * 0.6f, type, nade, mHud, t, frame, true);
                }
            }
            break;
        }
    }
}

// Writes one DAMAGE line, applying the region's spawn immunity and one combatant's personal armour.
void SSCombatSynth::applyDamage(Person& shooter, Person& victim, F32 raw, S16 type, const LLUUID& source,
                                const LLUUID& rezzer, F64 t, U32 frame, bool splash)
{
    (void)splash;
    if (raw <= 0.f)
    {
        return;
    }
    F32 final_damage = raw;
    std::string mods;

    if ((victim.mPos - victim.mSpawn).length() <= SS_SPAWN_SHIELD)
    {
        // The region's own experience attachment zeroes damage inside the spawn ring. This is the
        // legitimate reading the Script page is supposed to name first (analysis 5.14).
        final_damage = 0.f;
        mods = llformat(",\"modifications\":[{\"task_id\":\"%s\",\"events\":[{\"script\":\"ss_spawn_shield\",\"new_damage\":0.000}]}]",
                        mExperienceOwner.asString().c_str());
    }
    else if (victim.mId == mPeople[SS_ARMOUR_PERSON].mId)
    {
        final_damage = raw * 0.8f;
        mods = llformat(",\"modifications\":[{\"task_id\":\"%s\",\"events\":[{\"script\":\"armour_plate\",\"new_damage\":%.3f}]}]",
                        victim.mWeaponId.asString().c_str(), final_damage);
    }

    mPendingEvents.push_back(llformat(
        "{\"event\":\"DAMAGE\",\"damage\":%.3f,\"initial\":%.3f,\"type\":%d,\"source\":\"%s\",\"rezzer\":\"%s\","
        "\"owner\":\"%s\",\"target\":\"%s\",\"agent\":1,\"t\":%u%s}",
        final_damage, raw, (S32)type,
        source.asString().c_str(), rezzer.asString().c_str(),
        shooter.mId.asString().c_str(), victim.mId.asString().c_str(),
        frame, mods.c_str()));

    victim.mHealth -= final_damage;
    if (victim.mHealth <= 0.f)
    {
        kill(shooter, victim, source, rezzer, type, final_damage, t, frame, true);
    }
}

// Writes the DEATH line, which is the only kind that carries positions, and starts the respawn timer.
void SSCombatSynth::kill(Person& shooter, Person& victim, const LLUUID& source, const LLUUID& rezzer,
                         S16 type, F32 damage, F64 t, U32 frame, bool teleport)
{
    mPendingEvents.push_back(llformat(
        "{\"event\":\"DEATH\",\"damage\":%.3f,\"initial\":%.3f,\"type\":%d,\"source\":\"%s\",\"rezzer\":\"%s\","
        "\"owner\":\"%s\",\"target\":\"%s\",\"agent\":1,\"source_pos\":\"%s\",\"target_pos\":\"%s\",\"t\":%u}",
        damage, damage, (S32)type,
        source.asString().c_str(), rezzer.asString().c_str(),
        shooter.mId.asString().c_str(), victim.mId.asString().c_str(),
        ss_lsl_vec(shooter.mPos + LLVector3(0.f, 0.f, 1.4f)).c_str(),
        ss_lsl_vec(victim.mPos + LLVector3(0.f, 0.f, 1.0f)).c_str(),
        frame));

    victim.mAlive = false;
    victim.mHealth = 0.f;
    victim.mRespawnAt = t + 8.0;
    victim.mVel.clear();
    if (teleport)
    {
        // death_action 1 sends them to the parcel landing point: an 8-24 m jump with no velocity behind it.
        LLVector3 landing = victim.mSpawn + LLVector3(range(-7.f, 7.f), range(-7.f, 7.f), 0.f);
        if ((landing - victim.mPos).length() < 8.f)
        {
            landing += LLVector3(12.f, 6.f, 0.f);
        }
        victim.mPos = clampToRegion(landing);
        victim.mFlags |= FLAG_TELEPORT;
    }
}

// ---------------------------------------------------------------------------
// Scripted moments
// ---------------------------------------------------------------------------

// Fires the scheduled episodes the officer is meant to find; each one is a single beat of the story.
void SSCombatSynth::runCues(F64 t, U32 frame)
{
    while (mCue < SS_CUE_COUNT && (F64)SS_CUES[mCue].mAt <= t)
    {
        const U8 what = SS_CUES[mCue].mWhat;
        ++mCue;
        switch (what)
        {
            case CUE_FD_PAIR:
            {
                // The wearer's own attachment reports the hit first and better; the log repeats it a second
                // later. final_damage only ever fires in the local avatar's own attachments (bridge spec), so
                // the wire "target" for both halves is gAgent, not the synthetic Person the health bookkeeping
                // below is flavoured around.
                Person& shooter = mPeople[SS_SIDE_A];      // side B, first member
                Person& victim  = mPeople[0];              // side A, first member (flavour bookkeeping only)
                const F32 dmg = 13.5f;
                const LLUUID target_id = gAgent.getID();

                const Equipment* weapon_eq = nullptr;
                for (const Equipment& e : mEquipPool)
                {
                    if (e.mId == shooter.mWeaponId) { weapon_eq = &e; break; }
                }
                const std::string weapon_name  = weapon_eq ? weapon_eq->mName : std::string();
                const S32         weapon_point = weapon_eq ? weapon_eq->mAttachPoint : 6;
                const std::string weapon_group = (weapon_eq ? weapon_eq->mGroup : LLUUID::null).asString();
                const std::string weapon_maker = (weapon_eq ? weapon_eq->mCreator : LLUUID::null).asString();
                const std::string weapon_pos   = ss_lsl_vec(shooter.mPos + LLVector3(0.f, 0.f, 1.4f));

                // An attached, self-rezzed weapon is its own rezzer and its own root (analysis 5.5/5.13).
                const std::string source_details = llformat("[\"%s\",\"%s\",%d,\"%s\",\"%s\"]",
                    weapon_name.c_str(), weapon_pos.c_str(), weapon_point, weapon_group.c_str(), weapon_maker.c_str());
                const std::string rezzer_details = llformat("[\"%s\",\"%s\",\"%s\",%d,\"%s\",\"%s\"]",
                    shooter.mId.asString().c_str(), weapon_name.c_str(), weapon_pos.c_str(),
                    weapon_point, weapon_group.c_str(), weapon_maker.c_str());
                const std::string& root_details = source_details;

                const std::string target_pos = ss_lsl_vec(victim.mPos + LLVector3(0.f, 0.f, 1.0f));
                const std::string target_rot = ss_lsl_rot(0.f, 0.f, sinf(victim.mYaw * 0.5f), cosf(victim.mYaw * 0.5f));
                const std::string target_vel = ss_lsl_vec(victim.mVel);

                const std::string fd = llformat(
                    "{\"event\":\"FINAL_DAMAGE\",\"source\":\"%s\",\"sourceDetails\":%s,"
                    "\"rezzer\":\"%s\",\"rezzerDetails\":%s,\"root\":\"%s\",\"rootDetails\":%s,"
                    "\"owner\":\"%s\",\"damage\":%.3f,\"type\":0,\"original\":%.3f,"
                    "\"targetPos\":\"%s\",\"targetRot\":\"%s\",\"targetVel\":\"%s\"}",
                    shooter.mWeaponId.asString().c_str(), source_details.c_str(),
                    shooter.mWeaponId.asString().c_str(), rezzer_details.c_str(),
                    shooter.mWeaponId.asString().c_str(), root_details.c_str(),
                    shooter.mId.asString().c_str(), dmg, dmg,
                    target_pos.c_str(), target_rot.c_str(), target_vel.c_str());
                Msg fast;
                fast.mAt = t + 0.05;
                fast.mKind = MSG_RELAY;
                fast.mText = llformat("%u|", frame) + fd;
                mQueue.push_back(fast);

                const std::string twin = llformat(
                    "{\"event\":\"DAMAGE\",\"damage\":%.3f,\"initial\":%.3f,\"type\":0,\"source\":\"%s\","
                    "\"rezzer\":\"%s\",\"owner\":\"%s\",\"target\":\"%s\",\"agent\":1,\"t\":%u}",
                    dmg, dmg, shooter.mWeaponId.asString().c_str(), shooter.mWeaponId.asString().c_str(),
                    shooter.mId.asString().c_str(), target_id.asString().c_str(), frame);
                Msg late;
                late.mAt = t + 0.95;
                late.mKind = MSG_RELAY;
                late.mText = llformat("%u|[%s]", frame, twin.c_str());
                mQueue.push_back(late);
                victim.mHealth -= dmg;
                break;
            }
            case CUE_TRUNCATE:
            {
                mTruncateNext = true;
                break;
            }
            case CUE_AMBIGUOUS:
            {
                // Two attackers land inside one frame, so no ordering claim can be made between them.
                Person& a = mPeople[SS_SIDE_A + 1];
                Person& b = mPeople[SS_SIDE_A + 2];
                Person& victim = mPeople[1];
                victim.mAlive = true;
                victim.mHealth = 24.f;
                applyDamage(a, victim, 12.f, damageTypeFor(a.mWeapon), a.mWeaponId, a.mWeaponId, t, frame, false);
                if (victim.mAlive)
                {
                    applyDamage(b, victim, 18.f, damageTypeFor(b.mWeapon), b.mWeaponId, b.mWeaponId, t, frame, false);
                }
                break;
            }
            case CUE_FLASH:
            {
                // A foreign script writes its own dialect onto the combat channel: kept, typed CUSTOM.
                Person& blinded = mPeople[2];
                const LLUUID reporter = makeId();
                Msg m;
                m.mAt = t + 0.2;
                m.mKind = MSG_RELAY;
                m.mText = llformat("%u|%s%sFLASH|%s|3.0", frame,
                                   reporter.asString().c_str(), mExperienceOwner.asString().c_str(),
                                   blinded.mId.asString().c_str());
                mQueue.push_back(m);

                // Then he empties a magazine into nothing: flights, no hits. That context strip is mandatory
                // beside any through-wall signal (analysis 5.5).
                for (S32 s = 0; s < 5; ++s)
                {
                    const F64 when = t + 0.6 + (F64)s * 0.7;
                    LLVector3 from = blinded.mPos + LLVector3(0.f, 0.f, 1.4f);
                    LLVector3 to = from + LLVector3(range(-40.f, 40.f), range(-40.f, 40.f), range(-2.f, 6.f));
                    Flight f;
                    f.mObject = makeId();
                    f.mStart = when;
                    f.mEnd = when + 0.4;
                    f.mShooter = blinded.mId;
                    f.mShooterConfidence = CONF_LIKELY;
                    f.mHitGeometry = true;
                    for (S32 q = 0; q <= 4; ++q)
                    {
                        const F32 u = (F32)q / 4.f;
                        f.mPath.push_back(std::make_pair((F32)(when + 0.4 * (F64)u), from + (to - from) * u));
                    }
                    Msg fm;
                    fm.mAt = when + 0.4;
                    fm.mKind = MSG_FLIGHT;
                    fm.mIndex = (U32)mFlightPool.size();
                    mFlightPool.push_back(f);
                    mQueue.push_back(fm);
                }
                break;
            }
            case CUE_THROUGH_WALL:
            {
                // The attacker's track sits well behind the line and the killer line is long: the shape a
                // through-wall kill makes. Whether a wall was actually there is a Stage 2 raycast, not this.
                Person& killer = mPeople[SS_SIDE_A + 3];
                Person& victim = mPeople[4];
                victim.mAlive = true;
                victim.mHealth = SS_HEALTH;
                killer.mAlive = true;
                killer.mPos = clampToRegion(victim.mPos + LLVector3(-62.f, 9.f, 0.f));
                kill(killer, victim, killer.mWeaponId, killer.mWeaponId, damageTypeFor(killer.mWeapon), 34.f, t, frame, true);
                break;
            }
            case CUE_SITHACK:
            {
                // A teleport with no death: a redeploy, a sit hack, or a movement enhancer. Listed, never named.
                Person& p = mPeople[SS_PEOPLE - 1];
                p.mPos = clampToRegion(p.mPos + LLVector3(11.f, 9.f, 0.f));
                p.mFlags |= FLAG_TELEPORT;
                break;
            }
            case CUE_DEATH_NO_TP:
            default:
            {
                // A death with no teleport within 2 s: the respawn failed, or the HUD came off.
                Person& victim = mPeople[SS_PEOPLE - 1];
                Person& killer = mPeople[SS_SIDE_A + 4];
                victim.mAlive = true;
                killer.mAlive = true;
                kill(killer, victim, killer.mWeaponId, killer.mWeaponId, damageTypeFor(killer.mWeapon), 28.f, t, frame, false);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Wire
// ---------------------------------------------------------------------------

// Packs this tick's events into one relayed batch; the truncation cue cuts one of them mid-element.
void SSCombatSynth::flushEvents(F64 t, U32 frame)
{
    if (mPendingEvents.empty())
    {
        return;
    }
    std::string body = "[";
    for (size_t i = 0; i < mPendingEvents.size(); ++i)
    {
        if (i) body += ",";
        body += mPendingEvents[i];
    }
    body += "]";
    mPendingEvents.clear();

    if (mTruncateNext)
    {
        mTruncateNext = false;
        // llOwnerSay cuts at 1024 bytes, so the tail of a big batch simply never arrives.
        const size_t cut = (body.size() * 3) / 5;
        body = body.substr(0, llmax((size_t)2, cut));
    }

    Msg m;
    m.mAt = t + SS_RELAY_LAG + (F64)range(0.f, 0.12f);
    m.mKind = MSG_RELAY;
    m.mText = llformat("%u|", frame) + body;
    mQueue.push_back(m);
}

// Builds one GetTrackedAgents reply body and the viewer-side samples that sit between the ticks.
void SSCombatSynth::emitTick(F64 t, U32 frame)
{
    ++mSeq;
    std::string body = llformat("TA|%u|%u|%u|0", frame, mSeq - 1, mSeq);

    if (!mKeysSent)
    {
        mKeysSent = true;
        std::string keys = "\nK|";
        for (size_t i = 0; i < mTrackKeys.size(); ++i)
        {
            if (i) keys += ",";
            keys += llformat("%u=%s", (U32)i, mTrackKeys[i].asString().c_str());
        }
        body += keys;
    }

    const bool full = (mSeq % 8u) == 1u;  // a full snapshot every 4 s keeps a still track from going dark
    std::string entries;
    for (size_t i = 0; i < mPeople.size(); ++i)
    {
        const Person& p = mPeople[i];
        const bool moved = (p.mPos - mLastSent[i]).length() > 0.05f;
        if (!full && !moved && mLastFlags[i] == p.mFlags)
        {
            continue; // the script only sends what changed
        }
        mLastSent[i] = p.mPos;
        mLastFlags[i] = p.mFlags;
        if (!entries.empty()) entries += ";";
        F32 yaw_deg = fmodf(p.mYaw * RAD_TO_DEG, 360.f);
        if (yaw_deg < 0.f) yaw_deg += 360.f;
        entries += llformat("%u:%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%u",
                            (U32)i, p.mPos.mV[VX], p.mPos.mV[VY], p.mPos.mV[VZ],
                            yaw_deg, p.mVel.mV[VX], p.mVel.mV[VY], p.mVel.mV[VZ],
                            (U32)p.mFlags);
        if (p.mSeat >= 0)
        {
            entries += "," + mVehicle.asString(); // crew report the root they are riding
        }
    }
    if (!entries.empty())
    {
        body += llformat("\nT|%u|%u|", mSeq, frame) + entries;
    }

    Msg m;
    m.mAt = t + SS_TICK_LAG;
    m.mKind = MSG_TRACK;
    m.mText = body;
    mQueue.push_back(m);

    // The viewer's own object updates land between the ticks; these are the samples decimation sees.
    for (Person& p : mPeople)
    {
        Sample s;
        s.mTime = (F32)(t + 0.25);
        // Only a moving avatar jitters; a still one produces the identical sample decimation is meant to drop.
        const bool moving = p.mVel.length() > 0.05f;
        s.mPos = moving ? (p.mPos + LLVector3(range(-0.05f, 0.05f), range(-0.05f, 0.05f), 0.f)) : p.mPos;
        s.mVel = moving ? p.mVel : LLVector3();
        s.mYaw = p.mYaw;
        s.mFlags = p.mFlags;
        s.mSource = SAMPLE_VIEWER;
        s.mQuality = 200;
        if (p.mSeat >= 0)
        {
            s.mParent = mVehicle;
        }
        queueSample(p.mId, true, s, t + 0.25);
        p.mFlags = (U16)(p.mFlags & ~FLAG_TELEPORT); // a jump is one sample, not a state
    }
}

// Queues one deferred addSample call.
void SSCombatSynth::queueSample(const LLUUID& id, bool isAgent, const Sample& s, F64 at)
{
    Msg m;
    m.mAt = at;
    m.mKind = MSG_SAMPLE;
    m.mIndex = (U32)mSamplePool.size();
    mSamplePool.push_back(s);
    mSampleIds.push_back(id);
    mSampleIsAgent.push_back(isAgent ? (U8)1 : (U8)0);
    mQueue.push_back(m);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// Hands one queued message to the store through the same entry points the bridge will use.
void SSCombatSynth::dispatch(const Msg& msg)
{
    const F64 received = mLog.mSessionStart + msg.mAt;
    switch (msg.mKind)
    {
        case MSG_SETTINGS:
            mLog.ingestRegionSettings(msg.mText);
            break;
        case MSG_RELAY:
            mLog.ingestRelay(msg.mText, received);
            break;
        case MSG_TRACK:
            mLog.ingestTrackReply(msg.mText, received, SS_TICK_RTT);
            break;
        case MSG_SAMPLE:
            if (msg.mIndex < mSamplePool.size())
            {
                mLog.addSample(mSampleIds[msg.mIndex], mSampleIsAgent[msg.mIndex] != 0, mSamplePool[msg.mIndex]);
            }
            break;
        case MSG_EQUIP:
            if (msg.mIndex < mEquipPool.size())
            {
                Equipment eq = mEquipPool[msg.mIndex];
                eq.mFirstSeen += mLog.mSessionStart;
                eq.mLastSeen += mLog.mSessionStart;
                mLog.noteEquipment(eq);
            }
            break;
        case MSG_FLIGHT:
        default:
            if (msg.mIndex < mFlightPool.size())
            {
                Flight f = mFlightPool[msg.mIndex];
                f.mStart += mLog.mSessionStart;
                f.mEnd += mLog.mSessionStart;
                mLog.addFlight(f);
            }
            break;
    }
}

// Loads the whole session at once, with the receive times it was built with.
void SSCombatSynth::runBulk()
{
    for (; mNext < mQueue.size(); ++mNext)
    {
        dispatch(mQueue[mNext]);
    }
}

// Feeds whatever the wall clock has caught up to; the overlay animates off this.
void SSCombatSynth::idle(F64 now)
{
    if (!mLive)
    {
        return;
    }
    S32 budget = 400; // a stall must not dump ten seconds of raid into one frame
    while (mNext < mQueue.size() && (mLog.mSessionStart + mQueue[mNext].mAt) <= now && budget-- > 0)
    {
        dispatch(mQueue[mNext]);
        ++mNext;
    }
}
