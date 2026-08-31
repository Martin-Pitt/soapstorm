/**
 * @file ssvolcloud.h
 * @brief Atmo Magic: volumetric cloud puff field.
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

#ifndef SS_VOLCLOUD_H
#define SS_VOLCLOUD_H

#include "llsingleton.h"
#include "llpointer.h"
#include "lluuid.h"
#include "llimage.h"
#include "llrendertarget.h"
#include "llviewertexture.h"
#include "v2math.h"
#include "v3color.h"
#include "v3math.h"
#include "v4math.h"

#include <vector>
#include <unordered_map>

struct SSAtmoEnvCloudFieldState;

static const S32 SS_MAX_STRIKE_LIGHTS = 4;

// <SS:Nexii> The far-field squash cap as a fraction of MAX_FAR_CLIP: where the cloud field's drawn depth tops out, just short of the projection far plane so nothing rasterises against it. The
// knee (0.8 of the cap, set in update()) is where the compression starts; between them the whole remaining field is folded into the last fifth of drawn depth. Shared with lightning so bolt and
// cloud agree about drawn depth. [interaction: SSLightning] </SS:Nexii>
constexpr F32 SS_SQUASH_CAP_FRAC = 0.98f;

class SSVolCloud : public LLSingleton<SSVolCloud>
{
    LLSINGLETON_EMPTY_CTOR(SSVolCloud);

public:
    void update(F32 dt);

    void render();

    void clear();

    // <SS:Nexii> The primary deck's geometry and coverage: the auto dome altitude derivation and
    // every consumer that asks "how much cloud is overhead" mean the main field, not the under
    // deck bolted on below a sky build. </SS:Nexii>
    F32 cloudBaseZ() const { return mPrimary.mBaseZ; }
    F32 cloudTopZ() const { return mPrimary.mBaseZ + mPrimary.mThicknessM; }
    bool empty() const { return mPrimary.mPuffs.empty(); }

    F32 transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength);

    S32 puffCount() const { return (S32)(mPrimary.mPuffs.size() + mUnder.mPuffs.size()); }
    F32 lastBuildMS() const { return mLastBuildMS; }

    // <SS:Nexii> The convection noise map's gate for the weather. Precipitation asks the deck
    // it falls from two questions about a point of the sky: how much cloud is over it (x, a hole
    // in the map reads zero and takes the rain with it) and how tower-like the column is (y,
    // which tweaks the intensity toward the dense parts). The point handed in must already be
    // the WIND-TILTED one - where a drop falling at the weather's angle entered the deck, not
    // where it lands - because only the caller knows the fall; this side supplies everything
    // else, drift included. A deck with no map, or whose map has not read back yet, answers
    // neutral: everything present, nothing tower-like. [interaction: precipitation]
    LLVector2 precipNoiseAt(const LLVector3& pos_agent) const;

    // The weather deck's base height, metres, for the same tilt maths - how far above the
    // ground a drop's column reaches.
    F32 precipBaseZ() const;

    // Whether the weather deck has a built field and a noise map read back - checked before
    // precipitation pays for any of the gating.
    bool precipNoiseReady() const;

    // <SS:Nexii> The generated stand-ins a picker previews for each deck's noise map and
    // profile ramp: the procedural map or the built-in strip while it is what the deck runs,
    // null once an authored texture covers the field (the picker then shows the real asset).
    // These are live previews of the built decks, not of the edited asset.
    LLViewerTexture* noisePreviewTexture(bool under_deck) const;
    LLViewerTexture* profilePreviewTexture(bool under_deck) const;

private:
    struct Puff
    {
        LLVector3 mPosAgent;
        F32 mRadius = 0.f;
        F32 mAlpha = 0.f;
        LLColor3 mColor;
        F32 mCamDistSq = 0.f;

        // <SS:Nexii> This puff's anvil figure - the deck's own anvil raised by the noise map's
        // tower ramp where the column is one of the strong ones, and by the height ramp wherever
        // the puff climbs the deck's upper stretch toward the cirrus band. Rides on the puff so
        // the renderer can shear its top into a skirt without re-deriving anything.
        F32 mAnvil = 0.f;
    };

    // <SS:Nexii> One resolved cloud deck. The primary storm field and the optional under deck are
    // the same renderer run twice - each with its own resolved field state, textures, puff set and
    // uniforms - so a sky-themed build can hang a second layer at the bottom of the build while the
    // weather-driven deck stays overhead. Drawn far deck first; within a deck the puffs stay
    // depth-sorted, and decks separated by hundreds of metres hide the cross-deck ordering.
    // </SS:Nexii>
    struct Deck
    {
        std::vector<Puff> mPuffs;

        LLUUID mTexture;
        LLPointer<LLViewerFetchedTexture> mTextureRef;

        LLUUID mDetail;
        LLPointer<LLViewerFetchedTexture> mDetailRef;

        // <SS:Nexii> The base and detail maps' crossfade partners and the two eased weights,
        // live while the day cycle fades between keyframed textures. Bound on the shader's spare
        // reserved channels (bumpMap, specularMap) and mixed per sample; both weights 0 - and the
        // partners pinned on the current maps - whenever no fade runs.
        LLUUID mTextureNext;
        LLPointer<LLViewerFetchedTexture> mTextureNextRef;
        F32 mTextureBlend = 0.f;

        LLUUID mDetailNext;
        LLPointer<LLViewerFetchedTexture> mDetailNextRef;
        F32 mDetailBlend = 0.f;

        // <SS:Nexii> The convection noise map and its CPU copy. The GPU sees the same map the
        // field was shaped with - bound as the fragment stage's altDiffuseMap (a reserved
        // LLShaderMgr channel; only reserved names can be bound as textures, see the depthMap
        // note in ssVolCloudF.glsl) so the anvil's carving reads the same geography the towers
        // were grown from. mNoiseW zero means "no map, or not read back yet" - every consumer
        // then treats the field as unmodulated, and the cache fills a frame or two later once
        // the fetch lands.
        LLUUID mNoise;
        LLPointer<LLViewerFetchedTexture> mNoiseRef;
        std::vector<F32> mNoiseLuma;
        S32 mNoiseW = 0;
        S32 mNoiseH = 0;
        S32 mNoiseSrcW = 0;     // the raw image's size at cache time, to re-cache on upgrade
        S32 mNoiseSrcH = 0;

        // <SS:Nexii> And when nothing is authored, a square tileable map generated from the
        // weather seed - the same FBM for every client sharing the environment, so the tower
        // geography (and the holes it cuts) is syncable without anyone having to upload a
        // texture. The raw image is the single source: the CPU grid folds down out of it and
        // the GPU texture uploads from it.
        U32 mNoiseProcSeed = 0;
        LLPointer<LLImageRaw> mNoiseProcRaw;
        LLPointer<LLViewerTexture> mNoiseProcRef;

        // <SS:Nexii> The vertical profile ramp, when one is authored: the same fetch/readback
        // ladder as the noise map, but folded into a one-dimensional curve per channel - rows
        // of the readback averaged across, row 0 the deck's BASE (the same v the shader's
        // texture read samples). R tower weight, G carve guard, B cap band, A base fill.
        // mProfileN zero means "none authored, or not read back yet" - every consumer then runs
        // the built-in vertical curves.
        LLUUID mProfile;
        LLPointer<LLViewerFetchedTexture> mProfileRef;
        std::vector<F32> mProfileCurve;
        S32 mProfileN = 0;

        // <SS:Nexii> And when nothing is authored, a small strip painted from those built-in
        // curves - display only (it never reaches the shader; the built-ins ARE the shader's
        // maths), so a picker has something honest to preview for the None state.
        LLPointer<LLViewerTexture> mProfileProcRef;

        F32 mBaseZ = 0.f;
        F32 mThicknessM = 1.f;

        F32 mAnvil = 0.f;
        F32 mTextureMix = 0.f;
        F32 mPuffDensity = 0.8f;
        F32 mDetailScale = 1.f;
        F32 mDriftRate = 1.f;

        F32 mChurn = 0.f;
        F32 mCoverage = 0.f;

        // <SS:Nexii> The noise map's resolved shaping, baked at build time so precipitation
        // reads exactly the field the deck drew with. mNoiseTileM is metres per tile after the
        // Noise Scale slider (zero when there is no map); mNoiseHole is how hard the map's low
        // end cuts holes once moisture has lifted the floor and convection has kept the storm
        // gaps open. mNoiseTowerLo/Hi are the tower ramp's window - widened as the weather
        // consolidates into a storm, so the carving calms into large solid cells instead of
        // shredding the deck - and the shader's own carving reads the same window back.
        F32 mNoiseTileM = 0.f;
        F32 mNoiseHole = 0.f;
        F32 mNoiseTowerLo = 0.42f;
        F32 mNoiseTowerHi = 0.78f;

        // <SS:Nexii> The base veil: one soft sheet inset into the deck's floor, drawn under the
        // puffs so the field reads with its gaps filled instead of as balls over empty sky. Same
        // texture as the puffs, sampled aperiodically in the shader; the colour here is the shade
        // a puff at the deck's floor would wear - the same formulas - so sheet and lowest puffs
        // share one lighting. mSheetZ is the sheet's altitude (the inset), mSheetAlpha its ceiling.
        LLColor3 mSheetColor;
        F32 mSheetZ = 0.f;
        F32 mSheetAlpha = 0.f;

        F32 mMeanDistSq = 0.f;
    };

    void buildDeck(Deck& deck, const SSAtmoEnvCloudFieldState& field, F32 convection, F32 moisture, U32 salt);
    bool fetchDeckTextures(Deck& deck);

    // <SS:Nexii> The noise map's two answers for one point of a deck's field, in the AIR frame
    // (drift already subtracted): presence after the moisture floor and convection's say, and
    // the tower weight the gradient ramp hands back. Shared by the deck builder and the
    // precipitation gate so both always agree about where the holes are.
    void noiseFieldAt(const Deck& deck, F32 air_x, F32 air_y, F32& presence, F32& tower) const;

    // Wrapped bilinear sample of the cached grid, or -1 when the deck has no map cached yet.
    F32 noiseSample(const Deck& deck, F32 air_x, F32 air_y) const;

    // Folds the noise map's raw readback into the deck's small wrapped sample grid.
    void cacheNoiseGrid(Deck& deck, LLImageRaw* raw);

    // Generates (once per seed) the deck's square procedural noise map when nothing is
    // authored, and folds it into the same grid an authored map would fill.
    void ensureProceduralNoise(Deck& deck, U32 salt);

    // The vertical profile ramp: fetched and read back like the noise map, folded into one
    // averaged curve per channel (row 0 = the deck's base), and read on the CPU by the puff
    // placement so geometry and fragment carving run the same authored profile.
    void cacheProfileCurve(Deck& deck, LLImageRaw* raw);
    F32 profileSample(const Deck& deck, F32 v, S32 channel) const;

    // Which deck the weather reads: the authored source when it names the under deck and that
    // deck is on, the main field otherwise - the same default every "how much cloud is overhead"
    // consumer uses.
    const Deck* weatherDeck() const;

    Deck mPrimary;
    Deck mUnder;

    S32 mWeatherDeck = 0;

    F32 mLastCoverage = 0.f;
    F32 mLastBuildMS = 0.f;

    LLColor3 mAmbient;

    LLVector3 mLightDir;
    LLColor3 mSunColor;

    F32 mBeam = 1.f;

    F32 mEffRadius = 5000.f;
    F32 mSquashKnee = 1600.f;
    F32 mSquashCap = 2000.f;

public:
    F32 squashScale(F32 true_dist) const;
    F32 lastCoverage() const { return mLastCoverage; }
    F32 squashKnee() const { return mSquashKnee; }
    F32 squashCap() const { return mSquashCap; }
    F32 virtualRadius() const { return mEffRadius; }

private:

    LLRenderTarget mDepthCopy;

    std::vector<LLVector4> mStrikeLights;

    std::unordered_map<U64, std::vector<S32>> mOccGrid;
    std::vector<U32> mOccStamp;
    U32 mOccQuery = 0;
    F32 mMaxPuffR = 0.f;
    bool mOccGridDirty = true;
};

#endif
