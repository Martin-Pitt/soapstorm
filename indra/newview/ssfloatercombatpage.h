/**
 * @file ssfloatercombatpage.h
 * @brief Combat Log Page floater: one page per noun, with the level bar, a linked body and a related list.
 *        Also the ss: link type. Design: doc/combat_log_ux.md 2.2, 2.3, 3.8.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_FLOATERCOMBATPAGE_H
#define SS_FLOATERCOMBATPAGE_H

#include "sscombatlog.h"

#include "llfloater.h"
#include "llurlentry.h"

#include <string>

class LLButton;
class LLScrollListCtrl;
class LLTextBox;
class LLTextEditor;

// Link type for secondlife:///app/sscombat/<type>/<id>[@t]; rendering only, the click runs the command handler.
class SSUrlEntryCombat : public LLUrlEntryBase
{
public:
    SSUrlEntryCombat();

    std::string getLabel(const std::string& url, const LLUrlLabelCallback& cb) override;
    std::string getTooltip(const std::string& string) const override;
};

class SSFloaterCombatPage : public LLFloater
{
public:
    SSFloaterCombatPage(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // Registers the ss: link type once; safe to call from any Combat Log floater.
    static void registerUrlEntry();
    // Opens (or raises) the page for one address.
    static void openAddress(const std::string& address);

private:
    // Rebuilds header, level bar, body and the related list for the current noun.
    void refreshPage();
    // The body of refreshPage, run under the re-entry guard.
    void refreshPageBody();
    // Enables each level button where the noun means something at that level (the level row rule).
    void refreshLevelBar();
    // The generated prose plus links for the current noun.
    std::string buildBody() const;

    void buildCombatantRows();
    void buildDeathRows();
    void buildFamilyRows();
    void buildTeamRows();
    void buildEngagementRows();

    // Adds one row to the related list; address may be empty for a row that is not a link.
    void addRow(const std::string& a, const std::string& b, const std::string& c, const std::string& d,
                const std::string& address = std::string());
    // Renames the four generic columns for this noun type.
    void setColumns(const std::string& a, const std::string& b, const std::string& c, const std::string& d);

    void onClickLevel(S8 level);
    void onClickReconstruct();
    void onSelectRelated();

    SSCombat::NounRef   mRef;
    bool                mUpdating = false;      // guards the related list's own selection callback

    LLTextBox*          mHeaderText = NULL;
    LLTextEditor*       mBodyText = NULL;
    LLScrollListCtrl*   mRelatedList = NULL;
    LLButton*           mReconstruct = NULL;
    LLButton*           mLevelButtons[6];
};

#endif // SS_FLOATERCOMBATPAGE_H
