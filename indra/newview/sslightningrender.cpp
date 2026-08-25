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
    const F32 CORE_WIDTH_M = 1.6f;
    const F32 GLOW_WIDTH_MULT = 5.f;

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
    bool strikeOnScreen(const SSStrike& strike)
    {
        const LLVector3 center = (strike.mOrigin + strike.mGround) * 0.5f;
        const F32 flash_r = llmax(350.f, strike.mDistanceM * 0.18f);
        const F32 radius = (strike.mOrigin - strike.mGround).magVec() * 0.5f + flash_r;
        return LLViewerCamera::getInstance()->sphereInFrustum(center, radius) != 0;
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

void SSLightningRender::render()
{
    SSLightning* lightning = SSLightning::getInstance();

    mStats = DrawStats();
    mStats.mShaderOk = gSSLightningProgram.isComplete();
    mStats.mStrikes = (S32)lightning->strikes().size();

    if (lightning->strikes().empty()) return;
    if (!mStats.mShaderOk) return;

    // Same guard as the puff field, for the same reason - see SSVolCloud::render(): this is called from renderGeomPostDeferred, which also runs for HUDs, impostors, shadows and probe captures, and a
    // bolt has no business in any of them.
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        mStats.mGuarded = true;
        return;
    }

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

    // Strikes beyond the projection's far plane are SCALED toward the camera to fit inside it, the same trick the sky dome itself uses - draw distance governs what the sim sends, not how far a
    // viewer-side effect may draw. Scaling position and width by the same factor preserves the angular size exactly, parallax at those distances is nothing, and depth keeps working: nearer scene
    // geometry still occludes the squashed bolt, and the depth-squashed dome stays behind it.
    const F32 squash_limit = LLViewerCamera::getInstance()->getRenderFarPlane() * 0.75f;

    gSSLightningProgram.bind();

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

    // Additive over the finished scene: a discharge only ever adds light. Depth-tested but not depth-writing, so a hill in front of a distant bolt still hides it while the bolt hides nothing.
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);

    // Alpha writes ON - screen alpha IS the glow buffer (glowExtractF reads col.a) [interaction: glow], and lightning is the one Atmo pass meant to bloom. SSAtmoLightningGlow is the peak of ONE quad; overlaps accumulate, so tune down not up.
    gGL.setColorMask(true, true);

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.15f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);

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

        // See squash_limit above. Applied to every vertex and width below through sq()/squash.
        const F32 squash = (strike.mDistanceM > squash_limit) ? squash_limit / strike.mDistanceM : 1.f;
        auto sq = [&](const LLVector3& p) { return cam + (p - cam) * squash; };

        // ------------------------------------------------- channel ribbons
        // Once per still-glowing stroke, offset by wind * stroke age * drift: coincident in still air, fanned into ribbon lightning in a crosswind [interaction: wind].
        if (strike.mChannelBrightness > 0.001f && !strike.mChannel.empty())
        {
            static LLCachedControl<F32> ribbon_drift(gSavedSettings, "SSAtmoLightningRibbonDrift", 1.f);
            const LLVector3 wind = SSAtmoMagic::getInstance()->windXY();

            // Distance keeps apparent width from collapsing: past a couple of kilometres the ribbon grows with range so the bolt stays a visible filament rather than dropping below a pixel.
            const F32 dist_scale = llmax(1.f, strike.mDistanceM / 2000.f);

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

                    const LLVector3 pa = sq(parent.mPos + off);
                    const LLVector3 pb = sq(node.mPos + off);

                    const F32 wa = parent.mWidth * CORE_WIDTH_M * dist_scale * squash;
                    const F32 wb = node.mWidth * CORE_WIDTH_M * dist_scale * squash;

                    // v tiles by length so an electric texture keeps its scale whatever the segment happens to measure.
                    const F32 seg_len = (pb - pa).magVec();
                    const F32 v_span = seg_len / llmax(wa * 2.f, 0.001f);

                    // The glow sheath first, wide and dim, then the core hot on top of it. Additive, so the order is only clarity. The sheath is already the wide soft part; giving it a full share of
                    // the glow as well would bloom the bolt's halo rather than its channel, which is backwards - the channel is what is hot.
                    gGL.color4f(GLOW_COLOR.mV[0] * b * 0.22f,
                                GLOW_COLOR.mV[1] * b * 0.22f,
                                GLOW_COLOR.mV[2] * b * 0.22f, glow * b * 0.3f);
                    ribbon(pa, pb, cam,
                           wa * GLOW_WIDTH_MULT, wb * GLOW_WIDTH_MULT, 0.f, v_span);

                    gGL.color4f(CORE_COLOR.mV[0] * b,
                                CORE_COLOR.mV[1] * b,
                                CORE_COLOR.mV[2] * b, glow * b);
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
                const F32 r = (0.06f + 0.1f * hashUnit(h ^ 19u)) * squash;

                // A vertical micro-ribbon reads as a static arc where a dot reads as a firefly.
                LLVector3 tip = pos;
                tip.mV[VZ] += r * 6.f / llmax(squash, 0.01f);
                pos = sq(pos);
                tip = sq(tip);

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
                const LLVector3 tail = sq(pos - vel * 0.035f);
                pos = sq(pos);
                const F32 r = (0.05f + 0.05f * hashUnit(h ^ 21u)) * squash;

                gGL.color4f(CORE_COLOR.mV[0] * a, CORE_COLOR.mV[1] * a * 0.8f,
                            CORE_COLOR.mV[2] * a * 0.55f, glow * a * 0.5f);
                ribbon(tail, pos, cam, r * 0.35f, r, 0.f, 1.f);
            }
        }
    }

    gGL.end();
    gGL.flush();

    // ------------------------------------------------------- the sky flash
    // Sky flash as a soft disc AT the discharge - the honest shape (the air/cloud around the channel is what lights up), and real geometry means depth-testing for free. Separate begin/end because ss_radial is a uniform.
    {
        bool any_flash = false;
        for (const SSStrike& s : lightning->strikes())
        {
            if (s.mFlash > 0.002f) { any_flash = true; break; }
        }

        if (any_flash)
        {
            gSSLightningProgram.uniform1f(s_radial, 1.f);
            gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
            gGL.begin(LLRender::TRIANGLES);

            for (const SSStrike& strike : lightning->strikes())
            {
                if (strike.mFlash <= 0.002f) continue;
                if (!strikeOnScreen(strike)) continue;

                const F32 squash = (strike.mDistanceM > squash_limit) ? squash_limit / strike.mDistanceM : 1.f;

                // Big enough to be a region of lit sky rather than a lamp, and grown with distance so a far strike keeps a sensible angular size instead of shrinking to a spot.
                const F32 radius = llmax(350.f, strike.mDistanceM * 0.18f) * squash;
                const F32 a = llclamp(strike.mFlash, 0.f, 1.f);

                gGL.color4f(GLOW_COLOR.mV[0] * a, GLOW_COLOR.mV[1] * a,
                            GLOW_COLOR.mV[2] * a, glow * a);
                billboard(cam + (strike.mOrigin - cam) * squash, radius, cam);
            }

            gGL.end();
            gGL.flush();
        }
    }

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
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            gGL.setColorMask(true, false);
            gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
            gGL.begin(LLRender::TRIANGLES);

            const LLColor4 mark(1.f, 0.15f, 0.85f, 0.75f);

            for (const SSStrike& strike : lightning->strikes())
            {
                if (strike.mT >= 0.f || strike.mDone) continue;
                if (!strikeOnScreen(strike)) continue;

                const F32 squash = (strike.mDistanceM > squash_limit) ? squash_limit / strike.mDistanceM : 1.f;
                auto sq = [&](const LLVector3& p) { return cam + (p - cam) * squash; };

                gGL.color4fv(mark.mV);

                // The path the bolt will take: its full channel, faint, ahead of time - or origin to ground for a sheet strike with no channel built.
                if (!strike.mChannel.empty())
                {
                    for (const SSStrikeNode& node : strike.mChannel)
                    {
                        if (node.mParent < 0) continue;
                        const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];
                        ribbon(sq(parent.mPos), sq(node.mPos), cam, 0.3f * squash, 0.3f * squash, 0.f, 1.f);
                    }
                }
                else
                {
                    ribbon(sq(strike.mOrigin), sq(strike.mGround), cam, 0.4f * squash, 0.4f * squash, 0.f, 1.f);
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
                    ribbon(sq(g + LLVector3(sx, sy, 0.5f)), sq(g + LLVector3(ex, ey, 0.5f)), cam, 0.2f * squash, 0.2f * squash, 0.f, 1.f);
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
