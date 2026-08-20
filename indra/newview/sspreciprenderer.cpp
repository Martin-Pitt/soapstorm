/**
 * @file sspreciprenderer.cpp
 * @brief Atmo Magic precipitation renderer implementation.
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

#include "sspreciprenderer.h"
#include "ssatmomagic.h"

#include "lldrawpoolalpha.h"
#include "llglstates.h"
#include "llrender.h"
#include "llfasttimer.h"
#include "llstaticstringtable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

// <SS:Nexii> Atmo Magic precipitation renderer

extern bool gCubeSnapshot;

// Fast timer breakdown: the build pass is CPU (culling, fade maths, vertex
// writes) while the draw pass is the GL submission, so a jitter can be told
// apart from a stall
static LLTrace::BlockTimerStatHandle FTM_SS_PRECIP_RENDER("Atmo Magic Render");
static LLTrace::BlockTimerStatHandle FTM_SS_PRECIP_BUILD("Build Quads");
static LLTrace::BlockTimerStatHandle FTM_SS_PRECIP_DRAW("Draw");

// Indices are uploaded as 32 bit (see ensureBuffer): 16 bit would top out at
// 16383 quads, well under the combined tier caps.
static const U32 MAX_QUADS = 48000;
static const U32 VB_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;

static inline F32 smooth01(F32 t)
{
    t = llclamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Cross-fade against the live camera distance; tiers hand off in the
// overlap bands so moving through them never pops
static F32 bandFade(F32 dist, const F32* band)
{
    const F32 f_in = (band[1] > band[0]) ? smooth01((dist - band[0]) / (band[1] - band[0])) : 1.f;
    const F32 f_out = smooth01((band[3] - dist) / llmax(0.01f, band[3] - band[2]));
    return llmin(f_in, f_out);
}

static F32 ageFade(const SSPrecipParticle& p)
{
    const F32 t_in = (p.mTier == TIER_SHEETS) ? 0.8f : 0.15f;
    const F32 t_out = ssPrecipFadeOut(p.mTier);
    return llmin(1.f, p.mAge / t_in) * llclamp((p.mMaxAge - p.mAge) / t_out, 0.f, 1.f);
}

// A stream leaves the lip along the flow and bends into gravity and the local
// wind, so it is walked as a ballistic path and stitched as a ribbon of quads.
// The texture scrolls down it at the speed the water is running, which is what
// makes it read as moving rather than as a painted streak, and it fades toward
// the bottom so it dissolves into the splash instead of stopping at a hard
// edge.
//
// It hangs along the lip, not across the view. A stream stands for a whole
// slot of the eave - metres of it - so its width is a real span of the world
// with the gutter running through it, and it has to stay in the plane
// of the fall the way a curtain of water off a roof does. Billboarding it to
// the camera instead is what made a run of them read as a rack of thin strips
// swivelling as you walked past.
//
// The particle carries the path: mPos is the lip, mVel the exit velocity,
// mNormal the horizontal wind drift, mPlaneD the fall time, mSizeX the half
// span along the lip, mSizeY the repeats across it and mPhase the scroll
// offset the sim advances.
template <typename EmitFn>
U32 SSPrecipRenderer::emitStream(const SSPrecipParticle& p, F32 alpha, F32 stretch,
                                 EmitFn& emit)
{
    const F32 fall_time = llmax(p.mPlaneD, 0.05f);
    const F32 g = 9.81f;

    // Along the lip: at right angles to the way the water leaves the edge,
    // which is where the exit velocity points. Taken from the particle rather
    // than stored, because they are the same direction by construction.
    LLVector3 along(p.mVel.mV[VX], p.mVel.mV[VY], 0.f);
    if (along.normVec() < 0.0001f) along = LLVector3::x_axis;
    along = along % LLVector3::z_axis;
    if (along.normVec() < 0.0001f) along = LLVector3::y_axis;

    // And up the lip's own pitch, because plenty of eaves are not level. A
    // curtain hung horizontally off the rake of a gable or the side of a
    // valley cuts straight through the roof it is running off, with half of it
    // buried and half hanging in the air above the tiles. The slope rides on
    // the particle: the sim knows which stretch of lip this one spans and how
    // that stretch climbs, and it widens the span to match so the curtain
    // still covers its slot rather than the slot's shadow.
    if (p.mRunSlope != 0.f)
    {
        along.mV[VZ] += p.mRunSlope;
        along.normVec();
    }

    // Where the tiling starts across this stream. Neighbouring slots draw the
    // same art at the same scale, so without an offset per stream the seams
    // between them line up into a visible grid down the whole gutter.
    const F32 u_rep = llmax(0.05f, p.mSizeY);
    F32 u0 = SSAtmoNoise::hash01(p.mSeed ^ 0x6F4A21B3u);
    F32 u1 = u0 + u_rep;

    // Half of them wear the art the other way round. There are eight bakes and
    // a gutter in a downpour puts up more streams than that, so two of them
    // side by side will sometimes draw the same one; a mirror across is free
    // and stops that reading as the same curtain hung twice. Across only -
    // flipping a stream top to bottom would run the water up the roof.
    if (SSAtmoNoise::hashU32(p.mSeed ^ 0x2B7E1516u) & 1u)
    {
        const F32 swap = u0;
        u0 = u1;
        u1 = swap;
    }
    const F32 repeats = llmax(0.05f, p.mFloorZ);

    // Walk the fall once and keep it, with how far along each sample sits.
    //
    // Segments are cut at equal *times*, and water in free fall covers about
    // eight times as much ground in the last of those as in the first, so the
    // two ways of laying a texture down this path are genuinely different
    // pictures and both are worth having. Against distance the art keeps one
    // size the whole way down. Against time it travels with the water: a
    // feature marks a parcel that left the lip at a moment, and since two
    // parcels leaving a moment apart draw apart as they fall, the column
    // stretches toward the bottom the way real falling water does.
    LLVector3 pt[SS_STREAM_SEGMENTS + 1];
    F32 walked[SS_STREAM_SEGMENTS + 1];
    pt[0] = p.mPos;
    walked[0] = 0.f;

    for (S32 k = 1; k <= SS_STREAM_SEGMENTS; ++k)
    {
        const F32 t = fall_time * (F32)k / (F32)SS_STREAM_SEGMENTS;
        pt[k] = p.mPos + p.mVel * t + p.mNormal * (0.5f * t * t)
              - LLVector3(0.f, 0.f, 0.5f * g * t * t);
        walked[k] = walked[k - 1] + (pt[k] - pt[k - 1]).magVec();
    }

    const F32 total = llmax(walked[SS_STREAM_SEGMENTS], 0.01f);

    const F32 boost = 1.f + p.mGlow * 1.5f;
    const LLColor4U tint((U8)llmin((S32)(p.mTint.mV[0] * boost), 255),
                         (U8)llmin((S32)(p.mTint.mV[1] * boost), 255),
                         (U8)llmin((S32)(p.mTint.mV[2] * boost), 255),
                         255);

    // Everything that varies down the fall, at one point along it. Both ends of
    // every segment are evaluated with this and handed to the emitter, so the
    // fade is continuous across a join instead of stepping at each one - a
    // colour per segment drew the ribbon as a stack of visibly separate bands.
    // Width is not in here: a stream is one width from lip to landing, because
    // the alternative textures like a trapezoid and kinks every drop.
    struct End
    {
        F32 v;
        LLColor4U col;
    };

    auto endAt = [&](S32 k) -> End
    {
        // How far down the fall this end sits, by distance. Shape follows
        // this rather than the texture's own measure: the last third that
        // fades out is a third of the drop, whichever way the art is laid on.
        const F32 f = walked[k] / total;

        // And where the texture is, which is the blend. At zero the art is
        // pinned to the geometry and one size all the way down; at one it is
        // pinned to the water and elongates as the water does.
        const F32 f_tex = lerp(f, (F32)k / (F32)SS_STREAM_SEGMENTS, stretch);

        // Full for most of the fall, then out over the last third, so it meets
        // the ground as spray rather than as a cut-off band
        const F32 fade = 1.f - llclamp((f - 0.66f) / 0.34f, 0.f, 1.f);

        End e;
        e.v = p.mPhase - f_tex * repeats;
        e.col = tint;
        e.col.mV[3] = (U8)llclamp((S32)(alpha * fade * 255.f), 0, 255);
        return e;
    };

    U32 written = 0;
    End top = endAt(0);

    for (S32 k = 0; k < SS_STREAM_SEGMENTS; ++k)
    {
        const End bot = endAt(k + 1);

        if ((pt[k + 1] - pt[k]).magVecSquared() < 1e-8f) { top = bot; continue; }

        // Both ends dark: past the tail fade, and nothing below it will be
        // brighter either
        if (top.col.mV[3] < 1 && bot.col.mV[3] < 1) { top = bot; continue; }

        // Scrolled by the phase the sim advances, so the drips run down the
        // ribbon at the speed the water is actually moving.
        //
        // Both signs matter and both were wrong once. The emitter puts the
        // second coordinate on the lower corners, so a texture laid out from
        // the top down was upside down against a streak, where the axis is
        // -mVel and the top carries it. And with the phase *added*, a feature
        // at a fixed v sits at f = (v - phase) / repeats, which walks toward
        // the lip as the phase climbs: the water ran up the roof.
        emit(pt[k], pt[k + 1], along, p.mSizeX, top.col, bot.col,
             u0, u1, top.v, bot.v);

        ++written;
        top = bot;
    }

    return written;
}

bool SSPrecipRenderer::ensureBuffer(U32 quads)
{
    if (mVB.notNull() && mVBQuads >= quads) return true;

    U32 alloc = llmax(1024u, mVBQuads);
    while (alloc < quads) alloc *= 2;
    alloc = llmin(alloc, MAX_QUADS);

    mVB = new LLVertexBuffer(VB_MASK);
    // Double the index count: buffers allocate in 16 bit units and halve the
    // count when switched to 32 bit indices by setIndexData
    if (!mVB->allocateBuffer(alloc * 4, alloc * 6 * 2))
    {
        mVB = nullptr;
        mVBQuads = 0;
        return false;
    }

    // Index pattern and texture coordinates never change per quad slot
    std::vector<U32> indices((size_t)alloc * 6);
    U32 vtx = 0;
    for (U32 i = 0, o = 0; i < alloc; ++i, o += 6, vtx += 4)
    {
        indices[o + 0] = vtx + 0;
        indices[o + 1] = vtx + 1;
        indices[o + 2] = vtx + 2;
        indices[o + 3] = vtx + 1;
        indices[o + 4] = vtx + 3;
        indices[o + 5] = vtx + 2;
    }
    mVB->setIndexData(indices.data(), 0, (U32)indices.size());
    mVB->unmapBuffer();

    // Texture coordinates used to be written once here and left alone, since
    // every quad mapped the whole texture. A stream off an eave does not: it
    // scrolls its texture down itself at the speed the water is running, and
    // each of its segments takes a different span of it. So they are written
    // per quad in the build pass now, alongside the vertices and the colour.
    // Four LLVector2 stores against the four LLVector3 and four LLColor4U
    // already going out is not a cost worth keeping the special case for.

    mVBQuads = alloc;
    return true;
}

void SSPrecipRenderer::drawMaterial(SSPrecipSim* sim, S32 material)
{
    U32 quads_total = 0;
    for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
    {
        quads_total += mRanges[material][t].mQuads;
    }
    if (quads_total == 0) return;

    for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
    {
        const Range& range = mRanges[material][t];
        if (range.mQuads == 0) continue;

        LLViewerTexture* texturep = sim->texture((U8)t);
        if (!texturep) continue; // default particle texture not created yet
        texturep->addTextureStats(128.f * 128.f);
        gGL.getTexUnit(0)->bind(texturep);

        // Rebind for every draw: the texture bind above flushes gGL, which
        // can swap LLRender's own vertex buffer in underneath us
        mVB->setBuffer();
        mVB->drawRange(LLRender::TRIANGLES,
                       range.mStartQuad * 4,
                       (range.mStartQuad + range.mQuads) * 4 - 1,
                       range.mQuads * 6,
                       range.mStartQuad * 6);
    }
}

void SSPrecipRenderer::render()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_PRECIP_RENDER);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo->sim();
    if (!sim || sim->empty()) return;

    if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD) return;
    if (LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender || LLPipeline::sShadowRender || gCubeSnapshot) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (camera->cameraUnderWater()) return;

    const LLVector3 cam_pos = camera->getOrigin();
    const LLVector3 cam_right = -camera->getLeftAxis();
    const LLVector3 cam_up = camera->getUpAxis();

    // Fade bands against the active preset's radii; particles left over from
    // a previous preset just ride the same bands out
    const SSPrecipPreset& preset = atmo->preset();
    F32 bands[TIER_COUNT][4];
    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        SSPrecipSim::tierBands((SSPrecipTier)t, preset, bands[t][0], bands[t][1], bands[t][2], bands[t][3]);
    }

    for (S32 m = 0; m < MAT_COUNT; ++m)
    {
        for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
        {
            mBuckets[m][t].clear();
        }
    }

    // Live opacity tweaks, applied at draw time so the sliders affect
    // particles already in the air
    static LLCachedControl<F32> drop_alpha_setting(gSavedSettings, "SSAtmoDropAlpha", 1.f);
    static LLCachedControl<F32> ripple_alpha_setting(gSavedSettings, "SSAtmoRippleAlpha", 1.f);
    const F32 drop_alpha = llclamp((F32)drop_alpha_setting, 0.f, 2.f);
    const F32 ripple_alpha = llclamp((F32)ripple_alpha_setting, 0.f, 2.f);

    // Read from the live preset rather than carried on each stream: it is one
    // number for every stream in sight, and one left over from a preset
    // that has just been edited should pick the new shape up along with the
    // rest of the weather rather than keeping the old one until it fades.
    const F32 stream_stretch = llclamp(preset.mStreamStretch, 0.f, 1.f);

    for (const SSPrecipParticle& p : sim->particles())
    {
        const F32 dx = p.mPos.mV[VX] - cam_pos.mV[VX];
        const F32 dy = p.mPos.mV[VY] - cam_pos.mV[VY];
        const F32 dist = sqrtf(dx * dx + dy * dy);
        const F32 alpha = p.mAlpha * drop_alpha * ageFade(p) * bandFade(dist, bands[p.mTier]);
        if (alpha < 0.004f) continue;
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    for (const SSPrecipParticle& p : sim->ripples())
    {
        // A ripple or a splash is spending its life dying, so it ramps off
        // over the whole of it. A drip off an eave is not: it is a drop in
        // flight and has to stay a drop until it lands, so it takes the
        // ordinary in/out fade instead - and the ripple opacity dial, which
        // is about splashes, leaves it alone.
        // A stream is neither: it is a standing feature of the eave that fades
        // in when the gutter starts running and out when it stops, so it takes
        // the ordinary in/out ramp over a long life and none of the ripple
        // dial either.
        const bool drip = (p.mFlags & (PART_DRIP | PART_STREAM)) != 0;
        const F32 alpha = drip ? (p.mAlpha * drop_alpha * ageFade(p))
                               : (p.mAlpha * ripple_alpha * (1.f - p.mAge / p.mMaxAge));
        if (alpha < 0.004f) continue;
        // Impact effects carry their own material too: mana shards are emissive
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    // Eave streams. Anchored and long lived, so they take the plain in/out
    // ramp and none of the distance banding: one is only ever a few metres
    // from the roof it is running off.
    for (const SSPrecipParticle& p : sim->streams())
    {
        const F32 alpha = p.mAlpha * drop_alpha * ageFade(p);
        if (alpha < 0.004f) continue;
        mBuckets[p.mMaterial % MAT_COUNT][p.mTex % SS_PRECIP_MAX_TEXTURES].push_back({ &p, llmin(alpha, 1.f) });
    }

    // Streams are many quads each, so this counts quads rather than items
    U32 total = 0;
    for (S32 m = 0; m < MAT_COUNT; ++m)
    {
        for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
        {
            for (const Item& item : mBuckets[m][t])
            {
                total += (item.mPart->mKind == KIND_STREAM)
                       ? (U32)SS_STREAM_SEGMENTS : 1u;
            }
        }
    }
    total = llmin(total, MAX_QUADS);
    if (total == 0) return;

    if (!ensureBuffer(total)) return;

    {
    LL_RECORD_BLOCK_TIME(FTM_SS_PRECIP_BUILD);
    LLStrider<LLVector3> verticesp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;
    mVB->getVertexStrider(verticesp, 0, total * 4);
    mVB->getColorStrider(colorsp, 0, total * 4);
    mVB->getTexCoord0Strider(texcoordsp, 0, total * 4);

    // One quad's worth of geometry, colour and texture span. Streams call this
    // once per segment with a different slice of the texture each time;
    // everything else calls it once with the whole of it.
    // One segment of a stream's ribbon. Unlike a quad it carries a colour at
    // each end, because a stream fades as it falls and a colour per segment
    // made every join between them a visible step.
    //
    // Its width is one number for both ends, and deliberately so. Tapering the
    // ribbon meant unequal ends, and a trapezoid textured as two triangles is
    // interpolated affinely: the halves disagree about where the art goes and
    // every drop kinks along the diagonal between them. A stream that narrows
    // is not worth drawing the water in zig-zags for.
    //
    // Corners go out in the same order emitQuad writes them, so both share the
    // one index buffer.
    auto emitRibbon = [&](const LLVector3& top, const LLVector3& bot,
                          const LLVector3& across, F32 half,
                          const LLColor4U& col_top, const LLColor4U& col_bot,
                          F32 u0, F32 u1, F32 v_top, F32 v_bot)
    {
        *verticesp++ = bot - across * half;
        *verticesp++ = top - across * half;
        *verticesp++ = bot + across * half;
        *verticesp++ = top + across * half;

        *texcoordsp++ = LLVector2(u0, v_bot);
        *texcoordsp++ = LLVector2(u0, v_top);
        *texcoordsp++ = LLVector2(u1, v_bot);
        *texcoordsp++ = LLVector2(u1, v_top);

        *colorsp++ = col_bot;
        *colorsp++ = col_top;
        *colorsp++ = col_bot;
        *colorsp++ = col_top;
    };

    auto emitQuad = [&](const LLVector3& pos,
                        const LLVector3& x_axis, const LLVector3& y_axis,
                        const LLColor4U& col, F32 u0, F32 u1, F32 v0, F32 v1)
    {
        *verticesp++ = pos - x_axis + y_axis;
        *verticesp++ = pos - x_axis - y_axis;
        *verticesp++ = pos + x_axis + y_axis;
        *verticesp++ = pos + x_axis - y_axis;

        *texcoordsp++ = LLVector2(u0, v1);
        *texcoordsp++ = LLVector2(u0, v0);
        *texcoordsp++ = LLVector2(u1, v1);
        *texcoordsp++ = LLVector2(u1, v0);

        *colorsp++ = col;
        *colorsp++ = col;
        *colorsp++ = col;
        *colorsp++ = col;
    };

    U32 written = 0;
    for (S32 m = 0; m < MAT_COUNT; ++m)
    {
        for (U32 t = 0; t < SS_PRECIP_MAX_TEXTURES; ++t)
        {
            Range& range = mRanges[m][t];
            range.mStartQuad = written;
            U32 quads = 0;
            for (const Item& item : mBuckets[m][t])
            {
                const SSPrecipParticle& p = *item.mPart;

                // A stream is a ribbon of segments, everything else is one
                // quad. Checked against the cap up front so a stream is never
                // written half in.
                const U32 need = (p.mKind == KIND_STREAM) ? (U32)SS_STREAM_SEGMENTS : 1u;
                if (written + quads + need > total) break;

                LLVector3 x_axis, y_axis;
                LLVector3 pos = p.mPos;
                switch (p.mKind)
                {
                    case KIND_STREAM:
                    {
                        quads += emitStream(p, item.mAlpha, stream_stretch, emitRibbon);
                        continue;
                    }
                    case KIND_STREAK:
                    case KIND_SHEET:
                    {
                        // Texture-up runs against the motion so a streak's
                        // bright head leads the fall
                        y_axis = -p.mVel;
                        if (y_axis.normVec() < 0.0001f) y_axis = LLVector3::z_axis;
                        x_axis = y_axis % (pos - cam_pos);
                        if (x_axis.normVec() < 0.0001f) x_axis = cam_right;
                        x_axis *= p.mSizeX;
                        y_axis *= p.mSizeY;
                        break;
                    }
                    case KIND_FLAT:
                    {
                        // Surface-aligned expanding ring with fast-out ease
                        F32 tt = p.mAge / p.mMaxAge;
                        tt = 1.f - (1.f - tt) * (1.f - tt);
                        const F32 s = lerp(p.mSizeX, p.mSizeY, tt);
                        x_axis = p.mNormal % LLVector3::z_axis;
                        if (x_axis.normVec() < 0.0001f) x_axis = LLVector3::x_axis;
                        y_axis = p.mNormal % x_axis;
                        y_axis.normVec();
                        x_axis *= s;
                        y_axis *= s;
                        break;
                    }
                    default: // KIND_ROUND
                        x_axis = cam_right * p.mSizeX;
                        y_axis = cam_up * p.mSizeY;
                        break;
                }

                // Emissive types brighten through their tint. Large abstraction
                // quads get less of it: they are additively blended, so a full
                // boost on a 16x32m sheet saturates into a blob.
                const F32 tier_boost = (p.mTier == TIER_DROPS) ? 1.f
                                     : (p.mTier == TIER_CLUSTERS) ? 0.55f : 0.3f;
                const F32 boost = 1.f + p.mGlow * 1.5f * tier_boost;
                LLColor4U col((U8)llmin((S32)(p.mTint.mV[0] * boost), 255),
                              (U8)llmin((S32)(p.mTint.mV[1] * boost), 255),
                              (U8)llmin((S32)(p.mTint.mV[2] * boost), 255),
                              (U8)llclamp((S32)(item.mAlpha * 255.f), 0, 255));

                emitQuad(pos, x_axis, y_axis, col, 0.f, 1.f, 0.f, 1.f);
                ++quads;
            }
            range.mQuads = quads;
            written += quads;
        }
    }
    mVB->unmapBuffer();
    }

    // Late translucent pass: depth-tested against the scene, no depth
    // writes, glow buffer protected
    {
    LL_RECORD_BLOCK_TIME(FTM_SS_PRECIP_DRAW);
    LL_PROFILE_GPU_ZONE("atmo precip");
    LLGLSPipelineAlpha gls_pipeline_alpha;
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                  LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    // Shared fullbright setup: the emissive pass and every fallback path
    // run through it
    auto bind_fullbright = []()
    {
        LLGLSLShader* shader = &gDeferredFullbrightProgram;
        shader->bind();

        static LLCachedControl<F32> displayGamma(gSavedSettings, "RenderDeferredDisplayGamma");
        const F32 gamma = displayGamma;
        shader->uniform1f(LLShaderMgr::DISPLAY_GAMMA, (gamma > 0.1f) ? 1.0f / gamma : (1.0f / 2.2f));
        static LLStaticHashedString waterSign("waterSign");
        shader->uniform1f(waterSign, 1.f);
        shader->uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);
        shader->setMinimumAlpha(0.f);
    };

    // Lit pass: non-emissive particles (snow, ripples) shaded like other
    // lit alpha objects: probe ambient plus shadowed sun. The deferred bind
    // supplies shadow maps, probes and environment uniforms.
    {
        LLGLSLShader* lit = &gSSPrecipLitProgram;
        if (lit->isComplete())
        {
            gPipeline.bindDeferredShaderFast(*lit);
            lit->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);
            drawMaterial(sim, MAT_LIT);
        }
        else
        {
            bind_fullbright();
            drawMaterial(sim, MAT_LIT);
        }
    }

    // Emissive pass: embers, mana hail and PBR emissives are light sources
    // themselves, so they add to the scene rather than occluding it, and
    // write into the glow channel so post-process bloom picks them up
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE,
                  LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE);
    bind_fullbright();
    drawMaterial(sim, MAT_EMISSIVE);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                  LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    // Water pass: rain family through the refraction/env/specular shader,
    // falling back to the (still bound) fullbright path when it didn't
    // compile or is toggled off
    {
        static LLCachedControl<bool> use_rain_shader(gSavedSettings, "SSAtmoRainShader", true);
        LLGLSLShader* rain = &gSSPrecipRainProgram;
        if (use_rain_shader && rain->isComplete())
        {
            rain->bind();
            rain->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);

            // sceneMap only exists with SSR enabled; without it the shader
            // transmits probe irradiance instead
            static LLStaticHashedString ssRefract("ss_refract_strength");
            const bool has_scene = gPipeline.mSceneMap.getWidth() > 0;
            rain->uniform1f(ssRefract, has_scene ? 0.035f : 0.f);

            gPipeline.bindReflectionProbes(*rain);
            drawMaterial(sim, MAT_WATER);
            gPipeline.unbindReflectionProbes(*rain);
        }
        else
        {
            drawMaterial(sim, MAT_WATER);
        }
    }

    LLGLSLShader::unbind();
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    }
}

// </SS:Nexii>
