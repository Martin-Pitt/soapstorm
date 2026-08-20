/**
 * @file ssprecippreset.cpp
 * @brief Atmo Magic weather preset defaults, serialization and store.
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

#include "ssprecippreset.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llviewercontrol.h"

// <SS:Nexii> Atmo Magic weather presets

// static
const char* SSPrecipPreset::archetypeName(SSPrecipArchetype a)
{
    switch (a)
    {
        case SSPrecipArchetype::FLAKE: return "Flake";
        case SSPrecipArchetype::SOLID: return "Solid";
        case SSPrecipArchetype::RISER: return "Riser";
        default:                       return "Liquid";
    }
}

LLSD SSPrecipPreset::asLLSD() const
{
    LLSD sd;
    sd["name"] = mName;
    sd["archetype"] = (S32)mArchetype;

    sd["fall_speed"] = mFallSpeed;
    sd["fall_lo"] = mFallLo;
    sd["fall_hi"] = mFallHi;
    sd["sway"] = mSway;
    sd["wind_response"] = mWindResponse;

    sd["rate"] = mRate;
    sd["intensity_size"] = mIntensitySize;

    sd["tint"] = mTint.getValue();
    sd["glow"] = mGlow;
    sd["drop_shape"] = (S32)mDropShape;
    sd["drop_scale"] = mDropScale;
    sd["emissive"] = mEmissive;
    sd["water_shading"] = mWaterShading;

    for (S32 i = 0; i < TIER_COUNT; ++i)
    {
        LLSD tier;
        tier["enabled"] = mTiers[i].mEnabled;
        tier["kind"] = (S32)mTiers[i].mKind;
        tier["size_x"] = mTiers[i].mSizeX;
        tier["size_y"] = mTiers[i].mSizeY;
        tier["alpha"] = mTiers[i].mAlpha;
        tier["radius"] = mTiers[i].mRadius;
        sd["tiers"].append(tier);
    }

    sd["impact_strength"] = mImpactStrength;
    sd["shatter"] = mShatter;
    sd["ripple_size"] = mRippleSize;
    sd["ripple_alpha"] = mRippleAlpha;
    sd["ripple_life"] = mRippleLife;
    sd["crown_size"] = mCrownSize;
    sd["crown_alpha"] = mCrownAlpha;
    sd["crown_speed"] = mCrownSpeed;
    sd["crown_life"] = mCrownLife;
    sd["dark_mix"] = mDarkMix;
    sd["puff_mix"] = mPuffMix;

    sd["stream_span"] = mStreamSpan;
    sd["stream_alpha"] = mStreamAlpha;
    sd["stream_length"] = mStreamLength;
    sd["stream_stretch"] = mStreamStretch;
    sd["stream_wind"] = mStreamWind;
    sd["stream_scale"] = mStreamScale;
    sd["drip_scale"] = mDripScale;

    sd["textures"] = mTextures;
    sd["ripple_texture"] = mRippleTexture;
    sd["dark_texture"] = mDarkTexture;
    sd["puff_texture"] = mPuffTexture;

    sd["snd_light"] = mSounds.mAmbientLight;
    sd["snd_medium"] = mSounds.mAmbientMedium;
    sd["snd_heavy"] = mSounds.mAmbientHeavy;
    sd["snd_roof_open"] = mSounds.mRoofOpen;
    sd["snd_roof_small"] = mSounds.mRoofSmall;
    sd["snd_roof_medium"] = mSounds.mRoofMedium;
    sd["snd_roof_big"] = mSounds.mRoofBig;

    return sd;
}

void SSPrecipPreset::fromLLSD(const LLSD& sd)
{
    if (sd.has("name")) mName = sd["name"].asString();
    if (sd.has("archetype"))
    {
        mArchetype = (SSPrecipArchetype)llclamp(sd["archetype"].asInteger(),
                                                0, (S32)SSPrecipArchetype::COUNT - 1);
    }

    if (sd.has("fall_speed")) mFallSpeed = (F32)sd["fall_speed"].asReal();
    if (sd.has("fall_lo")) mFallLo = (F32)sd["fall_lo"].asReal();
    if (sd.has("fall_hi")) mFallHi = (F32)sd["fall_hi"].asReal();
    if (sd.has("sway")) mSway = (F32)sd["sway"].asReal();
    if (sd.has("wind_response")) mWindResponse = (F32)sd["wind_response"].asReal();

    if (sd.has("rate")) mRate = (F32)sd["rate"].asReal();
    if (sd.has("intensity_size")) mIntensitySize = (F32)sd["intensity_size"].asReal();

    if (sd.has("tint")) mTint.setValue(sd["tint"]);
    if (sd.has("glow")) mGlow = (F32)sd["glow"].asReal();
    if (sd.has("drop_shape")) mDropShape = (U8)llclamp(sd["drop_shape"].asInteger(), 0, 2);
    if (sd.has("drop_scale")) mDropScale = (F32)sd["drop_scale"].asReal();
    if (sd.has("emissive")) mEmissive = sd["emissive"].asBoolean();
    if (sd.has("water_shading")) mWaterShading = sd["water_shading"].asBoolean();

    if (sd.has("tiers"))
    {
        const LLSD& tiers = sd["tiers"];
        for (S32 i = 0; i < TIER_COUNT && i < (S32)tiers.size(); ++i)
        {
            const LLSD& t = tiers[i];
            mTiers[i].mEnabled = t["enabled"].asBoolean();
            mTiers[i].mKind = (U8)llclamp(t["kind"].asInteger(), 0, 3);
            mTiers[i].mSizeX = (F32)t["size_x"].asReal();
            mTiers[i].mSizeY = (F32)t["size_y"].asReal();
            mTiers[i].mAlpha = (F32)t["alpha"].asReal();
            mTiers[i].mRadius = (F32)t["radius"].asReal();
        }
    }

    if (sd.has("impact_strength")) mImpactStrength = (F32)sd["impact_strength"].asReal();
    if (sd.has("shatter")) mShatter = sd["shatter"].asBoolean();
    // Presets written before these existed keep the defaults, which are the
    // values the splash used when it was hardcoded
    if (sd.has("ripple_size")) mRippleSize = (F32)sd["ripple_size"].asReal();
    if (sd.has("ripple_alpha")) mRippleAlpha = (F32)sd["ripple_alpha"].asReal();
    if (sd.has("ripple_life")) mRippleLife = (F32)sd["ripple_life"].asReal();
    if (sd.has("crown_size")) mCrownSize = (F32)sd["crown_size"].asReal();
    if (sd.has("crown_alpha")) mCrownAlpha = (F32)sd["crown_alpha"].asReal();
    if (sd.has("crown_speed")) mCrownSpeed = (F32)sd["crown_speed"].asReal();
    if (sd.has("crown_life")) mCrownLife = (F32)sd["crown_life"].asReal();
    if (sd.has("dark_mix")) mDarkMix = (F32)sd["dark_mix"].asReal();
    if (sd.has("puff_mix")) mPuffMix = (F32)sd["puff_mix"].asReal();

    if (sd.has("stream_span")) mStreamSpan = (F32)sd["stream_span"].asReal();
    if (sd.has("stream_alpha")) mStreamAlpha = (F32)sd["stream_alpha"].asReal();
    if (sd.has("stream_length"))
    {
        mStreamLength = (F32)sd["stream_length"].asReal();
    }
    else if (sd.has("stream_reach"))
    {
        // Presets written before the length was in metres carried a fraction
        // of the way to the ground instead. There is no roof to measure it
        // against here, so it is taken against the tallest fall the slider
        // offers: a preset that reached all the way down still does.
        mStreamLength = llclamp((F32)sd["stream_reach"].asReal(), 0.05f, 1.f) * SS_STREAM_LENGTH_MAX;
    }
    if (sd.has("stream_stretch")) mStreamStretch = (F32)sd["stream_stretch"].asReal();
    if (sd.has("stream_wind")) mStreamWind = (F32)sd["stream_wind"].asReal();
    if (sd.has("stream_scale")) mStreamScale = (F32)sd["stream_scale"].asReal();
    if (sd.has("drip_scale")) mDripScale = (F32)sd["drip_scale"].asReal();

    if (sd.has("textures")) mTextures = sd["textures"].asString();
    if (sd.has("ripple_texture")) mRippleTexture = sd["ripple_texture"].asString();
    if (sd.has("dark_texture")) mDarkTexture = sd["dark_texture"].asString();
    if (sd.has("puff_texture")) mPuffTexture = sd["puff_texture"].asString();

    if (sd.has("snd_light")) mSounds.mAmbientLight = sd["snd_light"].asString();
    if (sd.has("snd_medium")) mSounds.mAmbientMedium = sd["snd_medium"].asString();
    if (sd.has("snd_heavy")) mSounds.mAmbientHeavy = sd["snd_heavy"].asString();
    if (sd.has("snd_roof_open")) mSounds.mRoofOpen = sd["snd_roof_open"].asString();
    if (sd.has("snd_roof_small")) mSounds.mRoofSmall = sd["snd_roof_small"].asString();
    if (sd.has("snd_roof_medium")) mSounds.mRoofMedium = sd["snd_roof_medium"].asString();
    if (sd.has("snd_roof_big")) mSounds.mRoofBig = sd["snd_roof_big"].asString();
}

SSPrecipPresetMgr::SSPrecipPresetMgr()
{
    refresh();
}

// static
std::string SSPrecipPresetMgr::presetDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather");
}

void SSPrecipPresetMgr::refresh()
{
    mPresets.clear();
    buildDefaults();
    loadUserPresets();

    // Everything just came from disk, so nothing is staged
    mSaved = mPresets;
}

void SSPrecipPresetMgr::stage(const SSPrecipPreset& preset)
{
    if (preset.mName.empty()) return;

    for (SSPrecipPreset& existing : mPresets)
    {
        if (existing.mName == preset.mName)
        {
            existing = preset;
            return;
        }
    }
    mPresets.push_back(preset);
}

const SSPrecipPreset* SSPrecipPresetMgr::findSaved(const std::string& name) const
{
    for (const SSPrecipPreset& p : mSaved)
    {
        if (p.mName == name) return &p;
    }
    return nullptr;
}

bool SSPrecipPresetMgr::isModified(const std::string& name) const
{
    const SSPrecipPreset* live = find(name);
    if (!live) return false;

    const SSPrecipPreset* saved = findSaved(name);
    if (!saved) return true;    // brand new, never written

    // Compared through the serialisation rather than field by field, so a
    // preset gaining a field cannot silently stop counting as modified
    return !llsd_equals(live->asLLSD(), saved->asLLSD());
}

const SSPrecipPreset* SSPrecipPresetMgr::find(const std::string& name) const
{
    for (const SSPrecipPreset& p : mPresets)
    {
        if (p.mName == name) return &p;
    }
    return nullptr;
}

const SSPrecipPreset& SSPrecipPresetMgr::active() const
{
    static LLCachedControl<std::string> selected(gSavedSettings, "SSAtmoPreset", "Rain");
    if (const SSPrecipPreset* p = find(selected))
    {
        return *p;
    }
    return mPresets.front();
}

bool SSPrecipPresetMgr::save(const SSPrecipPreset& preset)
{
    if (preset.mName.empty()) return false;

    const std::string dir = presetDir();
    if (!gDirUtilp->fileExists(dir))
    {
        LLFile::mkdir(dir);
    }

    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather",
                                                            preset.mName + ".xml");
    llofstream out(path.c_str());
    if (!out.is_open()) return false;
    LLSDSerialize::toPrettyXML(preset.asLLSD(), out);
    out.close();

    refresh();
    return true;
}

bool SSPrecipPresetMgr::remove(const std::string& name)
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather",
                                                            name + ".xml");
    if (!gDirUtilp->fileExists(path)) return false;

    LLFile::remove(path);
    refresh();
    return true;
}

void SSPrecipPresetMgr::loadUserPresets()
{
    const std::string dir = presetDir();
    if (!gDirUtilp->fileExists(dir)) return;

    for (const std::string& file : gDirUtilp->getFilesInDir(dir))
    {
        if (file.size() < 5 || file.compare(file.size() - 4, 4, ".xml") != 0) continue;

        llifstream in(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather", file).c_str());
        if (!in.is_open()) continue;

        LLSD sd;
        const S32 parsed = LLSDSerialize::fromXML(sd, in);
        in.close();
        if (parsed == LLSDParser::PARSE_FAILURE || !sd.isMap()) continue;

        SSPrecipPreset preset;
        preset.fromLLSD(sd);
        if (preset.mName.empty())
        {
            preset.mName = file.substr(0, file.size() - 4);
        }
        preset.mBuiltIn = false;

        // A saved preset sharing a built-in name overrides it, so a shipped
        // default can be retuned in place without losing the name
        bool replaced = false;
        for (SSPrecipPreset& existing : mPresets)
        {
            if (existing.mName == preset.mName)
            {
                existing = preset;
                replaced = true;
                break;
            }
        }
        if (!replaced) mPresets.push_back(preset);
    }
}

void SSPrecipPresetMgr::buildDefaults()
{
    // Rain: one preset spanning drizzle to downpour. mIntensitySize lets the
    // global precipitation slider grow the drops, not just add more of them.
    {
        SSPrecipPreset p;
        p.mName = "Rain";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::LIQUID;
        p.mFallSpeed = 9.5f;
        p.mFallLo = 16.f;  p.mFallHi = 26.f;
        p.mWindResponse = 1.f;
        p.mRate = 0.5f;
        p.mIntensitySize = 1.f;
        p.mWaterShading = true;
        p.mDropShape = DROP_TEARDROP;
        p.mImpactStrength = 1.0f;
        p.mRippleSize = 0.3f;   p.mRippleAlpha = 0.15f;  p.mRippleLife = 0.4f;
        p.mCrownSize  = 0.16f;  p.mCrownAlpha  = 0.11f;
        p.mCrownSpeed = 0.65f;  p.mCrownLife   = 0.4f;
        p.mTiers[TIER_DROPS]    = { true, KIND_STREAK, 0.028f, 0.55f, 0.8f, 18.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_STREAK, 0.38f,  1.9f,  0.5f, 96.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET,  9.f,    18.f,  0.3f, 224.f };

        // Sound pack. Each slot is a comma separated sequence played through
        // in order rather than a single looping asset, so the bed does not
        // settle into an audible 30 second repeat. Light is left empty: its
        // share folds back into the medium bed.
        p.mSounds.mAmbientMedium =
            "599c2146-48cf-d94f-b065-4266c2647f05,"
            "c2feb9c1-2b54-f42b-0f32-4cadcf48c6e4,"
            "c8cd50e0-6def-572f-872f-cc95a0cc275f,"
            "28aab769-2d8e-1225-eaeb-b2ae38b4ed69,"
            "39513c26-db70-cbd0-e32c-29619d9e2451,"
            "2f8d4ead-390e-fafe-3942-1b311afb5126";
        p.mSounds.mAmbientHeavy =
            "438e134b-93cf-9d1b-54b0-b0fec06364cd,"
            "1f8c2c1a-ebc1-fd1e-e669-575913cc155e,"
            "d1518660-67d3-e667-8422-4a25448e740e,"
            "48d30d8e-599c-d87e-2e40-aa3c93f9eba9";
        p.mSounds.mRoofOpen =
            "8ca2fb89-43f3-baa0-b4de-fdc1ef748239,"
            "6979e7e5-dfa8-fc4c-65fa-5e10149c5e9b,"
            "ba3728fc-545c-0c1d-7181-89559cb8e5f3,"
            "775af013-66af-df1b-db87-01d55afe4161,"
            "f2481ef2-c06c-2b3a-5a8a-29f10087aea1,"
            "0025b87e-3ff8-5f59-85a7-adef2d15b131";
        p.mSounds.mRoofSmall =
            "507a6b0d-d935-eb37-9e87-682970052c22";
        p.mSounds.mRoofMedium =
            "e1eda37b-4b1b-fd3a-f54f-67a83bd31ef4,"
            "5a9c8089-f637-dab2-f5dc-dcf87f95f775,"
            "d82b7010-ff2d-6163-c664-6a4c3feb19ed,"
            "64f24176-e927-3a48-8196-d45cb9176555,"
            "8c15d427-84a7-d07e-228a-0a539d374063";
        p.mSounds.mRoofBig =
            "3056a7c3-f0af-7ffd-3208-52a3b401d18c,"
            "e30cf3a1-c0c8-907a-04c7-06bb70945601";

        mPresets.push_back(p);
    }

    {
        SSPrecipPreset p;
        p.mName = "Snow";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::FLAKE;
        p.mFallSpeed = 1.4f;
        p.mFallLo = 5.f;  p.mFallHi = 9.f;
        p.mSway = 0.6f;
        p.mWindResponse = 1.f;
        p.mRate = 0.40f;
        p.mImpactStrength = 0.f;
        p.mTiers[TIER_DROPS]    = { true, KIND_ROUND, 0.055f, 0.055f, 0.85f, 24.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_ROUND, 0.35f,  0.35f,  0.35f, 64.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET, 4.f,    8.f,    0.10f, 128.f };
        mPresets.push_back(p);
    }

    {
        SSPrecipPreset p;
        p.mName = "Blizzard";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::FLAKE;
        p.mFallSpeed = 2.5f;
        p.mFallLo = 7.f;  p.mFallHi = 12.f;
        p.mSway = 2.2f;
        p.mWindResponse = 2.2f;
        p.mRate = 0.15f;
        p.mImpactStrength = 0.f;
        p.mTiers[TIER_DROPS]    = { true, KIND_ROUND, 0.04f, 0.04f, 0.70f, 24.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_ROUND, 0.4f,  0.4f,  0.30f, 64.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET, 8.f,   10.f,  0.12f, 128.f };
        mPresets.push_back(p);
    }

    {
        SSPrecipPreset p;
        p.mName = "Diamond Dust";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::FLAKE;
        p.mFallSpeed = 0.6f;
        p.mFallLo = 4.f;  p.mFallHi = 7.f;
        p.mSway = 0.6f;
        p.mWindResponse = 1.f;
        p.mRate = 0.10f;
        p.mGlow = 0.08f;
        p.mEmissive = true;
        p.mImpactStrength = 0.f;
        p.mTiers[TIER_DROPS]    = { true, KIND_ROUND, 0.02f, 0.02f, 0.50f, 20.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_ROUND, 0.25f, 0.25f, 0.20f, 48.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET, 4.f,   6.f,   0.07f, 96.f };
        mPresets.push_back(p);
    }

    {
        SSPrecipPreset p;
        p.mName = "Hail";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::SOLID;
        p.mFallSpeed = 20.f;
        p.mFallLo = 22.f;  p.mFallHi = 30.f;
        p.mWindResponse = 1.f;
        p.mRate = 0.08f;
        p.mImpactStrength = 1.2f;
        p.mTiers[TIER_DROPS]    = { true, KIND_ROUND, 0.085f, 0.085f, 1.00f, 28.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_ROUND, 0.45f,  0.45f,  0.60f, 96.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET, 7.f,    14.f,   0.10f, 224.f };
        mPresets.push_back(p);
    }

    {
        SSPrecipPreset p;
        p.mName = "Mana Embers";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::RISER;
        p.mFallSpeed = 0.9f;    // rise speed
        p.mFallLo = 2.f;  p.mFallHi = 4.f;
        p.mSway = 0.6f;
        p.mWindResponse = 0.15f;
        p.mRate = 0.08f;
        p.mTint = LLColor4(0.72f, 0.55f, 1.f, 1.f);
        p.mGlow = 0.55f;
        p.mEmissive = true;
        p.mImpactStrength = 0.f;
        p.mDarkMix = 0.25f;
        p.mPuffMix = 0.20f;
        p.mTiers[TIER_DROPS]    = { true, KIND_ROUND, 0.06f, 0.06f, 0.85f, 20.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_ROUND, 0.35f, 0.35f, 0.35f, 48.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET, 4.f,   5.f,   0.12f, 96.f };
        mPresets.push_back(p);
    }

    {
        SSPrecipPreset p;
        p.mName = "Mana Hail";
        p.mBuiltIn = true;
        p.mArchetype = SSPrecipArchetype::SOLID;
        p.mFallSpeed = 14.f;
        p.mFallLo = 24.f;  p.mFallHi = 34.f;
        p.mWindResponse = 1.f;
        p.mRate = 0.30f;
        p.mTint = LLColor4(0.72f, 0.55f, 1.f, 1.f);
        p.mGlow = 0.85f;
        p.mEmissive = true;
        p.mImpactStrength = 1.4f;
        p.mShatter = true;
        p.mDropShape = DROP_TEARDROP;
        p.mTiers[TIER_DROPS]    = { true, KIND_STREAK, 0.05f, 0.22f, 1.00f, 28.f };
        p.mTiers[TIER_CLUSTERS] = { true, KIND_STREAK, 0.3f,  0.9f,  0.18f, 96.f };
        p.mTiers[TIER_SHEETS]   = { true, KIND_SHEET,  8.f,   16.f,  0.05f, 224.f };
        mPresets.push_back(p);
    }
}

// </SS:Nexii>
