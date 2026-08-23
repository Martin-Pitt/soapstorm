/**
 * @file ssfloaterpreset.cpp
 * @brief Atmo Magic preset editor implementation.
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

#include "ssfloaterpreset.h"
#include "ssatmotrack.h"
#include "ssprecipvariants.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

// <SS:Nexii> Atmo Magic preset editor

static const char* TIER_PREFIX[TIER_COUNT] = { "drops", "clusters", "sheets" };

SSFloaterPreset::SSFloaterPreset(const LLSD& key) :
    LLFloater(key)
{
}

bool SSFloaterPreset::postBuild()
{
    getChild<LLComboBox>("preset_combo")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSelectPreset(); });

    getChild<LLButton>("new_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickNew(); });
    getChild<LLButton>("blank_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickBlank(); });
    getChild<LLButton>("rename_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRename(); });
    getChild<LLButton>("delete_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickDelete(); });
    getChild<LLButton>("revert_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRevert(); });
    getChild<LLButton>("save_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickSave(); });
    getChild<LLButton>("discard_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickDiscard(); });

    // Every editable widget funnels through one handler: read the whole form
    // back into the preset, save it, and let the sim pick it up next frame
    static const char* widgets[] = {
        "archetype_combo", "fall_speed", "fall_lo", "fall_hi", "sway",
        "wind_response", "rate", "intensity_size", "tint", "glow", "drop_shape",
        "drop_scale", "emissive", "water_shading", "impact_strength", "shatter",
        "ripple_size", "ripple_alpha", "ripple_life",
        "crown_size", "crown_alpha", "crown_speed", "crown_life",
        "dark_mix", "puff_mix",
        "stream_alpha", "stream_span", "stream_length", "stream_wind",
        "stream_scale", "stream_stretch", "drip_scale",
        "textures", "ripple_texture",
        "dark_texture", "puff_texture",
        "snd_light", "snd_medium", "snd_heavy",
        "snd_roof_open", "snd_roof_small", "snd_roof_medium", "snd_roof_big",
    };
    for (const char* name : widgets)
    {
        if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
        {
            ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitAny(); });
        }
    }

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        static const char* suffixes[] = { "enabled", "kind", "size_x", "size_y", "alpha", "radius" };
        for (const char* suffix : suffixes)
        {
            const std::string name = std::string(TIER_PREFIX[t]) + "_" + suffix;
            if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
            {
                ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitAny(); });
            }
        }
    }

    refreshPresetList();
    return true;
}

void SSFloaterPreset::onOpen(const LLSD& key)
{
    refreshPresetList();

    // Open on whatever the weather is currently running, unless told otherwise
    std::string want = key.isString() ? key.asString() : std::string();
    if (want.empty())
    {
        want = gSavedSettings.getString("SSAtmoPreset");
    }
    if (want.empty())
    {
        want = SSAtmoMagic::getInstance()->preset().mName;
    }
    loadPreset(want);
}

void SSFloaterPreset::refreshPresetList()
{
    LLComboBox* combo = getChild<LLComboBox>("preset_combo");
    const std::string selected = combo->getSelectedItemLabel();

    combo->removeall();
    for (const SSPrecipPreset& p : SSPrecipPresetManager::instance().presets())
    {
        combo->add(p.mName, p.mName);
    }
    if (!selected.empty())
    {
        combo->selectByValue(selected);
    }
}

void SSFloaterPreset::loadPreset(const std::string& name)
{
    const SSPrecipPreset* found = SSPrecipPresetManager::instance().find(name);
    if (!found) return;

    mEdited = *found;
    getChild<LLComboBox>("preset_combo")->selectByValue(mEdited.mName);
    getChild<LLLineEditor>("preset_name_editor")->setText(mEdited.mName);
    presetToControls();
    refreshTitle();
}

void SSFloaterPreset::presetToControls()
{
    mUpdating = true;

    getChild<LLUICtrl>("archetype_combo")->setValue((S32)mEdited.mArchetype);
    getChild<LLUICtrl>("fall_speed")->setValue(mEdited.mFallSpeed);
    getChild<LLUICtrl>("fall_lo")->setValue(mEdited.mFallLo);
    getChild<LLUICtrl>("fall_hi")->setValue(mEdited.mFallHi);
    getChild<LLUICtrl>("sway")->setValue(mEdited.mSway);
    getChild<LLUICtrl>("wind_response")->setValue(mEdited.mWindResponse);
    getChild<LLUICtrl>("rate")->setValue(mEdited.mRate);
    getChild<LLUICtrl>("intensity_size")->setValue(mEdited.mIntensitySize);

    getChild<LLColorSwatchCtrl>("tint")->set(mEdited.mTint);
    getChild<LLUICtrl>("glow")->setValue(mEdited.mGlow);
    getChild<LLUICtrl>("drop_shape")->setValue((S32)mEdited.mDropShape);
    getChild<LLUICtrl>("drop_scale")->setValue(mEdited.mDropScale);
    getChild<LLUICtrl>("emissive")->setValue(mEdited.mEmissive);
    getChild<LLUICtrl>("water_shading")->setValue(mEdited.mWaterShading);

    getChild<LLUICtrl>("impact_strength")->setValue(mEdited.mImpactStrength);
    getChild<LLUICtrl>("shatter")->setValue(mEdited.mShatter);
    getChild<LLUICtrl>("ripple_size")->setValue(mEdited.mRippleSize);
    getChild<LLUICtrl>("ripple_alpha")->setValue(mEdited.mRippleAlpha);
    getChild<LLUICtrl>("ripple_life")->setValue(mEdited.mRippleLife);
    getChild<LLUICtrl>("crown_size")->setValue(mEdited.mCrownSize);
    getChild<LLUICtrl>("crown_alpha")->setValue(mEdited.mCrownAlpha);
    getChild<LLUICtrl>("crown_speed")->setValue(mEdited.mCrownSpeed);
    getChild<LLUICtrl>("crown_life")->setValue(mEdited.mCrownLife);
    getChild<LLUICtrl>("dark_mix")->setValue(mEdited.mDarkMix);
    getChild<LLUICtrl>("puff_mix")->setValue(mEdited.mPuffMix);

    getChild<LLUICtrl>("stream_alpha")->setValue(mEdited.mStreamAlpha);
    getChild<LLUICtrl>("stream_span")->setValue(mEdited.mStreamSpan);
    getChild<LLUICtrl>("stream_length")->setValue(mEdited.mStreamLength);
    getChild<LLUICtrl>("stream_stretch")->setValue(mEdited.mStreamStretch);
    getChild<LLUICtrl>("stream_wind")->setValue(mEdited.mStreamWind);
    getChild<LLUICtrl>("stream_scale")->setValue(mEdited.mStreamScale);
    getChild<LLUICtrl>("drip_scale")->setValue(mEdited.mDripScale);

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        const std::string prefix = TIER_PREFIX[t];
        getChild<LLUICtrl>(prefix + "_enabled")->setValue(mEdited.mTiers[t].mEnabled);
        getChild<LLUICtrl>(prefix + "_kind")->setValue((S32)mEdited.mTiers[t].mKind);
        getChild<LLUICtrl>(prefix + "_size_x")->setValue(mEdited.mTiers[t].mSizeX);
        getChild<LLUICtrl>(prefix + "_size_y")->setValue(mEdited.mTiers[t].mSizeY);
        getChild<LLUICtrl>(prefix + "_alpha")->setValue(mEdited.mTiers[t].mAlpha);
        getChild<LLUICtrl>(prefix + "_radius")->setValue(mEdited.mTiers[t].mRadius);
    }

    getChild<LLUICtrl>("textures")->setValue(mEdited.mTextures);
    getChild<LLUICtrl>("ripple_texture")->setValue(mEdited.mRippleTexture);
    getChild<LLUICtrl>("dark_texture")->setValue(mEdited.mDarkTexture);
    getChild<LLUICtrl>("puff_texture")->setValue(mEdited.mPuffTexture);

    getChild<LLUICtrl>("snd_light")->setValue(mEdited.mSounds.mAmbientLight);
    getChild<LLUICtrl>("snd_medium")->setValue(mEdited.mSounds.mAmbientMedium);
    getChild<LLUICtrl>("snd_heavy")->setValue(mEdited.mSounds.mAmbientHeavy);
    getChild<LLUICtrl>("snd_roof_open")->setValue(mEdited.mSounds.mRoofOpen);
    getChild<LLUICtrl>("snd_roof_small")->setValue(mEdited.mSounds.mRoofSmall);
    getChild<LLUICtrl>("snd_roof_medium")->setValue(mEdited.mSounds.mRoofMedium);
    getChild<LLUICtrl>("snd_roof_big")->setValue(mEdited.mSounds.mRoofBig);

    mUpdating = false;
}

void SSFloaterPreset::controlsToPreset()
{
    mEdited.mArchetype = (SSPrecipArchetype)llclamp(getChild<LLUICtrl>("archetype_combo")->getValue().asInteger(),
                                                    0, (S32)SSPrecipArchetype::COUNT - 1);
    mEdited.mFallSpeed = (F32)getChild<LLUICtrl>("fall_speed")->getValue().asReal();
    mEdited.mFallLo = (F32)getChild<LLUICtrl>("fall_lo")->getValue().asReal();
    mEdited.mFallHi = llmax(mEdited.mFallLo, (F32)getChild<LLUICtrl>("fall_hi")->getValue().asReal());
    mEdited.mSway = (F32)getChild<LLUICtrl>("sway")->getValue().asReal();
    mEdited.mWindResponse = (F32)getChild<LLUICtrl>("wind_response")->getValue().asReal();
    mEdited.mRate = (F32)getChild<LLUICtrl>("rate")->getValue().asReal();
    mEdited.mIntensitySize = (F32)getChild<LLUICtrl>("intensity_size")->getValue().asReal();

    mEdited.mTint = getChild<LLColorSwatchCtrl>("tint")->get();
    mEdited.mGlow = (F32)getChild<LLUICtrl>("glow")->getValue().asReal();
    mEdited.mDropShape = (U8)llclamp(getChild<LLUICtrl>("drop_shape")->getValue().asInteger(), 0, 2);
    mEdited.mDropScale = (F32)getChild<LLUICtrl>("drop_scale")->getValue().asReal();
    mEdited.mEmissive = getChild<LLUICtrl>("emissive")->getValue().asBoolean();
    mEdited.mWaterShading = getChild<LLUICtrl>("water_shading")->getValue().asBoolean();

    mEdited.mImpactStrength = (F32)getChild<LLUICtrl>("impact_strength")->getValue().asReal();
    mEdited.mShatter = getChild<LLUICtrl>("shatter")->getValue().asBoolean();
    mEdited.mRippleSize = (F32)getChild<LLUICtrl>("ripple_size")->getValue().asReal();
    mEdited.mRippleAlpha = (F32)getChild<LLUICtrl>("ripple_alpha")->getValue().asReal();
    mEdited.mRippleLife = (F32)getChild<LLUICtrl>("ripple_life")->getValue().asReal();
    mEdited.mCrownSize = (F32)getChild<LLUICtrl>("crown_size")->getValue().asReal();
    mEdited.mCrownAlpha = (F32)getChild<LLUICtrl>("crown_alpha")->getValue().asReal();
    mEdited.mCrownSpeed = (F32)getChild<LLUICtrl>("crown_speed")->getValue().asReal();
    mEdited.mCrownLife = (F32)getChild<LLUICtrl>("crown_life")->getValue().asReal();
    mEdited.mDarkMix = (F32)getChild<LLUICtrl>("dark_mix")->getValue().asReal();
    mEdited.mPuffMix = (F32)getChild<LLUICtrl>("puff_mix")->getValue().asReal();

    mEdited.mStreamAlpha = (F32)getChild<LLUICtrl>("stream_alpha")->getValue().asReal();
    mEdited.mStreamSpan = (F32)getChild<LLUICtrl>("stream_span")->getValue().asReal();
    mEdited.mStreamLength = (F32)getChild<LLUICtrl>("stream_length")->getValue().asReal();
    mEdited.mStreamStretch = (F32)getChild<LLUICtrl>("stream_stretch")->getValue().asReal();
    mEdited.mStreamWind = (F32)getChild<LLUICtrl>("stream_wind")->getValue().asReal();
    mEdited.mStreamScale = (F32)getChild<LLUICtrl>("stream_scale")->getValue().asReal();
    mEdited.mDripScale = (F32)getChild<LLUICtrl>("drip_scale")->getValue().asReal();

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        const std::string prefix = TIER_PREFIX[t];
        mEdited.mTiers[t].mEnabled = getChild<LLUICtrl>(prefix + "_enabled")->getValue().asBoolean();
        mEdited.mTiers[t].mKind = (U8)llclamp(getChild<LLUICtrl>(prefix + "_kind")->getValue().asInteger(), 0, 3);
        mEdited.mTiers[t].mSizeX = (F32)getChild<LLUICtrl>(prefix + "_size_x")->getValue().asReal();
        mEdited.mTiers[t].mSizeY = (F32)getChild<LLUICtrl>(prefix + "_size_y")->getValue().asReal();
        mEdited.mTiers[t].mAlpha = (F32)getChild<LLUICtrl>(prefix + "_alpha")->getValue().asReal();
        mEdited.mTiers[t].mRadius = (F32)getChild<LLUICtrl>(prefix + "_radius")->getValue().asReal();
    }

    mEdited.mTextures = getChild<LLUICtrl>("textures")->getValue().asString();
    mEdited.mRippleTexture = getChild<LLUICtrl>("ripple_texture")->getValue().asString();
    mEdited.mDarkTexture = getChild<LLUICtrl>("dark_texture")->getValue().asString();
    mEdited.mPuffTexture = getChild<LLUICtrl>("puff_texture")->getValue().asString();

    mEdited.mSounds.mAmbientLight = getChild<LLUICtrl>("snd_light")->getValue().asString();
    mEdited.mSounds.mAmbientMedium = getChild<LLUICtrl>("snd_medium")->getValue().asString();
    mEdited.mSounds.mAmbientHeavy = getChild<LLUICtrl>("snd_heavy")->getValue().asString();
    mEdited.mSounds.mRoofOpen = getChild<LLUICtrl>("snd_roof_open")->getValue().asString();
    mEdited.mSounds.mRoofSmall = getChild<LLUICtrl>("snd_roof_small")->getValue().asString();
    mEdited.mSounds.mRoofMedium = getChild<LLUICtrl>("snd_roof_medium")->getValue().asString();
    mEdited.mSounds.mRoofBig = getChild<LLUICtrl>("snd_roof_big")->getValue().asString();
}

void SSFloaterPreset::applyLive()
{
    // Staged, not saved: the weather picks the edit up immediately so it can be
    // dialled in while watching it fall, but nothing reaches disk until Save.
    // The asterisk in the title is exactly that gap.
    SSPrecipPresetManager::instance().stage(mEdited);

    // Sizes and shapes are baked into the splatter textures, so drop the
    // bakes; they are keyed on the shape fields and would otherwise linger
    SSPrecipVariants::instance().clearCache();

    // Editing a preset implies you want to see it
    gSavedSettings.setString("SSAtmoPreset", mEdited.mName);

    refreshTitle();
}

void SSFloaterPreset::refreshTitle()
{
    const bool modified = SSPrecipPresetManager::instance().isModified(mEdited.mName);
    setTitle("ATMO MAGIC - PRESET EDITOR - " + mEdited.mName + (modified ? " *" : ""));
    getChild<LLUICtrl>("save_button")->setEnabled(modified);
}

void SSFloaterPreset::onClickSave()
{
    SSPrecipPresetManager::instance().save(mEdited);
    refreshPresetList();
    refreshTitle();
}

void SSFloaterPreset::onCommitAny()
{
    if (mUpdating) return;
    controlsToPreset();
    applyLive();
}

void SSFloaterPreset::onSelectPreset()
{
    loadPreset(getChild<LLComboBox>("preset_combo")->getValue().asString());
    gSavedSettings.setString("SSAtmoPreset", mEdited.mName);
}

void SSFloaterPreset::onClickNew()
{
    // Copy the preset on screen under a new name; built-ins stay untouched
    std::string base = mEdited.mName + " copy";
    std::string name = base;
    for (S32 i = 2; SSPrecipPresetManager::instance().find(name) && i < 100; ++i)
    {
        name = base + " " + llformat("%d", i);
    }

    mEdited.mName = name;
    mEdited.mBuiltIn = false;
    applyLive();
    refreshPresetList();
    getChild<LLComboBox>("preset_combo")->selectByValue(name);
}

// Returns a name based on "base" that no preset is using yet
static std::string uniquePresetName(const std::string& base)
{
    std::string name = base;
    for (S32 i = 2; SSPrecipPresetManager::instance().find(name) && i < 1000; ++i)
    {
        name = base + " " + llformat("%d", i);
    }
    return name;
}

void SSFloaterPreset::onClickBlank()
{
    // A fresh preset at its defaults, rather than a copy of what is on screen
    mEdited = SSPrecipPreset();
    mEdited.mName = uniquePresetName("New preset");
    mEdited.mBuiltIn = false;

    applyLive();
    refreshPresetList();
    getChild<LLComboBox>("preset_combo")->selectByValue(mEdited.mName);
    presetToControls();
}

void SSFloaterPreset::onClickRename()
{
    std::string name = getChild<LLLineEditor>("preset_name_editor")->getText();
    LLStringUtil::trim(name);

    if (name.empty() || name == mEdited.mName) return;

    if (SSPrecipPresetManager::instance().find(name))
    {
        LLNotificationsUtil::add("GenericAlert",
            LLSD().with("MESSAGE", "A preset with that name already exists."));
        return;
    }

    const std::string old_name = mEdited.mName;

    // Write the preset out under its new name first, so nothing is lost if the
    // old file cannot be removed. A renamed built-in becomes a user preset and
    // the shipped one returns to the list untouched.
    mEdited.mName = name;
    mEdited.mBuiltIn = false;
    SSPrecipPresetManager::instance().save(mEdited);
    SSPrecipPresetManager::instance().remove(old_name);

    // Follow the rename everywhere the old name was referenced, otherwise a
    // track would silently fall back to the editor default
    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    bool touched = false;
    for (S32 track = SS_TRACK_MIN; track <= SS_TRACK_MAX; ++track)
    {
        SSAtmoTrackConfig& cfg = tracks->editable(track);
        if (cfg.mPreset == old_name)
        {
            cfg.mPreset = name;
            touched = true;
        }
    }
    if (touched) tracks->commit();

    if (gSavedSettings.getString("SSAtmoPreset") == old_name)
    {
        gSavedSettings.setString("SSAtmoPreset", name);
    }

    SSPrecipVariants::instance().clearCache();
    refreshPresetList();
    getChild<LLComboBox>("preset_combo")->selectByValue(name);
}

void SSFloaterPreset::onClickDelete()
{
    const std::string name = mEdited.mName;
    if (!SSPrecipPresetManager::instance().remove(name))
    {
        // Built-ins have no file unless they were overridden, so there is
        // nothing to remove
        LLNotificationsUtil::add("GenericAlert",
            LLSD().with("MESSAGE", "This preset has no saved copy to delete."));
        return;
    }

    refreshPresetList();
    const auto& presets = SSPrecipPresetManager::instance().presets();
    if (!presets.empty())
    {
        loadPreset(presets.front().mName);
    }
}

void SSFloaterPreset::onClickDiscard()
{
    // Put the version on disk back, dropping whatever was staged. A preset that
    // has never been written has nothing to go back to.
    const SSPrecipPreset* saved = SSPrecipPresetManager::instance().findSaved(mEdited.mName);
    if (!saved) return;

    SSPrecipPresetManager::instance().stage(*saved);
    SSPrecipVariants::instance().clearCache();
    loadPreset(mEdited.mName);
}

void SSFloaterPreset::onClickRevert()
{
    // Drop the saved override so a built-in returns to its shipped values
    SSPrecipPresetManager::instance().remove(mEdited.mName);
    SSPrecipVariants::instance().clearCache();
    refreshPresetList();
    loadPreset(mEdited.mName);
}

// </SS:Nexii>
