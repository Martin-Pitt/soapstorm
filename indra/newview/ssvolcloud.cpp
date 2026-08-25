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
#include "sslightning.h"

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
    const S32 MAX_PUFFS = 1260;

    // A puff's radius relative to the cell it sits in. Over 0.5 they
    // overlap, which is what makes a field read as connected cloud rather
    // than as a polka dot pattern - and overlapping is cheap here because
    // the art is mostly transparent.
    const F32 PUFF_CELL_FRACTION = 0.85f;

    // How much wider than tall a puff is drawn.
    //
    // Cloud elements are not spheres. A lump of cumulus is several times
    // broader than it is deep, and a stratiform deck is broader still -
    // which is most of why a field of round puffs reads as a bag of balls
    // however well each one is shaded. Stretching the quad costs nothing
    // and the world-space noise does not care, since it is sampled by
    // position rather than across the quad.
    //
    // Roughly area-preserving, so widening them does not also make the
    // field heavier.
    const F32 PUFF_WIDE = 1.7f;
    const F32 PUFF_TALL = 0.62f;

    // Where in the layer the stretch has fully eased off, as a fraction of
    // its depth, and where it starts to.
    //
    // Flat is a property of the BASE, not of cloud. Everything sitting at
    // the condensation level got there the same way and spreads out along
    // it, which is why an overcast underside is a plane - but the same
    // stretch carried all the way up makes the top of a tower into a stack
    // of plates, when a top is the one part that should billow. So the
    // widening is held through the lowest slice and let go above it.
    const F32 PUFF_ROUND_LO = 0.15f;
    const F32 PUFF_ROUND_HI = 0.70f;

    // How many puffs each cell that holds cloud actually emits.
    //
    // Raised rather than shrinking the cells, deliberately. Halving CELL_M
    // would also triple the count, but it would take the puff size down with
    // it (base_radius is a fraction of the cell) and move the whole lattice,
    // so coverage, jitter and the size of a cloud body would all shift at
    // once. Stacking several into the same cell changes only the density,
    // which is the thing being asked for - the field gets thicker, not
    // finer.
    const S32 PUFFS_PER_CELL = 3;

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

    // The same curve GLSL's smoothstep gives, for shaping done CPU-side.
    F32 ss_smoothstep(F32 lo, F32 hi, F32 v)
    {
        const F32 t = llclamp((v - lo) / llmax(hi - lo, 1.0e-5f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    F32 hashUnit(S32 x, S32 y, U32 salt)
    {
        return (F32)(hashCell(x, y, salt) & 0x00ffffffu) / (F32)0x01000000;
    }

    // The two scales cloud groups itself at, in cells.
    //
    // One scale was not enough, and the size was wrong as well. At six cells
    // - about 1.5km - a lattice spans barely four steps across the whole
    // visible field, and value noise interpolated over four steps is not a
    // field of clouds, it is a ramp. Thresholding a ramp gives one enormous
    // region with a soft edge, which is exactly the "not really clustering"
    // look.
    //
    // Two octaves fix both. The coarse one groups weather into systems a
    // couple of kilometres across, the fine one breaks each system into
    // individual clouds - which is how a real sky is organised, and neither
    // scale alone can express it.
    const S32 CLUSTER_CELLS_BIG = 9;
    const S32 CLUSTER_CELLS_SMALL = 3;
    const F32 CLUSTER_OCTAVE_MIX = 0.4f;

    // How much of the coverage decision is the cluster field rather than
    // per-cell noise. Nearly all of it: the per-cell part only has to fray
    // the edges of a cloud, and any more of it scatters singletons across
    // the gaps, which is what a clustered sky is supposed to be free of.
    const F32 CLUSTER_WEIGHT = 0.85f;

    // How tall the EDGE of a cluster stands, as a fraction of the layer.
    //
    // A storm is not a slab of even depth. It is a tower over a core with
    // shallower cloud fraying out around it, and the tower is the part that
    // reaches the inversion and spreads. Without this the cluster field only
    // decided WHERE cloud was, never how much - so every puff in the sky had
    // the whole layer to sit in and the tapering and the anvil happened
    // everywhere at once, as one sky-wide shape rather than as storms.
    const F32 CLUSTER_EDGE_HEIGHT = 0.3f;

    // One octave of value noise over the cell grid: hash the corners of a
    // lattice and interpolate, smoothstepped so the lumps have soft
    // shoulders rather than diamond edges.
    F32 clusterOctave(S32 cx, S32 cy, S32 cells, U32 salt, F32 shift)
    {
        const F32 fx = (F32)cx / (F32)cells + shift;
        const F32 fy = (F32)cy / (F32)cells + shift;

        const S32 x0 = (S32)floorf(fx);
        const S32 y0 = (S32)floorf(fy);

        F32 tx = fx - (F32)x0;
        F32 ty = fy - (F32)y0;
        tx = tx * tx * (3.f - 2.f * tx);
        ty = ty * ty * (3.f - 2.f * ty);

        const F32 c00 = hashUnit(x0,     y0,     salt);
        const F32 c10 = hashUnit(x0 + 1, y0,     salt);
        const F32 c01 = hashUnit(x0,     y0 + 1, salt);
        const F32 c11 = hashUnit(x0 + 1, y0 + 1, salt);

        const F32 top = c00 + (c10 - c00) * tx;
        const F32 bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    // A COHERENT field over the cell grid - neighbouring cells get similar
    // values instead of independent ones - built from two scales.
    //
    // Thresholding white noise against coverage keeps each cell's decision
    // independent of its neighbours', so at low coverage what survives is
    // scattered singletons, which is nothing like weather. Value noise fixes
    // that; two octaves of it give the result an inside as well as an
    // outline.
    //
    // The fine octave is offset off the coarse one's lattice, or the two
    // share corners and the grid they are both built on starts to show.
    F32 clusterUnit(S32 cx, S32 cy)
    {
        const F32 big = clusterOctave(cx, cy, CLUSTER_CELLS_BIG, 101u, 0.f);
        const F32 small = clusterOctave(cx, cy, CLUSTER_CELLS_SMALL, 137u, 0.37f);
        return big * (1.f - CLUSTER_OCTAVE_MIX) + small * CLUSTER_OCTAVE_MIX;
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
    // Authored if the track says so, otherwise the old convection rule as a
    // default - see SSAtmoEnvCloudField::mBaseTexture.
    mTexture = field.mBaseTexture.notNull()
        ? field.mBaseTexture
        : LLUUID(field.mHasAnvil || convection > 0.6f
                 ? SSAtmoEnvCloudDome::CLOUD_TEXTURE_CUMULONIMBUS
                 : SSAtmoEnvCloudDome::CLOUD_TEXTURE_ALTOCUMULUS);

    mAuthoredDetail = field.mDetailTexture;

    // Lit from whatever is up. One mix per puff, not per fragment - see the
    // fragment shader on why.
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const LLColor3 sunlit = sky ? LLColor3(sky->getSunlightColor()) : LLColor3(1.f, 1.f, 1.f);
    const LLColor3 ambient = sky ? LLColor3(sky->getAmbientColor()) : LLColor3(0.4f, 0.4f, 0.5f);
    const LLVector3 light_dir = LLEnvironment::instance().getLightDirection();

    // Kept for render(), which shades each fragment rather than each puff.
    // The haze is the sky's own scattered light - the same colour the
    // celestial discs wash toward, and for the same reason.
    mBaseZ = field.mBaseHeightM;
    mThicknessM = llmax(1.f, field.mThicknessM);
    mAnvil = field.mAnvil;
    mTextureMix = field.mTextureMix;
    mPuffDensity = field.mPuffDensity;
    mDetailScale = field.mDetailScale;
    mDriftRate = field.mDriftRate;
    mLightDir = light_dir;
    mSunColor = sunlit;
    {
        const LLColor4 haze = sky ? sky->getHazeColor() : LLColor4(0.5f, 0.55f, 0.65f, 1.f);
        mHaze.setVec(haze.mV[0], haze.mV[1], haze.mV[2]);
    }

    // Storm cloud is darker than fair-weather cloud, and the field's own
    // churn is the closest thing it has to a "how angry is this" figure.
    const F32 gloom = field.mGloom;
    mChurn = llclamp(field.mChurn, 0.f, 1.f);

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

            // Coverage decides which cells hold cloud at all, tested against
            // a mostly-coherent field so that what survives is CLOUDS rather
            // than confetti - see clusterUnit.
            //
            // Still stable under changing weather: both halves are pure
            // functions of the cell, so the cells that remain as it dries
            // out are the ones that were there when it was wet, rather than
            // a fresh scatter every time it eases.
            const F32 gate = clusterUnit(cx, cy) * CLUSTER_WEIGHT
                           + hashUnit(cx, cy, 1u) * (1.f - CLUSTER_WEIGHT);
            if (gate > field.mCoverage) continue;

            // How deep inside its cluster this cell sits, 0 at the ragged
            // edge and 1 at the core.
            //
            // It falls straight out of the gate above: a cell only survives
            // when its field value is under the coverage, so how far UNDER
            // is how far in. The same number that decides whether there is
            // cloud here now also decides how much - which is what turns a
            // field of cloud into a field of clouds.
            const F32 coreness = llclamp(
                (field.mCoverage - gate) / llmax(field.mCoverage, 0.01f), 0.f, 1.f);

            // The tower this cell can build. Cores get the whole layer,
            // edges a shallow skirt - so the tapering and the anvil, which
            // are keyed to height, only ever happen over a core.
            const F32 cell_height = CLUSTER_EDGE_HEIGHT
                + (1.f - CLUSTER_EDGE_HEIGHT) * coreness;

            for (S32 sub = 0; sub < PUFFS_PER_CELL; ++sub)
            {
                // Each puff in the cell gets its own draw from the hash, by
                // stepping the salt. Eight apart so the sets cannot collide
                // with each other or with the coverage salt above.
                const U32 salt = (U32)sub * 8u;

                // Jittered off the lattice, or the field reads as a grid the
                // moment two puffs line up. It is also what separates the
                // puffs sharing a cell - without it they would be stacked on
                // one spot and the extra two would be doing nothing.
                const F32 jx = (hashUnit(cx, cy, 2u + salt) - 0.5f) * CELL_M * 0.8f;
                const F32 jy = (hashUnit(cx, cy, 3u + salt) - 0.5f) * CELL_M * 0.8f;

                LLVector3 pos;
                pos.mV[VX] = (F32)cx * CELL_M + CELL_M * 0.5f + jx + drift.mV[0];
                pos.mV[VY] = (F32)cy * CELL_M + CELL_M * 0.5f + jy + drift.mV[1];

                // Height within the layer. The anvil case spreads the top of
                // the field outward rather than piling it higher - which is
                // what an anvil is, a tower that has hit the inversion and
                // gone flat.
                // Where in this CELL's tower the puff sits, and where that
                // puts it in the layer as a whole. The two differ over
                // anything but a core, and the difference is the storm.
                const F32 up_cell = hashUnit(cx, cy, 4u + salt);
                const F32 up = up_cell * cell_height;
                pos.mV[VZ] = field.mBaseHeightM + up * field.mThicknessM;

                // The tower's width down its own height: a waist, then a flare.
            //
            // A step at 0.7 gave every puff above that line the same extra
            // width and everything below it none, which is a mushroom rather
            // than an anvil. What a real one does is narrow through the
            // middle - the tower is fed by a column, and a column is thinner
            // than the base it rose from - and then spread hard where it hits
            // the inversion and can go no higher.
            //
            // Scaled by how far into anvil the weather actually is, so a
            // merely convective sky keeps ordinary lumpy tops - and by how
            // much of a tower this cell is, so the shape belongs to a storm
            // rather than to the sky.
            //
            // Read against up_cell, not up: the waist is two thirds of the
            // way up THIS tower, wherever its top happens to be. Against the
            // layer it would sit at one absolute altitude across the whole
            // field, which is the sky-wide pinch this is meant to replace.
            const F32 waist = 1.f - 0.35f * ss_smoothstep(0.2f, 0.65f, up_cell);
            const F32 flare = 1.1f * ss_smoothstep(0.74f, 1.f, up_cell);
            const F32 flat = 1.f + field.mAnvil * coreness * (waist + flare - 1.f);

                const LLVector3 to_cam = pos - cam;
                const F32 dist_sq = to_cam.magVecSquared();
                // Skips this puff, not the cell: its siblings are jittered
                // elsewhere and may well be inside the radius.
                if (dist_sq > FIELD_RADIUS_M * FIELD_RADIUS_M) continue;

                Puff puff;
                puff.mPosAgent = pos;
                puff.mRadius = base_radius * size_gain * flat
                    * (0.7f + 0.6f * hashUnit(cx, cy, 5u + salt));
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
                // Narrow, deliberately.
                //
                // At full swing this is the single strongest thing separating one
                // puff from its neighbour, and since `up` is a per-cell hash the
                // separation is random - so the field came out as a patchwork of
                // flatly-lit cards at visibly different brightnesses, which is
                // the one thing that gives a billboard field away. A cloud's top
                // really is brighter than its base, but the difference belongs
                // across the layer, not between adjacent lumps. What variation
                // there is now comes mostly from the noise, which is continuous
                // across puffs and so cannot outline them.
                const F32 facing = llclamp(
                    0.5f + 0.2f * (light_dir.mV[VZ] * (up - 0.5f) * 2.f), 0.f, 1.f);
                puff.mColor = (ambient + sunlit * facing) * gloom;

                mPuffs.push_back(puff);
            }
        }
    }

    // Strikes light the cloud from inside - see the loop in ssVolCloudF.
    //
    // This is what sheet lightning IS: the great majority of discharges
    // never leave the cloud, and what an observer sees is the deck glowing
    // from within around a point they cannot see.
    //
    // Collected here, applied in the shader. The first attempt added a flat
    // colour per puff on the CPU, which lit every fragment of a puff by the
    // same amount - so a puff brightened rather than being lit from
    // anywhere, and the sphere term it then passed through belonged to the
    // sun, which dimmed the underside of a night deck exactly where a strike
    // beneath it should have been brightest.
    mStrikeLights.clear();
    for (const SSStrike& strike : SSLightning::getInstance()->strikes())
    {
        const F32 b = strike.mChannelBrightness * strike.mIntensity;
        if (b <= 0.004f) continue;

        mStrikeLights.push_back(LLVector4(strike.mOrigin.mV[VX],
                                          strike.mOrigin.mV[VY],
                                          strike.mOrigin.mV[VZ], b));
        if ((S32)mStrikeLights.size() >= SS_MAX_STRIKE_LIGHTS) break;
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
    if (!gSSVolCloudProgram.isComplete()) return;

    // Every pass that is not the one frame the user is looking at.
    //
    // renderGeomPostDeferred - where this is called from - runs for more
    // than the world: llviewerdisplay calls it again for HUD attachments,
    // and the pipeline calls it for impostors, shadows and probe captures.
    // In none of those is mRT->screen the bound target, so the depth copy
    // below would flush and re-bind a target nobody asked for and leave it
    // bound. During the HUD pass that means the INTERFACE draws into the
    // wrong place, every other frame.
    //
    // The same guard SSPrecipRenderer uses, for the same reason - rain has
    // no business in a HUD render either.
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender
        || LLPipeline::sShadowRender || gCubeSnapshot)
    {
        return;
    }

    // Fetched once and held - see mTextureRef.
    if (mTextureRef.isNull() || mTextureRef->getID() != mTexture)
    {
        mTextureRef = LLViewerTextureManager::getFetchedTexture(
            mTexture, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
        if (mTextureRef.notNull())
        {
            // BOOST_HIGH asks for it at full resolution; setNoDelete keeps
            // it from being reclaimed between the frames that want it. A
            // noise map that has slipped a mip or two is not a slightly
            // softer noise map, it is a uniform grey, and the field goes
            // with it.
            mTextureRef->setNoDelete();
        }
    }
    if (mTextureRef.isNull()) return;
    mTextureRef->addTextureStats((F32)MAX_IMAGE_AREA);

    // The dome's map, for the finer octaves - see mDomeTexRef. Read from the
    // live sky rather than from the track, so it follows whatever is
    // actually being drawn overhead even mid-transition.
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        const LLUUID dome_id = mAuthoredDetail.notNull()
            ? mAuthoredDetail
            : (sky ? sky->getCloudNoiseTextureId() : LLUUID::null);
        if (dome_id.notNull() && (mDomeTexRef.isNull() || mDomeTexture != dome_id))
        {
            mDomeTexture = dome_id;
            mDomeTexRef = LLViewerTextureManager::getFetchedTexture(
                dome_id, FTT_DEFAULT, true, LLGLTexture::BOOST_HIGH);
            if (mDomeTexRef.notNull())
            {
                mDomeTexRef->setNoDelete();
            }
        }
        if (mDomeTexRef.notNull())
        {
            mDomeTexRef->addTextureStats((F32)MAX_IMAGE_AREA);
        }
    }

    LLViewerFetchedTexture* tex = mTextureRef.get();

    LL_PROFILE_GPU_ZONE("atmo volumetric clouds");

    const LLViewerCamera* camera = LLViewerCamera::getInstance();

    // The scene's depth, copied somewhere it can legally be read.
    //
    // Not read in place: this pass draws into a target that has the scene
    // depth attached, and a shader sampling the texture its own pass has
    // bound as depth is undefined - it flickers.
    //
    // Copied exactly the way LLPipeline::doAtmospherics copies it, which is
    // worth following to the letter because two details in it are load
    // bearing:
    //
    //  - the current target is FLUSHED before another is bound, and re-bound
    //    afterwards. Binding a second target while the first is still bound
    //    nests them, and the inner flush() then pops to the wrong place -
    //    leaving the wrong framebuffer bound and every puff drawn after it
    //    going somewhere invisible. That is what made the whole field vanish
    //    depending on where the camera pointed.
    //
    //  - the copy is a full-screen pass through gCopyDepthProgram, which
    //    SAMPLES depth and writes gl_FragDepth, rather than a framebuffer
    //    blit. A blit has to agree with the driver about attachment formats
    //    and read/draw binding state in the middle of a frame; the shader
    //    path does not.
    const S32 view_w = (S32)gGLViewport[2];
    const S32 view_h = (S32)gGLViewport[3];
    bool soft = false;

    if (view_w > 0 && view_h > 0 && gCopyDepthProgram.isComplete())
    {
        if ((S32)mDepthCopy.getWidth() != view_w || (S32)mDepthCopy.getHeight() != view_h)
        {
            mDepthCopy.release();
            soft = mDepthCopy.allocate(view_w, view_h, GL_RGBA, true);
        }
        else
        {
            soft = true;
        }

        if (soft)
        {
            LL_PROFILE_GPU_ZONE("atmo cloud depth copy");

            // Write depth unconditionally - the point is to end up with a
            // copy of it, not to test against what is already there.
            LLGLDepthTest copy_depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

            gPipeline.mRT->screen.flush();
            mDepthCopy.bindTarget();

            gCopyDepthProgram.bind();

            S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
            S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
            gGL.getTexUnit(diff_map)->bind(&gPipeline.mRT->screen);
            gGL.getTexUnit(depth_map)->bind(&gPipeline.mRT->deferredScreen, true);

            // Only the depth is wanted; the colour attachment is along for
            // the ride because the copy shader writes one.
            gGL.setColorMask(false, false);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            gGL.setColorMask(true, true);

            gCopyDepthProgram.unbind();

            mDepthCopy.flush();
            gPipeline.mRT->screen.bindTarget();
        }
    }

    // Depth TESTED but not written: the layer has to sit behind a mountain
    // that stands in front of it, and its own quads have to blend through
    // each other rather than the nearest one punching a hole in the rest.
    //
    // Kept even with the soft fade below, which reaches zero at the same
    // place: the test throws the hidden fragments out before they are shaded
    // rather than after, and these quads are large.
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    // Colour but NOT alpha.
    //
    // The screen target'''s alpha channel is the glow buffer. generateGlow
    // sets GLOW_MIN_LUMINANCE to 9999, which kills the luminance terms in
    // glowExtractF.glsl and leaves this:
    //
    //     frag_color.a = max(col.a, ...);
    //
    // - so glow is whatever alpha the screen happens to be holding. A pass
    // that blends alpha into the screen is therefore writing glow, and every
    // puff was: the halo bleeding around corners is the glow blur, which
    // nothing but the glow buffer can produce.
    //
    // The same guard LLDrawPoolAlpha uses for the same reason.
    gGL.setColorMask(true, false);

    gSSVolCloudProgram.bind();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSVolCloudProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, tex, LLTexUnit::TT_TEXTURE);

    // Falls back to the puff's own map when the sky has no cloud texture, so
    // the octaves degrade to more of the same rather than to black.
    gSSVolCloudProgram.bindTexture(LLShaderMgr::CLOUD_NOISE_MAP,
                                   mDomeTexRef.notNull() ? mDomeTexRef.get() : tex,
                                   LLTexUnit::TT_TEXTURE);

    // The air's frame and its convection - the fragment shader samples the
    // noise by world position, so it needs both to know where the field is
    // and how fast it is turning over.
    static LLStaticHashedString s_drift("ss_drift");
    static LLStaticHashedString s_time("ss_time");
    static LLStaticHashedString s_churn("ss_churn");

    const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();
    gSSVolCloudProgram.uniform2f(s_drift, drift.mV[0], drift.mV[1]);
    gSSVolCloudProgram.uniform1f(s_time, (F32)LLFrameTimer::getElapsedSeconds());
    gSSVolCloudProgram.uniform1f(s_churn, mChurn);

    static LLStaticHashedString s_anvil("ss_anvil");
    gSSVolCloudProgram.uniform1f(s_anvil, mAnvil);

    static LLStaticHashedString s_base_z("ss_base_z");
    static LLStaticHashedString s_thick("ss_layer_thick");
    static LLStaticHashedString s_tex_mix("ss_tex_mix");
    static LLStaticHashedString s_puff_density("ss_puff_density");
    static LLStaticHashedString s_detail_scale("ss_detail_scale");
    static LLStaticHashedString s_drift_rate("ss_drift_rate");

    gSSVolCloudProgram.uniform1f(s_base_z, mBaseZ);
    gSSVolCloudProgram.uniform1f(s_thick, mThicknessM);
    gSSVolCloudProgram.uniform1f(s_tex_mix, mTextureMix);
    gSSVolCloudProgram.uniform1f(s_puff_density, mPuffDensity);
    gSSVolCloudProgram.uniform1f(s_detail_scale, mDetailScale);
    gSSVolCloudProgram.uniform1f(s_drift_rate, mDriftRate);

    // Which way the air is going, for the streaking at low convection.
    // Taken from the drift the layer has already accumulated - that IS the
    // wind, integrated - with a fallback for a dead calm, where the stretch
    // has to point somewhere and no direction is more right than another.
    static LLStaticHashedString s_wind("ss_wind");
    LLVector2 wind = drift;
    if (wind.length() < 0.001f)
    {
        wind.setVec(1.f, 0.f);
    }
    else
    {
        wind.normalize();
    }
    gSSVolCloudProgram.uniform2f(s_wind, wind.mV[0], wind.mV[1]);

    // Lighting, for the per-fragment shape - see SS_FORM_DARK.
    // The strikes lighting the deck this frame. Bound even when there are
    // none, because a stale count would leave the last flash burnt into the
    // cloud until the next one.
    {
        static LLStaticHashedString s_strike("ss_strike");
        static LLStaticHashedString s_strike_count("ss_strike_count");

        static LLStaticHashedString s_strike_color("ss_strike_color");
        const LLColor3 lit = SSAtmoMagic::getInstance()->lightningColor();
        gSSVolCloudProgram.uniform3fv(s_strike_color, 1, lit.mV);

        const S32 count = llmin((S32)mStrikeLights.size(), SS_MAX_STRIKE_LIGHTS);
        gSSVolCloudProgram.uniform1i(s_strike_count, count);
        if (count > 0)
        {
            gSSVolCloudProgram.uniform4fv(s_strike, count,
                                          (F32*)mStrikeLights.data());
        }
    }

    static LLStaticHashedString s_light_dir("ss_light_dir");
    static LLStaticHashedString s_sun_color("ss_sun_color");
    static LLStaticHashedString s_haze("ss_haze");
    static LLStaticHashedString s_cam_pos("ss_cam_pos");

    LLVector3 light = mLightDir;
    if (light.normalize() < 0.001f) light = LLVector3::z_axis;

    gSSVolCloudProgram.uniform3fv(s_light_dir, 1, light.mV);
    gSSVolCloudProgram.uniform3fv(s_sun_color, 1, mSunColor.mV);
    gSSVolCloudProgram.uniform3fv(s_haze, 1, mHaze.mV);
    gSSVolCloudProgram.uniform3fv(s_cam_pos, 1, camera->getOrigin().mV);

    // How far from a surface a puff starts thinning out, and the depth to
    // measure it against.
    //
    // Generous at 45m: this is not really an intersection fix, it is what
    // makes vapour behave like vapour near anything solid, so a platform
    // sitting in the layer wears fog rather than cutting a line across it.
    static const F32 SOFT_M = 112.5f;
    static LLStaticHashedString s_clip("ss_clip");
    static LLStaticHashedString s_soft("ss_soft_m");

    // Bound by RESERVED index, not by name - see the note on depthMap in the
    // fragment shader.
    if (soft)
    {
        soft = gSSVolCloudProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH,
                                              &mDepthCopy, true) >= 0;
    }

    // And the fade only switches on if that bind actually took.
    //
    // Worth being explicit about, because of how this fails: an unbound
    // sampler reads whatever is on unit 0, the comparison comes out
    // nonsense, and the fade takes every fragment to zero. The failure mode
    // of a depth read gone wrong is not a wrong-looking fade, it is no
    // clouds at all - so it defaults off rather than trusting the bind.
    gSSVolCloudProgram.uniform1f(s_soft, soft ? SOFT_M : 0.f);
    if (soft)
    {
        gSSVolCloudProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                     (F32)view_w, (F32)view_h);
        gSSVolCloudProgram.uniform2f(s_clip, camera->getNear(), camera->getFar());
    }


    const LLVector3 cam_pos = camera->getOrigin();

    // Only ever used to break a tie - see the frame built per puff below.
    const LLVector3 cam_right_fallback = camera->getLeftAxis() * -1.f;

    gGL.begin(LLRender::TRIANGLES);
    for (const Puff& puff : mPuffs)
    {
        // A frame built from the puff's OWN view ray and the world, never
        // from the camera's screen axes.
        //
        // Screen axes are what made the whole field turn with the camera:
        // every puff shared one orientation rigidly locked to the view, so
        // looking up and turning span the sky like a painted backdrop. The
        // fix is that nothing here may depend on which way the camera is
        // rolled - only on where it is.
        LLVector3 normal = cam_pos - puff.mPosAgent;
        if (normal.normalize() < 0.001f)
        {
            normal = LLVector3::z_axis;
        }

        // Looking along the layer, a puff should face the eye. Looking up
        // through it, it should lie flat - a deck seen from below is a
        // ceiling, not a wall of cards edge-on to each other. So the normal
        // leans toward vertical as the view ray does.
        //
        // This is also what makes straight up work at all: a camera-facing
        // quad has no defined roll when the view ray is the world axis you
        // would reference it against, and every fix for that pole either
        // pops or falls back to the camera. A flat quad has no such
        // problem, because its axes can simply be the world's.
        const F32 flatten = llclamp((fabsf(normal.mV[VZ]) - 0.6f) / 0.35f, 0.f, 1.f);
        if (flatten > 0.f)
        {
            const F32 sgn = (normal.mV[VZ] >= 0.f) ? 1.f : -1.f;
            normal = normal * (1.f - flatten) + LLVector3(0.f, 0.f, sgn) * flatten;
            if (normal.normalize() < 0.001f)
            {
                normal.setVec(0.f, 0.f, sgn);
            }
        }

        // The reference the roll is measured from, swinging from world up to
        // world east over the same range - so by the time the quad is flat
        // and world up would be useless, it is no longer being asked.
        LLVector3 ref = LLVector3::z_axis * (1.f - flatten)
                      + LLVector3::x_axis * flatten;
        ref.normalize();

        LLVector3 base_right = ref % normal;
        if (base_right.normalize() < 0.001f)
        {
            base_right = cam_right_fallback;
        }
        const LLVector3 base_up = normal % base_right;

        // No per-puff roll. There was one, to stop every quad showing the
        // same tile at the same orientation - but sampling the noise by
        // world position removes the repetition at its source, and rolling
        // a circular window achieves nothing anyway.
        // Broader than tall near the base, easing round with height - see
        // PUFF_ROUND_LO. The frame is world-referenced, so "tall" stays
        // vertical instead of following the camera's roll.
        const F32 layer_h = llclamp(
            (puff.mPosAgent.mV[VZ] - mBaseZ) / mThicknessM, 0.f, 1.f);
        const F32 round = llclamp(
            (layer_h - PUFF_ROUND_LO) / (PUFF_ROUND_HI - PUFF_ROUND_LO), 0.f, 1.f);

        const F32 wide = PUFF_WIDE + (1.f - PUFF_WIDE) * round;
        const F32 tall = PUFF_TALL + (1.f - PUFF_TALL) * round;

        const LLVector3 right = base_right * (puff.mRadius * wide);
        const LLVector3 up = base_up * (puff.mRadius * tall);

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

    gGL.setColorMask(true, true);
}

// </SS:Nexii>
