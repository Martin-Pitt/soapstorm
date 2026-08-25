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
    // Trunk width at full brightness, in metres. Real channels are a few
    // centimetres; drawn at that width a bolt kilometres away is thinner
    // than a pixel and simply vanishes, so this is a stage width, not a
    // physical one, and distance scales it further below.
    const F32 CORE_WIDTH_M = 1.6f;
    const F32 GLOW_WIDTH_MULT = 5.f;

    // One camera-facing ribbon segment. u runs across the width so the
    // shader can shape the falloff; v runs along the length so an electric
    // texture tiles down the channel.
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
    if (lightning->strikes().empty()) return;
    if (!gSSLightningProgram.isComplete()) return;

    // Same guard as the puff field, for the same reason - see
    // SSVolCloud::render(): this is called from renderGeomPostDeferred,
    // which also runs for HUDs, impostors, shadows and probe captures, and
    // a bolt has no business in any of them.
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }

    // Anything at all to draw this frame? Strikes spend most of their life
    // waiting to fire, and the wait must not cost a state change.
    bool anything = false;
    for (const SSStrike& s : lightning->strikes())
    {
        if (s.mChannelBrightness > 0.001f || s.mCharge > 0.001f) { anything = true; break; }
    }
    if (!anything) return;

    // The authored electric-line texture, if there is one; held resident
    // like the puff field's noise maps. Absent, the shader draws its own
    // filament, so the effect never waits on an asset.
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

    static LLStaticHashedString s_use_tex("ss_use_tex");
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

    // Additive over the finished scene: a discharge only ever adds light.
    // Depth-tested but not depth-writing, so a hill in front of a distant
    // bolt still hides it while the bolt hides nothing.
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ADD);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);

    // Alpha writes ON, unlike every other Atmo Magic pass.
    //
    // The screen target's alpha channel IS the glow buffer - glowExtractF
    // reads col.a - so writing alpha here is what puts the bolt into the
    // bloom. The puff field masks alpha off precisely to stay out of it,
    // and this pass was copied from that one, which left the one thing in
    // the whole system that genuinely emits light as the one thing that did
    // not glow.
    //
    // Kept deliberately faint. Alpha is ADDED like everything else in this
    // pass, so it accumulates wherever ribbons overlap - along the trunk,
    // and across the stacked strokes of a ribbon flash. A per-quad value
    // that looks right on its own blooms into a white smear where twenty of
    // them cross, so the dial sets the peak of a single quad and the
    // overlap is allowed to do the rest.
    gGL.setColorMask(true, true);

    static LLCachedControl<F32> glow_setting(gSavedSettings, "SSAtmoLightningGlow", 0.15f);
    const F32 glow = llclamp((F32)glow_setting, 0.f, 1.f);

    // Authored per track - see SSAtmoEnvWeather::mLightningColor. The sheath
    // carries the colour; the core is that colour pulled toward white by
    // however much whiteness the track asked for, so the default reads as
    // ordinary white-hot lightning in a violet glow and winding the
    // whiteness down gives a bolt coloured all the way through.
    const LLColor3 CORE_COLOR = SSAtmoMagic::getInstance()->lightningCoreColor();
    const LLColor3 GLOW_COLOR = SSAtmoMagic::getInstance()->lightningColor();

    gGL.begin(LLRender::TRIANGLES);

    for (const SSStrike& strike : lightning->strikes())
    {
        // ------------------------------------------------- channel ribbons
        //
        // Once per still-glowing stroke, not once per strike. The channel is
        // a column of ionised air and drifts with the wind between strokes,
        // so each stroke draws at an offset of wind x its own age - in still
        // air the copies coincide and nothing changes, in a crosswind they
        // fan out into ribbon lightning. The drift factor is a dial rather
        // than a constant because at 1.0 it is physically honest and at 2-3
        // it is legible from a distance, and both are defensible choices.
        if (strike.mChannelBrightness > 0.001f && !strike.mChannel.empty())
        {
            static LLCachedControl<F32> ribbon_drift(gSavedSettings, "SSAtmoLightningRibbonDrift", 1.f);
            const LLVector3 wind = SSAtmoMagic::getInstance()->windXY();

            // Distance keeps apparent width from collapsing: past a couple
            // of kilometres the ribbon grows with range so the bolt stays a
            // visible filament rather than dropping below a pixel.
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

                    // The leader's crawl: a node the leader has not reached
                    // yet does not exist. This is the forking wandering
                    // downward before the connection snaps the channel
                    // bright.
                    if (node.mReachedAt > strike.mLeaderProgress) continue;

                    const SSStrikeNode& parent = strike.mChannel[(size_t)node.mParent];

                    const LLVector3 pa = parent.mPos + off;
                    const LLVector3 pb = node.mPos + off;

                    const F32 wa = parent.mWidth * CORE_WIDTH_M * dist_scale;
                    const F32 wb = node.mWidth * CORE_WIDTH_M * dist_scale;

                    // v tiles by length so an electric texture keeps its
                    // scale whatever the segment happens to measure.
                    const F32 seg_len = (pb - pa).magVec();
                    const F32 v_span = seg_len / llmax(wa * 2.f, 0.001f);

                    // The glow sheath first, wide and dim, then the core hot
                    // on top of it. Additive, so the order is only clarity.
                    // The sheath is already the wide soft part; giving it a
                    // full share of the glow as well would bloom the bolt's
                    // halo rather than its channel, which is backwards - the
                    // channel is what is hot.
                    gGL.color4f(GLOW_COLOR.mV[0] * b * 0.22f,
                                GLOW_COLOR.mV[1] * b * 0.22f,
                                GLOW_COLOR.mV[2] * b * 0.22f, glow * b * 0.3f);
                    ribbon(pa, pb, cam,
                           wa * GLOW_WIDTH_MULT, wb * GLOW_WIDTH_MULT, 0.f, v_span);

                    gGL.color4f(CORE_COLOR.mV[0] * b,
                                CORE_COLOR.mV[1] * b,
                                CORE_COLOR.mV[2] * b, glow * b);
                    ribbon(pa, pb, cam, wa, wb, 0.f, v_span);
                }
            }
        }

        // ------------------------------------------------- gathering charge
        // Sparks crackling in the air where the strike is about to land,
        // only ever non-zero when the track asked for the effect. They
        // tighten toward the attachment point as the moment approaches -
        // scattered wide at the first crackle, converged by the strike.
        if (strike.mCharge > 0.001f)
        {
            const U32 seed = (U32)(strike.mFireAt * 271.0);
            const S32 count = 6 + (S32)(strike.mCharge * 10.f);
            const F32 spread = 6.f * (1.2f - strike.mCharge);

            for (S32 i = 0; i < count; ++i)
            {
                const U32 h = seed + (U32)i * 97u;

                // Each spark flickers on its own clock; the pow sharpens the
                // flicker into snaps rather than a shimmer.
                const F32 phase = fmodf(now * (2.f + 3.f * hashUnit(h)) + hashUnit(h ^ 7u) * 7.f, 1.f);
                const F32 flicker = powf(llmax(0.f, sinf(phase * F_TWO_PI)), 6.f);
                if (flicker < 0.05f) continue;

                LLVector3 pos = strike.mGround;
                pos.mV[VX] += (hashUnit(h ^ 11u) - 0.5f) * 2.f * spread;
                pos.mV[VY] += (hashUnit(h ^ 13u) - 0.5f) * 2.f * spread;
                pos.mV[VZ] += hashUnit(h ^ 17u) * spread * 1.5f + 0.4f;

                const F32 a = strike.mCharge * flicker;
                const F32 r = 0.06f + 0.1f * hashUnit(h ^ 19u);

                // A vertical micro-ribbon reads as a static arc where a dot
                // reads as a firefly.
                LLVector3 tip = pos;
                tip.mV[VZ] += r * 6.f;

                // Sparks glow too, but at a fraction: they are embers, not
                // a discharge, and a crowd of them at the channel's own glow
                // would out-bloom the bolt they are announcing.
                gGL.color4f(GLOW_COLOR.mV[0] * a, GLOW_COLOR.mV[1] * a,
                            GLOW_COLOR.mV[2] * a, glow * a * 0.5f);
                ribbon(pos, tip, cam, r, r * 0.4f, 0.f, 1.f);
            }
        }
    }

    gGL.end();
    gGL.flush();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, true);
    gSSLightningProgram.unbind();
}

// </SS:Nexii>
