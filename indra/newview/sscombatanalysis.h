/**
 * @file sscombatanalysis.h
 * @brief Stage 0 derived data for the Combat Log: engagements, lives, death attribution, teams and the
 *        active-combatant test. Simple, incremental, cache-per-question; the real solvers land in Stage 2.
 *        Model: doc/combat_log_analysis.md sections 5.6, 5.7, 5.8, 5.9.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATANALYSIS_H
#define SS_COMBATANALYSIS_H

#include "sscombatlog.h"

#include <map>

// Owned by SSCombatLog, which forwards its analysis contract here. Every answer is cached and every cache is
// dropped wholesale by invalidate(), which ingest calls; Stage 0 recomputes rather than patching.
class SSCombatAnalysis
{
public:
    SSCombatAnalysis(SSCombatLog& log);
    ~SSCombatAnalysis();

    // Drops every cache. Cheap; ingest calls it per line.
    void invalidate();

    const std::vector<SSCombat::Engagement>& engagements();
    const std::vector<SSCombat::Life>& lives(const LLUUID& combatant);
    SSCombat::Attribution attribution(U32 deathEvent);
    SSCombat::TeamAssignment team(const LLUUID& combatant);
    S32 teamCount();
    std::string teamName(S8 team);
    LLColor4 teamColor(S8 team);
    bool isCombatantAt(const LLUUID& id, F64 t);
    std::vector<LLUUID> combatants();

    // The synthetic generator knows the sides it wrote, so it seeds them rather than making the solve guess.
    // Public because SSCombatSynth reaches this object through SSCombatLog's friendship, not through its own.
    void setTeamHint(const LLUUID& combatant, S8 team);
    void clearTeamHints();

private:
    void buildEngagements();
    void buildLives();
    void buildTeams();
    // Names the sides once per solve; scanning the equipment registry per label would be a draw-loop cost.
    void buildTeamNames();
    void buildCombatants();
    // Position of the thing that was hit, from the event or from the track; false when neither knows.
    bool eventPosition(const SSCombat::Event& ev, LLVector3& out) const;

    SSCombatLog&                                        mLog;

    bool                                                mEngagementsValid = false;
    std::vector<SSCombat::Engagement>                   mEngagements;

    bool                                                mLivesValid = false;
    std::map<LLUUID, std::vector<SSCombat::Life> >      mLives;

    bool                                                mTeamsValid = false;
    std::map<LLUUID, SSCombat::TeamAssignment>          mTeams;
    S32                                                 mTeamCount = 0;
    std::vector<std::string>                            mTeamNames;
    std::map<LLUUID, S8>                                mTeamHints;   // set by the synthetic generator

    bool                                                mCombatantsValid = false;
    std::vector<LLUUID>                                 mCombatants;
};

#endif // SS_COMBATANALYSIS_H
