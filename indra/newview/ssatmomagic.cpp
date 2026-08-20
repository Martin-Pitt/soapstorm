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
#include "ssatmotrack.h"
#include "ssrainshadow.h"
#include "ssrunoff.h"
#include "sswindflow.h"

#include "llviewerobject.h"
#include "llviewerobjectlist.h"
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

#include <algorithm>

// <SS:Nexii> Atmo Magic weather system

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

// Track crossfade rates, per second. Precipitation swaps need to be quick
// enough not to feel like a bug and slow enough to read as weather; wind eases
// faster because the audio bed follows it directly.
static const F32 TRACK_FADE_RATE = 0.45f;
static const F32 WIND_FADE_RATE  = 0.8f;

// Wrap on the accumulated wind drift, metres. Far outside anything a gust
// pattern is sampled across, and a multiple of the region grid.
static const F64 WIND_DRIFT_WRAP = 1048576.0;

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

    // Weather is configured per EEP sky track and only runs for the track the
    // camera is in. Nothing is defined by default, so a track without a config
    // stays clear no matter what the master switch says.
    SSAtmoTrackMgr* tracks = SSAtmoTrackMgr::getInstance();
    mTrack = tracks->currentTrack();
    const SSAtmoTrackConfig& cfg = tracks->config(mTrack);

    const bool track_runs = enabled && cfg.runs();

    // The preset this track asks for; an empty or unrecognised name falls back
    // to whatever the preset editor currently has selected.
    SSPrecipPresetMgr& mgr = SSPrecipPresetMgr::instance();
    const SSPrecipPreset* named = cfg.mPreset.empty() ? nullptr : mgr.find(cfg.mPreset);
    const SSPrecipPreset& target_preset = named ? *named : mgr.active();

    // Crossfade. A preset cannot be interpolated, so precipitation eases to
    // nothing before a swap and back up afterwards: crossing a band boundary,
    // or a notecard arriving mid-session, reads as the weather changing rather
    // than teleporting.
    const F32 dt = llclamp((F32)gFrameIntervalSeconds, 0.f, 0.25f);
    const bool preset_changed = (target_preset.mName != mPresetName);

    const F32 blend_target = (track_runs && !preset_changed) ? 1.f : 0.f;
    mBlend += (blend_target - mBlend) * llclamp(TRACK_FADE_RATE * dt, 0.f, 1.f);

    if (preset_changed && mBlend <= 0.02f)
    {
        // Fully faded out; adopt the incoming preset and let it rise again
        mPreset = target_preset;
        mPresetName = target_preset.mName;
        mBlend = 0.f;
    }
    else if (!preset_changed)
    {
        // Same preset: keep tracking it so edits in the preset editor stay live
        mPreset = target_preset;
    }

    // Stay enabled while fading out, otherwise the sim is torn down before the
    // fade can be seen
    mEnabled = enabled && (cfg.runs() || mBlend > 0.01f);

    mPrecipitation = llclamp(cfg.mPrecipitation, 0.f, 1.f) * mBlend;
    mTurbulence = llclamp(cfg.mTurbulence, 0.f, 1.f);

    // Gust shape, folded into one depth figure with the turbulence it scales
    // by. Eased along with the crossfade so crossing a band boundary does not
    // step from a steady wind into a squall between frames.
    mGustDepth = llclamp(cfg.mGustDepth, 0.f, 3.f) * mTurbulence * mBlend;
    mGustLength = llclamp(cfg.mGustLength, 8.f, 2000.f);
    mGustVeer = llclamp(cfg.mGustVeer, 0.f, 90.f) * DEG_TO_RAD;

    // Ease the wind vector rather than the orientation, so a direction change
    // cannot sweep the long way round the compass
    const F32 target_speed = track_runs ? llmax(0.f, cfg.mWindSpeed) : 0.f;
    const LLVector3 target_wind = cfg.windDirection() * target_speed;
    mWind += (target_wind - mWind) * llclamp(WIND_FADE_RATE * dt, 0.f, 1.f);
    mWindXY.set(mWind.mV[VX], mWind.mV[VY], 0.f);
    mWindSpeed = mWind.magVec();

    // How far the air itself has travelled. Anything keyed to a pattern that
    // rides along with the wind - the flowmap's gust waves - phases off this
    // rather than off speed times the clock, which would jump the whole field
    // bodily every time the wind eased to a different speed.
    //
    // Seeded from the shared clock so two clients that have been standing in
    // the same steady wind agree on where in the cycle it is; after that they
    // only diverge by the history of speed changes they each saw. Wrapped a
    // long way out to keep the single precision the field is sampled at, at
    // the cost of one phase jump per several hours of hard wind.
    if (mWindDriftSeeded)
    {
        mWindDrift = fmod(mWindDrift + (F64)mWindSpeed * dt, WIND_DRIFT_WRAP);
    }
    else if (target_speed > 0.f)
    {
        mWindDrift = fmod(mNow * (F64)target_speed, WIND_DRIFT_WRAP);
        mWindDriftSeeded = true;
    }

    // Ground zero for this track: terrain and water at ground level, the band's
    // own base altitude up in the sky unless the config pins it to a platform
    mSkyTrack = tracks->isSkyTrack(mTrack);
    mGroundZero = cfg.mHasGround ? cfg.mGround : tracks->trackFloor(mTrack);
    mFallThrough = llclamp(cfg.mFallThrough, 0.f, 1.f);

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
        const F32 tilt = llclamp(mWindXY.magVec() * mPreset.mWindResponse / fall, 0.f, 2.f);
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

    // Queued edits are agent-space too. Left unshifted they would mature into
    // a dirty mark a region's width away from the object that caused it, and
    // every one of them would look like it had moved on the next settle pass
    // and re-arm, so a region crossing would both miss the real geometry and
    // stall the queue.
    for (auto& entry : mPendingEdits)
    {
        entry.second.mPos += offset;
    }
}

void SSAtmoMagic::queueImpact(F64 time, const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, const LLVector3& velocity, bool shatter,
                              bool from_runoff)
{
    // Bounded so a hitch or bad parameters can't grow this without limit.
    //
    // The bound has to hold everything in flight, not everything landing this
    // second: an impact is queued when its drop is spawned and dispatched when
    // it arrives, so the queue depth is the landing rate times the whole fall
    // time. Rain falling 16 to 26 metres at 9.5 m/s is better than two seconds
    // in the air, and a few thousand landings a second across the impact radius
    // puts ten thousand entries in here in steady state. The old bound of two
    // thousand was reached almost immediately and silently threw the rest away,
    // which is why the ripples read as a fraction of the drops.
    if (mImpacts.size() > 16384) return;
    mImpacts.emplace(time, Impact{ pos_agent, normal, velocity, strength, on_water, shatter, from_runoff });
}

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

        // Feeds the ambient loop loudness: sheltered spots see fewer
        // landings and sound quieter at the same weather parameters
        SSWeatherSounds::getInstance()->notifyImpact(
            impact.mStrength * (1.f - dist / IMPACT_SEE_RADIUS));

        // Ground truth for the runoff system: a landing that actually arrived,
        // after every throttle and cull between the weather parameters and the
        // screen. Drips off an eave are excluded - they are the output of that
        // system, not evidence about the rain feeding it.
        if (!impact.mRunoff)
        {
            SSRunoff::getInstance()->notifyImpact(impact.mPosAgent, IMPACT_SEE_RADIUS);
        }

        // Deterministic per-impact stream so the ripple and any shatter look
        // the same on every client watching the same landing
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

        // No per-landing one-shot. A field of individually triggered drops
        // never resolved into rain: the trigger rate had to stay low enough to
        // be affordable, which left it sounding like a handful of taps rather
        // than a downpour, and each sound fought the ambient bed instead of
        // adding to it. The bed carries the rain now, and notifyImpact above is
        // what makes it answer to how much is actually landing where you are
        // standing rather than to the weather parameters alone.
    }
}

void SSAtmoMagic::idle()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO);

    mNow = LLDate::now().secondsSinceEpoch();

    // Parcel description polling and notecard fetch; must run before params
    // are resolved so a config landing this frame is picked up immediately
    SSAtmoTrackMgr::getInstance()->idle();

    refreshParams();

    if (mEnabled && mNow - mLastAssetPoll > ASSET_POLL_PERIOD)
    {
        mLastAssetPoll = mNow;
        refreshAssets();
    }

    // Geometry changes that have held still long enough to be believed. Before
    // the maps update, so a settled edit is picked up by this frame's capture.
    settleEdits();

    ensureSim();

    if (mSim)
    {
        mSim->update(gFrameIntervalSeconds);
    }

    // Roof drainage. Runs before the impacts it schedules are dispatched, so a
    // drip released this frame is queued in time to be seen landing.
    SSRunoff::getInstance()->idle(gFrameIntervalSeconds);

    if (mEnabled)
    {
        processImpacts();
    }

    // Ambient rain/wind loops and the indoor probe cycle; fades everything
    // out itself when the system is off
    SSWeatherSounds::getInstance()->idle();
}


//-----------------------------------------------------------------------------
// Geometry change settling
//-----------------------------------------------------------------------------

// How long an object has to stay put before either map will rebuild for it.
// Long enough that a thrown prim, a bullet or a combat rez is gone before its
// entry matures; short enough that editing a roof shows up while you are still
// looking at it.
static const F64 EDIT_SETTLE_SECONDS = 4.0;

// A rezzing region can produce updates faster than they settle, and it holds
// thousands of prims, so this has to be roomy: an entry is a few dozen bytes
// and the cap exists to bound a pathological case, not to ration normal use.
//
// When it is hit the entry *furthest* from maturing is the one dropped. This
// was the other way round once, and dropping the oldest meant that under heavy
// rezzing - exactly when the maps most need to hear about the new geometry -
// the entries about to be confirmed were the ones thrown away, so nothing ever
// matured and the shadow map never learned the buildings were there. Rain fell
// straight through their roofs until the periodic full recapture caught up.
static const size_t MAX_PENDING_EDITS = 4096;

// Confirmations per frame. Each one dirties two maps; doing thousands in the
// frame a big build finishes settling would be its own hitch.
static const size_t MAX_SETTLE_PER_FRAME = 64;

// How far an object may drift and still count as the same resting object.
// Interpolation and terse updates jitter a stationary prim by centimetres.
static const F32 EDIT_SETTLE_SLOP = 0.25f;

// static
void SSAtmoMagic::onObjectUpdate(LLViewerObject* objectp)
{
    SSAtmoMagic* self = getInstance();
    if (!self->mEnabled) return;
    if (!objectp || objectp->isDead() || objectp->isAvatar() || objectp->isAttachment()) return;

    const LLVector3 scale = objectp->getScale();
    const F32 dim = llmax(scale.mV[VX], scale.mV[VY], scale.mV[VZ]);
    if (dim < 0.5f) return;     // too small to matter at either map's resolution

    // Already tagged as a bullet by its shape: long, thin, and almost always
    // gone within the second. Nothing shaped like that shelters anything from
    // rain or blocks any wind worth solving for.
    if (objectp->isLikelyProjectileBullet()) return;

    const LLVector3 pos = objectp->getRenderPosition();
    const F32 radius = scale.magVec() * 0.5f;

    // Note what is moving, do not discard it. Something in flight is not part
    // of the build yet, but it may be about to become part of it - a prim
    // being dragged into place, a physical object settling - and an object
    // whose last update carried a velocity would otherwise never be queued at
    // all, so the maps would never hear about it. The settle pass re-checks
    // position and keeps deferring until it actually stops; a projectile is
    // gone before that happens and is dropped without ever costing a rebuild.
    const bool moving = objectp->getVelocity().magVecSquared() > 0.25f
                     || objectp->getAngularVelocity().magVecSquared() > 0.25f;

    auto it = self->mPendingEdits.find(objectp->getID());
    if (it != self->mPendingEdits.end())
    {
        PendingEdit& edit = it->second;

        // Moved again, so it has not settled. Restart its clock rather than
        // letting a slow drag mature partway through.
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
        // Drop whatever is furthest from being believed, never what is closest
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
            // Rezzed, did whatever it was doing, and went. This is the whole
            // point of the delay: a projectile never gets this far, so it never
            // costs a rebuild.
            it = mPendingEdits.erase(it);
            continue;
        }

        const LLVector3 pos = objectp->getRenderPosition();
        if ((pos - it->second.mPos).magVecSquared() > EDIT_SETTLE_SLOP * EDIT_SETTLE_SLOP)
        {
            // Still moving, just not sending updates fast enough to have reset
            // the clock. Give it another window.
            it->second.mPos = pos;
            it->second.mRadius = objectp->getScale().magVec() * 0.5f;
            it->second.mSettleAt = mNow + EDIT_SETTLE_SECONDS;
            ++it->second.mResets;
            ++it;
            continue;
        }

        // It is part of the build now. Both maps hear about it once.
        SSRainShadowMap::getInstance()->markDirty(it->second.mPos, it->second.mRadius);
        SSWindFlowMap::markDirty(it->second.mPos, it->second.mRadius);
        ++mSettledEdits;

        it = mPendingEdits.erase(it);

        // The rest keep until next frame. They are already past their settle
        // time, so nothing is lost by finishing them a frame later.
        if (++confirmed >= MAX_SETTLE_PER_FRAME) break;
    }
}

//-----------------------------------------------------------------------------
// Geometry settling overlay
//-----------------------------------------------------------------------------

// A beacon over every entry still waiting out its settle delay. The counts in
// the info overlay say the queue is busy but not what is keeping it busy, and
// the two causes look identical from a number: a stream of short-lived objects
// passing through costs nothing, while a few objects that never hold still are
// a permanent tax on both maps. This puts each entry where you can walk to it
// and see what it actually is.
void SSAtmoMagic::renderDebug()
{
    if (mPendingEdits.empty()) return;

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // How tall the beacons stand. Tall enough to clear the roof of whatever is
    // holding the entry, since the offender is usually a prim inside a build
    // rather than something out in the open.
    static const F32 BEACON_HEIGHT = 24.f;

    // The box is depth tested against the world, so a marker inside a wall
    // reads as inside it. The beacon above it is not: its whole job is to be
    // findable from across the region.
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        gGL.begin(LLRender::LINES);
        for (const auto& entry : mPendingEdits)
        {
            const PendingEdit& edit = entry.second;

            // Green means it is about to mature and cost one rebuild, which is
            // the queue working. Amber means it has pushed itself back a few
            // times. Red means it has been re-arming for long enough that it is
            // never going to settle on its own - a swaying tree, a hovering
            // vehicle, a scripted prim nudging itself - and that is what this
            // overlay exists to find.
            LLColor4 color = colorForEdit(edit);

            const LLVector3& p = edit.mPos;
            const F32 r = llmax(edit.mRadius, 0.25f);

            // Axis-aligned box at the size the maps were told to dirty, not at
            // the object's own bounds: an entry that dirties far more than the
            // thing causing it is worth seeing as such.
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
                    // One edge along each axis per corner pair: twelve in all
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

            // The column fades out with height so a screen full of them still
            // reads as a set of points on the ground rather than as bars
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

// Shared by the boxes, the beacons and the info overlay's offender list, so
// the colour in the readout is the colour standing over the object.
// static
LLColor4 SSAtmoMagic::colorForEdit(const PendingEdit& edit)
{
    SSAtmoMagic* self = getInstance();

    // Fully bright as it comes due, dimmer while it still has time to run, so
    // a marker brightening and vanishing is a normal edit being believed
    const F32 remaining = (F32)llclamp((edit.mSettleAt - self->mNow) / EDIT_SETTLE_SECONDS, 0.0, 1.0);
    const F32 alpha = 0.35f + 0.55f * (1.f - remaining);

    if (edit.mResets >= 10)  return LLColor4(1.f, 0.2f, 0.15f, alpha);
    if (edit.mResets >= 2)   return LLColor4(1.f, 0.7f, 0.15f, alpha);
    return LLColor4(0.3f, 1.f, 0.4f, alpha);
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
    SSAtmoTrackMgr* tracks = SSAtmoTrackMgr::getInstance();
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

    // Feeds both maps below, so it sits ahead of them rather than inside
    // either. A prim change is held here until it has stayed put long enough
    // to be worth rebuilding for; "settling" high with "believed" flat means
    // neither map is hearing about the build at all.
    lines.push_back("-- geometry edits --");
    lines.push_back(llformat("queue      %d settling   %u believed",
                             (S32)atmo->pendingEdits(), atmo->settledEdits()));

    // A queue that sits at a steady handful is either harmless churn or the
    // same few objects re-arming forever, and the count alone cannot tell you
    // which. List the ones that have pushed themselves back the most, with
    // where to go and look; the beacon overlay marks these same entries in
    // world in the same colours.
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
        // Age and build count together: the map is meant to be static, so a
        // count that climbs while you stand still means something is churning
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

        // The travelling gust at this spot, and how long a wave takes to reach
        // here from the region's windward edge. Watch the multiplier and you
        // are watching the surge arrive.
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

        // What the mask holds over this exact spot. A column with nothing
        // captured over it, or a volume that is barely solid anywhere, both
        // mean the air has nothing to flow around however many passes it gets.
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

    // What the rain shadow answers for the same column. Most land here is mesh
    // and prims, so the difference between a surface the capture saw and the
    // terrain heightmap it falls back to is the difference between rain landing
    // on the roof over you and rain starting inside the room.
    {
        LLVector3 hit;
        bool on_water = false;
        const bool mapped = SSRainShadowMap::getInstance()->resolveColumn(cam, hit, on_water);
        lines.push_back(llformat("column     %s at %.1fm (%+.1fm)%s",
                                 mapped ? "mapped surface" : "heightmap guess",
                                 hit.mV[VZ], hit.mV[VZ] - cam.mV[VZ],
                                 on_water ? ", water" : ""));
    }

    // How hard the map is working. "heightmap guess" above with a capture age
    // climbing past the refresh interval means it has not caught up with the
    // build; a forced count that never moves while a region rezzes around you
    // means it is not being told the build changed at all.
    {
        SSRainShadowMap* shadow = SSRainShadowMap::getInstance();
        lines.push_back(llformat("shadow     %d regions at %d texels   %d dirty",
                                 shadow->tileCount(), (S32)shadow->resolution(),
                                 (S32)shadow->dirtyTileCount()));
        lines.push_back(llformat("captures   %u total, %u forced by edits   last %.1fs ago, %.1f ms",
                                 shadow->captureCount(), shadow->dirtyCaptureCount(),
                                 (F32)shadow->lastCaptureAge(), shadow->lastCaptureMS()));
    }

    lines.push_back("-- runoff --");

    {
        SSRunoff* runoff = SSRunoff::getInstance();
        lines.push_back(llformat("runoff     %d eaves over %d regions   %.1f drips/s   %d streams",
                                 runoff->eaveCount(), runoff->networkCount(),
                                 runoff->dripRate(),
                                 sim ? sim->streamCount() : 0));
        // Traces should be rare: the count climbing while you stand still means
        // something is churning the geometry serial
        lines.push_back(llformat("drainage   delivery x%.2f   traced %.1f ms   traces %u",
                                 runoff->delivery(), runoff->lastBuildMS(),
                                 runoff->buildCount()));
    }

    lines.push_back("-- audio --");
    lines.push_back(llformat("cover      %s   space %s%s",
                             audio->isCovered() ? "ROOFED" : "open sky",
                             SSWeatherSounds::spaceName(audio->space()),
                             audio->isCovered() ? ""
                                 : (std::string(" / ") + SSWeatherSounds::sizeName(audio->outdoorSize())).c_str()));
    if (audio->isCovered())
    {
        // Ceiling from the up ray, and how much more build the flowmap's
        // height capture says is stacked on top of it. A cellar reads as a
        // couple of metres of ceiling with storeys of burial above it.
        lines.push_back(llformat("roof       %.1fm above   buried %.1fm   occlusion %.2f",
                                 audio->roofDistance(), audio->burialDepth(),
                                 audio->burialOcclusion()));
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
