/**
 * @file ssprecippreset.h
 * @brief Atmo Magic weather presets: every per-type constant that used to be
 *        baked into switch tables now lives in an editable preset. A preset
 *        pairs a motion archetype (how it falls and lands) with the visual,
 *        density, impact and asset parameters that make it a specific kind of
 *        weather, so new weather can be authored without touching code.
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

#ifndef SS_PRECIPPRESET_H
#define SS_PRECIPPRESET_H

// <SS:Nexii> Atmo Magic weather presets

#include "llsd.h"
#include "llsingleton.h"
#include "v4color.h"

#include <string>
#include <vector>

// Detail tiers, nearest first. Avoid NEAR/FAR: Windows headers macro those.
enum SSPrecipTier : U8
{
    TIER_DROPS = 0,     // individual drops close around the camera
    TIER_CLUSTERS,      // one particle stands in for a handful of drops
    TIER_SHEETS,        // large shower curtains in the distance
    TIER_COUNT
};

// How a particle's quad is built by the renderer
enum SSPrecipKind : U8
{
    KIND_STREAK = 0,    // stretched along velocity, billboarded around that axis
    KIND_ROUND,         // camera-facing
    KIND_SHEET,         // like STREAK but around the mean fall direction
    KIND_FLAT           // surface-aligned, grows and fades (ripples)
};

// Which shading path the renderer uses
enum SSPrecipMaterial : U8
{
    MAT_LIT = 0,        // probe ambient + shadowed sun
    MAT_WATER,          // refraction/env/specular rain shader
    MAT_EMISSIVE,       // additive, writes glow
    MAT_COUNT
};

// Silhouette of a single drop in the generated textures. Teardrops read as
// liquid, so they are opt-in per preset rather than inferred from the drop
// being elongated - an elongated flake or crystal wants a different shape.
enum SSPrecipDropShape : U8
{
    DROP_DOT = 0,       // soft round mote
    DROP_TEARDROP,      // rounded head leading the fall, tapering tail
    DROP_SLIVER         // hard shard, tapered at both ends
};

// How a preset moves and lands. This is the part that stays in code; the
// numbers that shape it all live in the preset.
enum class SSPrecipArchetype : S32
{
    LIQUID = 0,     // rain: water shading, spreading ripples
    FLAKE,          // snow and dust: slow, swaying, no impact
    SOLID,          // hail: fast, hard impact, optionally shatters
    RISER,          // embers: rises out of the ground instead of falling
    COUNT
};

struct SSPrecipTierParams
{
    bool mEnabled = true;
    U8   mKind = KIND_ROUND;
    F32  mSizeX = 0.05f;    // half-width, meters
    F32  mSizeY = 0.05f;    // half-height, meters
    F32  mAlpha = 0.5f;
    F32  mRadius = 28.f;    // outer handoff distance for this tier
};

// A sound pack. Only the medium ambient bed is required: when the light or
// heavy variants are left empty their share of the blend folds back into
// medium, so a one-sound pack still fades correctly with intensity.
struct SSPrecipSounds
{
    std::string mAmbientLight;    // optional
    std::string mAmbientMedium;   // the default bed
    std::string mAmbientHeavy;    // optional
    std::string mRoofOpen;        // roof overhead, open sides
    std::string mRoofSmall;
    std::string mRoofMedium;
    std::string mRoofBig;
};

struct SSPrecipPreset
{
    std::string mName;
    bool mBuiltIn = false;

    SSPrecipArchetype mArchetype = SSPrecipArchetype::LIQUID;

    // Motion
    F32 mFallSpeed = 9.5f;      // terminal velocity, m/s
    F32 mFallLo = 16.f;         // spawn height above the landing point
    F32 mFallHi = 26.f;
    F32 mSway = 0.f;            // lateral wander while falling
    F32 mWindResponse = 1.f;    // how strongly horizontal wind carries it

    // Density
    F32 mRate = 0.16f;          // drops per m^2 per second at full precipitation

    // How much the global precipitation slider changes drop size, as opposed
    // to just count. Rain uses this to run drizzle through to a downpour.
    F32 mIntensitySize = 0.f;

    // Appearance
    LLColor4 mTint = LLColor4::white;
    F32 mGlow = 0.f;
    U8 mDropShape = DROP_DOT;
    bool mEmissive = false;
    bool mWaterShading = false;

    // Size of one drop as baked into the cluster and sheet textures, relative
    // to the individual-drop tier size those stand in for. Only the far tiers
    // draw drops inside their quad, so this leaves the near drops alone.
    F32 mDropScale = 1.f;

    SSPrecipTierParams mTiers[TIER_COUNT];

    // Impact
    F32 mImpactStrength = 0.7f;
    bool mShatter = false;

    // Landing ring: a surface-aligned ripple that spreads from a point out to
    // its end size and fades. Sizes are half-sizes in metres at full impact
    // strength; water spreads wider and lingers longer than a hard surface.
    F32 mRippleSize = 0.35f;    // end half-size on land
    F32 mRippleAlpha = 0.4f;    // opacity at birth, before the surface gate
    F32 mRippleLife = 0.45f;    // seconds to spread on land

    // Splash crown: a small mote thrown along the surface normal, then
    // ballistic. Zero size or opacity leaves the ring on its own.
    F32 mCrownSize = 0.05f;     // half-size, metres
    F32 mCrownAlpha = 0.35f;
    F32 mCrownSpeed = 0.6f;     // launch speed along the normal, m/s
    F32 mCrownLife = 0.3f;      // seconds

    // Riser flavour mix (mana embers and anything like them)
    F32 mDarkMix = 0.f;
    F32 mPuffMix = 0.f;

    // Assets
    std::string mTextures;      // CSV texture UUIDs, or pbr:UUID
    std::string mRippleTexture;
    std::string mDarkTexture;   // riser flavour: small dark sharp flecks
    std::string mPuffTexture;   // riser flavour: large vague clouds
    SSPrecipSounds mSounds;

    LLSD asLLSD() const;
    void fromLLSD(const LLSD& sd);

    bool risesFromGround() const { return mArchetype == SSPrecipArchetype::RISER; }
    bool makesImpacts() const { return mImpactStrength > 0.f && !risesFromGround(); }
    bool makesRipples() const { return mRippleSize > 0.f && mRippleAlpha > 0.f && mRippleLife > 0.f; }
    bool makesCrowns() const { return mCrownSize > 0.f && mCrownAlpha > 0.f && mCrownLife > 0.f; }
    U8 material() const { return mEmissive ? MAT_EMISSIVE : (mWaterShading ? MAT_WATER : MAT_LIT); }

    static const char* archetypeName(SSPrecipArchetype a);
};

class SSPrecipPresetMgr : public LLSingleton<SSPrecipPresetMgr>
{
    LLSINGLETON(SSPrecipPresetMgr);

public:
    // Built-in presets are recreated every run; user presets load from
    // <user settings>/ss_weather/*.xml
    void refresh();

    const std::vector<SSPrecipPreset>& presets() const { return mPresets; }
    const SSPrecipPreset* find(const std::string& name) const;

    // The preset the weather system should currently run, resolved from the
    // SSAtmoPreset setting. Falls back to the first built-in if the named
    // preset has been deleted.
    const SSPrecipPreset& active() const;

    bool save(const SSPrecipPreset& preset);   // writes to disk, refreshes
    bool remove(const std::string& name);

    // Apply an edit to the live weather without writing it to disk. The editor
    // stages every keystroke so the preset can be dialled in while watching it
    // fall, and only commits when asked; the asterisk in the editor title is
    // the difference between the two.
    void stage(const SSPrecipPreset& preset);

    // Whether the in-memory preset differs from the one on disk (or, for a
    // built-in with no user file, from its shipped values)
    bool isModified(const std::string& name) const;

    // The preset as last written, for reverting a staged edit
    const SSPrecipPreset* findSaved(const std::string& name) const;

    static std::string presetDir();

private:
    void buildDefaults();
    void loadUserPresets();

    std::vector<SSPrecipPreset> mPresets;

    // Snapshot of mPresets as of the last refresh, i.e. what is on disk
    std::vector<SSPrecipPreset> mSaved;
};

// </SS:Nexii>

#endif // SS_PRECIPPRESET_H
