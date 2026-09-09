/**
 * @file sscombatviews.h
 * @brief Combat Log custom views: the ribbon and the overhead Map renderer, plus the small label/landing
 *        helpers the floaters share. Design: doc/combat_log_ux.md 3.1, 3.4, 3.11.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATVIEWS_H
#define SS_COMBATVIEWS_H

#include "sscombatlog.h"

#include "lluictrl.h"
#include "v4color.h"

#include <string>
#include <vector>

// Shared label, landing and colour helpers. UI copy uses the plain level names and never "rung" or "ladder".
namespace SSCombatUI
{
    // Plain name of a level, exactly as it appears on the level bar and in the rail.
    std::string levelName(S8 level);
    // The level one step out (toward the abstract), clamped at Session.
    S8 levelOut(S8 level);
    // The level one step in (toward the concrete), clamped at Raw.
    S8 levelIn(S8 level);
    // What zooming in to a level would land on; back=true when it came from the view chain, false for the example.
    bool landingFor(S8 level, SSCombat::NounRef& out, bool& back);
    // "lands on: <label> (back)" / "(example)", or the reason the level has no subject yet.
    std::string landingText(S8 level);

    // Session-relative clock, mm:ss.d.
    std::string clockText(F64 t);
    // Session-relative clock without tenths, mm:ss.
    std::string shortClockText(F64 t);
    // Seconds as a spoken duration, "1h45m" / "6m12s".
    std::string durationText(F64 seconds);

    // secondlife:///app/sscombat/<type>/<id>[@t] for one noun.
    std::string nounSlurl(const SSCombat::NounRef& ref);
    // The tool's own ss:<type>/<id> address string.
    std::string nounAddress(const SSCombat::NounRef& ref);

    // Team colour with a readable fallback for unassigned.
    LLColor4 teamTint(S8 team);
    // Team of a combatant, -1 when unassigned.
    S8 teamOf(const LLUUID& id);
    // Display name, falling back to a short key.
    std::string nameOf(const LLUUID& id);
    // Confidence word from the one ladder: FIRM / LIKELY / WEAK / UNDETERMINED.
    std::string confidenceWord(U8 confidence);
    // Delivery word: hitscan / projectile / lobbed / area / unknown.
    std::string deliveryWord(U8 delivery);
    // Equipment family of the object that dealt a hit, or an empty string.
    std::string familyOf(const LLUUID& source, const LLUUID& rezzer);

    // Start and end of the drawn session span; end follows now while live.
    F64 spanStart();
    F64 spanEnd();
    // Index of the engagement containing t, or -1.
    S32 engagementAt(F64 t);
}

// The constant strip: death ticks, stacked damage rate, engagement bands, optional lanes, quality, cursor.
class SSRibbonView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

protected:
    friend class LLUICtrlFactory;
    SSRibbonView(const Params& p);

private:
    // Pixel x for a session time, and the inverse.
    S32 timeToX(F64 t) const;
    F64 xToTime(S32 x) const;
    // Moves the cursor to the time under x, leaves live and pushes nothing (scrubbing is not navigation).
    void scrubTo(S32 x);
    // Recomputes the per-bucket damage and coverage caches when the data changed.
    void rebuildBuckets();

    // Lane geometry, laid out top down from the current rect.
    struct Lanes
    {
        S32 mDeathsTop = 0, mDeathsBottom = 0;
        S32 mDamageTop = 0, mDamageBottom = 0;
        S32 mBandsTop = 0, mBandsBottom = 0;
        S32 mEscalationTop = 0, mEscalationBottom = 0;
        S32 mPresenceTop = 0, mPresenceBottom = 0;
        S32 mQualityTop = 0, mQualityBottom = 0;
    };
    Lanes laneLayout() const;

    std::vector<std::vector<F32> > mDamage;   // [team + 1][bucket], damage per 10 s bucket
    std::vector<F32>               mCoverage; // [bucket], 0..1 track coverage
    F32                            mDamagePeak = 1.f;
    size_t                         mBuiltEvents = 0;
    F64                            mBuiltEnd = 0.0;
    bool                           mDragging = false;
};

// Overhead 2D of region-local XY: team-coloured trails, death crosses, the cursor position, clickable marks.
class SSMapView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

    // Restricts the drawing to one engagement's window and members; -1 draws the whole session.
    void setEngagement(S32 index) { mEngagement = index; }
    S32 getEngagement() const { return mEngagement; }

protected:
    friend class LLUICtrlFactory;
    SSMapView(const Params& p);

private:
    // Fits the drawn window's positions into the rect; false when there is nothing to draw.
    bool computeBounds(F64& start, F64& end, F32& minX, F32& minY, F32& scale) const;

    struct Mark
    {
        LLRect              mRect;
        SSCombat::NounRef   mRef;
        std::string         mLabel;
    };

    S32                 mEngagement = -1;
    std::vector<Mark>   mMarks;
};

#endif // SS_COMBATVIEWS_H
