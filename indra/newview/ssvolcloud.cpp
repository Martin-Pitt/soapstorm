/**
 * @file ssvolcloud.cpp
 * @brief Atmo Magic: the volumetric cloud field. See the header.
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

#include "ssvolcloud.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvcloudfieldstate.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvtrackstate.h"
#include "ssatmomagic.h"

#include "llenvironment.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

// <SS:Nexii> Atmo Magic volumetric cloud field

// Set while a reflection probe is being captured. Declared the same way
// sssurfacefield.cpp declares it, rather than pulling in the header that
// owns it for one bool.
extern bool gCubeSnapshot;

namespace
{
    // The puff lattice. Cells are laid out on the WIND-RELATIVE grid rather
    // than on the world, so a puff belongs to the air rather than to the
    // ground under it: the whole field slides downwind and new cells arrive
    // from upwind instead of the pattern shimmering in place.
    const F32 CELL_M = 260.f;

    // How far out puffs are drawn. Past this they are a few pixels each and
    // the dome behind them is doing the work anyway.
    const F32 FIELD_RADIUS_M = 3200.f;

    // Ceiling on how many quads a frame may draw, whatever the coverage. A
    // full-coverage field over the whole radius is thousands of cells, and
    // beyond a few hundred overlapping alpha quads the fill cost is real
    // while the look stops changing.
    const S32 MAX_PUFFS = 420;

    // A puff's radius relative to the cell it sits in. Over 0.5 they
    // overlap, which is what makes a field read as connected cloud rather
    // than as a polka dot pattern - and overlapping is cheap here because
    // the art is mostly transparent.
    const F32 PUFF_CELL_FRACTION = 0.85f;

    // How much of the field's thickness a puff's own size follows. A deep
    // convective layer is drawn as bigger lumps, not merely more of them.
    const F32 PUFF_THICKNESS_GAIN = 0.35f;

    // Coverage below this draws nothing at all: a nearly clear sky should be
    // clear, not a scatter of ghosts.
    const F32 COVERAGE_FLOOR = 0.04f;

    // Integer hash, so the field is identical on every client looking at the
    // same weather - the same reason the precipitation sim hashes its cells.
    U32 hashCell(S32 x, S32 y, U32 salt)
    {
        U32 h = (U32)(x * 374761393) ^ (U32)(y * 668265263) ^ (salt * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    F32 hashUnit(S32 x, S32 y, U32 salt)
    {
        return (F32)(hashCell(x, y, salt) & 0x00ffffffu) / (F32)0x01000000;
    }
}

void SSVolCloud::clear()
{
    mPuffs.clear();
    mLastBuildMS = 0.f;
}

void SSVolCloud::update(F32 dt)
{
    mPuffs.clear();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoVolumetricClouds", true);
    if (!enabled) return;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr || !mgr->hasAsset()) return;

    LLTimer timer;

    // The track the camera is actually in, and its weather at this instant -
    // the same resolution path the applier uses, so the layer agrees with
    // the sky drawn around it.
    const SSAtmoEnvAsset& asset = mgr->asset();
    if (asset.mTracks.empty()) return;

    const F32 world_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(asset, world_z, world_z, true);
    S32 track_index = blend.mPrimaryTrack;
    if (track_index < 0 || track_index >= (S32)asset.mTracks.size()) track_index = 0;

    const SSAtmoEnvTrack& track = asset.mTracks[(size_t)track_index];
    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();

    const F32 moisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    const F32 convection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);

    const SSAtmoEnvCloudFieldState field =
        SSAtmoEnvCloudFieldResolver::resolve(track.mCloudField, moisture, convection, phase);

    if (field.mCoverage < COVERAGE_FLOOR || field.mThicknessM <= 1.f) return;

    // Which art the layer wears follows what the weather is doing to it: a
    // towering, churning field is cumulonimbus, a quieter one is
    // altocumulus. The cirrus-like layered map stays on the dome, where a
    // flat high deck belongs.
    mTexture = LLUUID(field.mHasAnvil || convection > 0.6f
                      ? SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS
                      : SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS);

    // Lit from whatever is up. One mix per puff, not per fragment - see the
    // fragment shader on why.
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const LLColor3 sunlit = sky ? LLColor3(sky->getSunlightColor()) : LLColor3(1.f, 1.f, 1.f);
    const LLColor3 ambient = sky ? LLColor3(sky->getAmbientColor()) : LLColor3(0.4f, 0.4f, 0.5f);
    const LLVector3 light_dir = LLEnvironment::instance().getLightDirection();

    // Storm cloud is darker than fair-weather cloud, and the field's own
    // churn is the closest thing it has to a "how angry is this" figure.
    const F32 gloom = 1.f - 0.55f * llclamp(field.mChurn, 0.f, 1.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();

    // Cell coordinates are taken in the air's frame - camera position minus
    // how far the air has travelled - so a cell keeps its identity while the
    // field slides past. Placing them on the ground grid instead would make
    // the puffs stand still while the dome behind them moved.
    const F32 air_x = cam.mV[VX] - drift.mV[0];
    const F32 air_y = cam.mV[VY] - drift.mV[1];

    const S32 cell_radius = (S32)ceilf(FIELD_RADIUS_M / CELL_M);
    const S32 cx0 = (S32)floorf(air_x / CELL_M);
    const S32 cy0 = (S32)floorf(air_y / CELL_M);

    const F32 base_radius = CELL_M * PUFF_CELL_FRACTION * 0.5f;
    const F32 size_gain = 1.f + PUFF_THICKNESS_GAIN * (field.mThicknessM / 500.f);

    for (S32 dy = -cell_radius; dy <= cell_radius; ++dy)
    {
        for (S32 dx = -cell_radius; dx <= cell_radius; ++dx)
        {
            const S32 cx = cx0 + dx;
            const S32 cy = cy0 + dy;

            // Coverage decides which cells hold cloud at all. Comparing a
            // per-cell hash against it gives a field that thins and fills
            // smoothly as the weather changes, and - because the hash is
            // stable - the cells that remain at low coverage are the same
            // ones that were there at high coverage rather than a fresh
            // scatter every time it eases.
            if (hashUnit(cx, cy, 1u) > field.mCoverage) continue;

            // Jittered off the lattice, or the field reads as a grid the
            // moment two puffs line up.
            const F32 jx = (hashUnit(cx, cy, 2u) - 0.5f) * CELL_M * 0.8f;
            const F32 jy = (hashUnit(cx, cy, 3u) - 0.5f) * CELL_M * 0.8f;

            LLVector3 pos;
            pos.mV[VX] = (F32)cx * CELL_M + CELL_M * 0.5f + jx + drift.mV[0];
            pos.mV[VY] = (F32)cy * CELL_M + CELL_M * 0.5f + jy + drift.mV[1];

            // Height within the layer. The anvil case spreads the top of
            // the field outward rather than piling it higher - which is what
            // an anvil is, a tower that has hit the inversion and gone flat.
            const F32 up = hashUnit(cx, cy, 4u);
            pos.mV[VZ] = field.mBaseHeightM + up * field.mThicknessM;

            const F32 flat = (field.mHasAnvil && up > 0.7f) ? 1.6f : 1.f;

            const LLVector3 to_cam = pos - cam;
            const F32 dist_sq = to_cam.magVecSquared();
            if (dist_sq > FIELD_RADIUS_M * FIELD_RADIUS_M) continue;

            Puff puff;
            puff.mPosAgent = pos;
            puff.mRadius = base_radius * size_gain * flat
                * (0.7f + 0.6f * hashUnit(cx, cy, 5u));
            puff.mCamDistSq = dist_sq;

            // Fade the outermost ring rather than letting puffs pop in at
            // the radius, and thin the whole field with coverage so a light
            // field is wispy rather than merely sparse.
            const F32 edge = 1.f - llclamp(
                (sqrtf(dist_sq) - FIELD_RADIUS_M * 0.75f) / (FIELD_RADIUS_M * 0.25f), 0.f, 1.f);
            puff.mAlpha = edge * llclamp(0.35f + 0.65f * field.mCoverage, 0.f, 1.f);

            // Lit by how much of this puff faces the light: a cheap stand-in
            // for the bright top and dark base a real cloud has, using the
            // puff's own offset from the layer's middle as its "normal".
            const F32 facing = llclamp(
                0.5f + 0.5f * (light_dir.mV[VZ] * (up - 0.5f) * 2.f), 0.f, 1.f);
            puff.mColor = (ambient + sunlit * facing) * gloom;

            mPuffs.push_back(puff);
        }
    }

    // Back to front, because they are alpha blended against each other.
    std::sort(mPuffs.begin(), mPuffs.end(),
              [](const Puff& a, const Puff& b) { return a.mCamDistSq > b.mCamDistSq; });

    if ((S32)mPuffs.size() > MAX_PUFFS)
    {
        // Drop the FAR ones: they are the small ones on screen, and cutting
        // from the back of a back-to-front list keeps the near cloud that
        // actually fills the view.
        mPuffs.erase(mPuffs.begin(), mPuffs.end() - MAX_PUFFS);
    }

    mLastBuildMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
}

void SSVolCloud::render()
{
    if (mPuffs.empty() || mTexture.isNull()) return;
    if (gCubeSnapshot) return;      // probe capture, not a frame anyone sees
    if (!gSSVolCloudProgram.isComplete()) return;

    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
        mTexture, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
    if (!tex) return;
    tex->addTextureStats((F32)MAX_IMAGE_AREA);

    LL_PROFILE_GPU_ZONE("atmo volumetric clouds");

    // Depth TESTED but not written: the layer has to sit behind a mountain
    // that stands in front of it, and its own quads have to blend through
    // each other rather than the nearest one punching a hole in the rest.
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    gSSVolCloudProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSVolCloudProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, tex, LLTexUnit::TT_TEXTURE);

    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam_right = camera->getLeftAxis() * -1.f;
    const LLVector3 cam_up = camera->getUpAxis();

    gGL.begin(LLRender::TRIANGLES);
    for (const Puff& puff : mPuffs)
    {
        const LLVector3 right = cam_right * puff.mRadius;
        const LLVector3 up = cam_up * puff.mRadius;

        gGL.color4f(puff.mColor.mV[0], puff.mColor.mV[1], puff.mColor.mV[2], puff.mAlpha);

        const LLVector3 tl = puff.mPosAgent - right + up;
        const LLVector3 bl = puff.mPosAgent - right - up;
        const LLVector3 tr = puff.mPosAgent + right + up;
        const LLVector3 br = puff.mPosAgent + right - up;

        // Two triangles rather than a strip: one begin/end for the whole
        // field beats a state change per puff, and a strip cannot carry
        // several quads without degenerate joins between them.
        gGL.texCoord2f(0.f, 1.f); gGL.vertex3fv(tl.mV);
        gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bl.mV);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(tr.mV);

        gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(tr.mV);
        gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bl.mV);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex3fv(br.mV);
    }
    gGL.end();
    gGL.flush();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSVolCloudProgram.unbind();
}

// </SS:Nexii>
