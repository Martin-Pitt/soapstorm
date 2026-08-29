/**
 * @file ssfloateratmoenvcreate.h
 * @brief Atmo Magic: the Create Environment chooser.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SS_FLOATERATMOENVCREATE_H
#define SS_FLOATERATMOENVCREATE_H

#include "llfloater.h"
#include "llinventorysettings.h"

#include <string>
#include <vector>

class LLInventoryPanel;
class LLViewerInventoryItem;

// <SS:Nexii> Atmo Magic: what "Create New Environment" means is a choice, not a behaviour. Four
// seeds exist - plain midday defaults, the stock four-sky day cycle, a list of skies the author
// picks (run through the same measure-and-stamp algorithm the stock seed uses), and an EEP day
// cycle mapped over by its own keyframe times. The chooser is a floater rather than a menu
// because two of the four need pickers anyway, and a radio group over embedded inventory panels
// keeps the whole decision in one place. See SSAtmoEnvManager for the seeds themselves.
class SSFloaterAtmoEnvCreate : public LLFloater
{
public:
    SSFloaterAtmoEnvCreate(const LLSD& key);

    bool postBuild() override;

    static void show();

private:
    enum EMode
    {
        MODE_EMPTY = 0,
        MODE_STOCK = 1,
        MODE_SKIES = 2,
        MODE_DAY_CYCLE = 3
    };

    EMode currentMode() const;

    // The panel's selection narrowed to full-perm settings of the kind the mode asks for.
    std::vector<LLViewerInventoryItem*> usableSelection(LLInventoryPanel* panel,
                                                        LLSettingsType::type_e type) const;

    void refresh();
    void onClickCreate();
};

#endif
