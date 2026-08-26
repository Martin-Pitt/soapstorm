/**
 * @file lldrawpoolwlsky.cpp
 * @brief LLDrawPoolWLSky class implementation
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lldrawpoolwlsky.h"

#include "llrendertarget.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "llerror.h"
#include "llface.h"
#include "llimage.h"
#include "llrender.h"
#include "llenvironment.h"
#include "llglslshader.h"
#include "llgl.h"

#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewercamera.h"
#include "pipeline.h"
#include "llsky.h"
#include "llvowlsky.h"
#include "llsettingsvo.h"
#include "llviewercontrol.h"
#include "llagent.h" // <SS:Nexii> for gAgent.getRegion()
#include "ssatmoenvapplier.h" // <SS:Nexii> Atmo Magic celestial billboards

extern bool gCubeSnapshot;

static LLStaticHashedString sCamPosLocal("camPosLocal");
static LLStaticHashedString sCustomAlpha("custom_alpha");
static LLStaticHashedString sRegionOffset("region_offset"); // <SS:Nexii> cloud parallax
static LLStaticHashedString sCloudDrift("ss_cloud_drift"); // <SS:Nexii> wind-driven cloud travel

// <SS:Nexii> Atmo Magic celestial discs - see ssCelestialF.glsl. Every look
// constant lives in the shader; these are the per-body handles.
static LLStaticHashedString sDiscColor("ss_disc_color");
static LLStaticHashedString sBodyDir("ss_body_dir");
static LLStaticHashedString sSunDir("ss_sun_dir");
static LLStaticHashedString sQuadRight("ss_quad_right");
static LLStaticHashedString sQuadUp("ss_quad_up");
static LLStaticHashedString sSunlight("ss_sunlight");
static LLStaticHashedString sEmissive("ss_emissive");
static LLStaticHashedString sPhaseShaded("ss_phase_shaded");
static LLStaticHashedString sDaylight("ss_daylight");
static LLStaticHashedString sFaceRot("ss_face_rot");

// <SS:Nexii> The dome cloud layer's virtual ALTITUDE, metres - an authored dome parameter now (SSAtmoEnvCloudDome::mHeightM), with the old derivation kept behind its Auto flag. Both the parallax
// (cloudsV) and the disc occlusion (ssCelestialF) scale by it - one authority, or the two slide apart, which is why it is resolved in the applier and only read here.
static LLStaticHashedString sCloudAltM("ss_cloud_alt_m");
// </SS:Nexii>

// Whether Atmo Magic should draw the discs at all. Its own shader replaces
// the stock sun and moon ones outright while it owns the sky, so a stock
// environment goes through untouched code.
static bool ss_atmo_discs_active()
{
    // Whenever the master switch is on, active Atmo environment or not: the additive compositing is the point. A stock alpha-blended disc REPLACES the sky behind it, and against a hot authored
    // sunset glow that replacement is DARKER than the surroundings - a dark cutout where the sun should be. Added, a disc can only ever brighten. Slot data falls back to a plain emissive sun and
    // a phase-shaded moon at the bind sites when no environment is driving the sky.
    static LLCachedControl<bool> ss_atmo(gSavedSettings, "SSAtmoEnabled", false);
    return ss_atmo && gSSCelestialProgram.isComplete();
}

// The quad axes LLVOSky builds for a body at `dir`, rebuilt here because the
// fragment shader has to put the sphere back together in the same frame the
// vertices were laid out in (updateHeavenlyBodyGeometry: right = dir x z,
// then up = right x dir).
static void ss_quad_axes(const LLVector3& dir, LLVector3& out_right, LLVector3& out_up)
{
    out_right = dir % LLVector3::z_axis;
    if (out_right.normalize() < 0.001f) out_right = LLVector3::x_axis;
    out_up = out_right % dir;
    out_up.normalize();
}

// One body's worth of uniforms.
static void ss_bind_disc(const LLColor4& tint, const LLVector3& body_dir,
                         const LLVector3& sun_dir, F32 sunlight,
                         bool emissive, bool phase_shaded)
{
    LLVector3 right, up;
    ss_quad_axes(body_dir, right, up);

    gSSCelestialProgram.uniform4fv(sDiscColor, 1, tint.mV);
    gSSCelestialProgram.uniform3fv(sBodyDir, 1, body_dir.mV);
    gSSCelestialProgram.uniform3fv(sSunDir, 1, sun_dir.mV);
    gSSCelestialProgram.uniform3fv(sQuadRight, 1, right.mV);
    gSSCelestialProgram.uniform3fv(sQuadUp, 1, up.mV);
    gSSCelestialProgram.uniform1f(sSunlight, sunlight);
    gSSCelestialProgram.uniform1f(sEmissive, emissive ? 1.f : 0.f);
    gSSCelestialProgram.uniform1f(sPhaseShaded, phase_shaded ? 1.f : 0.f);

    // How far this body's face is turned, relative to the quad it is drawn
    // on: the parallactic angle.
    //
    // The quad is a billboard whose up axis points at the zenith (see
    // ss_quad_axes), so without this the art is pinned to the HORIZON - the
    // maria in the same place on screen at moonrise as at moonset, while
    // the terminator sweeps across them because that is computed from the
    // sun's real direction. Half the face fixed to the ground and half to
    // the sky, which is a worse answer than either alone.
    //
    // A tidally locked body does keep one face turned toward its planet, so
    // the billboard is right about WHICH face. What it cannot know is the
    // roll: a real moon's north points at the celestial pole, not at the
    // observer's zenith, and the angle between those two is nothing at
    // culmination and tens of degrees near rise and set at temperate
    // latitudes. That rotation is why a crescent sits like a bowl low in
    // the sky and tips over as it climbs.
    //
    // Measured about the view direction, from the quad's own up axis to the
    // pole-ward one.
    const LLVector3 pole = SSAtmoEnvApplier::instance().observerPole();
    LLVector3 pole_tangent = pole - body_dir * (pole * body_dir);
    F32 cos_q = 1.f;
    F32 sin_q = 0.f;
    if (pole_tangent.normalize() > 0.001f)
    {
        cos_q = pole_tangent * up;
        sin_q = (up % pole_tangent) * body_dir;
    }
    gSSCelestialProgram.uniform2f(sFaceRot, cos_q, sin_q);

    // How much daylight the OBSERVER is standing in - nothing to do with
    // this body, which is why it is the sun's own elevation rather than
    // anything in the arguments. The shader uses it to fade earthshine out;
    // see SS_EARTHSHINE.
    //
    // From the sky's sun rather than the applier's resolved slot so that it
    // matches the sky actually being drawn even mid-transition, when the two
    // can briefly disagree.
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 sun_alt = sky ? sky->getSunDirection().mV[VZ] : 0.f;

    // Across the same band twilight happens in: full night by about six
    // degrees below the horizon, full day by about nine above.
    const F32 daylight = llclamp((sun_alt + 0.1f) / 0.25f, 0.f, 1.f);
    gSSCelestialProgram.uniform1f(sDaylight, daylight * daylight * (3.f - 2.f * daylight));


    // No airlight uniform any more. Estimating the haze over a disc from a
    // single sky-wide colour was always going to be wrong somewhere - it
    // was far too dark against a bright daytime sky, leaving the moon
    // crisp and pasted-on - and there is no need to estimate it at all now
    // that the disc is added to the sky the dome has already drawn.
}


static LLGLSLShader* cloud_shader = NULL;
static LLGLSLShader* sky_shader   = NULL;
static LLGLSLShader* sun_shader   = NULL;
static LLGLSLShader* moon_shader  = NULL;

static float sStarTime;

LLDrawPoolWLSky::LLDrawPoolWLSky(void) :
    LLDrawPool(POOL_WL_SKY)
{
}

LLDrawPoolWLSky::~LLDrawPoolWLSky()
{
}

LLViewerTexture *LLDrawPoolWLSky::getDebugTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::beginDeferredPass(S32 pass)
{
    sky_shader = &gDeferredWLSkyProgram;
    cloud_shader = &gDeferredWLCloudProgram;

    sun_shader = &gDeferredWLSunProgram;

    moon_shader = &gDeferredWLMoonProgram;
}

void LLDrawPoolWLSky::endDeferredPass(S32 pass)
{
    sky_shader   = nullptr;
    cloud_shader = nullptr;
    sun_shader   = nullptr;
    moon_shader  = nullptr;

    // clear the depth buffer so haze shaders can use unwritten depth as a mask
    glClear(GL_DEPTH_BUFFER_BIT);
}

void LLDrawPoolWLSky::renderDome(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader * shader,
                                 F32 scale) const
{
    llassert_always(NULL != shader);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();

    //chop off translation
    if (LLPipeline::sReflectionRender && camPosLocal.mV[2] > 256.f)
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], 256.f-camPosLocal.mV[2]*0.5f);
    }
    else
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    }


    // the windlight sky dome works most conveniently in a coordinate system
    // where Y is up, so permute our basis vectors accordingly.
    gGL.rotatef(120.f, 1.f / F_SQRT3, 1.f / F_SQRT3, 1.f / F_SQRT3);

    gGL.scalef(scale, scale, scale);

    gGL.translatef(0.f,-camHeightLocal, 0.f);

    // Draw WL Sky
    shader->uniform3f(sCamPosLocal, 0.f, camHeightLocal, 0.f);

    gSky.mVOWLSkyp->drawDome();

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

extern LLPointer<LLImageGL> gEXRImage;

static bool use_hdri_sky()
{
    static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
    static LLCachedControl<bool> irradiance_only(gSavedSettings, "RenderHDRIIrradianceOnly", false);

    return gCubeSnapshot && (!irradiance_only || !gPipeline.mReflectionMapManager.isRadiancePass()) ? gEXRImage.notNull() : // always use HDRI for reflection probes when available
        gEXRImage.notNull() ? hdri_split > 0.f : // fallback to EEP sky when split screen is zero
        false; // no HDRI available, always use EEP sky

}

void LLDrawPoolWLSky::renderSkyHazeDeferred(const LLVector3& camPosLocal, F32 camHeightLocal) const
{
    if (!gSky.mVOSkyp)
    {
        return;
    }

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY))
    {
        if (use_hdri_sky())
        {
            sky_shader = &gEnvironmentMapProgram;
            sky_shader->bind();
            S32 idx = sky_shader->enableTexture(LLShaderMgr::ENVIRONMENT_MAP);
            if (idx > -1)
            {
                gGL.getTexUnit(idx)->bind(gEXRImage);
            }

            static LLCachedControl<F32> hdri_exposure(gSavedSettings, "RenderHDRIExposure", 0.0f);
            static LLCachedControl<F32> hdri_rotation(gSavedSettings, "RenderHDRIRotation", 0.f);
            static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
            static LLStaticHashedString hdri_split_screen("hdri_split_screen");

            LLMatrix3 rot;
            rot.setRot(0.f, hdri_rotation*DEG_TO_RAD, 0.f);

            sky_shader->uniform1f(LLShaderMgr::SKY_HDR_SCALE, powf(2.f, hdri_exposure));
            sky_shader->uniformMatrix3fv(LLShaderMgr::DEFERRED_ENV_MAT, 1, GL_FALSE, (F32*) rot.mMatrix);
            sky_shader->uniform1f(hdri_split_screen, gCubeSnapshot ? 1.f : hdri_split);
        }
        else
        {
            sky_shader->bind();
        }

        LLGLSPipelineDepthTestSkyBox sky(true, true);

        sky_shader->uniform1i(LLShaderMgr::CUBE_SNAPSHOT, gCubeSnapshot ? 1 : 0);

        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLViewerTexture* rainbow_tex = gSky.mVOSkyp->getRainbowTex();
        LLViewerTexture* halo_tex  = gSky.mVOSkyp->getHaloTex();

        sky_shader->bindTexture(LLShaderMgr::RAINBOW_MAP, rainbow_tex);
        sky_shader->bindTexture(LLShaderMgr::HALO_MAP,  halo_tex);

        F32 moisture_level  = (float)psky->getSkyMoistureLevel();
        F32 droplet_radius  = (float)psky->getSkyDropletRadius();
        F32 ice_level       = (float)psky->getSkyIceLevel();

        // hobble halos and rainbows when there's no light source to generate them
        if (!psky->getIsSunUp() && !psky->getIsMoonUp())
        {
            moisture_level = 0.0f;
            ice_level      = 0.0f;
        }

        sky_shader->uniform1f(LLShaderMgr::MOISTURE_LEVEL, moisture_level);
        sky_shader->uniform1f(LLShaderMgr::DROPLET_RADIUS, droplet_radius);
        sky_shader->uniform1f(LLShaderMgr::ICE_LEVEL, ice_level);

        sky_shader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        sky_shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, psky->getIsSunUp() ? 1 : 0);

        /// Render the skydome
        renderDome(origin, camHeightLocal, sky_shader);

        sky_shader->unbind();
    }
}

void LLDrawPoolWLSky::renderStarsDeferred(const LLVector3& camPosLocal) const
{
    if (!gSky.mVOSkyp || use_hdri_sky())
    {
        return;
    }

    LLGLSPipelineBlendSkyBox gls_sky(true, false);

    gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

    F32 star_alpha = LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f;

    // If start_brightness is not set, exit
    if(star_alpha < 0.001f)
    {
        LL_DEBUGS("SKY") << "star_brightness below threshold." << LL_ENDL;
        return;
    }

    gDeferredStarProgram.bind();

    LLViewerTexture* tex_a = gSky.mVOSkyp->getBloomTex();
    LLViewerTexture* tex_b = gSky.mVOSkyp->getBloomTexNext();

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();

    if (tex_a && (!tex_b || (tex_a == tex_b)))
    {
        // Bind current and next sun textures
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b && !tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_b);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b != tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->bind(tex_b);
    }

    gGL.pushMatrix();
    gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    gGL.rotatef(gFrameTimeSeconds*0.01f, 0.f, 0.f, 1.f);
    gDeferredStarProgram.uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

    if (LLPipeline::sReflectionRender)
    {
        star_alpha = 1.0f;
    }
    gDeferredStarProgram.uniform1f(sCustomAlpha, star_alpha);

    sStarTime = (F32)LLFrameTimer::getElapsedSeconds() * 0.5f;

    gDeferredStarProgram.uniform1f(LLShaderMgr::WATER_TIME, sStarTime);

    gSky.mVOWLSkyp->drawStars();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

    gDeferredStarProgram.unbind();

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderSkyCloudsDeferred(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader* cloudshader) const
{
    if (use_hdri_sky())
    {
        return;
    }

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS) && gSky.mVOSkyp && gSky.mVOSkyp->getCloudNoiseTex())
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLGLSPipelineBlendSkyBox pipeline(true, true);

        cloudshader->bind();

        LLPointer<LLViewerTexture> cloud_noise      = gSky.mVOSkyp->getCloudNoiseTex();
        LLPointer<LLViewerTexture> cloud_noise_next = gSky.mVOSkyp->getCloudNoiseTexNext();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        F32 cloud_variance = psky ? (F32)psky->getCloudVariance() : 0.0f;
        F32 blend_factor   = psky ? (F32)psky->getBlendFactor() : 0.0f;

        if (psky->getCloudScrollRate().isExactlyZero())
        {
            blend_factor = 0.f;
        }

        // if we even have sun disc textures to work with...
        if (cloud_noise || cloud_noise_next)
        {
            if (cloud_noise && (!cloud_noise_next || (cloud_noise == cloud_noise_next)))
            {
                // Bind current and next sun textures
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next && !cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise_next, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next != cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP_NEXT, cloud_noise_next, LLTexUnit::TT_TEXTURE);
            }
        }

        cloudshader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
        cloudshader->uniform1f(LLShaderMgr::CLOUD_VARIANCE, cloud_variance);
        cloudshader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        // <SS:Nexii> Region-relative cloud parallax (doc/atmo_magic_cloud_parallax.md). Gated on an ACTIVE Atmo environment, not just the compiled-in SS_ATMO define: the master toggle bakes the
        // shader variant, but an enabled-yet-idle viewer falling back to a plain EEP sky must leave it pixel-stock - zeros make both additive terms vanish. (The drift below already self-gates:
        // it is zero unless an Atmo environment is driving the sky.)
        const bool atmo_env_active = SSAtmoEnvApplier::instance().isActive();
        LLViewerRegion* region       = gAgent.getRegion();
        F32             region_width = region ? region->getWidth() : REGION_WIDTH_METERS;
        F32             region_off_x = atmo_env_active ? (camPosLocal.mV[VX] - region_width * 0.5f) : 0.f;
        F32             region_off_y = atmo_env_active ? (camPosLocal.mV[VY] - region_width * 0.5f) : 0.f;
        cloudshader->uniform2f(sRegionOffset, region_off_x, region_off_y);

        // ...and how far the deck itself has travelled on the wind. Zero
        // unless an Atmo Magic environment is driving the sky, which is also
        // the only thing that knows what the wind is doing.
        const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();
        cloudshader->uniform2f(sCloudDrift, drift.mV[0], drift.mV[1]);

        // The layer's own altitude for the parallax scale - see SSAtmoEnvApplier::cloudDomeAltitudeMetres.
        cloudshader->uniform1f(sCloudAltM, SSAtmoEnvApplier::instance().cloudDomeAltitudeMetres());
        // </SS:Nexii>

        /// Render the skydome
        // <SS:Nexii> The cloud layer is drawn a little nearer than the haze
        // backdrop behind it: 0.3325 of the dome radius rather than the
        // stock 0.333, which is about 4988m instead of 4995m - twelve
        // metres, a fifth of one percent.
        //
        // Deliberately tiny. What actually stopped the sun flickering
        // through cloud was giving the celestial discs the far end of the
        // depth range (see ssCelestialV.glsl); this is only headroom on top
        // of that, and headroom does not need to cost apparent cloud size
        // or parallax. Earlier passes here took it to 0.22 and then 0.32,
        // both of which moved the deck visibly to buy margin that was not
        // in short supply.
        renderDome(camPosLocal, camHeightLocal, cloudshader, 0.3325f);

        cloudshader->unbind();

        // <SS:Nexii> The volumetric layer used to be drawn here, on top of
        // the dome. It is now a late translucent pass instead - see
        // LLPipeline::renderGeomPostDeferred. Drawn in the sky pass it could
        // only ever be part of the backdrop: everything rendered afterwards,
        // water included, painted straight over it, and it had no scene
        // depth to soften itself against.

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
    }
}

void LLDrawPoolWLSky::renderHeavenlyBodies()
{
    if (!gSky.mVOSkyp || use_hdri_sky()) return;

    LLGLSPipelineBlendSkyBox gls_skybox(true, true); // SL-14113 we need moon to write to depth to clip stars behind

    // <SS:Nexii> Atmo Magic's discs are ADDED to the sky rather than
    // composited over it - the whole atmosphere is in front of a celestial
    // body, so the sky already drawn at those pixels is exactly the airlight
    // over the disc. See the note in ssCelestialF.glsl.
    //
    // Only when Atmo Magic owns the sky: the stock discs are built to be
    // composited and would come out as bright smears added to it.
    const bool ss_additive_discs = ss_atmo_discs_active();
    if (ss_additive_discs)
    {
        gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);
    }

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    // <SS:Nexii> Celestial quads onto a true camera-centred shell.
    //
    // LLVOSky::updateHeavenlyBodyGeometry bakes mCameraPosAgent (the sky
    // drawable's position, i.e. the camera) into the sun and moon face
    // vertices, and this pass then translated by the camera origin as
    // well - so every celestial quad sat at
    //     camera + cameraPosAgent + dir * HEAVENLY_BODY_DIST
    // instead of camera + dir * HEAVENLY_BODY_DIST. On the ground the
    // extra term is small enough to pass for correct; at altitude it is
    // not. In a 3000m skybox it threw the moon roughly 3000m out along
    // the camera vector, putting the quad within a few hundred metres of
    // the cloud dome (dome radius 15000 scaled by 0.333 in renderDome,
    // so ~5000m out) - two surfaces at nearly the same depth, which is
    // exactly the moon/cloud z-fighting this fixes.
    //
    // Subtracting the baked offset here rather than removing it from
    // LLVOSky keeps the fix to the draw site: the faces' own vertex data
    // is shared with the reflection and glow paths, which expect it in
    // agent space. Our own billboards below add the same term for the
    // same reason, so all three land on one shell.
    // (gSky.mVOSkyp is non-null here - this function returns early above.)
    const LLVector3 shell_origin = origin - gSky.mVOSkyp->getCameraPosAgent();
    gGL.pushMatrix();
    gGL.translatef(shell_origin.mV[0], shell_origin.mV[1], shell_origin.mV[2]);
    // </SS:Nexii>

    LLFace * face = gSky.mVOSkyp->mFace[LLVOSky::FACE_SUN];

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();
    bool can_use_vertex_shaders = gPipeline.shadersLoaded();
    bool can_use_windlight_shaders = gPipeline.canUseWindLightShaders();


    if (gSky.mVOSkyp->getSun().getDraw() && face && face->getGeomCount())
    {
        LLPointer<LLViewerTexture> tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLPointer<LLViewerTexture> tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        // if we even have sun disc textures to work with...
        if (tex_a || tex_b)
        {
            // <SS:Nexii> Atmo Magic draws its own discs - see
            // ss_atmo_discs_active - so a stock environment goes through the
            // untouched path below and this one never runs for it.
            if (ss_atmo_discs_active())
            {
                SSAtmoEnvApplier& atmo = SSAtmoEnvApplier::instance();

                gSSCelestialProgram.bind();
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP,
                    tex_a ? tex_a : tex_b, LLTexUnit::TT_TEXTURE);

                LLSettingsSky::ptr_t atmo_sky = LLEnvironment::instance().getCurrentSky();
                const LLVector3 body_dir = atmo_sky ? atmo_sky->getSunDirection()
                                                    : LLVector3::z_axis;

                // A star in the sun slot lights itself. A body that is not
                // emissive gets the phase and eclipse treatment instead,
                // which is what a moon standing in as someone's sun should
                // look like.
                // White, NOT getSun().getInterpColor().
                //
                // Neither stock disc shader reads the colour uniform the sky
                // pass sets - sunDiscF writes its texture out verbatim and
                // moonF only scales by moon brightness - so nothing has ever
                // depended on that colour being sensible, and it is not: it
                // comes through as black, which multiplied straight into a
                // shader that DOES read it turned both discs black.
                //
                // The disc's own art carries its colour, and how bright it
                // is comes from the light reaching it. A tint on top would
                // be a third opinion; white leaves the other two alone.
                // Plain-sky fallbacks: with no Atmo environment resolving the slots, the sun slot is simply a star.
                const bool applier_on = atmo.isActive();
                ss_bind_disc(LLColor4::white,
                             body_dir, applier_on ? atmo.sunSlotSunDirection() : body_dir,
                             applier_on ? atmo.sunSlotSunlight() : 1.f,
                             applier_on ? atmo.sunSlotEmissive() : true,
                             applier_on ? atmo.sunSlotPhaseShaded() : false);

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.unbind();
            }
            // if and only if we have a texture defined, render the sun disc
            else if (can_use_vertex_shaders && can_use_windlight_shaders)
            {
                sun_shader->bind();

                if (tex_a && (!tex_b || (tex_a == tex_b)))
                {
                    // Bind current and next sun textures
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b && !tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b != tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    sun_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                }

                LLColor4 color(gSky.mVOSkyp->getSun().getInterpColor());
                sun_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
                sun_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

                sun_shader->unbind();
            }
        }
    }

    face = gSky.mVOSkyp->mFace[LLVOSky::FACE_MOON];

    if (gSky.mVOSkyp->getMoon().getDraw() && face && face->getTexture(LLRender::DIFFUSE_MAP) && face->getGeomCount() && moon_shader)
    {
        LLViewerTexture* tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLViewerTexture* tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        LLColor4 color(gSky.mVOSkyp->getMoon().getInterpColor());

        if (can_use_vertex_shaders && can_use_windlight_shaders && (tex_a || tex_b))
        {
            LLSettingsSky::ptr_t moon_sky = LLEnvironment::instance().getCurrentSky();

            // <SS:Nexii> Atmo Magic's own disc shader, when it owns the sky.
            if (ss_atmo_discs_active() && moon_sky)
            {
                SSAtmoEnvApplier& atmo = SSAtmoEnvApplier::instance();

                gSSCelestialProgram.bind();
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP,
                    tex_a ? tex_a : tex_b, LLTexUnit::TT_TEXTURE);

                // White - see the note on the sun above.
                // Plain-sky fallbacks: an ordinary moon - reflective, phase-shaded, lit by the sky's own sun.
                const bool moon_applier_on = atmo.isActive();
                ss_bind_disc(LLColor4::white, moon_sky->getMoonDirection(),
                             moon_applier_on ? atmo.moonSunDirection() : moon_sky->getSunDirection(),
                             moon_applier_on ? atmo.moonSlotSunlight() : 1.f,
                             moon_applier_on ? atmo.moonSlotEmissive() : false,
                             moon_applier_on ? atmo.moonSlotPhaseShaded() : true);

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.unbind();
            }
            // </SS:Nexii>
            else
            {
            moon_shader->bind();

            if (tex_a && (!tex_b || (tex_a == tex_b)))
            {
                // Bind current and next sun textures
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b && !tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b != tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //moon_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
            }

            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

            F32 moon_brightness = (float)psky->getMoonBrightness();

            moon_shader->uniform1f(LLShaderMgr::MOON_BRIGHTNESS, moon_brightness);
            moon_shader->uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, gSky.mVOSkyp->getMoon().getColor().mV);
            moon_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
            //moon_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
            moon_shader->uniform3fv(LLShaderMgr::DEFERRED_MOON_DIR, 1, psky->getMoonDirection().mV); // shader: moon_dir

            face->renderIndexed();

            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

            moon_shader->unbind();
            }
        }
    }

    // <SS:Nexii> Atmo Magic: the active track's non-emitter celestial
    // bodies as camera-facing textured quads - the design doc Planetary
    // section's "quad/billboard only for v1". The applier publishes an
    // empty vector whenever it is inactive, so this whole block costs one
    // emptiness check when Atmo Magic is off. Drawn after the sun and moon
    // so their quads (and the moon's star-clipping depth write, SL-14113)
    // always land first. The moon shader is reused wholesale: it is the
    // one shader in this pass whose every uniform is per-body suppliable
    // (moon_dir is just the body's direction), and it buys the same
    // horizon fade, moon-brightness scaling, transparent-texel discard
    // and star-clipping depth layer the moon itself gets - a sun-shader
    // body would sit on the stars' depth layer instead and have them
    // poke through it.
    const std::vector<SSAtmoEnvBillboard>& billboards =
        SSAtmoEnvApplier::instance().celestialBillboards();
    if (!billboards.empty() && moon_shader
        && can_use_vertex_shaders && can_use_windlight_shaders
        && gSSCelestialProgram.isComplete())
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        // Atmo Magic's own disc shader. The billboards are its own bodies, so
        // unlike the two light slots there is no stock path to fall back to -
        // the moon shader used to stand in here, which meant borrowing its
        // moon-specific horizon fade and brightness and then fighting both.
        gSSCelestialProgram.bind();

        // White, for the same reason the two slots use it - see the note
        // there. The body's art carries its colour and the light reaching it
        // carries its brightness.
        const LLColor4 bb_color(LLColor4::white);

        // Sizing matches LLVOSky::updateHeavenlyBodyGeometry's chain for
        // the moon (dist * factor * disk radius * disc scale, plus its
        // near-horizon enlargement), with the disc scale coming from the
        // same diameter mapping the applier feeds setMoonScale - so a
        // billboard body and the moon at equal angular diameter render at
        // equal size, through their whole arc.
        const F32 disk_radius = gSky.mVOSkyp->getMoon().getDiskRadius();

        for (const SSAtmoEnvBillboard& body : billboards)
        {
            const LLVector3& dir = body.mDirection;

            // Camera-facing frame: horizon-aligned right, then up within
            // the quad's plane, both perpendicular to the view direction.
            // Near zenith/nadir the horizontal cross degenerates; a
            // body's roll is arbitrary (it is a disc), so any fixed
            // horizontal axis serves there.
            LLVector3 bb_right = dir % LLVector3::z_axis;
            if (bb_right.normalize() < 0.001f)
            {
                bb_right = LLVector3::x_axis;
            }
            LLVector3 bb_up = bb_right % dir;
            bb_up.normalize();

            const F32 enlargm_factor = 1.f - dir.mV[VZ];
            const F32 horiz_enlargement = 1.f + enlargm_factor * 0.3f;
            const F32 vert_enlargement = 1.f + enlargm_factor * 0.2f;
            const F32 half_size =
                SSAtmoEnvApplier::celestialDiscScale(body.mAngularDiameterDeg)
                * HEAVENLY_BODY_DIST * HEAVENLY_BODY_FACTOR * disk_radius;

            // Land on the SAME shell the sun/moon quads occupy, which
            // means carrying the same mCameraPosAgent term their face
            // vertices carry: this pass now subtracts it once from the
            // whole matrix (see shell_origin above), so adding it here
            // cancels out and every celestial quad ends up exactly
            // HEAVENLY_BODY_DIST from the camera. Dropping it instead
            // would put billboards a whole camera-position vector away
            // from the sun and moon, and they would parallax against
            // both as the camera moved.
            const LLVector3 center = dir * HEAVENLY_BODY_DIST
                + gSky.mVOSkyp->getCameraPosAgent();
            const LLVector3 half_right = (horiz_enlargement * half_size) * bb_right;
            const LLVector3 half_up = (vert_enlargement * half_size) * bb_up;

            // A body without a custom texture still shows as a
            // recognisable disc, chosen by the BODY's kind rather than
            // any slot: sun-kind bodies get the stock sun disc, the rest
            // the stock moon disc. The sun's stand-in is the blank-sun
            // ASSET - GetDefaultSunTextureId() is null, meaning "EEP's
            // built-in sun rendering", which a billboard cannot draw.
            const LLUUID tex_id = body.mTexture.notNull()
                ? body.mTexture
                : body.mIsSun ? LLSettingsSky::GetBlankSunTextureId()
                              : LLSettingsSky::GetDefaultMoonTextureId();
            LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
                tex_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
            if (!tex)
            {
                continue;
            }
            // Keep the fetcher feeding full resolution, the way
            // LLVOSky::updateTextures() does for the sun and moon; while
            // still loading, binding draws whatever placeholder the
            // fetched texture currently holds.
            tex->addTextureStats(static_cast<F32>(MAX_IMAGE_AREA));

            // Everything the disc shader needs about this body: where it is,
            // where its own star is, how much of that star's light reaches
            // it, and whether it lights itself or takes a phase.
            //
            // Brightness is a consequence of those rather than an authored
            // dial - see SSAtmoEnvCelestialBody - and every look constant
            // (emissive gain, earthshine, terminator softness) lives in the
            // shader, so there is no magic number on this side at all.
            ss_bind_disc(bb_color, dir, body.mSunDirection, body.mSunlight,
                         body.mEmissive, body.mPhaseShaded);
            gSSCelestialProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, tex,
                                            LLTexUnit::TT_TEXTURE);

            gGL.begin(LLRender::TRIANGLE_STRIP);
            gGL.texCoord2f(0.f, 1.f);
            gGL.vertex3fv((center - half_right + half_up).mV);
            gGL.texCoord2f(0.f, 0.f);
            gGL.vertex3fv((center - half_right - half_up).mV);
            gGL.texCoord2f(1.f, 1.f);
            gGL.vertex3fv((center + half_right + half_up).mV);
            gGL.texCoord2f(1.f, 0.f);
            gGL.vertex3fv((center + half_right - half_up).mV);
            gGL.end();
            // Flush while this body's texture and moon_dir are still
            // bound - the next iteration rebinds both.
            gGL.flush();
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gSSCelestialProgram.unbind();
    }
    // </SS:Nexii>

    // <SS:Nexii> Back to ordinary compositing for whatever draws next.
    if (ss_additive_discs)
    {
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_WL_SKY);
    if (!gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY) || gSky.mVOSkyp.isNull())
    {
        return;
    }

    // TODO: remove gSky.mVOSkyp and fold sun/moon into LLVOWLSky
    gSky.mVOSkyp->updateGeometry(gSky.mVOSkyp->mDrawable);

    const F32 camHeightLocal = LLEnvironment::instance().getCamHeight();

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders())
    {
        renderSkyHazeDeferred(origin, camHeightLocal);
        renderHeavenlyBodies();
        if (!gCubeSnapshot)
        {
            renderStarsDeferred(origin);
        }

        if (!gCubeSnapshot || gPipeline.mReflectionMapManager.isRadiancePass()) // don't draw clouds in irradiance maps to avoid popping
        {
            renderSkyCloudsDeferred(origin, camHeightLocal, cloud_shader);
        }
    }
}



LLViewerTexture* LLDrawPoolWLSky::getTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::resetDrawOrders()
{
}

//static
void LLDrawPoolWLSky::cleanupGL()
{
}

//static
void LLDrawPoolWLSky::restoreGL()
{
}
