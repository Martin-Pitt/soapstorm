/**
 * @file sslightningrender.cpp
 * @brief See sslightningrender.h.
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

#include "sslightningrender.h"

#include "sslightning.h"
#include "ssatmomagic.h"
#include "ssvolcloud.h"

#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llrender.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    const F32 SPARK_LIFE_S = 1.4f;
    const F32 SPARK_GRAVITY = 9.8f;
    const S32 SPARK_COUNT = 40;

    const F32 CORE_WIDTH_M = 2.2f;
    const F32 GLOW_WIDTH_MULT = 7.f;

    // One camera-faced quad segment - every bolt, spark and marker is built from these.
    void ribbon(const LLVector3& a, const LLVector3& b, const LLVector3& cam,
                F32 width_a, F32 width_b, F32 v0, F32 v1)
    {
        LLVector3 seg = b - a;
        LLVector3 mid = (a + b) * 0.5f;
        LLVector3 view = mid - cam;
        LLVector3 side = seg % view;
        if (side.normalize() <= 0.f) return;

        const LLVector3 a0 = a - side * width_a;
        const LLVector3 a1 = a + side * width_a;
        const LLVector3 b0 = b - side * width_b;
        const LLVector3 b1 = b + side * width_b;

        gGL.texCoord2f(0.f, v0); gGL.vertex3fv(a0.mV);
        gGL.texCoord2f(1.f, v0); gGL.vertex3fv(a1.mV);
        gGL.texCoord2f(1.f, v1); gGL.vertex3fv(b1.mV);

        gGL.texCoord2f(0.f, v0); gGL.vertex3fv(a0.mV);
        gGL.texCoord2f(1.f, v1); gGL.vertex3fv(b1.mV);
        gGL.texCoord2f(0.f, v1); gGL.vertex3fv(b0.mV);
    }

    // Camera-faced square for flash discs and point glows.
    void billboard(const LLVector3& center, F32 radius, const LLVector3& cam)
    {
        LLVector3 to_cam = cam - center;
        if (to_cam.normalize() <= 0.f) return;

        LLVector3 ref = (fabsf(to_cam.mV[VZ]) > 0.9f)
            ? LLVector3(1.f, 0.f, 0.f) : LLVector3(0.f, 0.f, 1.f);
        LLVector3 right = to_cam % ref;
        if (right.normalize() <= 0.f) return;
        LLVector3 up = right % to_cam;
        if (up.normalize() <= 0.f) return;

        const LLVector3 r = right * radius;
        const LLVector3 u = up * radius;

        const LLVector3 bl = center - r - u;
        const LLVector3 br = center + r - u;
        const LLVector3 tl = center - r + u;
        const LLVector3 tr = center + r + u;

        gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bl.mV);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex3fv(br.mV);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(tr.mV);

        gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bl.mV);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(tr.mV);
        gGL.texCoord2f(0.f, 1.f); gGL.vertex3fv(tl.mV);
    }

    // Applies the shared far-field squash so bolts sit at the same drawn depth as the cloud field.
    LLVector3 drawnPoint(const LLVector3& p, const LLVector3& cam, F32& scale_out)
    {
        const LLVector3 rel = p - cam;
        scale_out = SSVolCloud::getInstance()->squashScale(rel.magVec());
        return cam + rel * scale_out;
    }

    // Cheap frustum test on the channel trunk (or flash sphere) so off-screen strikes skip all vertex work.
    bool strikeOnScreen(const SSStrike& strike)
    {
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 cam = camera->getOrigin();
        const F32 flash_r = llmax(350.f, strike.mDistanceM * 0.18f);

        F32 s = 1.f;
        if (strike.mChannel.empty())
        {
            const LLVector3 center = drawnPoint((strike.mOrigin + strike.mGround) * 0.5f, cam, s);
            return camera->sphereInFrustum(center,
                ((strike.mOrigin - strike.mGround).magVec() * 0.5f + flash_r) * s) != 0;
        }

        S32 trunk_n = 0;
        for (const SSStrikeNode& node : strike.mChannel) { if (node.mTrunk) ++trunk_n; else break; }
        if (trunk_n <= 0) return true;

        const F32 span = (strike.mChannel[0].mPos
                          - strike.mChannel[(size_t)(trunk_n - 1)].mPos).magVec();
        const F32 r = llmax(flash_r, span * 0.3f);

        const S32 samples[3] = { 0, trunk_n / 2, trunk_n - 1 };
        for (S32 i = 0; i < 3; ++i)
        {
            const LLVector3 p = drawnPoint(strike.mChannel[(size_t)samples[i]].mPos, cam, s);
            if (camera->sphereInFrustum(p, r * s) != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Stateless integer hash behind spark and corona randomness - deterministic per strike, no RNG state to drift.
    U32 hash3(U32 x)
    {
        x ^= x >> 16; x *= 0x7feb352du;
        x ^= x >> 15; x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }
    // Hash to [0,1).
    F32 hashUnit(U32 x) { return (F32)(hash3(x) & 0xffffffu) / (F32)0x1000000; }
}

// Additive glow discs along the channel (or at a sheet strike's origin) that wash the sky while a strike flashes.
void SSLightningRender::renderFlash()
{
    SSLightning* lightning = SSLightning::getInstance();

    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }
    if (!gSSLightningProgram.isComplete()) return;

    bool any_flash = false;
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mFlash > 0.002f) { any_flash = true; break; }
    }
    if (!any_flash) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.4f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);
    const LLColor3 GLOW_COLOR = SSAtmoMagic::getInstance()->lightningColor();

    gSSLightningProgram.bind();
    static LLStaticHashedString s_use_tex("ss_use_tex");
    static LLStaticHashedString s_radial("ss_radial");
    gSSLightningProgram.uniform1f(s_use_tex, 0.f);
    gSSLightningProgram.uniform1f(s_radial, 1.f);

    {
        static LLStaticHashedString s_squash("ss_squash");
        static LLStaticHashedString s_cam("ss_cam_pos");
        SSVolCloud* vol = SSVolCloud::getInstance();
        gSSLightningProgram.uniform3f(s_squash, vol->squashKnee(), vol->squashCap(), vol->virtualRadius());
        gSSLightningProgram.uniform3fv(s_cam, 1, cam.mV);
    }
    gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);

    LLGLDisable cull(GL_CULL_FACE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setColorMask(true, true);

    gGL.begin(LLRender::TRIANGLES);
    for (const SSStrike& strike : lightning->strikes())
    {
        if (strike.mFlash <= 0.002f) continue;
        if (!strikeOnScreen(strike)) continue;

        const F32 a = llclamp(strike.mFlash, 0.f, 1.f);

        if (!strike.mChannel.empty())
        {
            const F32 radius = llmax(160.f, strike.mDistanceM * 0.08f);
            const S32 DISCS = 5;

            S32 trunk_n = 0;
            for (const SSStrikeNode& node : strike.mChannel) { if (node.mTrunk) ++trunk_n; else break; }

            if (trunk_n > 0)
            {
                for (S32 di = 0; di < DISCS; ++di)
                {
                    const S32 want = (S32)((F32)di / (F32)(DISCS - 1) * (F32)(trunk_n - 1));

                    gGL.color4f(GLOW_COLOR.mV[0] * a * 0.35f, GLOW_COLOR.mV[1] * a * 0.35f,
                                GLOW_COLOR.mV[2] * a * 0.35f, glow * a * 0.08f);
                    billboard(strike.mChannel[(size_t)want].mPos, radius, cam);
                }
            }
        }
        else
        {
            const F32 radius = llmax(350.f, strike.mDistanceM * 0.18f);
            gGL.color4f(GLOW_COLOR.mV[0] * a * 0.6f, GLOW_COLOR.mV[1] * a * 0.6f,
                        GLOW_COLOR.mV[2] * a * 0.6f, glow * a * 0.12f);
            billboard(strike.mOrigin, radius, cam);
        }
    }
    gGL.end();
    gGL.flush();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, true);
    gSSLightningProgram.unbind();
}

// Draws every live strike: layered core and glow ribbons per return stroke, ground-charge shimmer, impact sparks, debug markers.
void SSLightningRender::render()
{
    SSLightning* lightning = SSLightning::getInstance();

    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        mStats.mGuarded = true;
        return;
    }

    mStats = DrawStats();
    mStats.mShaderOk = gSSLightningProgram.isComplete();
    mStats.mStrikes = (S32)lightning->strikes().size();
    if (lightning->strikes().empty()) return;
    if (!mStats.mShaderOk) return;

    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugStrikeMarkers", false);

    bool anything = false;
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mChannelBrightness > 0.001f || s.mCharge > 0.001f || s.mFlash > 0.001f
            || (markers && s.mT < 0.f))
        {
            anything = true;
            break;
        }
    }
    if (!anything) return;

    static LLCachedControl<std::string> tex_setting(gSavedSettings, "SSAtmoLightningTexture", "");
    const std::string tex_str = tex_setting;
    const LLUUID tex_id(tex_str);
    if (tex_id.notNull() && (mTextureRef.isNull() || mTexture != tex_id))
    {
        mTexture = tex_id;
        mTextureRef = LLViewerTextureManager::getFetchedTexture(
            tex_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (mTextureRef.notNull()) mTextureRef->setNoDelete();
    }
    if (tex_id.isNull()) { mTexture.setNull(); mTextureRef = NULL; }

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 now = (F32)LLFrameTimer::getElapsedSeconds();

    gSSLightningProgram.bind();

    {
        static LLStaticHashedString s_squash("ss_squash");
        static LLStaticHashedString s_cam("ss_cam_pos");
        SSVolCloud* vol_squash = SSVolCloud::getInstance();
        gSSLightningProgram.uniform3f(s_squash, vol_squash->squashKnee(), vol_squash->squashCap(), vol_squash->virtualRadius());
        gSSLightningProgram.uniform3fv(s_cam, 1, cam.mV);
    }

    static LLStaticHashedString s_use_tex("ss_use_tex");
    static LLStaticHashedString s_radial("ss_radial");
    const bool textured = mTextureRef.notNull() && mTextureRef->hasGLTexture();
    if (textured)
    {
        gGL.getTexUnit(0)->bind(mTextureRef);
        mTextureRef->addTextureStats(512.f * 512.f);
    }
    else
    {
        gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
    }
    gSSLightningProgram.uniform1f(s_use_tex, textured ? 1.f : 0.f);
    gSSLightningProgram.uniform1f(s_radial, 0.f);

    LLGLDisable cull(GL_CULL_FACE);

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);

    gGL.setColorMask(true, true);

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.4f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);

    static LLCachedControl<F32> occl_setting(gSavedSettings, "SSAtmoLightningOcclusion", 0.85f);
    const F32 occ_strength = llclamp((F32)occl_setting, 0.f, 1.f);

    const LLColor3 CORE_COLOR = SSAtmoMagic::getInstance()->lightningCoreColor();
    const LLColor3 GLOW_COLOR = SSAtmoMagic::getInstance()->lightningColor();

    const bool sparks_on = SSAtmoMagic::getInstance()->lightningSparks();

    gGL.begin(LLRender::TRIANGLES);

    for (const SSStrike& strike : lightning->strikes())
    {
        if (strike.mChannelBrightness > 0.001f) mStats.mBright++;
        if (!strikeOnScreen(strike)) { mStats.mOffScreen++; continue; }

        if (strike.mChannelBrightness > 0.001f && !strike.mChannel.empty())
        {
            static LLCachedControl<F32> ribbon_drift(gSavedSettings, "SSAtmoLightningRibbonDrift", 3.f);
            const LLVector3 wind = SSAtmoMagic::getInstance()->windXY();

            SSVolCloud* vol = SSVolCloud::getInstance();
            if (occ_strength > 0.f && !vol->empty()
                && (now - strike.mOccAt > 0.25f
                    || (cam - strike.mOccCam).magVecSquared() > 16.f))
            {
                for (const SSStrikeNode& node : strike.mChannel)
                {
                    node.mOcc = vol->transmittance(cam, node.mPos, occ_strength);
                }
                strike.mOccAt = now;
                strike.mOccCam = cam;
            }
            const bool occluding = occ_strength > 0.f && !vol->empty();

            const F32 dist_scale = llmax(1.f, strike.mDistanceM / 1000.f);

            for (S32 k = 0; k < strike.mStrokeCount; ++k)
            {
                const F32 b = strike.mStrokeBright[k] * strike.mIntensity;
                if (b <= 0.012f) continue;

                const LLVector3 off = wind
                    * (strike.mStrokeAt[k] * llclamp((F32)ribbon_drift, 0.f, 5.f));

                for (const SSStrikeNode& node : strike.mChannel)
                {
                    if (node.mParent < 0) continue;

                    if (node.mReachedAt > strike.mLeaderProgress) continue;

                    const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];

                    const F32 occ = occluding ? (node.mOcc + parent.mOcc) * 0.5f : 1.f;
                    if (occ < 0.01f) continue;

                    const LLVector3 pa = parent.mPos + off;
                    const LLVector3 pb = node.mPos + off;

                    const F32 wa = parent.mWidth * CORE_WIDTH_M * dist_scale;
                    const F32 wb = node.mWidth * CORE_WIDTH_M * dist_scale;

                    const F32 seg_len = (pb - pa).magVec();
                    const F32 v_span = seg_len / llmax(wa * 2.f, 0.001f);

                    const F32 bo = b * occ;
                    gGL.color4f(GLOW_COLOR.mV[0] * bo * 0.22f,
                                GLOW_COLOR.mV[1] * bo * 0.22f,
                                GLOW_COLOR.mV[2] * bo * 0.22f, glow * bo * 0.3f);
                    ribbon(pa, pb, cam,
                           wa * GLOW_WIDTH_MULT, wb * GLOW_WIDTH_MULT, 0.f, v_span);

                    gGL.color4f(CORE_COLOR.mV[0] * bo,
                                CORE_COLOR.mV[1] * bo,
                                CORE_COLOR.mV[2] * bo, glow * bo);
                    ribbon(pa, pb, cam, wa, wb, 0.f, v_span);
                    mStats.mSegments++;
                }
            }
        }

        if (strike.mCharge > 0.001f)
        {
            const U32 seed = (U32)(strike.mFireAt * 271.0);
            const S32 count = 6 + (S32)(strike.mCharge * 10.f);
            const F32 spread = 6.f * (1.2f - strike.mCharge);

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 97u;

                const F32 phase = fmodf(now * (2.f + 3.f * hashUnit(h)) + hashUnit(h ^ 7u) * 7.f, 1.f);
                const F32 flicker = powf(llmax(0.f, sinf(phase * F_TWO_PI)), 6.f);
                if (flicker < 0.05f) continue;

                LLVector3 pos = strike.mGround;
                pos.mV[VX] += (hashUnit(h ^ 11u) - 0.5f) * 2.f * spread;
                pos.mV[VY] += (hashUnit(h ^ 13u) - 0.5f) * 2.f * spread;
                pos.mV[VZ] += hashUnit(h ^ 17u) * spread * 1.5f + 0.4f;

                const F32 a = strike.mCharge * flicker;
                const F32 r = 0.06f + 0.1f * hashUnit(h ^ 19u);

                LLVector3 tip = pos;
                tip.mV[VZ] += r * 6.f;

                gGL.color4f(GLOW_COLOR.mV[0] * a, GLOW_COLOR.mV[1] * a,
                            GLOW_COLOR.mV[2] * a, glow * a * 0.5f);
                ribbon(pos, tip, cam, r, r * 0.4f, 0.f, 1.f);
            }
        }

        if (sparks_on && strike.mKind == STRIKE_GROUND
            && strike.mT >= 0.f && strike.mT < SPARK_LIFE_S)
        {
            const U32 seed = (U32)(strike.mFireAt * 613.0) ^ 0x5a7au;
            const S32 count = (S32)(SPARK_COUNT * (0.4f + strike.mIntensity * 0.6f));

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 131u;

                const F32 t = strike.mT - hashUnit(h ^ 3u) * 0.06f;
                if (t <= 0.f) continue;

                const F32 life = SPARK_LIFE_S * (0.45f + 0.55f * hashUnit(h ^ 5u));
                if (t > life) continue;

                const F32 ang = hashUnit(h) * F_TWO_PI;
                const F32 speed = 3.f + 9.f * hashUnit(h ^ 9u) * (0.5f + strike.mIntensity);
                const F32 rise = 0.5f + 1.6f * hashUnit(h ^ 15u);

                LLVector3 vel(cosf(ang) * speed, sinf(ang) * speed, speed * rise);

                LLVector3 pos = strike.mGround + vel * t;
                pos.mV[VZ] -= 0.5f * SPARK_GRAVITY * t * t;
                if (pos.mV[VZ] < strike.mGround.mV[VZ]) continue;

                vel.mV[VZ] -= SPARK_GRAVITY * t;

                const F32 fade = 1.f - (t / life);
                const F32 a = fade * fade * strike.mIntensity;
                if (a < 0.02f) continue;

                const LLVector3 tail = pos - vel * 0.035f;
                const F32 r = 0.05f + 0.05f * hashUnit(h ^ 21u);

                gGL.color4f(CORE_COLOR.mV[0] * a, CORE_COLOR.mV[1] * a * 0.8f,
                            CORE_COLOR.mV[2] * a * 0.55f, glow * a * 0.5f);
                ribbon(tail, pos, cam, r * 0.35f, r, 0.f, 1.f);
            }
        }
    }

    gGL.end();
    gGL.flush();

    if (markers)
    {
        bool any_pending = false;
        for (const SSStrike& s : lightning->strikes())
        {
            if (s.mT < 0.f && !s.mDone) { any_pending = true; break; }
        }

        if (any_pending)
        {
            LLGLDepthTest marker_depth(GL_FALSE);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, false);
            gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
            gGL.begin(LLRender::TRIANGLES);

            const LLColor4 mark(1.f, 0.15f, 0.85f, 0.75f);

            for (const SSStrike& strike : lightning->strikes())
            {
                if (strike.mT >= 0.f || strike.mDone) continue;
                if (!strikeOnScreen(strike)) continue;

                gGL.color4fv(mark.mV);

                const F32 mw = llmax(0.4f, strike.mDistanceM * 0.004f);

                if (!strike.mChannel.empty())
                {
                    for (const SSStrikeNode& node : strike.mChannel)
                    {
                        if (node.mParent < 0) continue;
                        const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];
                        ribbon(parent.mPos, node.mPos, cam, mw, mw, 0.f, 1.f);
                    }
                }
                else
                {
                    ribbon(strike.mOrigin, strike.mGround, cam, mw, mw, 0.f, 1.f);
                }

                const F32 half = 7.f;
                const LLVector3& g = strike.mGround;
                for (S32 e = 0; e < 4; ++e)
                {
                    const F32 sx = (e == 0 || e == 3) ? -half : half;
                    const F32 sy = (e < 2) ? -half : half;
                    const F32 ex = (e == 0 || e == 1) ? half : -half;
                    const F32 ey = (e == 0 || e == 3) ? -half : half;
                    ribbon(g + LLVector3(sx, sy, 0.5f), g + LLVector3(ex, ey, 0.5f), cam, mw * 0.6f, mw * 0.6f, 0.f, 1.f);
                }
            }

            gGL.end();
            gGL.flush();
        }
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, true);
    gSSLightningProgram.unbind();
}
