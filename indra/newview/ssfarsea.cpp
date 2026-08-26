/**
 * @file ssfarsea.cpp
 * @brief Atmo Magic: far sea - see ssfarsea.h.
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
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "llworld.h"

// The sea's floor radius: at least the cloud field's virtual reach, so on a flat world (planet radius 0) or at sea level the water still meets the sky at the same horizon the cloud deck does.
static const F32 SEA_MIN_RADIUS_M = 6800.f;
// Radius ceiling: from a 2000m skybox on an Earth-sized planet the true horizon is ~160km out and the squash delivers it happily, but a runaway (huge planet, huge altitude) has to stop while
// single-precision coordinates still have centimetres in them.
static const F32 SEA_MAX_RADIUS_M = 500000.f;
// How far past the true tangent horizon the rim reaches. The rim itself drops BELOW the tangent by construction (the droop in waterV), so the visible horizon is the curve's envelope - the
// silhouette a real sphere shows - rather than the mesh edge, and the mesh edge hides behind it.
static const F32 SEA_HORIZON_OVERSHOOT = 1.35f;
// The frame lattice, in cells per hole HALF EXTENT: the canonical mesh is a square annulus hung on the stock-water union rect (hole tiles + edge patches + regions, ~2048m square), inner edge one
// cell INSIDE the rect - a tucked apron the depth sink hides under stock water, so seam pinholes and the <=0.5m stock origin-rounding mismatch show sunken sea, never void. 32 cells across the
// half extent gives ~32m cells at a 2048m footprint - the same step stock water subdivides to, so cell squareness (undistorted UVs, stock-identical wave interpolation) carries straight across
// the seam. The rect itself world-anchors the lattice: waves cannot swim, and cells only re-step when the rect changes (region cross, neighbour connect) - a one-off, not motion.
static const S32 SEA_HOLE_HALF_CELLS = 32;
// The frame's outer half extent: 4x the hole's, i.e. the identity lattice runs out to ~4km at a 2048m footprint before waterV's blend band (blend start -> the outer square edge) carries it 0% ->
// 100% onto the camera-centred rim circle - the square's edge becomes the round horizon, ~1024 points around it, and the shared squash band does the rest. The 128/32 ratio is baked into waterV's
// chebyshev normalisation as 0.25 - change one, change both. [interaction: waterV.glsl placement block]
static const S32 SEA_FRAME_HALF_CELLS = 128;

void SSFarSea::render(LLGLSLShader* shader)
{
    // The same band formula the cloud field uses (ssvolcloud.cpp update: cap just inside the projection far plane, knee at 80% of it), computed HERE rather than read from SSVolCloud - the cloud
    // field only computes its copy when it builds, so on a clear sky its knee/cap sit at zero and the sea silently refused to draw. The ocean must not inherit the weather's lifecycle; keeping the
    // formula identical keeps water and cloud agreeing about drawn depth whenever both exist. [interaction: SSVolCloud squash band]
    const F32 cap = LLViewerCamera::getInstance()->getRenderFarPlane() * 0.98f;
    const F32 knee = cap * 0.8f;

    // Overlay values report THIS frame's draw, not a memory of better times: a stale rim once masked a frame-by-frame refusal to draw as a working sea. Zero until the draw call actually issues.
    mLastKnee = knee;
    mLastRSea = 0.f;

    if (!shader || knee <= 0.f)
    {
        return;
    }

    if (mVB.isNull())
    {
        build();
        if (mVB.isNull())
        {
            return;
        }
    }

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 water_h = LLEnvironment::instance().getWaterHeight();

    // Planet radius in km drives both how far the horizon is and how much the sea drops away toward it; 0 means a flat world - fixed radius, no droop. Earth by default: sensible curvature for
    // worlds whose environment never says otherwise. Small values make small worlds with visibly close, visibly curved horizons.
    static LLCachedControl<F32> planet_km(gSavedSettings, "SSAtmoSeaPlanetRadiusKm", 6371.f);
    const F32 planet_r = llmax(0.f, (F32)planet_km) * 1000.f;
    const F32 eye_h = llmax(cam.mV[2] - water_h, 2.f);

    // Tangent horizon distance sqrt(2*R*h), overshot so the rim falls behind the silhouette. Altitude-aware: climbing to a skybox makes the horizon recede and drop the way a planet's does,
    // instead of leaving a fixed disc floating 16 degrees below eye level.
    F32 r_sea = SEA_MIN_RADIUS_M;
    if (planet_r > 0.f)
    {
        r_sea = llclamp(SEA_HORIZON_OVERSHOOT * sqrtf(2.f * planet_r * eye_h), SEA_MIN_RADIUS_M, SEA_MAX_RADIUS_M);
    }

    // Stock water's outer footprint: region water, hole tiles and edge patches are all REAL water, and the frame lattice hangs on the union rectangle of all of them - connecting to the stock
    // geometry instead of overlapping it, which is what makes their depth relationship a non-issue at any distance (only the one-cell apron ever sits under stock water). Recomputed per frame
    // from the live objects: it only actually changes on region crossings and draw-distance changes, but chasing those triggers is more code than a dozen bounding-box unions.
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

    // Debug: hoist the sea 2m ABOVE the water instead of sinking it, making it overdraw the stock planes and identify itself unmistakably. Splits "the draw never lands fragments" from "the
    // fragments are all hidden by depth or shading" without touching a fragment shader.
    static LLCachedControl<bool> debug_lift(gSavedSettings, "SSAtmoDebugFarSeaLift", false);
    const F32 sea_h = debug_lift ? water_h + 2.f : water_h;

    // Degenerate rect guard: the region loop guarantees a footprint in practice, but a sub-cell rect would put the whole frame inside its own apron; a live refusal must read as one on the overlay.
    const F32 half_x = 0.5f * (hole_max_x - hole_min_x);
    const F32 half_y = 0.5f * (hole_max_y - hole_min_y);
    if (half_x < 32.f || half_y < 32.f)
    {
        return;
    }

    // Where the blend to the rim starts, as a chebyshev fraction of the frame (the rect edge sits at 1/4 = SEA_HOLE_HALF_CELLS/SEA_FRAME_HALF_CELLS): the knee's fraction of the frame's edge
    // extent when that fits, so the identity lattice runs to the knee and the blend band lives where the squash already owns the vertices. Clamped: the floor keeps a 0.2-half-extent identity
    // margin beyond the rect so the blend can never touch the seam, and the ceiling guarantees a real band even when a huge draw distance pushes the knee past the frame - blended cells inside
    // the knee then skew the wave warp a little, at kilometres out where the waves have faded to nothing.
    const F32 sea_bound = (F32)SEA_FRAME_HALF_CELLS / (F32)SEA_HOLE_HALF_CELLS;
    const F32 blend0 = llclamp(knee / (sea_bound * llmin(half_x, half_y)), 0.3f, 0.85f);

    mLastRSea = r_sea;

    static bool logged_once = false;
    if (!logged_once)
    {
        logged_once = true;
        LL_INFOS("SSFarSea") << "first draw: knee " << knee << " cap " << cap << " rim " << r_sea
                             << " water_h " << water_h << " planet_r " << planet_r << " blend0 " << blend0
                             << " hole (" << hole_min_x << "," << hole_min_y << ")-(" << hole_max_x << "," << hole_max_y << ")"
                             << " verts " << mVertCount << " indices " << mIndexCount << LL_ENDL;
    }

    // The sea's whole per-frame state is these uniforms; the vertex buffer is canonical and immutable (see build). ss_squash shares knee and cap with the cloud field so water and cloud agree
    // about drawn depth, but carries the sea's own virtual radius - the horizon is allowed to be much farther than the clouds go. ss_squash and ss_sea are zeroed after the draw: the stock planes
    // rendered by this same shader must stay entirely unexpanded and unsquashed, and zeros fail both gates in waterV.
    static LLStaticHashedString s_ss_squash("ss_squash");
    static LLStaticHashedString s_ss_sea("ss_sea");
    static LLStaticHashedString s_ss_sea_hole("ss_sea_hole");
    shader->uniform3f(s_ss_squash, knee, cap, r_sea);
    shader->uniform4f(s_ss_sea, blend0, r_sea, sea_h, planet_r);
    shader->uniform4f(s_ss_sea_hole, hole_min_x, hole_min_y, hole_max_x, hole_max_y);

    mVB->setBuffer();
    mVB->drawRange(LLRender::TRIANGLES, 0, mVertCount - 1, mIndexCount, 0);

    shader->uniform3f(s_ss_squash, 0.f, 0.f, 0.f);
    shader->uniform4f(s_ss_sea, 0.f, 0.f, 0.f, 0.f);
}

void SSFarSea::build()
{
    // Canonical mesh, built exactly once for the process: one square FRAME lattice in units of the hole rect's half extent (cell = 1/SEA_HOLE_HALF_CELLS, chebyshev 1 = the rect edge, chebyshev
    // SEA_FRAME_HALF_CELLS/SEA_HOLE_HALF_CELLS = the outer square edge). No world position, height, knee or rim is baked in anywhere, so nothing that happens at runtime ever needs a rebuild.
    const S32 lattice = SEA_FRAME_HALF_CELLS * 2 + 1;
    mVertCount = (U32)(lattice * lattice);

    // Indices: every cell of the frame. The vertex sheet stays a full square (the hole interior's unused vertices cost ~1MB once and keep the indexing trivial); the index buffer skips cells that
    // lie entirely inside the apron - everything within one cell of the rect edge stays, so the innermost ring tucks under stock water and depth hides it.
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
    // Doubled index allocation: buffers count in 16-bit units and halve when setIndexData switches them to 32-bit.
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
