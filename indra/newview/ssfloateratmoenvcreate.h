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
#include "lluuid.h"

#include <string>
#include <vector>

class LLScrollListItem;
class LLScrollListCell;

// <SS:Nexii> What "Create New Environment" means is a choice, not a behaviour. Four seeds exist -
// plain midday defaults, the stock four-sky day cycle, a list of skies the author supplies (run
// through the same measure-and-stamp algorithm the stock seed uses), and an EEP day cycle mapped
// over by its own keyframe times. The two latter are fed by dropping settings onto this floater,
// the same gesture the editor itself takes - not by browsing - and Create stays parked until the
// chosen mode has actually been given something. See SSAtmoEnvManager for the seeds themselves.
class SSFloaterAtmoEnvCreate : public LLFloater
{
public:
    SSFloaterAtmoEnvCreate(const LLSD& key);

    bool postBuild() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                           EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

    // Opens the chooser centred within the environment editor floater.
    static void show();

private:
    enum EMode
    {
        MODE_EMPTY = 0,
        MODE_STOCK = 1,
        MODE_SKIES = 2,
        MODE_DAY_CYCLE = 3
    };

    struct DroppedSky
    {
        LLUUID mAssetId;
        std::string mName;
        // -1 when the name names no position, otherwise the fixed slot phase (0=midnight,
        // 0.25=sunrise, 0.5=noon, 0.75=sunset) the sky lands at instead of being measured.
        F64 mSlot = -1.0;
    };

    EMode currentMode() const;
    void setMode(EMode mode);

    void refresh();
    void rebuildSkyList();
    void onClickCreate();
    void onClickRemoveSky();

    static F64 slotForName(const std::string& name);
    static std::string slotLabel(F64 slot);
    static std::string rowForSky(const DroppedSky& sky);

    std::vector<DroppedSky> mDroppedSkies;
    LLUUID mDroppedDayCycle;
    std::string mDroppedDayCycleName;
};

#endif
