/**
 * @file ssatmomagic.cpp
 * @brief Atmo Magic: synced weather manager, deterministic noise, impact queue.
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

#include "ssatmomagic.h"
#include "ssprecipitation.h"
#include "ssweathersounds.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llaudioengine.h"
#include "lldate.h"
#include "llfetchedgltfmaterial.h"
#include "llgltfmateriallist.h"
#include "llfasttimer.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "pipeline.h"

// <SS:Nexii> Atmo Magic weather system

static const F32 IMPACT_HEAR_RADIUS = 20.f;  // sounds only from impacts this close
static const F32 IMPACT_SEE_RADIUS  = 32.f;  // ripples only this close
static const F64 ASSET_POLL_PERIOD  = 2.0;   // re-check asset list settings this often

// Fixed in code: every client has to walk the same streams, so this is not a
// user setting.
static const U32 SS_ATMO_SEED = 0x5EED1337u;

static LLTrace::BlockTimerStatHandle FTM_SS_ATMO("Atmo Magic");
static LLTrace::BlockTimerStatHandle FTM_SS_ATMO_IMPACTS("Impacts");

namespace SSAtmoNoise
{

static F32 latticeGrad(U32 seed, S32 ix)
{
    return hash01(combine(seed, (U32)ix)) * 2.f - 1.f;
}

static F32 latticeGrad2(U32 seed, S32 ix, S32 iy)
{
    return hash01(combine(seed, combine((U32)ix, (U32)iy * 0x27d4eb2fu))) * 2.f - 1.f;
}

static inline F32 quintic(F32 t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

F32 value1(F32 x, U32 seed)
{
    F32 fx = floorf(x);
    S32 ix = (S32)fx;
    F32 t = quintic(x - fx);
    return lerp(latticeGrad(seed, ix), latticeGrad(seed, ix + 1), t);
}

F32 value2(F32 x, F32 y, U32 seed)
{
    F32 fx = floorf(x);
    F32 fy = floorf(y);
    S32 ix = (S32)fx;
    S32 iy = (S32)fy;
    F32 tx = quintic(x - fx);
    F32 ty = quintic(y - fy);
    F32 a = lerp(latticeGrad2(seed, ix, iy),     latticeGrad2(seed, ix + 1, iy),     tx);
    F32 b = lerp(latticeGrad2(seed, ix, iy + 1), latticeGrad2(seed, ix + 1, iy + 1), tx);
    return lerp(a, b, ty);
}

F32 fbm1(F32 x, U32 seed, S32 octaves)
{
    F32 sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
    for (S32 i = 0; i < octaves; ++i)
    {
        sum += amp * value1(x * freq, combine(seed, (U32)i));
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return sum / norm;
}

F32 fbm2(F32 x, F32 y, U32 seed, S32 octaves)
{
    F32 sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
    for (S32 i = 0; i < octaves; ++i)
    {
        sum += amp * value2(x * freq, y * freq, combine(seed, (U32)i));
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return sum / norm;
}

} // namespace SSAtmoNoise

SSAtmoMagic::SSAtmoMagic()
{
}

SSAtmoMagic::~SSAtmoMagic()
{
}

U32 SSAtmoMagic::seed() const
{
    return SS_ATMO_SEED;
}

// static
F32 SSAtmoMagic::voidWaterHeight()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    return regionp ? regionp->getWaterHeight() : 20.f;
}

void SSAtmoMagic::refreshParams()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);
    static LLCachedControl<F32>  precipitation(gSavedSettings, "SSAtmoPrecipitation", 0.5f);
    static LLCachedControl<F32>  turbulence(gSavedSettings, "SSAtmoTurbulence", 0.3f);
    static LLCachedControl<F32>  wind_direction(gSavedSettings, "SSAtmoWindDirection", 225.f);
    static LLCachedControl<F32>  wind_speed(gSavedSettings, "SSAtmoWindSpeed", 4.f);

    mEnabled = enabled;
    mPrecipitation = llclamp((F32)precipitation, 0.f, 1.f);
    mTurbulence = llclamp((F32)turbulence, 0.f, 1.f);
    mWindSpeed = llmax(0.f, (F32)wind_speed);

    const F32 rad = (F32)wind_direction * DEG_TO_RAD;
    mWindXY.set(sinf(rad) * mWindSpeed, cosf(rad) * mWindSpeed, 0.f);

    // Everything type specific now comes from the active preset
    mPreset = SSPrecipPresetMgr::instance().active();
    mHasWeather = mEnabled && mPrecipitation > 0.02f;

    // Mean fall direction: wind tilt over fall speed; slow types drift hard.
    // Rising types keep a near-vertical axis so the shadow map stays usable
    // as a ground height map for them.
    const F32 fall = mPreset.mFallSpeed;
    if (!mHasWeather || fall <= 0.f)
    {
        mRainDirection.set(0.f, 0.f, -1.f);
    }
    else if (mPreset.risesFromGround())
    {
        mRainDirection.set(mWindXY.mV[0] * 0.02f, mWindXY.mV[1] * 0.02f, -1.f);
        mRainDirection.normVec();
    }
    else
    {
        const F32 tilt = llclamp(mWindSpeed * mPreset.mWindResponse / fall, 0.f, 2.f);
        LLVector3 dir_xy = mWindXY;
        dir_xy.normVec();
        mRainDirection = dir_xy * tilt;
        mRainDirection.mV[VZ] = -1.f;
        mRainDirection.normVec();
    }
}

F32 SSAtmoMagic::gustEnvelopeAt(F64 time) const
{
    if (mTurbulence <= 0.f) return 1.f;

    // Slow waves plus a sharpened burst channel; both pure functions of the
    // shared clock so gust timing lines up across clients. fbm output
    // hovers well inside [-1,1], so both channels are stretched hard —
    // otherwise the envelope barely moves and gusts are imperceptible.
    const F32 t = (F32)fmod(time, 4096.0);
    F32 wave = SSAtmoNoise::fbm1(t * (0.05f + 0.15f * mTurbulence), SS_ATMO_SEED ^ 0xA17C0FEEu);
    wave = llclamp(wave * 2.2f + 0.5f, 0.f, 1.f);
    wave = wave * wave * (3.f - 2.f * wave);

    F32 burst = llclamp(SSAtmoNoise::fbm1(t * 0.7f, SS_ATMO_SEED ^ 0x00B57A9Du, 2) * 2.5f, 0.f, 1.f);
    burst = burst * burst * burst * 2.f;

    // At full turbulence rain arrives in real waves: near-lulls between
    // gusts, more than double strength inside them
    return lerp(1.f, 0.15f + 1.7f * wave + burst, mTurbulence);
}

F32 SSAtmoMagic::areaFactorAt(F64 global_x, F64 global_y) const
{
    // Pattern drifts downwind; fmod keeps precision at large grid coordinates
    const F64 drift = mNow * 0.35;
    const F32 x = (F32)fmod(global_x - mWindXY.mV[0] * drift, 8192.0) * 0.02f;
    const F32 y = (F32)fmod(global_y - mWindXY.mV[1] * drift, 8192.0) * 0.02f;
    const F32 n = 0.5f + 0.5f * SSAtmoNoise::fbm2(x, y, SS_ATMO_SEED ^ 0x5EED0A2Bu);
    return 0.35f + 1.3f * n;
}

static void parseAssetList(const std::string& value, std::vector<SSAtmoAsset>& out)
{
    out.clear();
    std::string::size_type pos = 0;
    while (pos < value.size())
    {
        std::string::size_type end = value.find(',', pos);
        if (end == std::string::npos) end = value.size();
        std::string token = value.substr(pos, end - pos);
        pos = end + 1;

        LLStringUtil::trim(token);
        if (token.empty()) continue;

        SSAtmoAsset asset;
        if (token.compare(0, 4, "pbr:") == 0)
        {
            asset.mIsPBR = true;
            token = token.substr(4);
            LLStringUtil::trim(token);
        }
        if (LLUUID::validate(token))
        {
            asset.mID.set(token);
            out.push_back(asset);
        }
    }
}

void SSAtmoMagic::refreshAssets()
{
    // Keyed on the preset name plus its asset strings, so switching or
    // editing a preset re-parses and anything else is a cheap no-op
    const std::string fingerprint = mPreset.mName + "|" + mPreset.mTextures + "|"
                                  + mPreset.mRippleTexture + "|" + mPreset.mSounds.mImpacts;
    if (fingerprint == mAssetsFingerprint) return;
    mAssetsFingerprint = fingerprint;

    parseAssetList(mPreset.mTextures, mTextureAssets);

    std::vector<SSAtmoAsset> ripple;
    parseAssetList(mPreset.mRippleTexture, ripple);
    mRippleTexture = nullptr;
    if (!ripple.empty() && !ripple[0].mIsPBR)
    {
        mRippleTexture = LLViewerTextureManager::getFetchedTexture(ripple[0].mID);
    }

    std::vector<SSAtmoAsset> sounds;
    parseAssetList(mPreset.mSounds.mImpacts, sounds);
    mImpactSounds.clear();
    for (const SSAtmoAsset& snd : sounds)
    {
        mImpactSounds.push_back(snd.mID);
        if (gAudiop)
        {
            gAudiop->preloadSound(snd.mID);
        }
    }
}

LLViewerTexture* SSAtmoMagic::textureFor(const SSAtmoAsset& asset, LLColor4& tint, F32& glow)
{
    if (asset.mIsPBR)
    {
        // Approximated through the legacy particle renderer: base color
        // texture, factor as tint, emissive strength as glow. A dedicated
        // PBR particle draw pool can take over here later.
        LLFetchedGLTFMaterial* mat = gGLTFMaterialList.getMaterial(asset.mID);
        if (mat)
        {
            tint = mat->mBaseColor;
            glow = llclamp((mat->mEmissiveColor.mV[0] + mat->mEmissiveColor.mV[1] + mat->mEmissiveColor.mV[2]) / 3.f, 0.f, 1.f);
            if (mat->mBaseColorTexture.notNull()) return mat->mBaseColorTexture;
            if (mat->mEmissiveTexture.notNull()) return mat->mEmissiveTexture;
        }
        return nullptr;
    }
    return LLViewerTextureManager::getFetchedTexture(asset.mID);
}

LLViewerTexture* SSAtmoMagic::pickParticleTexture(SSRandStream& rng, LLColor4& tint, F32& glow)
{
    tint = LLColor4::white;
    glow = 0.f;
    if (mTextureAssets.empty()) return nullptr;
    return textureFor(mTextureAssets[rng.rand((S32)mTextureAssets.size())], tint, glow);
}

// static
LLViewerTexture* SSAtmoMagic::textureFromList(const std::string& csv)
{
    if (csv.empty()) return nullptr;

    std::vector<SSAtmoAsset> assets;
    parseAssetList(csv, assets);
    if (assets.empty() || assets[0].mIsPBR) return nullptr;
    return LLViewerTextureManager::getFetchedTexture(assets[0].mID);
}

LLViewerTexture* SSAtmoMagic::rippleTexture()
{
    return mRippleTexture;
}

LLUUID SSAtmoMagic::pickImpactSound(SSRandStream& rng)
{
    if (mImpactSounds.empty()) return LLUUID::null;
    return mImpactSounds[rng.rand((S32)mImpactSounds.size())];
}

void SSAtmoMagic::ensureSim()
{
    if (mEnabled)
    {
        if (!mSim)
        {
            mSim = std::make_unique<SSPrecipSim>();
        }
    }
    else if (mSim)
    {
        mSim.reset();
        mImpacts.clear();
    }
}

void SSAtmoMagic::shift(const LLVector3& offset)
{
    if (mSim)
    {
        mSim->shift(offset);
    }
    for (auto& impact : mImpacts)
    {
        impact.second.mPosAgent += offset;
    }
}

void SSAtmoMagic::queueImpact(F64 time, const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, const LLVector3& velocity, bool shatter)
{
    // Bounded so a hitch or bad parameters can't grow this without limit
    if (mImpacts.size() > 2048) return;
    mImpacts.emplace(time, Impact{ pos_agent, normal, velocity, strength, on_water, shatter });
}

void SSAtmoMagic::processImpacts()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO_IMPACTS);

    static LLCachedControl<bool> ripples(gSavedSettings, "SSAtmoRipples", true);
    static LLCachedControl<bool> sounds(gSavedSettings, "SSAtmoSounds", true);
    static LLCachedControl<F32>  sound_gain(gSavedSettings, "SSAtmoSoundGain", 0.6f);
    static LLCachedControl<F32>  master_vol(gSavedSettings, "SSAtmoVolumeMaster", 0.8f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    while (!mImpacts.empty() && mImpacts.begin()->first <= mNow)
    {
        const Impact impact = mImpacts.begin()->second;
        mImpacts.erase(mImpacts.begin());

        const F32 dist = (impact.mPosAgent - cam).magVec();
        if (dist > IMPACT_SEE_RADIUS) continue;

        // Feeds the ambient loop loudness: sheltered spots see fewer
        // landings and sound quieter at the same weather parameters
        SSWeatherSounds::getInstance()->notifyImpact(
            impact.mStrength * (1.f - dist / IMPACT_SEE_RADIUS));

        // Deterministic per-impact stream so sound choice/probability lines
        // up across clients hearing the same landing
        SSRandStream rng(SSAtmoNoise::combine(SS_ATMO_SEED,
            SSAtmoNoise::combine((U32)(S32)(impact.mPosAgent.mV[VX] * 16.f),
                                 (U32)(S32)(impact.mPosAgent.mV[VY] * 16.f))));
        rng.next();

        if (ripples && mSim)
        {
            mSim->spawnRipple(impact.mPosAgent, impact.mStrength, impact.mOnWater, impact.mNormal, rng);
            if (impact.mShatter)
            {
                mSim->spawnShatter(impact.mPosAgent, impact.mNormal, impact.mVelocity,
                                   impact.mStrength, rng);
            }
        }

        if (sounds && gAudiop && dist < IMPACT_HEAR_RADIUS && !mImpactSounds.empty())
        {
            const F32 closeness = 1.f - dist / IMPACT_HEAR_RADIUS;
            if (rng.frand() < 0.12f * impact.mStrength * closeness)
            {
                const LLUUID sound_id = pickImpactSound(rng);
                // Landings beyond a wall are heard through it, not around it
                const F32 occlusion = SSWeatherSounds::getInstance()->occlusionGain(impact.mPosAgent);
                const F32 gain = llclamp((F32)sound_gain * (F32)master_vol * impact.mStrength
                                         * closeness * occlusion, 0.f, 1.f);
                gAudiop->triggerSound(sound_id, gAgent.getID(), gain,
                                      LLAudioEngine::AUDIO_TYPE_AMBIENT,
                                      gAgent.getPosGlobalFromAgent(impact.mPosAgent));

                // A sharp sound outdoors bounces off whatever wall or cliff
                // is nearest; only a fraction get one so the field of drops
                // does not turn into a wash of echoes
                if (occlusion > 0.9f && rng.frand() < 0.25f)
                {
                    SSWeatherSounds::getInstance()->scheduleSlapback(sound_id, impact.mPosAgent, gain);
                }
            }
        }
    }
}

void SSAtmoMagic::idle()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO);

    mNow = LLDate::now().secondsSinceEpoch();
    refreshParams();

    if (mEnabled && mNow - mLastAssetPoll > ASSET_POLL_PERIOD)
    {
        mLastAssetPoll = mNow;
        refreshAssets();
    }

    ensureSim();

    if (mSim)
    {
        mSim->update(gFrameIntervalSeconds);
    }

    if (mEnabled)
    {
        processImpacts();
    }

    // Ambient rain/wind loops and the indoor probe cycle; fades everything
    // out itself when the system is off
    SSWeatherSounds::getInstance()->idle();
}


// static
void SSAtmoMagic::drawInfo()
{
    static LLCachedControl<bool> show_info(gSavedSettings, "SSAtmoShowInfo", false);
    if (!show_info) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSWeatherSounds* audio = SSWeatherSounds::getInstance();
    SSPrecipSim* sim = atmo->sim();

    const SSPrecipPreset& preset = atmo->preset();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3d cam_global = gAgent.getPosGlobalFromAgent(cam);
    const LLVector3 dir = atmo->rainDirection();

    static LLCachedControl<bool> use_rain_shader(gSavedSettings, "SSAtmoRainShader", true);

    std::vector<std::string> lines;
    lines.push_back(llformat("ATMO MAGIC  %s", atmo->isEnabled() ? "[enabled]" : "[disabled]"));
    lines.push_back(llformat("preset     %s (%s)", preset.mName.c_str(),
                             SSPrecipPreset::archetypeName(preset.mArchetype)));
    lines.push_back(llformat("precip     %.2f    turbulence %.2f",
                             atmo->precipitation(), atmo->turbulence()));
    lines.push_back(llformat("wind       %.1f m/s   fall %.1f m/s   dir %.2f %.2f %.2f",
                             atmo->windSpeed(), preset.mFallSpeed,
                             dir.mV[VX], dir.mV[VY], dir.mV[VZ]));
    lines.push_back(llformat("gust x%.2f   area x%.2f   %s",
                             atmo->gustEnvelopeAt(atmo->sharedTime()),
                             atmo->areaFactorAt(cam_global.mdV[VX], cam_global.mdV[VY]),
                             atmo->hasWeather() ? "active" : "idle"));

    if (sim)
    {
        lines.push_back(llformat("particles  drops %d  clusters %d  sheets %d  ripples %d",
                                 sim->tierCount(TIER_DROPS), sim->tierCount(TIER_CLUSTERS),
                                 sim->tierCount(TIER_SHEETS), (S32)sim->ripples().size()));
    }
    else
    {
        lines.push_back("particles  (sim idle)");
    }

    const bool shader_live = use_rain_shader && preset.mWaterShading && gSSPrecipRainProgram.isComplete();
    lines.push_back(llformat("shading    rain %s   lit %s   SSR %s",
                             shader_live ? "water" : "fallback",
                             gSSPrecipLitProgram.isComplete() ? "on" : "fallback",
                             gPipeline.mSceneMap.getWidth() > 0 ? "on" : "off"));

    lines.push_back("-- audio --");
    lines.push_back(llformat("cover      %s   space %s%s",
                             audio->isCovered() ? "ROOFED" : "open sky",
                             SSWeatherSounds::spaceName(audio->space()),
                             audio->isCovered() ? ""
                                 : (std::string(" / ") + SSWeatherSounds::sizeName(audio->outdoorSize())).c_str()));
    if (audio->isCovered())
    {
        lines.push_back(llformat("roof       %.1fm above", audio->roofDistance()));
    }
    lines.push_back(llformat("walls      %d hit   avg %.1fm   blend %.2f",
                             audio->wallCount(), audio->wallDistance(), audio->coverBlend()));
    lines.push_back(llformat("impacts    %.1f/s   %d queued   loops %d",
                             audio->impactRate(), (S32)atmo->pendingImpacts(), audio->activeLoops()));
    lines.push_back(llformat("probe age  %.2fs", (F32)audio->lastProbeAge()));

    const LLFontGL* font = LLFontGL::getFontMonospace();
    const S32 line_h = font->getLineHeight();
    const S32 left = 12;
    S32 top = gViewerWindow->getWorldViewRectScaled().getHeight() - 32;

    gGL.pushMatrix();
    for (const std::string& line : lines)
    {
        // Header and section breaks in amber, values in white
        const bool header = (line.compare(0, 2, "--") == 0) || (line.compare(0, 4, "ATMO") == 0);
        const LLColor4 color = header ? LLColor4(1.f, 0.75f, 0.3f, 1.f) : LLColor4(0.9f, 0.95f, 1.f, 1.f);
        font->renderUTF8(line, 0, (F32)left, (F32)top, color,
                         LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        top -= line_h;
    }
    gGL.popMatrix();
}

// </SS:Nexii>
