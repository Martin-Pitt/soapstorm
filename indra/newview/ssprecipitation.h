/**
 * @file ssprecipitation.h
 * @brief Atmo Magic precipitation simulation: a self-contained particle
 *        system with three LOD tiers. Individual drops fall around the
 *        camera, clustered drops carry the mid range, and large shower
 *        sheets fill the distance; all tiers run from the same deterministic
 *        weather field and cross-fade as the camera moves, so the far
 *        abstraction always matches what is actually falling there.
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

#ifndef SS_PRECIPITATION_H
#define SS_PRECIPITATION_H

// <SS:Nexii> Atmo Magic precipitation simulation

#include "ssatmomagic.h"

#include "llpointer.h"
#include "v3math.h"
#include "v4coloru.h"

#include <cfloat>
#include <vector>

class LLViewerTexture;

enum SSPrecipFlags : U8
{
    PART_SWAY   = 0x01, // lateral wander while falling (snow, dust, embers)
    PART_GUSTY  = 0x02, // stronger wander (blizzard)
    PART_LANDED = 0x04  // settled on a surface, fading out where it stopped
};

// Sim-wide texture table size; the renderer's bucket count matches this.
// One preset alone needs VARIANT_COUNT * TIER_COUNT entries plus the utility
// shapes, and leftovers from a previous preset linger while they fade, so
// this has to have real headroom above that.
static const U32 SS_PRECIP_MAX_TEXTURES = 64;

struct SSPrecipParticle
{
    LLVector3 mPos;
    LLVector3 mVel;
    LLVector3 mNormal = LLVector3(0.f, 0.f, 1.f); // surface basis for KIND_FLAT, impact plane for shards
    F32 mPlaneD = -FLT_MAX;  // shards die when they fall back through this plane
    F32 mFloorZ = -FLT_MAX;  // surface last resolved under a drifting particle
    F32 mAge = 0.f;
    F32 mMaxAge = 1.f;
    F32 mSizeX = 0.05f;     // half-width; for KIND_FLAT the start half-size
    F32 mSizeY = 0.05f;     // half-height; for KIND_FLAT the end half-size
    F32 mAlpha = 1.f;
    F32 mGlow = 0.f;
    F32 mPhase = 0.f;
    LLColor4U mTint = LLColor4U(255, 255, 255, 255);
    U32 mSeed = 0;          // this particle's visual stream, and the seed its
                            // replacement is derived from when it lands
    U8 mKind = KIND_ROUND;
    U8 mTex = 0;
    U8 mTier = TIER_DROPS;
    U8 mFlags = 0;
    U8 mMaterial = MAT_LIT;
};

// Seconds a particle spends fading out at the end of its life. The renderer
// applies it; the sim needs the same number so a particle that has just met
// the ground can be given exactly that much life left and fade out on the
// surface instead of popping. Sheets are large enough that a quick fade
// reads as a flicker, so they take longer over it.
inline F32 ssPrecipFadeOut(U8 tier) { return (tier == TIER_SHEETS) ? 0.8f : 0.25f; }

class SSPrecipSim
{
public:
    SSPrecipSim();

    // Integrate live particles and run the deterministic tier spawners.
    // Called once per frame from the manager.
    void update(F32 dt);

    // Landing effects from the manager's impact queue: a surface-aligned
    // expanding ring (gated toward horizontal surfaces) plus a small splash
    // crown thrown along the surface normal
    void spawnRipple(const LLVector3& pos_agent, F32 strength, bool on_water,
                     const LLVector3& normal, SSRandStream& rng);

    // One drip shed off an eave by the runoff network: a fat drop released at
    // the lip with the flow still pushing it outward, ballistic down to the
    // landing point, and an impact scheduled for when it gets there so the
    // street answers with a ripple and a sound like any other landing.
    // volume is how many raindrops' worth of collected water it carries.
    void spawnDrip(const LLVector3& lip_agent, const LLVector3& out_dir,
                   const LLVector3& land_agent, F32 volume, SSRandStream& rng);

    // Drops per square metre per second the individual-drop tier is asking
    // for over this spot, gusts and the area field included. This is the rate
    // a roof collects water at, which is what the runoff network turns into
    // catchment.
    static F32 dropRateAt(const LLVector3& pos_agent);

    // Mana hail shatters on landing: a handful of glowing shards thrown off
    // the surface carrying the impact speed, then pulled down by gravity
    void spawnShatter(const LLVector3& pos_agent, const LLVector3& normal,
                      const LLVector3& velocity, F32 strength, SSRandStream& rng);

    // Agent-space origin moved (region crossing)
    void shift(const LLVector3& offset);

    void clear();

    const std::vector<SSPrecipParticle>& particles() const { return mParticles; }
    const std::vector<SSPrecipParticle>& ripples() const { return mRipples; }
    LLViewerTexture* texture(U8 index) const;
    bool empty() const { return mParticles.empty() && mRipples.empty(); }
    S32 tierCount(SSPrecipTier tier) const { return mTierCount[tier]; }

    // Cross-fade bands for a tier at the current type: fully hidden below
    // in_lo / beyond out_hi, fully shown between in_hi and out_lo. The
    // renderer applies these against the live camera distance so tiers hand
    // off smoothly as the camera moves.
    static void tierBands(SSPrecipTier tier, const SSPrecipPreset& preset,
                          F32& in_lo, F32& in_hi, F32& out_lo, F32& out_hi);

    // Sprite geometry for the procedural splatter textures: the tier's quad
    // size, the individual drop size, and how many drops one tier particle
    // stands in for. False when the tier is disabled in the preset.
    static bool tierSprite(const SSPrecipPreset& preset, SSPrecipTier tier,
                           F32& quad_x, F32& quad_y, F32& drop_x, F32& drop_y, S32& splats);

private:
    void spawnTier(SSPrecipTier tier, U64 tick, F64 tick_time);
    void spawnTierCell(SSPrecipTier tier, U64 tick, F64 tick_time, S32 cx, S32 cy, F32 env,
                       const LLVector3& cam_agent, const LLVector3d& agent_origin_global);
    // has_floor is false for a sky band column with nothing under it: the
    // particle falls through the imaginary track floor and fades rather than
    // settling on it
    void emitParticle(SSPrecipTier tier, const LLVector3& hit_pos, F32 fall_len, F32 gust,
                      F32 size_jitter, F32 phase, F32 riser_age, F32 gust_jitter, U32 vis_seed,
                      bool has_floor);
    U8 textureIndex(LLViewerTexture* texturep);
    void resetTextureTable();
    // Riser presets can mix in small dark sharp flecks and large vague
    // clouds. Chosen from the visual stream so it stays synced.
    void applyEmberFlavor(SSPrecipParticle& part, LLColor4& tint, SSRandStream& vis,
                          const SSPrecipPreset& preset);
    void pushRipple(const SSPrecipParticle& part);

    // Put a fresh particle in the air where one just landed. Derived from the
    // dying particle's own seed, so every client holding that particle grows
    // the same replacement.
    void respawnParticle(SSPrecipTier tier, U32 seed, const LLVector3& impact_pos, F32 env);

    std::vector<SSPrecipParticle> mParticles;
    std::vector<SSPrecipParticle> mRipples;
    std::vector<LLPointer<LLViewerTexture>> mTextures;
    S32 mTierCount[TIER_COUNT];
    F32 mTierTarget[TIER_COUNT] = { 0.f };      // population the current rate sustains
    F32 mTierSpawnAccum[TIER_COUNT] = { 0.f };  // per-tick scratch behind it
    F32 mMeanLife[TIER_COUNT] = { 0.f };        // measured air time, feeding the target
    U64 mLastTick[TIER_COUNT];
    size_t mRippleCursor = 0;
};

// </SS:Nexii>

#endif // SS_PRECIPITATION_H
