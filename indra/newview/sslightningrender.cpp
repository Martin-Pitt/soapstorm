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
#include "llworld.h"
#include "llsurface.h"
#include "llrender.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    const F32 SPARK_LIFE_S = 1.4f;
    const F32 SPARK_GRAVITY = 9.8f;
    const S32 SPARK_COUNT = 40;

    // <SS:Nexii> The recorded ground-strike finish: the last 10-20m of a ground bolt turns
    // from the authored bolt colour to a yellow-red amber and flares much hotter than the
    // mid-air channel as it comes down to the attachment. </SS:Nexii>
    const LLColor3 AMBER_COLOR(1.f, 0.42f, 0.07f);
    const F32 AMBER_ZONE_M = 18.f;
    const F32 AMBER_BOOST = 2.2f;

    // <SS:Nexii> The decay's dissolve-to-sparks: after a return stroke peaks, the beam does not
    // dim as one ribbon - each segment keeps its own random extinction threshold, so the
    // channel breaks apart into chunks and finally individual sparks. The instant a segment
    // pops, it leaves a dying ember spark behind. Timing lives in SSDissolve (sslightning.h),
    // shared with the model so the strike's lifetime always covers the show. </SS:Nexii>

    // <SS:Nexii> Secondary sparks: what a primary impact spark throws off where its arc comes
    // down on a surface - smaller, dimmer, shorter-lived, no further generations. </SS:Nexii>
    const F32 SECONDARY_LIFE_S = 0.6f;

    // The pre-strike charge field: sparks live a fraction of a second, then respawn
    // elsewhere (the duty cycle is at least double the life) until the strike fires.
    const F32 CHARGE_SPARK_LIFE_S = 0.36f;
    const S32 CHARGE_SPARK_MAX = 90;

    // Corona discharge / St. Elmo's fire: ionized air reads blue-violet whatever colour
    // the bolt itself is authored as.
    const LLColor3 CORONA_COLOR(0.5f, 0.36f, 1.f);

    const F32 CORE_WIDTH_M = 2.2f;
    const F32 GLOW_WIDTH_MULT = 7.f;

    // One camera-faced quad segment - every bolt, spark and marker is built from these.
    // side_a/side_b, when given, replace the quad's own perpendicular at that end: a joint
    // hands the same merged side vector to both quads sharing a turn's corner so their
    // corner edges coincide and the pieces read as one continuous line instead of two
    // butted quads gapping and doubling around the bend.
    void ribbon(const LLVector3& a, const LLVector3& b, const LLVector3& cam,
                F32 width_a, F32 width_b, F32 v0, F32 v1,
                const LLVector3* side_a = nullptr, const LLVector3* side_b = nullptr)
    {
        LLVector3 seg = b - a;
        LLVector3 mid = (a + b) * 0.5f;
        LLVector3 view = mid - cam;
        LLVector3 side = seg % view;
        if (side.normalize() <= 0.f) return;

        const LLVector3& sa = side_a ? *side_a : side;
        const LLVector3& sb = side_b ? *side_b : side;

        const LLVector3 a0 = a - sa * width_a;
        const LLVector3 a1 = a + sa * width_a;
        const LLVector3 b0 = b - sb * width_b;
        const LLVector3 b1 = b + sb * width_b;

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

        LLVector3 ref = (llabs(to_cam.mV[VZ]) > 0.9f)
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

// Draws every live strike: layered core and glow ribbons per return stroke, the gathering-charge
// spark field with its corona haze, impact sparks, debug markers.
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

    // The charge field leans into its own physics: air breakdown is violet-blue regardless
    // of the bolt's authored colour, so the sparks and haze read electric even in a red storm.
    const LLColor3 SPARK_COLOR = CORONA_COLOR * 0.7f + GLOW_COLOR * 0.3f;
    const LLColor3 CORONA_TINT = CORONA_COLOR * 0.85f + GLOW_COLOR * 0.15f;

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

            // The strike's stable per-node hash seed, so the dissolve thresholds and ember
            // sparks hold the same pattern every frame and every return stroke.
            const U32 strike_seed = (U32)(strike.mFireAt * 3571.0) ^ 0x11feu;

            static LLCachedControl<F32> amber_setting(gSavedSettings, "SSAtmoLightningGroundAmber", 1.f);
            const F32 amber_str = llclamp((F32)amber_setting, 0.f, 2.f);

            static LLCachedControl<F32> dissolve_setting(gSavedSettings, "SSAtmoLightningDissolve", 1.f);
            const F32 dissolve = llclamp((F32)dissolve_setting, 0.f, 2.f);

            // Merged corners. Each node with exactly one child is a turn, not a fork, so the
            // quad ending there and the quad starting there share one side vector - the
            // average of the two segments' own sides - and both quads' corner edges at the
            // node land on the same two points. The overlap wedge on the inside of the turn
            // and the notch on the outside both collapse into a single shared edge, which is
            // all a vector line's corner join ever was. Forks (two or more children) and
            // tips keep plain butts: there is no single "other side" to merge with.
            const S32 node_n = (S32)strike.mChannel.size();
            std::vector<S32> sole_child((size_t)node_n, -1);
            for (S32 i = 0; i < node_n; ++i)
            {
                const S32 p = strike.mChannel[(size_t)i].mParent;
                if (p >= 0)
                {
                    sole_child[(size_t)p] = (sole_child[(size_t)p] == -1) ? i : -2;
                }
            }
            std::vector<LLVector3> joint_side((size_t)node_n, LLVector3::zero);
            for (S32 i = 0; i < node_n; ++i)
            {
                const S32 c = sole_child[(size_t)i];
                if (c < 0) continue;
                if (strike.mChannel[(size_t)i].mParent < 0) continue;

                // Both halves of the joint must be on screen for it to exist - the growing
                // leader's leading quad stays plain-butt until its continuation arrives.
                if (strike.mChannel[(size_t)i].mReachedAt > strike.mLeaderProgress
                    || strike.mChannel[(size_t)c].mReachedAt > strike.mLeaderProgress) continue;

                const LLVector3& p = strike.mChannel[(size_t)i].mPos;
                const LLVector3 view = p - cam;
                LLVector3 s_in = (p - strike.mChannel[(size_t)strike.mChannel[(size_t)i].mParent].mPos) % view;
                LLVector3 s_out = (strike.mChannel[(size_t)c].mPos - p) % view;
                s_in.normalize();
                s_out.normalize();
                LLVector3 merged = s_in + s_out;
                if (merged.magVecSquared() < 1.e-10f)
                {
                    merged = s_in;
                }
                else
                {
                    merged.normalize();
                }
                joint_side[(size_t)i] = merged;
            }

            for (S32 k = 0; k < strike.mStrokeCount; ++k)
            {
                const F32 b = strike.mStrokeBright[k] * strike.mIntensity;
                if (b <= 0.012f && dissolve <= 0.f) continue;

                const LLVector3 off = wind
                    * (strike.mStrokeAt[k] * llclamp((F32)ribbon_drift, 0.f, 5.f));

                const F32 since = llmax(0.f, strike.mT - strike.mStrokeAt[k]);

                for (S32 i = 0; i < node_n; ++i)
                {
                    const SSStrikeNode& node = strike.mChannel[(size_t)i];

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

                    // The dissolve-to-sparks mask: each segment keeps its own random extinction
                    // threshold over the stroke's tail - the alpha mask turned up until that
                    // piece is fully discarded - so the beam breaks apart into chunks and then
                    // individual sparks instead of dimming as one ribbon. The instant a segment
                    // pops it leaves a dying ember spark behind at its centre.
                    F32 seg = 1.f;
                    F32 ember = 0.f;
                    if (dissolve > 0.f)
                    {
                        const F32 thr = hashUnit(hash3(strike_seed ^ (U32)i * 1313u));
                        // The instant this segment's mask is turned up: its own random
                        // threshold inside the dissolve window, sped by the setting.
                        const F32 pop_at = SSDissolve::LAG_S
                            + thr * (SSDissolve::SPAN_S / dissolve);
                        const F32 age = since - pop_at;
                        if (age >= 0.f)
                        {
                            seg = llclamp(1.f - age * 90.f, 0.f, 1.f);
                            // The ember lingers its own short life after the pop - a dying
                            // spark where the piece was, not just a snapped-off edge.
                            ember = llclamp(1.f - age / (SSDissolve::EMBER_S / dissolve), 0.f, 1.f);
                        }
                    }
                    if (seg <= 0.f && ember <= 0.f) continue;

                    // The amber ground flash: the last 10-20m of a ground bolt turns from the
                    // authored colour to a yellow-red amber and flares much hotter down there.
                    F32 amber = 0.f;
                    F32 amber_boost = 1.f;
                    if (strike.mKind == STRIKE_GROUND)
                    {
                        const F32 h = (pa.mV[VZ] + pb.mV[VZ]) * 0.5f - strike.mGround.mV[VZ];
                        amber = (1.f - llclamp(h / AMBER_ZONE_M, 0.f, 1.f)) * amber_str;
                        if (amber > 0.f) amber_boost = 1.f + amber * AMBER_BOOST;
                    }
                    LLColor3 core_col = CORE_COLOR;
                    LLColor3 glow_col = GLOW_COLOR;
                    if (amber > 0.f)
                    {
                        const F32 na = 1.f - amber;
                        core_col.mV[0] = CORE_COLOR.mV[0] * na + AMBER_COLOR.mV[0] * amber;
                        core_col.mV[1] = CORE_COLOR.mV[1] * na + AMBER_COLOR.mV[1] * amber;
                        core_col.mV[2] = CORE_COLOR.mV[2] * na + AMBER_COLOR.mV[2] * amber;
                        glow_col.mV[0] = GLOW_COLOR.mV[0] * na + AMBER_COLOR.mV[0] * amber;
                        glow_col.mV[1] = GLOW_COLOR.mV[1] * na + AMBER_COLOR.mV[1] * amber;
                        glow_col.mV[2] = GLOW_COLOR.mV[2] * na + AMBER_COLOR.mV[2] * amber;
                    }

                    // The joint side lives on the shared node, so both quads of a turn read
                    // the same vector from opposite ends of their common corner.
                    const LLVector3* start_side = joint_side[(size_t)node.mParent].magVecSquared() > 0.f
                        ? &joint_side[(size_t)node.mParent] : nullptr;
                    const LLVector3* end_side = joint_side[(size_t)i].magVecSquared() > 0.f
                        ? &joint_side[(size_t)i] : nullptr;

                    const F32 bo = b * occ * seg * amber_boost;
                    if (bo > 0.012f)
                    {
                        gGL.color4f(glow_col.mV[0] * bo * 0.22f,
                                    glow_col.mV[1] * bo * 0.22f,
                                    glow_col.mV[2] * bo * 0.22f, glow * bo * 0.3f);
                        ribbon(pa, pb, cam,
                               wa * GLOW_WIDTH_MULT, wb * GLOW_WIDTH_MULT, 0.f, v_span,
                               start_side, end_side);

                        gGL.color4f(core_col.mV[0] * bo,
                                    core_col.mV[1] * bo,
                                    core_col.mV[2] * bo, glow * bo);
                        ribbon(pa, pb, cam, wa, wb, 0.f, v_span, start_side, end_side);
                        mStats.mSegments++;
                    }

                    // The dying spark a popped segment leaves behind - bright on its own, not
                    // riding the stroke's decayed glow, so the falling-apart tail keeps reading
                    // as individual sparks until the channel is fully discarded.
                    if (ember > 0.03f)
                    {
                        const U32 eh = hash3(strike_seed ^ (U32)i * 977u);
                        const F32 ea = hashUnit(eh ^ 7u) * F_TWO_PI;
                        const F32 et = (hashUnit(eh ^ 13u) - 0.5f) * 1.3f;
                        const LLVector3 edir(cosf(ea) * cosf(et), sinf(ea) * cosf(et), sinf(et));
                        const F32 elen = llmax(wa, wb) * (1.5f + 3.5f * hashUnit(eh ^ 19u));
                        const F32 er = llmax(wa, wb) * (0.2f + 0.15f * hashUnit(eh ^ 23u));
                        const F32 eb = strike.mIntensity * ember * (0.30f + 0.25f * hashUnit(eh ^ 29u));
                        if (eb > 0.02f)
                        {
                            const LLVector3 emid = (pa + pb) * 0.5f;
                            const LLVector3 e0 = emid - edir * elen * 0.5f;
                            const LLVector3 e1 = emid + edir * elen * 0.5f;
                            gGL.color4f(core_col.mV[0] * eb, core_col.mV[1] * eb * 0.95f,
                                        core_col.mV[2] * eb * 0.9f, glow * eb);
                            ribbon(e0, e1, cam, er * 0.4f, er * 0.7f, 0.f, 1.f);
                        }
                    }
                }
            }
        }

        if (strike.mCharge > 0.001f)
        {
            const U32 seed = (U32)(strike.mFireAt * 271.0);
            const F32 spread = 6.f * (1.2f - strike.mCharge);

            // The ionizing field gathering around the attachment: a swarm of tiny sparks
            // that each live a fraction of a second, sprint through a small erratic spiral
            // arc and vanish. Entirely stateless - a spark's whole life is hashed out of
            // (strike, index, respawn count) and the clock, so there is no per-spark state
            // to tick and the field cannot desync from its strike.
            const S32 count = (S32)(CHARGE_SPARK_MAX * strike.mCharge
                                    * (0.4f + 0.6f * strike.mIntensity));

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 71u;

                // Very short life inside a longer cycle: the spark pops in, is gone for a
                // while, reappears - the field shimmers by popping, not by glowing steadily.
                const F32 life = CHARGE_SPARK_LIFE_S * (0.45f + 0.55f * hashUnit(h ^ 3u));
                const F32 duty = 2.f + 2.5f * hashUnit(h ^ 5u);
                const F32 offset = hashUnit(h ^ 7u);

                const F32 cycle = life * duty;
                const F32 age = fmodf(now / cycle + offset, 1.f) * cycle;
                if (age > life) continue;

                // The respawn count folds into the hash, so every reappearance lands the
                // arc somewhere new instead of retracing the same loop forever.
                const U32 g = hash3(h ^ ((U32)(now / cycle + offset) * 31u + 97u));

                // Birthplace: a disc around the attachment that contracts as the moment
                // approaches, and denser near the ground than up in the air.
                const F32 ang0 = hashUnit(g) * F_TWO_PI;
                const F32 rad = spread * sqrtf(hashUnit(g ^ 11u));
                const F32 field_h = 0.8f + spread * 0.8f;
                const LLVector3 spawn = strike.mGround
                    + LLVector3(cosf(ang0) * rad, sinf(ang0) * rad,
                                0.3f + field_h * hashUnit(g ^ 13u) * hashUnit(g ^ 13u));

                // The arc's plane: a tilted spiral axis, so sparks climb, dive and
                // corkscrew instead of all circling flat.
                const F32 az = hashUnit(g ^ 17u) * F_TWO_PI;
                const F32 tilt = (hashUnit(g ^ 19u) - 0.5f) * 1.9f;
                const LLVector3 axis(cosf(az) * cosf(tilt), sinf(az) * cosf(tilt), sinf(tilt));
                LLVector3 arc_a = axis % LLVector3::z_axis;
                if (arc_a.normalize() <= 0.f) arc_a = LLVector3::x_axis;
                LLVector3 arc_b = axis % arc_a;
                arc_b.normalize();

                // Snow sway, exaggerated and erratic: the winding angle wobbles and the
                // radius breathes, so no arc is a circle and none read alike. The turn
                // count scales with the life so every spark sweeps at a comparable clip -
                // a shorter life is a tighter sprint, not a vibration. Both sway and
                // breathe run on the clock, not on normalized life-time: a per-life term
                // would oscillate a 160ms spark at 50Hz and alias into jitter.
                const F32 spin = (1.5f + 2.5f * hashUnit(g ^ 23u)) * (life * 4.f) * F_TWO_PI
                    * (hashUnit(g ^ 29u) < 0.5f ? -1.f : 1.f);
                const F32 w1 = hashUnit(g ^ 31u) * F_TWO_PI;
                const F32 r_arc = 0.1f + 0.35f * hashUnit(g ^ 37u);
                const F32 sway_hz = 4.f + 5.f * hashUnit(g ^ 41u);
                const F32 sway_amp = 0.7f + 0.9f * hashUnit(g ^ 43u);
                const F32 breathe = 0.4f + 0.6f * hashUnit(g ^ 47u);
                const F32 breathe_hz = 2.f + 2.f * hashUnit(g ^ 51u);
                const LLVector3 drift = axis * (0.5f + 0.8f * hashUnit(g ^ 53u))
                    + LLVector3(0.f, 0.f, 0.2f + 0.8f * hashUnit(g ^ 59u));

                auto sparkPos = [&](F32 t) -> LLVector3
                {
                    const F32 u = llclamp(t / life, 0.f, 1.f);
                    const F32 a = w1 + u * spin
                        + sinf(t * sway_hz * F_TWO_PI + w1 * 3.f) * sway_amp;
                    const F32 r = r_arc * (1.f - breathe
                        + breathe * (0.5f + 0.5f * sinf(t * breathe_hz * F_TWO_PI + w1)));
                    return spawn + drift * t + (arc_a * cosf(a) + arc_b * sinf(a)) * r;
                };

                // Squared sine envelope: snaps on, snaps off - each spark exists bright
                // and brief, which is what a discharge in air is.
                const F32 env = sinf(age / life * F_PI);
                const F32 a = env * env * strike.mCharge;
                if (a < 0.03f) continue;

                const F32 r = 0.02f + 0.03f * hashUnit(g ^ 61u);

                gGL.color4f(SPARK_COLOR.mV[0] * a, SPARK_COLOR.mV[1] * a,
                            SPARK_COLOR.mV[2] * a, glow * a * 0.6f);
                ribbon(sparkPos(llmax(0.f, age - 0.03f)), sparkPos(age), cam,
                       r * 0.35f, r, 0.f, 1.f);
            }
        }

        if (sparks_on && strike.mKind == STRIKE_GROUND
            && strike.mT >= 0.f && strike.mT < SPARK_LIFE_S + SECONDARY_LIFE_S)
        {
            static LLCachedControl<F32> secondary_setting(gSavedSettings, "SSAtmoLightningSecondarySparks", 1.f);
            const F32 secondary = llclamp((F32)secondary_setting, 0.f, 2.f);

            const U32 seed = (U32)(strike.mFireAt * 613.0) ^ 0x5a7au;
            const S32 count = (S32)(SPARK_COUNT * (0.4f + strike.mIntensity * 0.6f));
            const F32 ground_z = strike.mGround.mV[VZ];

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 131u;

                const F32 t = strike.mT - hashUnit(h ^ 3u) * 0.06f;
                if (t <= 0.f) continue;

                const F32 life = SPARK_LIFE_S * (0.45f + 0.55f * hashUnit(h ^ 5u));

                const F32 ang = hashUnit(h) * F_TWO_PI;
                const F32 speed = 3.f + 9.f * hashUnit(h ^ 9u) * (0.5f + strike.mIntensity);
                const F32 rise = 0.5f + 1.6f * hashUnit(h ^ 15u);

                // The arc's own flight: risen out of the attachment, pulled back by gravity.
                // The parabola returns to its launch height at t_hit - that crossing is the
                // surface impact, and everything after it is the secondary sparks' moment.
                const F32 vel_z0 = speed * rise;
                const F32 t_hit = 2.f * vel_z0 / SPARK_GRAVITY;
                const bool hit_surface = (t_hit > 0.f) && (t_hit < life);
                const F32 sec_age = strike.mT - t_hit;

                // A secondary only outlives its primary: once the primary's own arc is done
                // and no secondary is still alight there is nothing left to draw for this index.
                if (t > life && !(hit_surface && sec_age >= 0.f && sec_age < SECONDARY_LIFE_S))
                {
                    continue;
                }

                LLVector3 vel(cosf(ang) * speed, sinf(ang) * speed, vel_z0);
                LLVector3 pos = strike.mGround + vel * t;
                pos.mV[VZ] -= 0.5f * SPARK_GRAVITY * t * t;
                vel.mV[VZ] = vel_z0 - SPARK_GRAVITY * t;

                // The surface under the spark: the attachment's own height (a deck, a build
                // roof) or the terrain where the arc has travelled to, whichever is higher.
                const F32 surf = llmax(ground_z,
                    LLWorld::getInstance()->resolveLandHeightAgent(pos));
                if (pos.mV[VZ] < surf) continue;

                if (t <= life)
                {
                    const F32 fade = 1.f - (t / life);
                    const F32 a = fade * fade * strike.mIntensity;
                    if (a >= 0.02f)
                    {
                        const LLVector3 tail = pos - vel * 0.035f;
                        const F32 r = 0.05f + 0.05f * hashUnit(h ^ 21u);

                        gGL.color4f(CORE_COLOR.mV[0] * a, CORE_COLOR.mV[1] * a * 0.8f,
                                    CORE_COLOR.mV[2] * a * 0.55f, glow * a * 0.5f);
                        ribbon(tail, pos, cam, r * 0.35f, r, 0.f, 1.f);
                    }
                }

                // The impact: when the primary's parabola comes back down on a surface, it
                // throws off a few smaller, dimmer secondary sparks that scatter, fall under
                // the same gravity, and die with no further generations. Stateless like the
                // primary - every secondary's whole flight is hashed out of its parent's roll.
                if (hit_surface && secondary > 0.f && sec_age >= 0.f && sec_age < SECONDARY_LIFE_S)
                {
                    const LLVector3 hit(
                        strike.mGround.mV[VX] + cosf(ang) * speed * t_hit,
                        strike.mGround.mV[VY] + sinf(ang) * speed * t_hit, 0.f);
                    const F32 surf_hit = llmax(ground_z,
                        LLWorld::getInstance()->resolveLandHeightAgent(hit));

                    const S32 sec_n = 1 + ((hashUnit(h ^ 37u) < 0.55f) ? 1 : 0);
                    const U32 sg = hash3(h ^ 0x5151u);
                    for (S32 j = 0; j < sec_n; ++j)
                    {
                        const U32 sh = hash3(sg + (U32)j * 331u);

                        const F32 s_ang = hashUnit(sh ^ 3u) * F_TWO_PI;
                        const F32 s_spd = (1.5f + 4.5f * hashUnit(sh ^ 7u))
                                        * (0.4f + strike.mIntensity * 0.6f) * secondary;
                        const F32 s_rise = 0.3f + 1.1f * hashUnit(sh ^ 11u);
                        const F32 s_life = SECONDARY_LIFE_S * (0.45f + 0.55f * hashUnit(sh ^ 13u));
                        if (sec_age > s_life) continue;

                        const F32 s_vz = s_spd * s_rise;
                        const LLVector3 s_vel(cosf(s_ang) * s_spd, sinf(s_ang) * s_spd, s_vz);
                        LLVector3 s_pos = hit + s_vel * sec_age;
                        s_pos.mV[VZ] -= 0.5f * SPARK_GRAVITY * sec_age * sec_age;
                        if (s_pos.mV[VZ] < surf_hit) continue;

                        const F32 s_fade = 1.f - sec_age / s_life;
                        const F32 s_a = s_fade * s_fade * strike.mIntensity * 0.55f;
                        if (s_a < 0.02f) continue;

                        const LLVector3 s_tail = s_pos - s_vel * 0.025f;
                        const F32 s_r = 0.03f + 0.03f * hashUnit(sh ^ 17u);

                        gGL.color4f(CORE_COLOR.mV[0] * s_a, CORE_COLOR.mV[1] * s_a * 0.8f,
                                    CORE_COLOR.mV[2] * s_a * 0.55f, glow * s_a * 0.4f);
                        ribbon(s_tail, s_pos, cam, s_r * 0.35f, s_r, 0.f, 1.f);
                    }
                }
            }
        }
    }

    gGL.end();
    gGL.flush();

    // Corona discharge along the spark field: St. Elmo's fire. The same ionized air that
    // throws the sparks also reads as a soft blue-violet haze hanging over the ground,
    // blooming late in the buildup (charge squared) and sputtering slowly - patches of
    // glow breathing on their own clocks rather than one pulsing blob. A second batch
    // with the shader's disc falloff, because a haze wants a radial edge, not a ribbon's
    // line core; back to ribbon mode before the debug markers below.
    bool any_corona = false;
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mCharge > 0.2f) { any_corona = true; break; }
    }
    if (any_corona)
    {
        gSSLightningProgram.uniform1f(s_radial, 1.f);

        gGL.begin(LLRender::TRIANGLES);
        for (const SSStrike& strike : lightning->strikes())
        {
            if (strike.mCharge <= 0.2f) continue;
            if (!strikeOnScreen(strike)) continue;

            const U32 seed = (U32)(strike.mFireAt * 271.0) ^ 0xc0fau;
            const S32 CORONA_DISCS = 4;
            const F32 spread = 6.f * (1.2f - strike.mCharge);

            for (S32 i = 0; i < CORONA_DISCS; ++i)
            {
                const U32 h = seed + (U32)i * 61u;

                const F32 pulse = powf(llmax(0.f, sinf(now * (1.2f + 1.6f * hashUnit(h ^ 7u))
                                                        + hashUnit(h ^ 9u) * 7.f)), 3.f);
                const F32 a = strike.mCharge * strike.mCharge * pulse
                    * (0.4f + 0.6f * strike.mIntensity);
                if (a < 0.02f) continue;

                const F32 ang = hashUnit(h) * F_TWO_PI;
                const F32 rad = spread * (0.25f + 0.55f * hashUnit(h ^ 3u));
                const LLVector3 pos = strike.mGround
                    + LLVector3(cosf(ang) * rad, sinf(ang) * rad,
                                0.25f + 1.2f * hashUnit(h ^ 5u));

                const F32 r = spread * (0.3f + 0.3f * hashUnit(h ^ 11u));

                gGL.color4f(CORONA_TINT.mV[0] * a * 0.5f, CORONA_TINT.mV[1] * a * 0.5f,
                            CORONA_TINT.mV[2] * a * 0.5f, glow * a * 0.25f);
                billboard(pos, r, cam);
            }
        }
        gGL.end();
        gGL.flush();

        gSSLightningProgram.uniform1f(s_radial, 0.f);
    }

    if (markers)
    {
        bool any_pending = false;
        for (const SSStrike& s : lightning->strikes())
        {
            if (s.mT <= -SSLightning::MARKER_HIDE_S && !s.mDone) { any_pending = true; break; }
        }

        if (any_pending)
        {
            LLGLDepthTest marker_depth(GL_FALSE);
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, false);
            gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
            gGL.begin(LLRender::TRIANGLES);

            for (const SSStrike& strike : lightning->strikes())
            {
                if (strike.mT > -SSLightning::MARKER_HIDE_S || strike.mDone) continue;
                if (!strikeOnScreen(strike)) continue;

                // One colour per kind, and only the geometry that kind actually has - a sheet has no
                // channel and no attachment, so the old origin-to-ground line read as a down-strike.
                gGL.color4fv(SSLightning::kindDebugColor(strike.mKind).mV);

                const F32 mw = llmax(0.4f, strike.mDistanceM * 0.004f);

                if (strike.mKind == STRIKE_SHEET)
                {
                    // In-cloud flash: ring the cloud the flash will bloom in.
                    const F32 r = llmax(40.f, strike.mDistanceM * 0.06f);
                    const S32 SIDES = 8;
                    for (S32 e = 0; e < SIDES; ++e)
                    {
                        const F32 a0 = (F32)e / (F32)SIDES * F_TWO_PI;
                        const F32 a1 = (F32)(e + 1) / (F32)SIDES * F_TWO_PI;
                        ribbon(strike.mOrigin + LLVector3(cosf(a0) * r, sinf(a0) * r, 0.f),
                               strike.mOrigin + LLVector3(cosf(a1) * r, sinf(a1) * r, 0.f),
                               cam, mw, mw, 0.f, 1.f);
                    }
                    continue;
                }

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

                // Forks never reach the ground, so the attachment box is ground strikes only.
                if (strike.mKind != STRIKE_GROUND) continue;

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
