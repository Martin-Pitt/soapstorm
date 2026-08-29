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
#include "llfolderviewitem.h"
#include "llfolderviewmodelinventory.h"
#include "llinventoryfilter.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "llnotificationsutil.h"
#include "llpermissionsflags.h"
#include "llradiogroup.h"
#include "lltextbox.h"
#include "llviewerinventory.h"

// Registration name this floater lives under - see LLViewerFloaterReg.
static const std::string CREATE_FLOATER_NAME = "ss_atmo_env_create";

// Floater shell; all content is wired in postBuild.
SSFloaterAtmoEnvCreate::SSFloaterAtmoEnvCreate(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterAtmoEnvCreate::postBuild()
{
    LLRadioGroup* mode = getChild<LLRadioGroup>("mode_radio");
    mode->setValue(MODE_EMPTY);
    mode->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });

    // Two pickers over one panel widget: both hold settings, and only the settings type
    // (sky vs day cycle) differs, which is a filter set here rather than in the XUI.
    LLInventoryPanel* skies = getChild<LLInventoryPanel>("skies_panel");
    skies->setFilterTypes(0x1 << LLInventoryType::IT_SETTINGS);
    skies->setFilterSettingsTypes(0x01 << static_cast<U64>(LLSettingsType::ST_SKY));
    skies->setShowFolderState(LLInventoryFilter::SHOW_NON_EMPTY_FOLDERS);
    skies->setSelectCallback([this](const std::deque<LLFolderViewItem*>&, bool) { refresh(); });

    LLInventoryPanel* day_cycles = getChild<LLInventoryPanel>("daycycle_panel");
    day_cycles->setFilterTypes(0x1 << LLInventoryType::IT_SETTINGS);
    day_cycles->setFilterSettingsTypes(0x01 << static_cast<U64>(LLSettingsType::ST_DAYCYCLE));
    day_cycles->setShowFolderState(LLInventoryFilter::SHOW_NON_EMPTY_FOLDERS);
    day_cycles->setSelectCallback([this](const std::deque<LLFolderViewItem*>&, bool) { refresh(); });

    getChild<LLUICtrl>("create_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickCreate(); });
    getChild<LLUICtrl>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { closeFloater(); });

    refresh();
    return true;
}

// Opens the chooser.
void SSFloaterAtmoEnvCreate::show()
{
    LLFloaterReg::showInstance(CREATE_FLOATER_NAME);
}

SSFloaterAtmoEnvCreate::EMode SSFloaterAtmoEnvCreate::currentMode() const
{
    return (EMode)getChild<LLRadioGroup>("mode_radio")->getValue().asInteger();
}

// The panel's selection narrowed to finished, full-perm settings of the asked-for kind. Folders
// and anything the item model cannot resolve drop out silently; the summary line accounts for
// what was dropped so an unusable pick does not vanish without a trace.
std::vector<LLViewerInventoryItem*> SSFloaterAtmoEnvCreate::usableSelection(
    LLInventoryPanel* panel, LLSettingsType::type_e type) const
{
    std::vector<LLViewerInventoryItem*> usable;
    if (!panel) return usable;

    for (LLFolderViewItem* view : panel->getSelectedItems())
    {
        const LLFolderViewModelItemInventory* model =
            dynamic_cast<const LLFolderViewModelItemInventory*>(view->getViewModelItem());
        if (!model) continue;

        LLViewerInventoryItem* item = gInventory.getItem(model->getUUID());
        if (!item || item->getAssetUUID().isNull()) continue;
        if (item->getSettingsType() != type) continue;
        if (!item->checkPermissionsSet(PERM_ITEM_UNRESTRICTED)) continue;

        usable.push_back(item);
    }
    return usable;
}

// Mode-specific visibility, button state and the summary line.
void SSFloaterAtmoEnvCreate::refresh()
{
    const EMode mode = currentMode();

    getChild<LLView>("skies_panel")->setVisible(mode == MODE_SKIES);
    getChild<LLView>("daycycle_panel")->setVisible(mode == MODE_DAY_CYCLE);

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
            LLInventoryPanel* panel = getChild<LLInventoryPanel>("skies_panel");
            const std::vector<LLViewerInventoryItem*> usable = usableSelection(panel, LLSettingsType::ST_SKY);

            const S32 picked = (S32)panel->getSelectedItems().size();
            if (usable.empty())
            {
                summary = "Select one or more skies - only full-permission sky settings can be imported.";
            }
            else if ((S32)usable.size() < picked)
            {
                summary = llformat("%d of %d selected are usable - only full-permission sky settings can be imported.",
                                   (S32)usable.size(), picked);
            }
            else
            {
                summary = llformat("%d sky%s selected - each is measured against the track's own sun and stamped as keyframes.",
                                   (S32)usable.size(), usable.size() == 1 ? "" : "s");
            }
            can_create = !usable.empty();
            break;
        }

        case MODE_DAY_CYCLE:
        {
            LLInventoryPanel* panel = getChild<LLInventoryPanel>("daycycle_panel");
            const std::vector<LLViewerInventoryItem*> usable = usableSelection(panel, LLSettingsType::ST_DAYCYCLE);

            if (usable.empty())
            {
                summary = "Select a day cycle - only full-permission settings can be imported. "
                          "Its sky keyframes map over at their authored times.";
            }
            else
            {
                summary = llformat("Maps \"%s\" over: every sky keyframe lands at its authored time of day.",
                                   usable.front()->getName().c_str());
            }
            can_create = !usable.empty();
            break;
        }
    }

    getChild<LLTextBox>("hint_text")->setText(summary);
    getChild<LLUICtrl>("create_button")->setEnabled(can_create);
}

// Runs the chosen seed and adopts what it wrote. The same on_created shape the editor's own
// create button used before the chooser existed - the editor picks the new asset up on its next
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
            std::vector<LLViewerInventoryItem*> usable =
                usableSelection(getChild<LLInventoryPanel>("skies_panel"), LLSettingsType::ST_SKY);
            if (usable.empty()) return;

            std::vector<LLUUID> ids;
            std::vector<std::string> names;
            ids.reserve(usable.size());
            names.reserve(usable.size());
            for (const LLViewerInventoryItem* item : usable)
            {
                ids.push_back(item->getAssetUUID());
                names.push_back(item->getName());
            }
            SSAtmoEnvManager::createFromSkies(ids, names, LLUUID::null, on_created);
            break;
        }

        case MODE_DAY_CYCLE:
        {
            std::vector<LLViewerInventoryItem*> usable =
                usableSelection(getChild<LLInventoryPanel>("daycycle_panel"), LLSettingsType::ST_DAYCYCLE);
            if (usable.empty()) return;

            SSAtmoEnvManager::createFromDayCycle(usable.front()->getAssetUUID(), LLUUID::null, on_created);
            break;
        }
    }

    closeFloater();
}
