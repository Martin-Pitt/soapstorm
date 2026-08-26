/**
 * @file ssfarsea.cpp
 * @brief See ssfarsea.h.
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

#include "ssfarsea.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "llshadermgr.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "llworld.h"

static const F32 SEA_MIN_RADIUS_M = 6800.f;
static const F32 SEA_MAX_RADIUS_M = 500000.f;
// <SS:Nexii> DIAGNOSTIC BASELINE: no overshoot. The 1.35 existed to push the rim past the tangent distance so the planet-drooped sea closed its own silhouette gap - with the droop removed
// (flat baseline, waterV.glsl) everything past the tangent renders into the top pixel row or two of the drawn wall: pure vertex and fill waste at the rim. Currently 0.9, pulling the rim
// slightly INSIDE the tangent as a concentric-ring experiment (visibly lowers the horizon line a touch). Restore 1.35 in lockstep with the droop. </SS:Nexii>
static const F32 SEA_HORIZON_OVERSHOOT = 0.9f;
static const S32 SEA_HOLE_HALF_CELLS = 32;
static const S32 SEA_FRAME_HALF_CELLS = 64;

// The eye the sea is anchored to - normally the live camera, but freezable in place so the mesh stops following the camera and its drawn shape can be inspected from outside. The frame and
// squash both degenerate BY DESIGN around any eye outside the identity zone, so from an out-of-bounds camera the live-anchored mesh only ever shows the vacated hole around the camera itself.
LLVector3 SSFarSea::anchorEye() const
{
    static LLCachedControl<bool> freeze(gSavedSettings, "SSAtmoDebugFarSeaFreezeEye", false);
    if (freeze)
    {
        if (!mEyeFrozen)
        {
            mFrozenEye = LLViewerCamera::getInstance()->getOrigin();
            mEyeFrozen = true;
        }
        return mFrozenEye;
    }
    mEyeFrozen = false;
    return LLViewerCamera::getInstance()->getOrigin();
}

// The shared squash band plus this frame's rim and planet radius - one formula so stock planes, frame and clouds always agree.
void SSFarSea::band(F32& knee, F32& cap, F32& rim, F32& planet_r) const
{
    cap = MAX_FAR_CLIP * SS_SQUASH_CAP_FRAC;
    knee = cap * SS_SEA_SQUASH_KNEE_FRAC;

    static LLCachedControl<F32> planet_km(gSavedSettings, "SSAtmoSeaPlanetRadiusKm", 6371.f);
    planet_r = llmax(0.f, (F32)planet_km) * 1000.f;
    const F32 eye_h = llmax(anchorEye().mV[2] - LLEnvironment::instance().getWaterHeight(), 2.f);

    rim = SEA_MIN_RADIUS_M;
    if (planet_r > 0.f)
    {
        rim = llclamp(SEA_HORIZON_OVERSHOOT * sqrtf(2.f * planet_r * eye_h), SEA_MIN_RADIUS_M, SEA_MAX_RADIUS_M);
    }
}

// Puts the live squash band on the shader for the STOCK water planes, so seam neighbours move identically in drawn space.
void SSFarSea::bindSquash(LLGLSLShader* shader)
{
    if (!shader)
    {
        return;
    }
    F32 knee = 0.f, cap = 0.f, rim = 0.f, planet_r = 0.f;
    band(knee, cap, rim, planet_r);
    static LLStaticHashedString s_ss_squash("ss_squash");
    shader->uniform3f(s_ss_squash, knee, cap, rim);
    // anchorEye, not the raw camera: with the freeze-eye debug on, the whole eye-relative construction (placement, pull, squash) stays anchored where it was frozen. [interaction: anchorEye]
    shader->uniform3fv(LLShaderMgr::WATER_EYEVEC, 1, anchorEye().mV);
}

// Binds the frame's per-frame uniforms and draws the canonical annulus; zeroes the band on every path out so nothing leaks.
void SSFarSea::render(LLGLSLShader* shader)
{
    F32 knee = 0.f, cap = 0.f, r_sea = 0.f, planet_r = 0.f;
    band(knee, cap, r_sea, planet_r);

    mLastKnee = knee;
    mLastRSea = 0.f;

    if (!shader)
    {
        return;
    }

    static LLStaticHashedString s_ss_squash("ss_squash");
    static LLStaticHashedString s_ss_sea("ss_sea");
    static LLStaticHashedString s_ss_sea_hole("ss_sea_hole");
    auto zero_band = [&shader]()
    {
        shader->uniform3f(s_ss_squash, 0.f, 0.f, 0.f);
        shader->uniform4f(s_ss_sea, 0.f, 0.f, 0.f, 0.f);
    };

    if (knee <= 0.f)
    {
        zero_band();
        return;
    }

    if (mVB.isNull())
    {
        build();
        if (mVB.isNull())
        {
            zero_band();
            return;
        }
    }

    const F32 water_h = LLEnvironment::instance().getWaterHeight();

    F32 hole_min_x = 0.f, hole_min_y = 0.f, hole_max_x = 0.f, hole_max_y = 0.f;
    {
        bool any = false;
        auto grow = [&](const LLVector3& p, const LLVector3& s)
        {
            const F32 x0 = p.mV[0] - s.mV[0] * 0.5f, x1 = p.mV[0] + s.mV[0] * 0.5f;
            const F32 y0 = p.mV[1] - s.mV[1] * 0.5f, y1 = p.mV[1] + s.mV[1] * 0.5f;
            if (!any) { hole_min_x = x0; hole_max_x = x1; hole_min_y = y0; hole_max_y = y1; any = true; }
            else
            {
                hole_min_x = llmin(hole_min_x, x0); hole_max_x = llmax(hole_max_x, x1);
                hole_min_y = llmin(hole_min_y, y0); hole_max_y = llmax(hole_max_y, y1);
            }
        };
        LLWorld* world = LLWorld::getInstance();
        for (LLViewerRegion* regionp : world->getRegionList())
        {
            const F32 w = regionp->getWidth();
            const LLVector3 o = regionp->getOriginAgent();
            grow(LLVector3(o.mV[0] + w * 0.5f, o.mV[1] + w * 0.5f, 0.f), LLVector3(w, w, 0.f));
        }
        for (const LLPointer<LLVOWater>& w : world->holeWaterObjects())
        {
            if (w.notNull() && !w->isDead()) grow(w->getPositionAgent(), w->getScale());
        }
        const LLPointer<LLVOWater>* edges = world->edgeWaterObjects();
        for (S32 i = 0; i < 8; ++i)
        {
            if (edges[i].notNull() && !edges[i]->isDead()) grow(edges[i]->getPositionAgent(), edges[i]->getScale());
        }
    }

    static LLCachedControl<bool> debug_lift(gSavedSettings, "SSAtmoDebugFarSeaLift", false);
    const F32 sea_h = debug_lift ? water_h + 2.f : water_h;

    const F32 half_x = 0.5f * (hole_max_x - hole_min_x);
    const F32 half_y = 0.5f * (hole_max_y - hole_min_y);
    if (half_x < 32.f || half_y < 32.f)
    {
        zero_band();
        return;
    }

    const F32 sea_bound = (F32)SEA_FRAME_HALF_CELLS / (F32)SEA_HOLE_HALF_CELLS;
    const F32 blend0 = llclamp(knee / (sea_bound * llmin(half_x, half_y)), 0.55f, 0.85f);

    mLastRSea = r_sea;

    shader->uniform3f(s_ss_squash, knee, cap, r_sea);
    shader->uniform4f(s_ss_sea, blend0, r_sea, sea_h, planet_r);
    shader->uniform4f(s_ss_sea_hole, hole_min_x, hole_min_y, hole_max_x, hole_max_y);

    mVB->setBuffer();
    mVB->drawRange(LLRender::TRIANGLES, 0, mVertCount - 1, mIndexCount, 0);

    zero_band();
}

// Builds the immutable canonical frame lattice once - all world placement comes from uniforms, so it never rebuilds.
void SSFarSea::build()
{
    const S32 lattice = SEA_FRAME_HALF_CELLS * 2 + 1;
    mVertCount = (U32)(lattice * lattice);

    const F32 cell = 1.f / (F32)SEA_HOLE_HALF_CELLS;
    const S32 apron = SEA_HOLE_HALF_CELLS - 1;
    std::vector<U32> indices;
    for (S32 cy = -SEA_FRAME_HALF_CELLS; cy < SEA_FRAME_HALF_CELLS; ++cy)
    {
        for (S32 cx = -SEA_FRAME_HALF_CELLS; cx < SEA_FRAME_HALF_CELLS; ++cx)
        {
            if (llmax(abs(cx), abs(cx + 1)) <= apron && llmax(abs(cy), abs(cy + 1)) <= apron)
            {
                continue;
            }
            const U32 v00 = (U32)((cy + SEA_FRAME_HALF_CELLS) * lattice + (cx + SEA_FRAME_HALF_CELLS));
            const U32 v10 = v00 + 1;
            const U32 v01 = v00 + (U32)lattice;
            const U32 v11 = v01 + 1;
            indices.push_back(v00); indices.push_back(v10); indices.push_back(v11);
            indices.push_back(v00); indices.push_back(v11); indices.push_back(v01);
        }
    }
    mIndexCount = (U32)indices.size();

    mVB = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0);
    if (!mVB->allocateBuffer(mVertCount, mIndexCount * 2))
    {
        mVB = nullptr;
        mVertCount = 0;
        mIndexCount = 0;
        return;
    }

    LLStrider<LLVector3> verticesp;
    LLStrider<LLVector3> normalsp;
    LLStrider<LLVector2> texcoordsp;
    mVB->getVertexStrider(verticesp, 0, mVertCount);
    mVB->getNormalStrider(normalsp, 0, mVertCount);
    mVB->getTexCoord0Strider(texcoordsp, 0, mVertCount);

    for (S32 gy = -SEA_FRAME_HALF_CELLS; gy <= SEA_FRAME_HALF_CELLS; ++gy)
    {
        for (S32 gx = -SEA_FRAME_HALF_CELLS; gx <= SEA_FRAME_HALF_CELLS; ++gx)
        {
            *verticesp++ = LLVector3(gx * cell, gy * cell, 0.f);
            *normalsp++ = LLVector3(0.f, 0.f, 1.f);
            *texcoordsp++ = LLVector2(0.f, 0.f);
        }
    }

    mVB->setIndexData(indices.data(), 0, (U32)indices.size());
    mVB->unmapBuffer();
}
