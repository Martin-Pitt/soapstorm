/**
 * @file ssatmomagic.cpp
 * @brief See ssatmomagic.h.
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
#include "ssatmotrack.h"
#include "ssatmoenvapplier.h"
#include "ssatmoenvbridge.h"
#include "ssrainshadow.h"
#include "ssavatarwet.h"
#include "ssvolcloud.h"
#include "sslightning.h"
#include "sslightningrender.h"
#include "sssurfacefield.h"
#include "sswindflow.h"

#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "ssprecipitation.h"
#include "ssprecippreset.h"
#include "sssoundscape.h"
#include "sssoundmeta.h"

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

#include <algorithm>

static const F32 IMPACT_SEE_RADIUS  = 32.f;
static const F64 ASSET_POLL_PERIOD  = 2.0;

static const U32 SS_ATMO_SEED = 0x5EED1337u;

static LLTrace::BlockTimerStatHandle FTM_SS_ATMO("Atmo Magic");
static LLTrace::BlockTimerStatHandle FTM_SS_ATMO_IMPACTS("Impacts");

namespace SSAtmoNoise
{

// 1D lattice hash to [-1,1].
static F32 latticeGrad(U32 seed, S32 ix)
{
    return hash01(combine(seed, (U32)ix)) * 2.f - 1.f;
}

// 2D lattice hash to [-1,1].
static F32 latticeGrad2(U32 seed, S32 ix, S32 iy)
{
    return hash01(combine(seed, combine((U32)ix, (U32)iy * 0x27d4eb2fu))) * 2.f - 1.f;
}

// Quintic fade.
static inline F32 quintic(F32 t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

// 1D value noise.
F32 value1(F32 x, U32 seed)
{
    F32 fx = floorf(x);
    S32 ix = (S32)fx;
    F32 t = quintic(x - fx);
    return lerp(latticeGrad(seed, ix), latticeGrad(seed, ix + 1), t);
}

// 2D value noise.
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

// 1D fractal noise - the deterministic wobble everything shares.
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

// 2D fractal noise.
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

}

static const F32 TRACK_FADE_RATE = 0.45f;
static const F32 WIND_FADE_RATE  = 0.8f;

static const F64 WIND_DRIFT_WRAP = 1048576.0;

// Singleton shell; state arrives via refreshParams.
SSAtmoMagic::SSAtmoMagic()
{
}

SSAtmoMagic::~SSAtmoMagic()
{
}

// The static seed all deterministic weather derives from.
U32 SSAtmoMagic::seed() const
{
    return SS_ATMO_SEED;
}

// Water height used where no region answers.
F32 SSAtmoMagic::voidWaterHeight()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    return regionp ? regionp->getWaterHeight() : 20.f;
}

// Re-derives the whole running weather from the active track config: preset, intensity easing, wind, gusts, lightning handoff.
void SSAtmoMagic::refreshParams()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);

    SSAtmoTrackConfig v3_cfg;
    bool v3_is_ground_track = true;
    const F32 world_z = gAgent.getPositionAgent().mV[VZ];
    LLViewerRegion* agent_region = gAgent.getRegion();
    const LLUUID region_id = agent_region ? agent_region->getRegionID() : LLUUID::null;

    const bool teleported = !mV3PrevWorldZValid
        || region_id != mV3PrevRegionID
        || fabsf(world_z - mV3PrevWorldZ) > 60.f;

    const bool v3_active = SSAtmoEnvBridge::resolveActiveTrack(
        world_z, mV3PrevWorldZValid ? mV3PrevWorldZ : world_z, teleported, v3_cfg, v3_is_ground_track);

    mV3PrevWorldZ = world_z;
    mV3PrevWorldZValid = true;
    mV3PrevRegionID = region_id;

    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    mTrack = tracks->currentTrack();
    const SSAtmoTrackConfig& cfg = v3_active ? v3_cfg : tracks->config(mTrack);

    const bool track_runs = enabled && cfg.runs();

    SSPrecipPresetManager& mgr = SSPrecipPresetManager::instance();
    const SSPrecipPreset* named = cfg.mPreset.empty() ? nullptr : mgr.find(cfg.mPreset);
    const SSPrecipPreset& target_preset = named ? *named : mgr.active();

    const F32 dt = llclamp((F32)gFrameIntervalSeconds, 0.f, 0.25f);
    const bool preset_changed = (target_preset.mName != mPresetName);

    const F32 blend_target = (track_runs && !preset_changed) ? 1.f : 0.f;
    mBlend += (blend_target - mBlend) * llclamp(TRACK_FADE_RATE * dt, 0.f, 1.f);

    if (preset_changed && mBlend <= 0.02f)
    {
        mPreset = target_preset;
        mPresetName = target_preset.mName;
        mBlend = 0.f;
    }
    else if (!preset_changed)
    {
        mPreset = target_preset;
    }

    mEnabled = enabled && (cfg.runs() || mBlend > 0.01f);

    mSwitchedOn = enabled;

    mTemperatureC = cfg.mTemperatureC;

    mLightningColor = cfg.mLightningColor;
    mLightningCoreWhite = cfg.mLightningCoreWhite;

    mLightning = cfg.mLightning;
    mLightningCharge = cfg.mLightningCharge;
    mLightningSparks = cfg.mLightningSparks;
    mLightningIntervalMin = cfg.mLightningIntervalMin;
    mLightningIntervalMax = cfg.mLightningIntervalMax;
    mLightningIntensity = cfg.mLightningIntensity;

    mPrecipitation = llclamp(cfg.mPrecipitation, 0.f, 1.f) * mBlend;
    mTurbulence = llclamp(cfg.mTurbulence, 0.f, 1.f);

    mGustDepth = llclamp(cfg.mGustDepth, 0.f, 3.f) * mTurbulence * mBlend;
    mGustLength = llclamp(cfg.mGustLength, 8.f, 2000.f);
    mGustVeer = llclamp(cfg.mGustVeer, 0.f, 90.f) * DEG_TO_RAD;

    const F32 target_speed = track_runs ? llmax(0.f, cfg.mWindSpeed) : 0.f;
    const LLVector3 target_wind = cfg.windDirection() * target_speed;
    mWind += (target_wind - mWind) * llclamp(WIND_FADE_RATE * dt, 0.f, 1.f);
    mWindXY.set(mWind.mV[VX], mWind.mV[VY], 0.f);
    mWindSpeed = mWind.magVec();

    if (mWindDriftSeeded)
    {
        mWindDrift = fmod(mWindDrift + (F64)mWindSpeed * dt, WIND_DRIFT_WRAP);
    }
    else if (target_speed > 0.f)
    {
        mWindDrift = fmod(mNow * (F64)target_speed, WIND_DRIFT_WRAP);
        mWindDriftSeeded = true;
    }

    mSkyTrack = v3_active ? !v3_is_ground_track : tracks->isSkyTrack(mTrack);
    mGroundZero = cfg.mHasGround ? cfg.mGround : tracks->trackFloor(mTrack);
    mFallThrough = llclamp(cfg.mFallThrough, 0.f, 1.f);

    mHasWeather = mEnabled && mPrecipitation > 0.02f;

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
        const F32 tilt = llclamp(mWindXY.magVec() * mPreset.mWindResponse / fall, 0.f, 2.f);
        LLVector3 dir_xy = mWindXY;
        dir_xy.normVec();
        mRainDirection = dir_xy * tilt;
        mRainDirection.mV[VZ] = -1.f;
        mRainDirection.normVec();
    }
}

// The gust strength envelope at a moment - shared time, so every client agrees.
F32 SSAtmoMagic::gustEnvelopeAt(F64 time) const
{
    if (mTurbulence <= 0.f) return 1.f;

    const F32 t = (F32)fmod(time, 4096.0);
    F32 wave = SSAtmoNoise::fbm1(t * (0.05f + 0.15f * mTurbulence), SS_ATMO_SEED ^ 0xA17C0FEEu);
    wave = llclamp(wave * 2.2f + 0.5f, 0.f, 1.f);
    wave = wave * wave * (3.f - 2.f * wave);

    F32 burst = llclamp(SSAtmoNoise::fbm1(t * 0.7f, SS_ATMO_SEED ^ 0x00B57A9Du, 2) * 2.5f, 0.f, 1.f);
    burst = burst * burst * burst * 2.f;

    return lerp(1.f, 0.15f + 1.7f * wave + burst, mTurbulence);
}

// Slow spatial modulation of intensity over global position - showers have edges.
F32 SSAtmoMagic::areaFactorAt(F64 global_x, F64 global_y) const
{
    const F64 drift = mNow * 0.35;
    const F32 x = (F32)fmod(global_x - mWindXY.mV[0] * drift, 8192.0) * 0.02f;
    const F32 y = (F32)fmod(global_y - mWindXY.mV[1] * drift, 8192.0) * 0.02f;
    const F32 n = 0.5f + 0.5f * SSAtmoNoise::fbm2(x, y, SS_ATMO_SEED ^ 0x5EED0A2Bu);
    return 0.35f + 1.3f * n;
}

// Splits a CSV setting into asset entries.
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

// Reloads the texture and asset lists from settings.
void SSAtmoMagic::refreshAssets()
{
    const std::string fingerprint = mPreset.mName + "|" + mPreset.mTextures + "|"
                                  + mPreset.mRippleTexture;
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

}

// Fetches an asset's texture, tint and glow.
LLViewerTexture* SSAtmoMagic::textureFor(const SSAtmoAsset& asset, LLColor4& tint, F32& glow)
{
    if (asset.mIsPBR)
    {
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

// Rolls a particle texture from the configured set.
LLViewerTexture* SSAtmoMagic::pickParticleTexture(SSRandStream& rng, LLColor4& tint, F32& glow)
{
    tint = LLColor4::white;
    glow = 0.f;
    if (mTextureAssets.empty()) return nullptr;
    return textureFor(mTextureAssets[rng.rand((S32)mTextureAssets.size())], tint, glow);
}

// First usable texture from a CSV list.
LLViewerTexture* SSAtmoMagic::textureFromList(const std::string& csv)
{
    if (csv.empty()) return nullptr;

    std::vector<SSAtmoAsset> assets;
    parseAssetList(csv, assets);
    if (assets.empty() || assets[0].mIsPBR) return nullptr;
    return LLViewerTextureManager::getFetchedTexture(assets[0].mID);
}

// The ripple ring texture.
LLViewerTexture* SSAtmoMagic::rippleTexture()
{
    return mRippleTexture;
}

// Creates the particle sim on first need.
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

// Keeps agent-space state coherent across region origin shifts.
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

    for (auto& entry : mPendingEdits)
    {
        entry.second.mPos += offset;
    }
}

// Queues a drop impact to be processed at its own arrival time.
void SSAtmoMagic::queueImpact(F64 time, const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, const LLVector3& velocity, bool shatter,
                              bool from_runoff)
{
    if (mImpacts.size() > 16384) return;
    mImpacts.emplace(time, Impact{ pos_agent, normal, velocity, strength, on_water, shatter, from_runoff });
}

// Plays due impacts: sounds and effects, whatever the preset says an arrival does.
void SSAtmoMagic::processImpacts()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO_IMPACTS);

    static LLCachedControl<bool> ripples(gSavedSettings, "SSAtmoRipples", true);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    while (!mImpacts.empty() && mImpacts.begin()->first <= mNow)
    {
        const Impact impact = mImpacts.begin()->second;
        mImpacts.erase(mImpacts.begin());

        const F32 dist = (impact.mPosAgent - cam).magVec();
        if (dist > IMPACT_SEE_RADIUS) continue;

        SSSoundscape::getInstance()->notifyImpact(
            impact.mStrength * (1.f - dist / IMPACT_SEE_RADIUS));

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

    }
}

// The per-frame heartbeat: params, sim, sounds, fields, lightning - everything driven from here.
void SSAtmoMagic::idle()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO);

    mNow = LLDate::now().secondsSinceEpoch();

    SSAtmoTrackManager::getInstance()->idle();

    refreshParams();

    if (mEnabled && mNow - mLastAssetPoll > ASSET_POLL_PERIOD)
    {
        mLastAssetPoll = mNow;
        refreshAssets();
    }

    settleEdits();

    ensureSim();

    if (mSim)
    {
        mSim->update(gFrameIntervalSeconds);
    }

    SSSurfaceField::getInstance()->idle(gFrameIntervalSeconds);

    SSAvatarWet::getInstance()->idle(gFrameIntervalSeconds);

    SSLightning::getInstance()->idle(gFrameIntervalSeconds);

    SSVolCloud::getInstance()->update(gFrameIntervalSeconds);

    if (mEnabled)
    {
        processImpacts();
    }

    SSSoundscape::getInstance()->idle();
}

static const F64 EDIT_SETTLE_SECONDS = 4.0;

static const size_t MAX_PENDING_EDITS = 4096;

static const size_t MAX_SETTLE_PER_FRAME = 64;

static const F32 EDIT_SETTLE_SLOP = 0.25f;

// Feeds the settle queue: geometry must hold still a while before the maps spend a recapture on it.
void SSAtmoMagic::onObjectUpdate(LLViewerObject* objectp)
{
    SSAtmoMagic* self = getInstance();
    if (!self->mEnabled) return;
    if (!objectp || objectp->isDead() || objectp->isAvatar() || objectp->isAttachment()) return;

    const LLVector3 scale = objectp->getScale();
    const F32 dim = llmax(scale.mV[VX], scale.mV[VY], scale.mV[VZ]);
    if (dim < 0.5f) return;

    if (objectp->isLikelyProjectileBullet()) return;

    const LLVector3 pos = objectp->getRenderPosition();
    const F32 radius = scale.magVec() * 0.5f;

    const bool moving = objectp->getVelocity().magVecSquared() > 0.25f
                     || objectp->getAngularVelocity().magVecSquared() > 0.25f;

    auto it = self->mPendingEdits.find(objectp->getID());
    if (it != self->mPendingEdits.end())
    {
        PendingEdit& edit = it->second;

        if (moving || (edit.mPos - pos).magVecSquared() > EDIT_SETTLE_SLOP * EDIT_SETTLE_SLOP)
        {
            edit.mPos = pos;
            edit.mRadius = radius;
            edit.mSettleAt = self->mNow + EDIT_SETTLE_SECONDS;
            ++edit.mResets;
        }
        return;
    }

    if (self->mPendingEdits.size() >= MAX_PENDING_EDITS)
    {
        auto worst = self->mPendingEdits.begin();
        for (auto e = self->mPendingEdits.begin(); e != self->mPendingEdits.end(); ++e)
        {
            if (e->second.mSettleAt > worst->second.mSettleAt) worst = e;
        }
        if (worst->second.mSettleAt <= self->mNow + EDIT_SETTLE_SECONDS) return;
        self->mPendingEdits.erase(worst);
    }

    PendingEdit edit;
    edit.mPos = pos;
    edit.mRadius = radius;
    edit.mSettleAt = self->mNow + EDIT_SETTLE_SECONDS;
    edit.mFirstSeen = self->mNow;
    self->mPendingEdits[objectp->getID()] = edit;
}

// Promotes settled edits into rain-shadow dirty marks.
void SSAtmoMagic::settleEdits()
{
    if (mPendingEdits.empty()) return;

    size_t confirmed = 0;

    for (auto it = mPendingEdits.begin(); it != mPendingEdits.end(); )
    {
        if (mNow < it->second.mSettleAt)
        {
            ++it;
            continue;
        }

        LLViewerObject* objectp = gObjectList.findObject(it->first);

        if (!objectp || objectp->isDead())
        {
            it = mPendingEdits.erase(it);
            continue;
        }

        const LLVector3 pos = objectp->getRenderPosition();
        if ((pos - it->second.mPos).magVecSquared() > EDIT_SETTLE_SLOP * EDIT_SETTLE_SLOP)
        {
            it->second.mPos = pos;
            it->second.mRadius = objectp->getScale().magVec() * 0.5f;
            it->second.mSettleAt = mNow + EDIT_SETTLE_SECONDS;
            ++it->second.mResets;
            ++it;
            continue;
        }

        SSRainShadowMap::getInstance()->markDirty(it->second.mPos, it->second.mRadius);
        SSWindFlowMap::markDirty(it->second.mPos, it->second.mRadius);
        ++mSettledEdits;

        it = mPendingEdits.erase(it);

        if (++confirmed >= MAX_SETTLE_PER_FRAME) break;
    }
}

// Draws pending settle edits as boxes.
void SSAtmoMagic::renderDebug()
{
    if (mPendingEdits.empty()) return;

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    static const F32 BEACON_HEIGHT = 24.f;

    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        gGL.begin(LLRender::LINES);
        for (const auto& entry : mPendingEdits)
        {
            const PendingEdit& edit = entry.second;

            LLColor4 color = colorForEdit(edit);

            const LLVector3& p = edit.mPos;
            const F32 r = llmax(edit.mRadius, 0.25f);

            gGL.color4fv(color.mV);
            const LLVector3 lo = p - LLVector3(r, r, r);
            const LLVector3 hi = p + LLVector3(r, r, r);
            const F32 xs[2] = { lo.mV[VX], hi.mV[VX] };
            const F32 ys[2] = { lo.mV[VY], hi.mV[VY] };
            const F32 zs[2] = { lo.mV[VZ], hi.mV[VZ] };
            for (S32 a = 0; a < 2; ++a)
            {
                for (S32 b = 0; b < 2; ++b)
                {
                    gGL.vertex3f(xs[0], ys[a], zs[b]);  gGL.vertex3f(xs[1], ys[a], zs[b]);
                    gGL.vertex3f(xs[a], ys[0], zs[b]);  gGL.vertex3f(xs[a], ys[1], zs[b]);
                    gGL.vertex3f(xs[a], ys[b], zs[0]);  gGL.vertex3f(xs[a], ys[b], zs[1]);
                }
            }
        }
        gGL.end();
    }

    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.begin(LLRender::LINES);
        for (const auto& entry : mPendingEdits)
        {
            const PendingEdit& edit = entry.second;
            LLColor4 color = colorForEdit(edit);

            const LLVector3& p = edit.mPos;
            const S32 steps = 6;
            for (S32 i = 0; i < steps; ++i)
            {
                const F32 t0 = (F32)i / (F32)steps;
                const F32 t1 = (F32)(i + 1) / (F32)steps;
                gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE],
                            color.mV[VALPHA] * (1.f - t0));
                gGL.vertex3f(p.mV[VX], p.mV[VY], p.mV[VZ] + BEACON_HEIGHT * t0);
                gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE],
                            color.mV[VALPHA] * (1.f - t1));
                gGL.vertex3f(p.mV[VX], p.mV[VY], p.mV[VZ] + BEACON_HEIGHT * t1);
            }
        }
        gGL.end();
    }

    gGL.flush();
}

// Settle state to overlay colour.
LLColor4 SSAtmoMagic::colorForEdit(const PendingEdit& edit)
{
    SSAtmoMagic* self = getInstance();

    const F32 remaining = (F32)llclamp((edit.mSettleAt - self->mNow) / EDIT_SETTLE_SECONDS, 0.0, 1.0);
    const F32 alpha = 0.35f + 0.55f * (1.f - remaining);

    if (edit.mResets >= 10)  return LLColor4(1.f, 0.2f, 0.15f, alpha);
    if (edit.mResets >= 2)   return LLColor4(1.f, 0.7f, 0.15f, alpha);
    return LLColor4(0.3f, 1.f, 0.4f, alpha);
}

// The debug overlay text block: weather, sim, maps, sea, lightning.
void SSAtmoMagic::drawInfo()
{
    static LLCachedControl<bool> show_info(gSavedSettings, "SSAtmoShowInfo", false);
    if (!show_info) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSSoundscape* audio = SSSoundscape::getInstance();
    SSPrecipSim* sim = atmo->sim();

    const SSPrecipPreset& preset = atmo->preset();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3d cam_global = gAgent.getPosGlobalFromAgent(cam);
    const LLVector3 dir = atmo->rainDirection();

    static LLCachedControl<bool> use_rain_shader(gSavedSettings, "SSAtmoRainShader", true);

    std::vector<std::string> lines;
    lines.push_back(llformat("ATMO MAGIC  %s", atmo->isEnabled() ? "[enabled]" : "[disabled]"));
    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    lines.push_back(llformat("track      %d of 4   %s   ground zero %.0fm%s",
                             atmo->track(), tracks->statusText().c_str(),
                             atmo->groundZero(), atmo->isSkyTrack() ? " (sky)" : ""));
    if (atmo->trackBlend() < 0.99f)
    {
        lines.push_back(llformat("crossfade  %.2f", atmo->trackBlend()));
    }
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

    lines.push_back("-- geometry edits --");
    lines.push_back(llformat("queue      %d settling   %u believed",
                             (S32)atmo->pendingEdits(), atmo->settledEdits()));

    {
        std::vector<const std::pair<const LLUUID, PendingEdit>*> worst;
        for (const auto& entry : atmo->mPendingEdits)
        {
            if (entry.second.mResets >= 2) worst.push_back(&entry);
        }
        std::sort(worst.begin(), worst.end(),
                  [](const std::pair<const LLUUID, PendingEdit>* a,
                     const std::pair<const LLUUID, PendingEdit>* b)
                  { return a->second.mResets > b->second.mResets; });

        if (worst.empty())
        {
            lines.push_back("           nothing re-arming, queue is passing through");
        }
        else
        {
            lines.push_back(llformat("re-arming  %d of %d never settling",
                                     (S32)worst.size(), (S32)atmo->pendingEdits()));
        }

        for (size_t i = 0; i < worst.size() && i < 5; ++i)
        {
            const PendingEdit& edit = worst[i]->second;
            const LLVector3 delta = edit.mPos - cam;
            lines.push_back(llformat("  %s  x%u in %.0fs   %.0fm away   %.0f %.0f %.0f  r%.1f",
                                     worst[i]->first.asString().substr(0, 8).c_str(),
                                     edit.mResets, (F32)(atmo->mNow - edit.mFirstSeen),
                                     delta.magVec(),
                                     edit.mPos.mV[VX], edit.mPos.mV[VY], edit.mPos.mV[VZ],
                                     edit.mRadius));
        }
    }

    lines.push_back("-- wind flow --");
    SSWindFlowMap* flow = SSWindFlowMap::getInstance();
    if (!SSWindFlowMap::isSupported())
    {
        lines.push_back("flowmap    unavailable (needs OpenGL 4.3)");
    }
    else if (!flow->isValid())
    {
        lines.push_back("flowmap    idle");
    }
    else
    {
        lines.push_back(llformat("domain     %.0fm at %d texels (%.1fm/cell)  %d tiles",
                                 flow->extent(), flow->resolution(),
                                 flow->cellSize(), flow->tileCount()));
        lines.push_back(llformat("solved     %.0fs ago   builds %u",
                                 (F32)flow->age(), flow->buildCount()));

        std::string slabs;
        for (S32 i = 0; i <= flow->sliceCount(); ++i)
        {
            slabs += llformat("%s%.0f", i ? " / " : "", flow->sliceAltitude(i));
        }
        lines.push_back(llformat("slabs      %d   %s", flow->sliceCount(), slabs.c_str()));

        const LLVector3 local = flow->sample(cam);
        lines.push_back(llformat("local wind %.1f %.1f %.1f  (%.1f m/s)  exposure %.2f",
                                 local.mV[VX], local.mV[VY], local.mV[VZ],
                                 local.magVec(), flow->exposure(cam)));

        static LLCachedControl<F32> gust_travel(gSavedSettings, "SSAtmoWindGustTravel", 1.f);

        F32 gust_scale = 1.f, gust_veer = 0.f;
        flow->gustAt(cam, atmo->sharedTime(), gust_scale, gust_veer);

        const F32 gust_length = atmo->gustLength();
        const F32 gust_speed = llmax(0.1f, atmo->windSpeed()
                                           * llclamp((F32)gust_travel, 0.01f, 4.f));
        lines.push_back(llformat("gust wave  x%.2f   veer %+.0f deg   fronts every %.1fs",
                                 gust_scale, gust_veer * RAD_TO_DEG,
                                 gust_length / gust_speed));
        lines.push_back(llformat("build      %.1f ms", flow->lastSolveMS()));

        F32 top = 0.f;
        if (flow->surfaceAt(cam, top))
        {
            lines.push_back(llformat("surface    %.1fm top   %.0f%% solid   %.1f%% under the surface",
                                     top, flow->solidFill() * 100.f,
                                     flow->carvedFraction() * 100.f));
        }
        else
        {
            lines.push_back(llformat("surface    open column, nothing captured   %.0f%% solid",
                                     flow->solidFill() * 100.f));
        }
    }

    lines.push_back("-- rain shadow --");

    {
        LLVector3 hit;
        bool on_water = false;
        const bool mapped = SSRainShadowMap::getInstance()->resolveColumn(cam, hit, on_water);
        lines.push_back(llformat("column     %s at %.1fm (%+.1fm)%s",
                                 mapped ? "mapped surface" : "heightmap guess",
                                 hit.mV[VZ], hit.mV[VZ] - cam.mV[VZ],
                                 on_water ? ", water" : ""));
    }

    {
        SSRainShadowMap* shadow = SSRainShadowMap::getInstance();
        lines.push_back(llformat("shadow     %d regions at %d texels   %d dirty",
                                 shadow->tileCount(), (S32)shadow->resolution(),
                                 (S32)shadow->dirtyTileCount()));
        lines.push_back(llformat("captures   %u total, %u forced by edits   last %.1fs ago, %.1f ms",
                                 shadow->captureCount(), shadow->dirtyCaptureCount(),
                                 (F32)shadow->lastCaptureAge(), shadow->lastCaptureMS()));
    }

    lines.push_back("-- surface --");

    {
        SSSurfaceField* surface = SSSurfaceField::getInstance();
        lines.push_back(llformat("surface    %d fields   wet %.2f   snow %.0f mm   puddle %.0f mm   %.1f ms",
                                 surface->fieldCount(), surface->peakWet(),
                                 surface->peakSnow() * 1000.f,
                                 surface->peakPuddle() * 1000.f,
                                 surface->lastTickMS()));
    }

    lines.push_back("-- audio --");
    lines.push_back(llformat("analysis   %d sounds ready   %d pending",
                             SSSoundMeta::getInstance()->readyCount(),
                             SSSoundMeta::getInstance()->pendingCount()));
    lines.push_back(llformat("cover      %s   space %s%s",
                             audio->isCovered() ? "ROOFED" : "open sky",
                             SSSoundscape::spaceName(audio->space()),
                             audio->isCovered() ? ""
                                 : (std::string(" / ") + SSSoundscape::sizeName(audio->outdoorSize())).c_str()));
    if (audio->isCovered())
    {
        lines.push_back(llformat("roof       %.1fm above   buried %.1fm   occlusion %.2f",
                                 audio->roofDistance(), audio->burialDepth(),
                                 audio->burialOcclusion()));
    }
    lines.push_back(llformat("walls      %d hit   avg %.1fm   blend %.2f",
                             audio->wallCount(), audio->wallDistance(), audio->coverBlend()));
    {
        SSLightning* lit = SSLightning::getInstance();
        const F64 next = lit->nextStrikeIn();
        lines.push_back(llformat("lightning  %d live   flash %.2f   %s   %d thunder pending",
                                 lit->liveCount(), lit->flash(),
                                 next < 0.0 ? "not thundery"
                                            : llformat("next in %.0fs", next).c_str(),
                                 audio->pendingThunder()));
        {
            const SSLightningRender::DrawStats& ds = SSLightningRender::getInstance()->stats();
            lines.push_back(llformat("  draw     %d live / %d bright / %d offscreen   %d segs",
                                     ds.mStrikes, ds.mBright, ds.mOffScreen, ds.mSegments));
        }
        for (const SSStrike& st : lit->strikes())
        {
            lines.push_back(llformat("  %-6s %5.0fm   %+.2fs   leader %.2f   bright %.2f   ch %d   st %d%s",
                                     SSLightning::kindName(st.mKind),
                                     st.mDistanceM, st.mT,
                                     st.mLeaderProgress, st.mChannelBrightness,
                                     (S32)st.mChannel.size(), st.mStrokeCount,
                                     st.mAudible ? "" : "   [silent, shadow zone]"));
        }
    }

    for (S32 self = 1; self >= 0; --self)
    {
        const SSSoundscape::StepDebug& st = audio->lastStep(self != 0);
        const char* who = self ? "you " : "them";

        if (st.mWhen < 0.0)
        {
            lines.push_back(llformat("step %s  none yet", who));
            continue;
        }

        static const char* ACTION[] = { "walk", "run", "jump", "land" };
        const char* act = (st.mAction >= 0 && st.mAction < 4) ? ACTION[st.mAction] : "?";

        std::string surface("(not reached)");
        if (st.mSurface >= 0)
        {
            surface = SSFootstepSounds::surfaceKey((SSStepSurface)st.mSurface);
        }

        lines.push_back(llformat("step %s  %.1fs ago   %s / %s   %s(%c)   wet %.2f%s",
                                 who,
                                 (F32)(atmo->sharedTime() - st.mWhen),
                                 surface.c_str(), act,
                                 st.mIndoors ? "in" : "out", st.mIndoorsFrom,
                                 st.mWet,
                                 st.mFieldValid ? "" : "   [field: NO ANSWER]"));
        if (st.mWhyNot[0])
        {
            lines.push_back(llformat("  SILENT   %s   [%s]",
                                     st.mWhyNot, st.mSource.c_str()));
        }
        else
        {
            lines.push_back(llformat("  played   %s of %d   [%s]",
                                     st.mPicked.asString().substr(0, 8).c_str(),
                                     st.mListSize, st.mSource.c_str()));
        }
        lines.push_back(llformat("  mode     %s",
                                 st.mMode == 'S' ? "per-impact segments" :
                                 st.mMode == 'L' ? "attached loop" : "-"));
        if (st.mMode == 'S')
        {
            // The number to eyeball against the gait: SL walks a step roughly every 0.5s and runs one roughly every 0.3s, so a gap near double that means footfalls are being missed rather than
            // played per step. Any drops at all mean the anti-spam gate is firing, which it should not have to during a steady walk.
            lines.push_back(llformat("  cadence  %.2fs between steps   %.1f/s   %d dropped",
                                     st.mStepGap,
                                     st.mStepGap > 0.01f ? 1.f / st.mStepGap : 0.f,
                                     st.mStepDropped));
        }
    }

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
        const bool header = (line.compare(0, 2, "--") == 0) || (line.compare(0, 4, "ATMO") == 0);
        const LLColor4 color = header ? LLColor4(1.f, 0.75f, 0.3f, 1.f) : LLColor4(0.9f, 0.95f, 1.f, 1.f);
        font->renderUTF8(line, 0, (F32)left, (F32)top, color,
                         LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        top -= line_h;
    }
    gGL.popMatrix();
}
