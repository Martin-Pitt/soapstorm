/**
 * @file ssscreenfx.cpp
 * @brief Atmo Magic: the screen-space presentation shell - see ssscreenfx.h for the naming note (SSScreenFXPost, not
 *        SSScreenFX: that identifier is the core's namespace, ssscreenfxcore.h, DO NOT EDIT).
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

#include "ssscreenfx.h"

#include "ssatmomagic.h"
#include "sswindflow.h"

#include "llappviewer.h"
#include "llfasttimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llmath.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// One frame of every screen-space source. See ssscreenfx.h.
void SSScreenFXPost::idle(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    LLViewerCamera* cam = LLViewerCamera::getInstance();

    // <SS:Nexii> With Atmo off there is no temperature to jump from: feed the accumulator a constant so stepThermal only ever decays it, never mistaking the system's absence for a thermal event.
    static const F32 DISABLED_BASELINE_C = 20.f;
    const F32 temp_c = atmo->isEnabled() ? atmo->temperatureC() : DISABLED_BASELINE_C;
    SSScreenFX::stepThermal(mThermal, temp_c, dt);

    const F32 sun_up01 = atmo->isEnabled() ? atmo->sunUp() : 0.f;
    const F32 baseline = SSScreenFX::mirageBaseline(temp_c, sun_up01);
    const F32 wind_ms = atmo->isEnabled() ? atmo->windSpeed() : 0.f;
    // <SS:Nexii> Snow does not cool the ground the way rain does, so only a liquid preset suppresses the shimmer.
    const F32 rain01 = (atmo->isEnabled() && !atmo->preset().isGranular()) ? atmo->precipitation() : 0.f;
    const F32 fov_deg = cam->getView() * RAD_TO_DEG;
    mMirage = SSScreenFX::mirageStrength(baseline, mThermal.mShock, wind_ms, rain01, fov_deg);

    const LLVector3 at_axis = cam->getAtAxis();
    const F32 pitch = at_axis.mV[VZ];
    const LLVector3 rain_dir = atmo->isEnabled() ? atmo->rainDirection() : LLVector3(0.f, 0.f, -1.f);
    const F32 rain_align = -(rain_dir * at_axis);
    const F32 intensity = atmo->isEnabled()
        ? atmo->precipitation() * (atmo->preset().isGranular() ? 0.4f : 1.0f)
        : 0.f;
    const F32 demand = SSScreenFX::lensDemand(pitch, rain_align, intensity);

    const bool underwater = cam->cameraUnderWater();
    const bool surfaced = mWasUnderwater && !underwater;
    SSScreenFX::stepLens(mLens, demand, temp_c, underwater, surfaced, dt);
    mWasUnderwater = underwater;

    // <SS:Nexii> Gravity and wind projected into the screen by the rotation part of the current modelview - the same basis the lens shader's drops fall along and the heat shimmer's wind-driven ripple reads.
    const glm::mat3 rot(get_current_modelview());
    const glm::vec3 gravity_view = rot * glm::vec3(0.f, 0.f, -1.f);
    const LLVector3 wind_agent = SSWindFlowMap::getInstance()->sample(cam->getOrigin());
    const glm::vec3 wind_view = rot * glm::vec3(wind_agent.mV[VX], wind_agent.mV[VY], wind_agent.mV[VZ]);

    mHeatWindX = wind_view.x;
    mHeatWindY = wind_view.y;

    F32 dx = 0.f, dy = -1.f;
    SSScreenFX::slideDir(gravity_view.x, gravity_view.y, wind_view.x, wind_view.y, dx, dy);
    mSlideDX = dx;
    mSlideDY = dy;
}

// The heat-shimmer pass. See ssscreenfx.h.
bool SSScreenFXPost::renderHeat(LLRenderTarget* src, LLRenderTarget* dst)
{
    static LLCachedControl<bool> heat_on(gSavedSettings, "SSAtmoHeatShimmer", true);
    if (!heat_on) return false;

    static LLCachedControl<F32> heat_dial(gSavedSettings, "SSAtmoHeatShimmerStrength", 1.f);
    const F32 dialed = mMirage * llmax((F32)heat_dial, 0.f);
    if (dialed < 0.005f) return false;

    if (!gSSPostHeatProgram.isComplete()) return false;

    LL_PROFILE_GPU_ZONE("ss heat shimmer");

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);

    dst->bindTarget();

    gSSPostHeatProgram.bind();
    gSSPostHeatProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_BILINEAR);
    gSSPostHeatProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true);

    gSSPostHeatProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());

    static LLStaticHashedString u_strength("ssHeatStrength");
    static LLStaticHashedString u_time("ssHeatTime");
    static LLStaticHashedString u_wind("ssHeatWind");
    static LLStaticHashedString u_aspect("ssHeatAspect");

    // <SS:Nexii> The core's ceiling is SHOCK_MAX * ZOOM_MAX = 6.0 before the dial; clamped here so a maxed-out dial still reads as a mirage against the shader's 0.004 offset scale, never a smear.
    gSSPostHeatProgram.uniform1f(u_strength, llmin(dialed, 6.f));
    gSSPostHeatProgram.uniform1f(u_time, gFrameTimeSeconds);
    gSSPostHeatProgram.uniform2f(u_wind, mHeatWindX, mHeatWindY);
    const F32 aspect = (dst->getHeight() > 0) ? (F32)dst->getWidth() / (F32)dst->getHeight() : 1.f;
    gSSPostHeatProgram.uniform1f(u_aspect, aspect);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    // <SS:Nexii> unbind() does not unbind textures - src and the deferred depth would otherwise stay bound on their units while src becomes the next pass's draw FBO.
    gSSPostHeatProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
    gSSPostHeatProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    gSSPostHeatProgram.unbind();
    dst->flush();

    return true;
}

// The lens-drops pass. See ssscreenfx.h.
bool SSScreenFXPost::renderLens(LLRenderTarget* src, LLRenderTarget* dst)
{
    static LLCachedControl<bool> lens_on(gSavedSettings, "SSAtmoLensDrops", true);
    if (!lens_on) return false;

    static LLCachedControl<F32> lens_condensation(gSavedSettings, "SSAtmoLensCondensation", 1.f);
    const F32 fog = mLens.mFog * llmax((F32)lens_condensation, 0.f);
    if (mLens.mWet + fog <= 0.01f) return false;

    if (!gSSPostLensProgram.isComplete()) return false;

    LL_PROFILE_GPU_ZONE("ss lens drops");

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);

    dst->bindTarget();

    gSSPostLensProgram.bind();
    gSSPostLensProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_BILINEAR);

    gSSPostLensProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());

    static LLStaticHashedString u_wet("ssLensWet");
    static LLStaticHashedString u_fog("ssLensFog");
    static LLStaticHashedString u_time("ssLensTime");
    static LLStaticHashedString u_slide("ssLensSlide");
    static LLStaticHashedString u_aspect("ssLensAspect");
    static LLStaticHashedString u_scale("ssLensScale");

    static LLCachedControl<F32> lens_scale(gSavedSettings, "SSAtmoLensDropsStrength", 1.f);

    gSSPostLensProgram.uniform1f(u_wet, mLens.mWet);
    gSSPostLensProgram.uniform1f(u_fog, fog);
    gSSPostLensProgram.uniform1f(u_time, gFrameTimeSeconds);
    gSSPostLensProgram.uniform2f(u_slide, mSlideDX, mSlideDY);
    const F32 aspect = (dst->getHeight() > 0) ? (F32)dst->getWidth() / (F32)dst->getHeight() : 1.f;
    gSSPostLensProgram.uniform1f(u_aspect, aspect);
    gSSPostLensProgram.uniform1f(u_scale, llclamp((F32)lens_scale, 0.f, 2.f));

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    // <SS:Nexii> unbind() does not unbind textures - src would otherwise stay bound on its unit while it becomes the next pass's draw FBO.
    gSSPostLensProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
    gSSPostLensProgram.unbind();
    dst->flush();

    return true;
}
