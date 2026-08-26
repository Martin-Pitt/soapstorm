/**
 * @file sslightningrender.cpp
 * @brief Atmo Magic lightning - drawing the strikes. See sslightningrender.h.
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
    // How long the impact burst lasts and how fiercely it throws. Sparks are not lightning - they are debris the strike heated - so they live far longer than the discharge and fall under gravity
    // like anything else thrown into the air.
    const F32 SPARK_LIFE_S = 1.4f;
    const F32 SPARK_GRAVITY = 9.8f;
    const S32 SPARK_COUNT = 40;

    // Trunk width at full brightness, in metres. Real channels are a few centimetres; drawn at that width a bolt kilometres away is thinner than a pixel and simply vanishes, so this is a stage
    // width, not a physical one, and distance scales it further below.
    const F32 CORE_WIDTH_M = 2.2f;
    const F32 GLOW_WIDTH_MULT = 7.f;

    // One camera-facing ribbon segment. u runs across the width so the shader can shape the falloff; v runs along the length so an electric texture tiles down the channel.
    void ribbon(const LLVector3& a, const LLVector3& b, const LLVector3& cam,
                F32 width_a, F32 width_b, F32 v0, F32 v1)
    {
        LLVector3 seg = b - a;
        LLVector3 mid = (a + b) * 0.5f;
        LLVector3 view = mid - cam;
        LLVector3 side = seg % view;    // cross: perpendicular to both
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

    // A camera-facing quad, for the flash discs. Built from the view's own right and up rather than from the world's, so it stays square on from any angle including straight overhead - which is
    // exactly where a strike usually is.
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

    // Whether any part of a strike's drawn extent could touch the screen: a sphere over the whole channel, padded by the widest thing it draws (the sky-flash disc). Entirely off screen, the
    // GEOMETRY is skipped - but only the geometry. The scene light and the in-cloud puff glow keep running, because a strike behind the camera still lights the world in front of it, and that lit
    // world (including whatever reflections catch it) is the glimpse you get of a bolt you did not see. Mirrors/probes never see the bolt itself anyway - the gCubeSnapshot guard, and probe update
    // cadence could not catch a 50ms flash regardless.
    // A test point pulled to its DRAWN position - the vertex squash keeps far strikes inside the projection, so the frustum (whose far plane would reject true positions out there) must be asked
    // about where things are drawn.
    LLVector3 drawnPoint(const LLVector3& p, const LLVector3& cam, F32& scale_out)
    {
        const LLVector3 rel = p - cam;
        scale_out = SSVolCloud::getInstance()->squashScale(rel.magVec());
        return cam + rel * scale_out;
    }

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

        // Three padded spheres at the trunk's start, middle and end, not one sphere around everything: bounding by the origin-ground axis culled crawlers crossing mid-view (their reach is
        // sideways), and one sphere over the whole 2.4km crawler kept strikes alive when only the empty air between their arms was on screen. Radii overlap along the run, so any on-screen
        // stretch keeps the strike; partially off-screen is the GPU clipper's job, not this test's.
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

    U32 hash3(U32 x)
    {
        x ^= x >> 16; x *= 0x7feb352du;
        x ^= x >> 15; x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }
    F32 hashUnit(U32 x) { return (F32)(hash3(x) & 0xffffffu) / (F32)0x1000000; }
}

void SSLightningRender::renderFlash()
{
    // The pre-cloud half: sky-flash discs only - see the header for why they stay under the puffs while the ribbons moved above them. Soft discs AT the discharge, the honest shape (the air/cloud
    // around the channel is what lights up), and real geometry means depth-testing for free.
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

    // The far-field squash happens per VERTEX in ssLightningV, from the same ss_squash band the puff field binds - all geometry here is built at TRUE positions and the shader fits it inside the
    // projection, which is what keeps a bolt depth-consistent with the compressed cloud it lives in [interaction: SSVolCloud -> shared squash].
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
            // The flash paints the CHANNEL'S EXTENT, not one hot dot at the origin: several dimmer discs strung along the trunk. One origin disc had two failures at once - it out-shone the bolt
            // that made it (the fork structure was invisible inside its own flash), and it said nothing about shape, so a crawler wandering kilometres across the deck flashed as a stationary
            // point. Additive, so where the trunk doubles back the overlaps rebuild local brightness on their own.
            const F32 radius = llmax(160.f, strike.mDistanceM * 0.08f);
            const S32 DISCS = 5;

            S32 trunk_n = 0;
            for (const SSStrikeNode& node : strike.mChannel) { if (node.mTrunk) ++trunk_n; else break; }

            if (trunk_n > 0)
            {
                for (S32 di = 0; di < DISCS; ++di)
                {
                    const S32 want = (S32)((F32)di / (F32)(DISCS - 1) * (F32)(trunk_n - 1));

                    // Alpha IS the bloom feed, and it gets a small fraction of the discs' visible strength: five stacked additive discs at full glow share out-bloomed the bolt they exist to
                    // announce - the flash should light the sky, not halo it.
                    gGL.color4f(GLOW_COLOR.mV[0] * a * 0.35f, GLOW_COLOR.mV[1] * a * 0.35f,
                                GLOW_COLOR.mV[2] * a * 0.35f, glow * a * 0.08f);
                    billboard(strike.mChannel[(size_t)want].mPos, radius, cam);
                }
            }
        }
        else
        {
            // Sheet has no channel to trace; one disc at the discharge, dimmed from the old full-strength figure that was reading as a floodlight.
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

void SSLightningRender::render()
{
    SSLightning* lightning = SSLightning::getInstance();

    // Same guard as the puff field, for the same reason - see SSVolCloud::render(): this is called from renderGeomPostDeferred, which also runs for HUDs, impostors, shadows and probe captures, and a
    // bolt has no business in any of them.
    // Guarded passes return BEFORE touching mStats: render() runs several times a frame (HUD, impostors, probes after the main view), and the first version of these diagnostics let the HUD
    // pass overwrite the main pass's numbers with an empty guarded set - the overlay then reported "[guarded pass!] 0 segs" forever while the main pass was drawing fine.
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        mStats.mGuarded = true;    // sticky note only; the counts stay the main pass's
        return;
    }

    mStats = DrawStats();
    mStats.mShaderOk = gSSLightningProgram.isComplete();
    mStats.mStrikes = (S32)lightning->strikes().size();
    if (lightning->strikes().empty()) return;
    if (!mStats.mShaderOk) return;

    // Anything at all to draw this frame? Strikes spend most of their life waiting to fire, and the wait must not cost a state change.
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

    // The authored electric-line texture, if there is one; held resident like the puff field's noise maps. Absent, the shader draws its own filament, so the effect never waits on an asset.
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

    // All geometry here is built at TRUE positions; ssLightningV squashes far vertices into the projection with the same ss_squash band the puff field uses (draw distance governs what the sim
    // sends, not how far a viewer-side effect may draw) - one mapping, so a bolt inside a compressed far cloud stays inside it in drawn depth [interaction: SSVolCloud -> shared squash].
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

    // No face culling: ribbon quads take their side vector from segment x view, so their winding flips with the segment's direction - a channel walked top-down winds consistently BACKWARDS for a
    // viewer on the ground, and with culling left on (the pipeline's state here) every quad this pass emitted was silently discarded. 184 segments, nothing on screen, under any shader, at any
    // width, depth on or off: that investigation ends here. Camera-facing strips from arbitrary directions are double-sided by nature.
    LLGLDisable cull(GL_CULL_FACE);

    // Additive over the finished scene: a discharge only ever adds light. Depth-tested but not depth-writing, so a hill in front of a distant bolt still hides it while the bolt hides nothing.
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);

    // Alpha writes ON - screen alpha IS the glow buffer (glowExtractF reads col.a) [interaction: glow], and lightning is the one Atmo pass meant to bloom. SSAtmoLightningGlow is the peak of ONE quad; overlaps accumulate, so tune down not up.
    gGL.setColorMask(true, true);

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.4f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);

    // How hard the puff field bites into a bolt behind it - see the header comment and SSVolCloud::transmittance. At 0 the cached values are ignored entirely, not merely left stale.
    static LLCachedControl<F32> occl_setting(gSavedSettings, "SSAtmoLightningOcclusion", 0.85f);
    const F32 occ_strength = llclamp((F32)occl_setting, 0.f, 1.f);

    // Authored per track - see SSAtmoEnvWeather::mLightningColor. The sheath carries the colour; the core is that colour pulled toward white by however much whiteness the track asked for, so the
    // default reads as ordinary white-hot lightning in a violet glow and winding the whiteness down gives a bolt coloured all the way through.
    const LLColor3 CORE_COLOR = SSAtmoMagic::getInstance()->lightningCoreColor();
    const LLColor3 GLOW_COLOR = SSAtmoMagic::getInstance()->lightningColor();

    // Sparks are the track's call, like the gathering charge - see SSAtmoEnvWeather. Off by default: a bolt does not scatter embers where it lands, whatever films suggest.
    const bool sparks_on = SSAtmoMagic::getInstance()->lightningSparks();

    gGL.begin(LLRender::TRIANGLES);

    for (const SSStrike& strike : lightning->strikes())
    {
        if (strike.mChannelBrightness > 0.001f) mStats.mBright++;
        if (!strikeOnScreen(strike)) { mStats.mOffScreen++; continue; }    // off screen entirely: light effects only

        // ------------------------------------------------- channel ribbons
        // Once per still-glowing stroke, offset by wind * stroke age * drift: coincident in still air, fanned into ribbon lightning in a crosswind [interaction: wind].
        if (strike.mChannelBrightness > 0.001f && !strike.mChannel.empty())
        {
            static LLCachedControl<F32> ribbon_drift(gSavedSettings, "SSAtmoLightningRibbonDrift", 3.f);
            const LLVector3 wind = SSAtmoMagic::getInstance()->windXY();

            // Refresh this strike's per-node transmittance cache on its throttle (see SSStrike::mOccAt). Real node positions, not squashed ones - the field only exists near the camera and the
            // rays to a far strike cross it exactly where the drawn puffs are.
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

            // Distance keeps apparent width from collapsing: past a couple of kilometres the ribbon grows with range so the bolt stays a visible filament rather than dropping below a pixel.
            // Holds the core at roughly 4px minimum at any range - a 2px additive filament over a bright storm deck is invisible, which was the other half of the original invisibility.
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

                    // The leader's crawl: a node the leader has not reached yet does not exist. This is the forking wandering downward before the connection snaps the channel bright.
                    if (node.mReachedAt > strike.mLeaderProgress) continue;

                    const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];

                    // The veil: this segment's share of what survives the puffs in front of it. Both colour and glow alpha scale by it, so a swallowed stretch neither draws nor blooms - the fork
                    // structure fades through thin deck and breaks where the cloud is solid, exactly the reference-photo look.
                    const F32 occ = occluding ? (node.mOcc + parent.mOcc) * 0.5f : 1.f;
                    if (occ < 0.01f) continue;

                    const LLVector3 pa = parent.mPos + off;
                    const LLVector3 pb = node.mPos + off;

                    const F32 wa = parent.mWidth * CORE_WIDTH_M * dist_scale;
                    const F32 wb = node.mWidth * CORE_WIDTH_M * dist_scale;

                    // v tiles by length so an electric texture keeps its scale whatever the segment happens to measure.
                    const F32 seg_len = (pb - pa).magVec();
                    const F32 v_span = seg_len / llmax(wa * 2.f, 0.001f);

                    // The glow sheath first, wide and dim, then the core hot on top of it. Additive, so the order is only clarity. The sheath is already the wide soft part; giving it a full share of
                    // the glow as well would bloom the bolt's halo rather than its channel, which is backwards - the channel is what is hot.
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

        // ------------------------------------------------- gathering charge
        // Sparks crackling in the air where the strike is about to land, only ever non-zero when the track asked for the effect. They tighten toward the attachment point as the moment approaches -
        // scattered wide at the first crackle, converged by the strike.
        if (strike.mCharge > 0.001f)
        {
            const U32 seed = (U32)(strike.mFireAt * 271.0);
            const S32 count = 6 + (S32)(strike.mCharge * 10.f);
            const F32 spread = 6.f * (1.2f - strike.mCharge);

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 97u;

                // Each spark flickers on its own clock; the pow sharpens the flicker into snaps rather than a shimmer.
                const F32 phase = fmodf(now * (2.f + 3.f * hashUnit(h)) + hashUnit(h ^ 7u) * 7.f, 1.f);
                const F32 flicker = powf(llmax(0.f, sinf(phase * F_TWO_PI)), 6.f);
                if (flicker < 0.05f) continue;

                LLVector3 pos = strike.mGround;
                pos.mV[VX] += (hashUnit(h ^ 11u) - 0.5f) * 2.f * spread;
                pos.mV[VY] += (hashUnit(h ^ 13u) - 0.5f) * 2.f * spread;
                pos.mV[VZ] += hashUnit(h ^ 17u) * spread * 1.5f + 0.4f;

                const F32 a = strike.mCharge * flicker;
                const F32 r = 0.06f + 0.1f * hashUnit(h ^ 19u);

                // A vertical micro-ribbon reads as a static arc where a dot reads as a firefly.
                LLVector3 tip = pos;
                tip.mV[VZ] += r * 6.f;

                // Sparks glow too, but at a fraction: they are embers, not a discharge, and a crowd of them at the channel's own glow would out-bloom the bolt they are announcing.
                gGL.color4f(GLOW_COLOR.mV[0] * a, GLOW_COLOR.mV[1] * a,
                            GLOW_COLOR.mV[2] * a, glow * a * 0.5f);
                ribbon(pos, tip, cam, r, r * 0.4f, 0.f, 1.f);
            }
        }

        // ------------------------------------------------- impact sparks
        // Impact sparks: ballistic and purely procedural from (fireAt, index) hashes - the whole flight is a function of strike age, nothing stored, so a spark cannot desync from its strike.
        if (sparks_on && strike.mKind == STRIKE_GROUND
            && strike.mT >= 0.f && strike.mT < SPARK_LIFE_S)
        {
            const U32 seed = (U32)(strike.mFireAt * 613.0) ^ 0x5a7au;
            const S32 count = (S32)(SPARK_COUNT * (0.4f + strike.mIntensity * 0.6f));

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 131u;

                // Staggered launches, so the burst has a front rather than every ember leaving in the same instant.
                const F32 t = strike.mT - hashUnit(h ^ 3u) * 0.06f;
                if (t <= 0.f) continue;

                const F32 life = SPARK_LIFE_S * (0.45f + 0.55f * hashUnit(h ^ 5u));
                if (t > life) continue;

                const F32 ang = hashUnit(h) * F_TWO_PI;
                const F32 speed = 3.f + 9.f * hashUnit(h ^ 9u) * (0.5f + strike.mIntensity);
                const F32 rise = 0.5f + 1.6f * hashUnit(h ^ 15u);

                LLVector3 vel(cosf(ang) * speed, sinf(ang) * speed, speed * rise);

                // Position and velocity from the same ballistic solution, so the streak below always lies along the direction of travel - including on the way back down.
                LLVector3 pos = strike.mGround + vel * t;
                pos.mV[VZ] -= 0.5f * SPARK_GRAVITY * t * t;
                if (pos.mV[VZ] < strike.mGround.mV[VZ]) continue;   // landed

                vel.mV[VZ] -= SPARK_GRAVITY * t;

                // Fading, and dimming as it cools - an ember does both.
                const F32 fade = 1.f - (t / life);
                const F32 a = fade * fade * strike.mIntensity;
                if (a < 0.02f) continue;

                // Drawn as a short streak along its own motion, which is what a fast ember reads as; a dot at this speed reads as a firefly.
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

    // ------------------------------------------------ pending-strike markers
    // Debug: where the next strike will land and the bolt it will take, drawn while it is still counting down (the countdown text half lives in SSLightning::advance). Alpha-blended, NOT additive:
    // additive magenta over a white storm deck is invisible, and being seen against white cloud is this marker's whole job.
    if (markers)
    {
        bool any_pending = false;
        for (const SSStrike& s : lightning->strikes())
        {
            if (s.mT < 0.f && !s.mDone) { any_pending = true; break; }
        }

        if (any_pending)
        {
            // (The invisible-bolt investigation once ran these through gDebugProgram as an A/B against our shader; the culprit was face culling, the shader was acquitted, and the markers are
            // back on it for the soft-edged ribbon look.)
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

                // ANGULAR width, not metres: a 0.3m ribbon at 1.5km is a fraction of a pixel and rasterises to nothing - the original invisibility had this as half its cause. Scaled so the
                // marker holds roughly 4-6px at any range.
                const F32 mw = llmax(0.4f, strike.mDistanceM * 0.004f);

                // The path the bolt will take: its full channel, faint, ahead of time - or origin to ground for a sheet strike with no channel built.
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

                // A thin box around the attachment point, sized to the charge-spark spread so the two agree about where "here" is.
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

// </SS:Nexii>
