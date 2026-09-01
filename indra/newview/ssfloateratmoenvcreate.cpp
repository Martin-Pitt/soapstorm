/**
 * @file ssfloateratmoenvcreate.cpp
 * @brief See ssfloateratmoenvcreate.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssfloateratmoenvcreate.h"

#include "ssatmoenvmanager.h"

#include "llbutton.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llpermissionsflags.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "llscrolllistcell.h"
#include "lltextbox.h"
#include "llviewerinventory.h"

#include <algorithm>
#include <cctype>

// Registration name this floater lives under - see LLViewerFloaterReg.
static const std::string CREATE_FLOATER_NAME = "ss_atmo_env_create";

// The editor floater this chooser opens inside of.
static const std::string ENV_FLOATER_NAME = "ss_atmo_env";

// <SS:Nexii> The fixed day positions the "From skies I choose" mode maps onto, addressed by the
// sky's inventory NAME - the mirror of the stamping table in SSAtmoEnvManager::seedSkyPhases.
// The two lists must stay in step, and the F64 here is the day-phase (0=midnight, 0.25=sunrise,
// 0.5=noon, 0.75=sunset).
namespace
{
    struct SSSeedPosition
    {
        const char* mName;
        F64 mPhase;
    };

    const SSSeedPosition SEED_POSITIONS[] = {
        { "midnight",                 0.00 },
        { "dawn",                     0.21 },
        { "sunrise",                  0.25 },
        { "post sunrise",             0.27 },
        { "morning golden hour",      0.30 },
        { "morning umbra",            0.36 },
        { "noon",                     0.50 },
        { "afternoon",                0.56 },
        { "evening golden hour",      0.67 },
        { "sunset",                   0.75 },
        { "evening near sunset",      0.78 },
        { "dusk",                     0.83 },
        { "evening umbra",            0.86 },
        { "evening",                  0.81 },
        { "night",                    0.94 },
    };

    // Position names that are also their own slot, ordered most-specific first so "Evening
    // Umbra" beats "Umbra" and the phase pairs with the position table.
    const char* const NAMED_SLOTS[] = {
        "evening golden hour",
        "evening near sunset",
        "evening sunset",
        "evening umbra",
        "morning golden hour",
        "morning post sunrise",
        "morning sunrise",
        "morning umbra",
        "noon",
        "daylight",
        "dusk",
        "dawn",
        "night",
        "sunset",
        "sunrise",
        "midnight",
    };

    std::string ss_to_lower(std::string s)
    {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }
}

F64 SSFloaterAtmoEnvCreate::slotForName(const std::string& name)
{
    const std::string lower = ss_to_lower(name);

    for (const char* slot : NAMED_SLOTS)
    {
        if (lower.find(slot) == std::string::npos) continue;

        for (const SSSeedPosition& pos : SEED_POSITIONS)
        {
            if (lower.find(pos.mName) != std::string::npos)
            {
                return pos.mPhase;
            }
        }
        return -1.0;
    }
    return -1.0;
}

// The human-readable position for a slot phase; the "auto" label shows the sky will be
// measured against the dominant body of the sky - whichever of sun and moon stands higher.
std::string SSFloaterAtmoEnvCreate::slotLabel(F64 slot)
{
    if (slot < 0.0) return "auto (dominant body)";

    const F64 day = std::fmod(slot, 1.0);
    const F64 hours_since_midnight = day * 24.0;
    const int whole = (int)std::floor(hours_since_midnight);
    const int frac = (int)std::floor((hours_since_midnight - (double)whole) * 60.0);
    return llformat("%02d:%02d", whole, frac);
}

std::string SSFloaterAtmoEnvCreate::rowForSky(const DroppedSky& sky)
{
    const std::string pos = (sky.mSlot < 0.0) ? "auto (dominant body)" : slotLabel(sky.mSlot);
    return pos + " - " + sky.mName;
}

// Floater shell; all content is wired in postBuild.
SSFloaterAtmoEnvCreate::SSFloaterAtmoEnvCreate(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoEnvCreate::postBuild()
{
    LLRadioGroup* mode = getChild<LLRadioGroup>("mode_radio");
    mode->setSelectedIndex(MODE_EMPTY);
    mode->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });

    getChild<LLUICtrl>("create_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCreate(); });
    getChild<LLUICtrl>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLUICtrl>("remove_sky_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRemoveSky(); });

    LLScrollListCtrl* skies = getChild<LLScrollListCtrl>("skies_list");
    skies->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });

    refresh();
    return true;
}

// Opens the chooser inside the editor floater rather than wherever the window manager
// cascades it - the choice belongs to the editor it acts on.
void SSFloaterAtmoEnvCreate::show()
{
    LLFloaterReg::showInstance(CREATE_FLOATER_NAME);

    LLFloater* chooser = LLFloaterReg::findInstance(CREATE_FLOATER_NAME);
    LLFloater* env = LLFloaterReg::findInstance(ENV_FLOATER_NAME);
    if (chooser && env && env->getVisible() && !env->isMinimized())
    {
        chooser->centerWithin(env->getRect());
    }
}

SSFloaterAtmoEnvCreate::EMode SSFloaterAtmoEnvCreate::currentMode() const
{
    // <SS:Nexii> The index, not getValue(): a radio item without a payload answers getValue()
    // with an empty LLSD, which asInteger()s to 0 - every mode would read as EMPTY forever.
    return (EMode)getChild<LLRadioGroup>("mode_radio")->getSelectedIndex();
}

// Switches the radio; refresh is called by the caller (setSelectedIndex with an event only
// happens on a real click, where the commit callback runs refresh anyway).
void SSFloaterAtmoEnvCreate::setMode(EMode mode)
{
    getChild<LLRadioGroup>("mode_radio")->setSelectedIndex((S32)mode);
}

// Mode-specific visibility, button state and the summary line. The radio commits into here, so
// both paths (hand click and programmatic switch after a drop) land in the same refresh. The
// skies list is NOT rebuilt here - its selection commit lands here too, and rebuilding under a
// select would eat the row the author just clicked.
void SSFloaterAtmoEnvCreate::refresh()
{
    const EMode mode = currentMode();

    getChild<LLView>("skies_list")->setVisible(mode == MODE_SKIES);
    getChild<LLView>("remove_sky_button")->setVisible(mode == MODE_SKIES);
    getChild<LLView>("daycycle_zone")->setVisible(mode == MODE_DAY_CYCLE);

    std::string summary;
    bool can_create = false;

    switch (mode)
    {
        case MODE_EMPTY:
            summary = "Nothing is fetched or seeded - the ground track starts from the midday defaults.";
            can_create = true;
            break;

        case MODE_STOCK:
            summary = "Seeds the ground track from the four shipped skies (Daylight, Night, Sunrise, Sunset), "
                      "timed by measuring each sky's own sun.";
            can_create = true;
            break;

        case MODE_SKIES:
        {
            const size_t count = mDroppedSkies.size();
            summary = (count == 0)
                ? "Drag skies from Inventory onto this window. Each is measured against the dominant body of the sky - whichever of its sun and moon stands higher - and stamped as keyframes. Skies whose name names a position (e.g. \"Morning Golden Hour\", \"Noon\") land at that fixed slot instead."
                : llformat("%zd sky%s supplied - named skies land on their slot (Morning Golden Hour, Noon...), the rest are measured against the dominant body of the sky (whichever of sun and moon stands higher) and stamped as keyframes.",
                           count, count == 1 ? "" : "s");

            can_create = count > 0;
            break;
        }

        case MODE_DAY_CYCLE:
        {
            summary = mDroppedDayCycle.isNull()
                ? "Drag a day cycle from Inventory onto this window. Its sky keyframes map over at their authored times."
                : "Every sky keyframe of the supplied day cycle lands at its authored time of day.";

            getChild<LLTextBox>("daycycle_text")->setText(
                mDroppedDayCycle.isNull()
                    ? "(nothing dropped yet)"
                    : llformat("Day cycle: %s", mDroppedDayCycleName.c_str()));

            can_create = mDroppedDayCycle.notNull();
            break;
        }
    }

    getChild<LLTextBox>("hint_text")->setText(summary);
    getChild<LLUICtrl>("create_button")->setEnabled(can_create);
    getChild<LLUICtrl>("remove_sky_button")->setEnabled(
        mode == MODE_SKIES && !getChild<LLScrollListCtrl>("skies_list")->getAllSelected().empty());
}

// Resyncs the list widget with the supplied skies. Only called when the set itself changed.
void SSFloaterAtmoEnvCreate::rebuildSkyList()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("skies_list");
    list->deleteAllItems();
    for (const DroppedSky& sky : mDroppedSkies)
    {
        // <SS:Nexii> Two data columns - the position the sky will land at and the sky's own name -
        // and the row keeps its sky asset id as its row value. The leading "index" column is empty
        // (see the XUI): it is the drag-spacer that keeps the row text from under the drag icon.
        LLScrollListCell::Params index_cell;
        index_cell.column("index").value("");

        LLScrollListCell::Params name_cell;
        name_cell.column("skies").value(sky.mName);
        name_cell.label(rowForSky(sky));

        LLScrollListItem::Params row;
        row.columns.add(index_cell).add(name_cell);
        row.value(sky.mAssetId.asString());
        list->addRow(row, ADD_BOTTOM);
    }
}

// The whole floater is the drop target: skies land in the list, day cycles become the seed, and
// a drop switches the radio to the mode it feeds so the author sees what took. Only full-perm
// settings are taken, the same rule a sky dropped onto the editor obeys.
bool SSFloaterAtmoEnvCreate::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop,
                                               EDragAndDropType cargo_type, void* cargo_data,
                                               EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_SETTINGS)
    {
        *accept = ACCEPT_NO;
        return false;
    }

    const LLViewerInventoryItem* item =
        cargo_data ? gInventory.getItem(((const LLInventoryItem*)cargo_data)->getUUID()) : nullptr;
    if (!item || item->getAssetUUID().isNull())
    {
        *accept = ACCEPT_NO;
        return true;
    }

    if (!item->checkPermissionsSet(PERM_ITEM_UNRESTRICTED))
    {
        *accept = ACCEPT_NO;
        tooltip_msg = "Only full-permission settings can be used.";
        return true;
    }

    const LLSettingsType::type_e type = item->getSettingsType();
    const bool is_sky = (type == LLSettingsType::ST_SKY);
    const bool is_day_cycle = (type == LLSettingsType::ST_DAYCYCLE);
    if (!is_sky && !is_day_cycle)
    {
        *accept = ACCEPT_NO;
        tooltip_msg = "Only skies and day cycles can seed an environment.";
        return true;
    }

    *accept = ACCEPT_YES_MULTI;
    tooltip_msg = item->getName();

    if (drop)
    {
        if (is_sky)
        {
            bool have = false;
            const F64 slot = slotForName(item->getName());
            if (slot < 0.0)
            {
                for (const DroppedSky& sky : mDroppedSkies)
                {
                    if (sky.mName == item->getName())
                    {
                        have = true;
                        break;
                    }
                }
            }

            if (!have)
            {
                DroppedSky sky;
                sky.mAssetId = item->getAssetUUID();
                sky.mName = item->getName();
                sky.mSlot = slot;
                mDroppedSkies.push_back(sky);
                rebuildSkyList();
            }
            setMode(MODE_SKIES);
        }
        else
        {
            mDroppedDayCycle = item->getAssetUUID();
            mDroppedDayCycleName = item->getName();
            setMode(MODE_DAY_CYCLE);
        }

        refresh();
    }

    return true;
}

// Takes the selected rows back out of the supplied list.
void SSFloaterAtmoEnvCreate::onClickRemoveSky()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("skies_list");

    std::vector<LLUUID> remove_ids;
    for (LLScrollListItem* item : list->getAllSelected())
    {
        remove_ids.push_back(item->getUUID());
    }
    if (remove_ids.empty()) return;

    for (const LLUUID& id : remove_ids)
    {
        for (auto it = mDroppedSkies.begin(); it != mDroppedSkies.end(); ++it)
        {
            if (it->mAssetId == id)
            {
                mDroppedSkies.erase(it);
                break;
            }
        }
    }

    rebuildSkyList();
    refresh();
}

// Runs the chosen seed and adopts what it wrote. The editor picks the new asset up on its next
// poll, and showInstance brings it forward and forces the full refresh now.
void SSFloaterAtmoEnvCreate::onClickCreate()
{
    const EMode mode = currentMode();

    auto on_created = [](const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)
    {
        if (item_id.isNull() || asset_id.isNull()) return;
        SSAtmoEnvManager::getInstance()->adoptCreated(item_id, asset_id, asset);
        LLFloaterReg::showInstance("ss_atmo_env");
    };

    switch (mode)
    {
        case MODE_EMPTY:
            SSAtmoEnvManager::createEmptyNotecard(LLUUID::null, on_created);
            break;

        case MODE_STOCK:
            SSAtmoEnvManager::createDefaultNotecard(LLUUID::null, on_created);
            break;

        case MODE_SKIES:
        {
            if (mDroppedSkies.empty()) return;

            std::vector<LLUUID> ids;
            std::vector<std::string> names;
            ids.reserve(mDroppedSkies.size());
            names.reserve(mDroppedSkies.size());
            for (const DroppedSky& sky : mDroppedSkies)
            {
                ids.push_back(sky.mAssetId);
                names.push_back(sky.mName);
            }
            SSAtmoEnvManager::createFromSkies(ids, names, LLUUID::null, on_created);
            break;
        }

        case MODE_DAY_CYCLE:
        {
            if (mDroppedDayCycle.isNull()) return;

            SSAtmoEnvManager::createFromDayCycle(mDroppedDayCycle, LLUUID::null, on_created);
            break;
        }
    }

    closeFloater();
}
