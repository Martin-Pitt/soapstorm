/**
 * @file ssatmomagic.h
 * @brief Atmo Magic: client-side synced weather. Deterministic precipitation
 *        (rain/snow/hail/fantasy) driven by shared parameters, a static seed
 *        and wall-clock time, so every client running the same settings sees
 *        the same weather at the same moment.
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

#ifndef SS_ATMOMAGIC_H
#define SS_ATMOMAGIC_H

// <SS:Nexii> Atmo Magic weather system

#include "ssprecippreset.h"

#include "llpointer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"
#include "v4color.h"

#include <map>
#include <memory>
#include <vector>

class LLViewerTexture;
class SSPrecipSim;

// Deterministic PRNG / noise. LLPerlinNoise seeds its tables from rand() so
// two clients disagree; everything here is a pure function of (seed, input)
// and reproduces bit-exact across clients and sessions.
namespace SSAtmoNoise
{
    inline U32 hashU32(U32 x)
    {
        x = x * 747796405u + 2891336453u;
        U32 w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
        return (w >> 22u) ^ w;
    }

    inline U32 combine(U32 a, U32 b) { return hashU32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2))); }

    inline F32 hash01(U32 x) { return (F32)(hashU32(x) & 0x00ffffffu) / (F32)0x01000000; }

    // Value noise with quintic smoothing, output roughly [-1, 1]
    F32 value1(F32 x, U32 seed);
    F32 value2(F32 x, F32 y, U32 seed);
    F32 fbm1(F32 x, U32 seed, S32 octaves = 3);
    F32 fbm2(F32 x, F32 y, U32 seed, S32 octaves = 3);
}

// Small deterministic stream generator (PCG-style); one instance per
// (seed, tick, cell) so overlapping viewers draw identical sequences.
class SSRandStream
{
public:
    explicit SSRandStream(U32 seed) : mState(seed ? seed : 0x2545f491u) {}

    U32 next()
    {
        mState = mState * 747796405u + 2891336453u;
        U32 w = ((mState >> ((mState >> 28u) + 4u)) ^ mState) * 277803737u;
        return (w >> 22u) ^ w;
    }
    F32 frand() { return (F32)(next() & 0x00ffffffu) / (F32)0x01000000; }
    F32 frand(F32 lo, F32 hi) { return lo + frand() * (hi - lo); }
    S32 rand(S32 n) { return n > 0 ? (S32)(next() % (U32)n) : 0; }

private:
    U32 mState;
};

// One entry of a per-type asset list: either a plain (blinn-phong) particle
// texture or a PBR material ("pbr:<uuid>" in the setting). PBR materials are
// approximated through the legacy particle renderer using their base color
// texture, factor tint and emissive strength.
struct SSAtmoAsset
{
    LLUUID mID;
    bool   mIsPBR = false;
};

class SSAtmoMagic : public LLSingleton<SSAtmoMagic>
{
    LLSINGLETON(SSAtmoMagic);
    // Defined in the .cpp where SSPrecipSim is complete, so the unique_ptr
    // member can destruct against the full type
    ~SSAtmoMagic();

public:
    // Per-frame driver, called from the display loop. Owns the particle
    // source lifetime, the impact (ripple/sound) queue and asset refresh.
    void idle();

    bool isEnabled() const { return mEnabled; }

    // Shared wall clock all deterministic streams are keyed on
    F64 sharedTime() const { return mNow; }
    U32 seed() const;

    // Parameters, cached once per frame from gSavedSettings
    F32 precipitation() const { return mPrecipitation; }
    F32 turbulence() const { return mTurbulence; }
    F32 windSpeed() const { return mWindSpeed; }
    LLVector3 windXY() const { return mWindXY; } // horizontal, m/s

    // Full wind vector including any vertical component. Track configs store
    // direction as a rotation off north, so wind can tilt up or down; the
    // horizontal projection above is what precipitation drift and the
    // deterministic area field use.
    LLVector3 wind() const { return mWind; }

    // EEP sky track the camera is in (1..4). Weather is configured per track
    // and only ever runs for this one, so a rainy ground level can sit under
    // a clear skybox band.
    S32 track() const { return mTrack; }

    // Ground zero for the active track: terrain/water at ground level, the
    // track's base altitude (or a configured platform height) up in the sky.
    F32 groundZero() const { return mGroundZero; }
    bool isSkyTrack() const { return mSkyTrack; }

    // Fraction of drops that survive reaching a sky track's floor with no
    // platform under them; the rest fade out on the way down rather than
    // piling up on an invisible plane.
    F32 fallThrough() const { return mFallThrough; }

    // Track-change crossfade, 0..1. Precipitation is scaled by this so
    // crossing a band boundary eases instead of snapping.
    F32 trackBlend() const { return mBlend; }

    // The preset currently driving the weather, and whether it is actually
    // precipitating. All per-type constants live in the preset now.
    const SSPrecipPreset& preset() const { return mPreset; }
    bool hasWeather() const { return mHasWeather; }

    // Water surface height used for columns over the void beyond region
    // borders, so rain keeps falling there instead of cutting off at the
    // sim edge; the agent region's water height, or SL's 20m default
    static F32 voidWaterHeight();

    // Mean fall direction for the active type (normalized, includes wind
    // tilt but not gusts); this is what the rain shadow map is aligned to.
    LLVector3 rainDirection() const { return mRainDirection; }

    // Deterministic wave/burst envelope at a shared timestamp
    F32 gustEnvelopeAt(F64 time) const;
    // Slow-drifting noise field that raises/lowers precipitation per area,
    // sampled with global (grid) coordinates
    F32 areaFactorAt(F64 global_x, F64 global_y) const;

    // Asset selection; tint/glow let PBR entries carry their factors into
    // the legacy particle path. Returns null when nothing is configured
    // (caller falls back to the default particle texture).
    LLViewerTexture* pickParticleTexture(SSRandStream& rng, LLColor4& tint, F32& glow);
    LLViewerTexture* rippleTexture();
    // First valid UUID in a comma separated list, fetched; null when the list
    // holds nothing usable, so callers can fall back to a generated shape
    static LLViewerTexture* textureFromList(const std::string& csv);

    // Landing events scheduled at spawn time; processed here once due so
    // ripples/sounds fire even when the drop itself was throttled away.
    // The surface normal orients the ripple and gates it toward horizontal
    // surfaces.
    // velocity is the drop's motion at the moment it lands; shattering types
    // hand it to the shards so they fly off carrying the impact speed.
    void queueImpact(F64 time, const LLVector3& pos_agent, F32 strength, bool on_water,
                     const LLVector3& normal, const LLVector3& velocity, bool shatter);

    // The live particle simulation (null while disabled); the renderer
    // draws straight from it
    SSPrecipSim* sim() { return mSim.get(); }

    // Agent-space origin moved (region crossing); called from
    // LLWorld::shiftRegions alongside the stock particle sim
    void shift(const LLVector3& offset);

    // Diagnostic overlay: live weather, particle and audio state. Drawn
    // from LLViewerWindow::draw when SSAtmoShowInfo is set.
    static void drawInfo();
    size_t pendingImpacts() const { return mImpacts.size(); }

private:
    void refreshParams();
    void refreshAssets();
    void ensureSim();
    void processImpacts();
    LLViewerTexture* textureFor(const SSAtmoAsset& asset, LLColor4& tint, F32& glow);

    F64 mNow = 0.0;
    bool mEnabled = false;

    F32 mPrecipitation = 0.f;
    F32 mTurbulence = 0.f;
    F32 mWindSpeed = 0.f;
    LLVector3 mWindXY;
    LLVector3 mWind;
    LLVector3 mRainDirection;
    SSPrecipPreset mPreset;
    bool mHasWeather = false;

    // Per-track state
    S32 mTrack = 1;
    F32 mGroundZero = 0.f;
    bool mSkyTrack = false;
    F32 mFallThrough = 1.f;

    // Crossfade: the preset cannot be blended, so precipitation eases to zero
    // before a swap and back up afterwards. mPresetName is what mPreset was
    // resolved from, so a track or notecard change is detectable.
    F32 mBlend = 0.f;
    std::string mPresetName;

    std::unique_ptr<SSPrecipSim> mSim;

    struct Impact
    {
        LLVector3 mPosAgent;
        LLVector3 mNormal;
        LLVector3 mVelocity;
        F32 mStrength;
        bool mOnWater;
        bool mShatter;
    };
    std::multimap<F64, Impact> mImpacts;

    // Parsed asset lists, re-read when the setting strings change
    std::vector<SSAtmoAsset> mTextureAssets;
    LLPointer<LLViewerTexture> mRippleTexture;
    std::string mAssetsFingerprint;
    F64 mLastAssetPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_ATMOMAGIC_H
