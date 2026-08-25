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
    PART_LANDED = 0x04, // settled on a surface, fading out where it stopped
    PART_DRIP   = 0x08, // shed off an eave: holds its alpha for the whole fall
    PART_STREAM = 0x10, // a running sheet off an eave rather than a single drip
    PART_CROWN  = 0x20  // splash crown: opens from a point over its life like
                        // the ring beside it, rather than holding one size
};

// Segments a stream is drawn as. It leaves the lip along the flow and bends into gravity and the local wind, so it cannot be one flat quad; each segment is a straight piece of a curve that a single
// quad could not follow. They carry a colour at each end rather than one apiece, because a stream fades down its length and a step in that at a join makes the ribbon read as a stack of separate
// bands. Eight is enough to read as continuous water over the two or three metres one spans.
static const S32 SS_STREAM_SEGMENTS = 8;

// Metres of water one repeat of a stream's texture covers, when the preset does not say. Ordinarily it does: the sheet variants are baked with the drops in them sized against that tier's own quad,
// so tiling the art over exactly that many metres is what puts those drops at their true size on a stream. Only a preset with no sheet dimensions to read falls back here.
static const F32 SS_STREAM_TEX_METRES = 1.6f;

// The rate the scroll has to run at for the texture to travel with the water. The repeat count belongs to the particle - it comes from the fall and the span, which is the sim's business - and both
// sides have to use the same one: features sit at s = (phase - v) / repeats, so ds/dt is phase' / repeats, and that has to come out at one fall per fall time. When the renderer and the sim each
// guessed at it separately the texture crawled on a short fall and raced on a long one.
inline F32 ssStreamScroll(F32 repeats, F32 fall_time)
{
    return repeats / llmax(fall_time, 0.05f);
}

// Sim-wide texture table size; the renderer's bucket count matches this. One preset alone needs VARIANT_COUNT * TIER_COUNT entries plus the utility shapes, and leftovers from a previous preset
// linger while they fade, so this has to have real headroom above that.
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

    // Streams only: which tier's variants their texture was baked from, which is not the tier they behave as. It decides how the art tiles, so it has to be remembered rather than re-derived - the
    // texture is picked once and the tiling is worked out on every refresh, and if the flow widening a sheet could change the answer the two would come apart.
    U8 mArt = TIER_CLUSTERS;

    // Streams only: how the lip this one hangs off climbs, in metres of rise per metre along it. A stream spans a stretch of eave, and plenty of eaves are not level - the rake up the side of a
    // gable, the sides of a valley, a sloped awning - so a curtain hung horizontally off one cuts straight through the roof it is supposed to be running off. The width axis follows the lip instead,
    // and this is its pitch.
    F32 mRunSlope = 0.f;
};

// Seconds a particle spends fading out at the end of its life. The renderer applies it; the sim needs the same number so a particle that has just met the ground can be given exactly that much life
// left and fade out on the surface instead of popping. Sheets are large enough that a quick fade reads as a flicker, so they take longer over it.
inline F32 ssPrecipFadeOut(U8 tier) { return (tier == TIER_SHEETS) ? 0.8f : 0.25f; }

class SSPrecipSim
{
public:
    SSPrecipSim();

    // Integrate live particles and run the deterministic tier spawners. Called once per frame from the manager.
    void update(F32 dt);

    // Landing effects from the manager's impact queue: a surface-aligned expanding ring (gated toward horizontal surfaces) plus a small splash crown thrown along the surface normal
    void spawnRipple(const LLVector3& pos_agent, F32 strength, bool on_water,
                     const LLVector3& normal, SSRandStream& rng);

    // One drip shed off an eave by the runoff network: a fat drop released at the lip with the flow still pushing it outward, ballistic down to the landing point, and an impact scheduled for when it
    // gets there so the street answers with a ripple and a sound like any other landing. volume is how many raindrops' worth of collected water it carries.
    void spawnDrip(const LLVector3& lip_agent, const LLVector3& out_dir,
                   const LLVector3& land_agent, F32 volume, SSRandStream& rng);

    // A running sheet off an eave, for when a run is shedding faster than separate drips can carry. Past a certain rate water stops leaving an edge as drops at all and comes off as a continuous
    // fall, and drawing that as more and more individual drips both looks wrong and costs a particle each. One of these stands in for a whole stretch of lip. Anchored: it is refreshed in place while
    // the eave keeps running and fades out when it stops, rather than being respawned every frame. width is how much of the eave this one covers, in metres. A stream is a curtain hanging off its
    // stretch of the lip, not a strand: the run hands out its length in slot-wide pieces and they stand side by side, so a gutter in a downpour comes off as one sheet the length of the roof.
    // run_slope is the pitch of the lip over the stretch this one spans, in metres of rise per metre along it, so a curtain off a gable rake hangs along the rake instead of cutting horizontally
    // through the roof. width is measured along that lip, slope included.
    void refreshStream(U32 key, const LLVector3& lip_agent, const LLVector3& out_dir,
                       const LLVector3& land_agent, F32 strength, F32 width,
                       F32 run_slope, SSRandStream& rng);

    // Streams live in their own list rather than the effects ring: they are anchored and long lived, and a ring that recycles its oldest slot would drop the busiest gutter on the roof the moment a
    // shower filled it with splashes.
    const std::vector<SSPrecipParticle>& streams() const { return mStreams; }
    S32 streamCount() const { return (S32)mStreams.size(); }

    // Drips currently in flight. They share the effects pool with ripples and splash crowns, but they must not be budgeted against it: in heavy rain the splashes alone fill that pool, and a budget
    // that counted them would shut the eaves off exactly when it is raining hardest.
    S32 dripCount() const { return mDripCount; }

    // Drops per square metre per second the individual-drop tier is asking for over this spot, gusts and the area field included. This is the rate a roof collects water at, which is what the runoff
    // network turns into catchment.
    static F32 dropRateAt(const LLVector3& pos_agent);

    // Mana hail shatters on landing: a handful of glowing shards thrown off the surface carrying the impact speed, then pulled down by gravity
    void spawnShatter(const LLVector3& pos_agent, const LLVector3& normal,
                      const LLVector3& velocity, F32 strength, SSRandStream& rng);

    // Agent-space origin moved (region crossing)
    void shift(const LLVector3& offset);

    void clear();

    const std::vector<SSPrecipParticle>& particles() const { return mParticles; }
    const std::vector<SSPrecipParticle>& ripples() const { return mRipples; }
    LLViewerTexture* texture(U8 index) const;
    bool empty() const { return mParticles.empty() && mRipples.empty() && mStreams.empty(); }
    S32 tierCount(SSPrecipTier tier) const { return mTierCount[tier]; }

    // Cross-fade bands for a tier at the current type: fully hidden below in_lo / beyond out_hi, fully shown between in_hi and out_lo. The renderer applies these against the live camera distance so
    // tiers hand off smoothly as the camera moves.
    static void tierBands(SSPrecipTier tier, const SSPrecipPreset& preset,
                          F32& in_lo, F32& in_hi, F32& out_lo, F32& out_hi);

    // Sprite geometry for the procedural splatter textures: the tier's quad size, the individual drop size, and how many drops one tier particle stands in for. False when the tier is disabled in the
    // preset.
    static bool tierSprite(const SSPrecipPreset& preset, SSPrecipTier tier,
                           F32& quad_x, F32& quad_y, F32& drop_x, F32& drop_y, S32& splats);

private:
    void spawnTier(SSPrecipTier tier, U64 tick, F64 tick_time);
    void spawnTierCell(SSPrecipTier tier, U64 tick, F64 tick_time, S32 cx, S32 cy, F32 env,
                       const LLVector3& cam_agent, const LLVector3d& agent_origin_global);
    // has_floor is false for a sky band column with nothing under it: the particle falls through the imaginary track floor and fades rather than settling on it
    void emitParticle(SSPrecipTier tier, const LLVector3& hit_pos, F32 fall_len, F32 gust,
                      F32 size_jitter, F32 phase, F32 riser_age, F32 gust_jitter, U32 vis_seed,
                      bool has_floor);
    U8 textureIndex(LLViewerTexture* texturep);
    void resetTextureTable();
    // Riser presets can mix in small dark sharp flecks and large vague clouds. Chosen from the visual stream so it stays synced.
    void applyEmberFlavor(SSPrecipParticle& part, LLColor4& tint, SSRandStream& vis,
                          const SSPrecipPreset& preset);
    void pushRipple(const SSPrecipParticle& part);

    // Put a fresh particle in the air where one just landed. Derived from the dying particle's own seed, so every client holding that particle grows the same replacement.
    void respawnParticle(SSPrecipTier tier, U32 seed, const LLVector3& impact_pos, F32 env);

    std::vector<SSPrecipParticle> mParticles;
    std::vector<SSPrecipParticle> mRipples;
    S32 mDripCount = 0;                 // PART_DRIP entries live in mRipples

    // Anchored eave streams, keyed by mSeed. Refreshed in place by the runoff while their run keeps shedding, and aged out when it stops.
    std::vector<SSPrecipParticle> mStreams;
    void updateStreams(F32 dt);
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
