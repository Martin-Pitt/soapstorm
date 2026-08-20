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

#include "lldrawable.h"
#include "lldrawpoolalpha.h"
#include "llglstates.h"
#include "llrender.h"
#include "llfasttimer.h"
#include "llstaticstringtable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvovolume.h"
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
static const U32 VB_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                         | LLVertexBuffer::MAP_TANGENT
                         | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;

// How far a surface-aligned ripple is pushed along the view ray, as a
// fraction of its distance from the camera, and the least it is ever pushed.
//
// The lift along the surface normal a ripple is spawned with is enough to
// keep it out of the ground it sits on geometrically, but not enough to win
// the depth test: a decal lying nearly parallel to the surface under it has
// almost no depth separation from that surface at a grazing angle, which is
// exactly the angle most ripples are seen at, and they drop out in patches.
// Sliding the quad along the ray to the camera is a pure depth bias - a point
// moved toward the eye along its own view ray does not move on screen at all -
// and taking it as a fraction of the distance keeps the bias constant in the
// depth buffer's own terms rather than vanishing into precision out at range.
static const F32 RIPPLE_DEPTH_BIAS = 0.0035f;
static const F32 RIPPLE_DEPTH_BIAS_MIN = 0.02f;

// Extra lift along the surface normal, as a fraction of the ring's current
// half-size. Tolerates roughly five degrees of disagreement between the plane
// the ripple was given and the ground actually under its far edge.
static const F32 RIPPLE_NORMAL_LIFT = 0.09f;

// What a splash crown's size is worth at each end of its life, against the
// preset's crown size. It starts as near a point as it can be seen at and
// spreads past the nominal size as it goes, so the preset's number reads as
// the size the crown passes through rather than a cap on it.
static const F32 CROWN_START_SCALE = 0.15f;
static const F32 CROWN_END_SCALE = 1.5f;

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
    //
    // Which of the two right angles matters, because the pitch below is signed
    // along this axis and the runoff network measured it along the other one.
    // The network's run axis is z cross out; taking out cross z here instead
    // gave the same line pointing the other way, so a curtain off a rake
    // leaned up where the rake went down and every stream on a gable was
    // mirrored about the horizontal.
    LLVector3 along(p.mVel.mV[VX], p.mVel.mV[VY], 0.f);
    if (along.normVec() < 0.0001f) along = LLVector3::x_axis;
    along = LLVector3::z_axis % along;
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

        // Ask the bound shader which unit its diffuseMap landed on rather than
        // assuming zero. Every one of these passes carries a different set of
        // samplers - the projector pass pulls in the whole of deferredUtil for
        // its projection maths - and the channel a sampler is assigned depends
        // on which others are present.
        LLGLSLShader* cur = LLGLSLShader::sCurBoundShaderPtr;
        S32 tex_channel = cur ? cur->getTextureChannel(LLShaderMgr::DIFFUSE_MAP) : 0;
        if (tex_channel < 0) tex_channel = 0;
        gGL.getTexUnit(tex_channel)->bind(texturep);

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
    LLStrider<LLVector3> normalsp;
    LLStrider<LLVector4a> tangentsp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;
    mVB->getVertexStrider(verticesp, 0, total * 4);
    mVB->getNormalStrider(normalsp, 0, total * 4);
    mVB->getTangentStrider(tangentsp, 0, total * 4);
    mVB->getColorStrider(colorsp, 0, total * 4);
    mVB->getTexCoord0Strider(texcoordsp, 0, total * 4);

    // The normal the next quad goes out with. Billboards carry the direction
    // to the camera, which is the normal the lit shader used to assume for
    // everything; a ripple carries the normal of the surface it is lying on,
    // so it takes the sun and the shadow map the way that surface does rather
    // than the way a flake hanging in the air does.
    LLVector3 emit_normal(0.f, 0.f, 1.f);

    // The quad's long axis, alongside the normal. A billboard is a flat card
    // standing in for something round, and the rain shader needs to know which
    // way that something is lying to shade it as water rather than as a
    // screen-aligned smear: for a streak it is the fall, for a ribbon the run
    // of the water down it.
    LLVector3 emit_axis(0.f, 0.f, 1.f);

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

        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;

        const LLVector4a axis4(emit_axis.mV[VX], emit_axis.mV[VY], emit_axis.mV[VZ], 1.f);
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;

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

        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;
        *normalsp++ = emit_normal;

        const LLVector4a axis4(emit_axis.mV[VX], emit_axis.mV[VY], emit_axis.mV[VZ], 1.f);
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;
        *tangentsp++ = axis4;

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

                // Everything but a ripple is a billboard of some sort, so it
                // starts out facing the eye and the flat case overwrites it
                LLVector3 to_cam = cam_pos - pos;
                const F32 cam_dist = to_cam.normVec();
                emit_normal = (cam_dist > 0.0001f) ? to_cam : LLVector3::z_axis;
                emit_axis = LLVector3::z_axis;

                switch (p.mKind)
                {
                    case KIND_STREAM:
                    {
                        // A ribbon hangs in the plane of its own fall, so the
                        // water runs down it rather than across
                        emit_axis = LLVector3::z_axis;
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

                        // Captured before the axes are scaled: this is the
                        // thread of water the rain shader wraps its droplet
                        // normal around
                        emit_axis = y_axis;

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

                        // Lit as the surface it lies on, not as a billboard
                        emit_normal = p.mNormal;
                        emit_axis = x_axis;

                        // Lift along the normal, growing with the ring. The
                        // spawn lift is a fixed clearance for a point; a ring
                        // that has opened to a third of a metre reaches well
                        // past the sample its normal was measured at, and the
                        // surface under the far side of it is only ever
                        // approximately the plane it was handed. This buys a
                        // few degrees of that divergence across the whole
                        // disc, which is what a flat quad on real ground
                        // needs and what a depth bias alone cannot give it.
                        pos += p.mNormal * (RIPPLE_NORMAL_LIFT * s);

                        // Depth bias along the view ray. A ring that has grown
                        // wide reaches further over ground that is not flat
                        // under all of it, so the floor of the bias grows with
                        // it rather than staying at the spawn lift.
                        pos += to_cam * llmax(RIPPLE_DEPTH_BIAS_MIN + s * 0.1f,
                                              cam_dist * RIPPLE_DEPTH_BIAS);
                        break;
                    }
                    default: // KIND_ROUND
                    {
                        // A splash crown opens from a point the way the ring
                        // beside it does, and keeps growing past its nominal
                        // size as it fades: the water thrown up spreads as it
                        // goes, so a crown that held one size read as a dot
                        // being switched off rather than a splash dispersing.
                        // Every other round particle is a drop and holds the
                        // size it was spawned at.
                        F32 scale = 1.f;
                        if (p.mFlags & PART_CROWN)
                        {
                            F32 tt = llclamp(p.mAge / p.mMaxAge, 0.f, 1.f);
                            tt = 1.f - (1.f - tt) * (1.f - tt);  // fast out
                            scale = lerp(CROWN_START_SCALE, CROWN_END_SCALE, tt);
                        }
                        // A round drop stands for a sphere, so which way its
                        // axis points barely matters, but the fall is the one
                        // direction it is actually stretched along
                        emit_axis = (p.mVel.magVecSquared() > 0.0001f)
                                  ? -p.mVel * (1.f / p.mVel.magVec()) : LLVector3::z_axis;

                        x_axis = cam_right * (p.mSizeX * scale);
                        y_axis = cam_up * (p.mSizeY * scale);
                        break;
                    }
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

    // The drop's own surface, shared by the daylight and the projector passes.
    // Both shade the same water off the same coverage gradient, so they read
    // the same dials: a drop that is round in the sun cannot flatten out the
    // moment a spotlight crosses it.
    auto bind_drop_shading = [](LLGLSLShader* shader)
    {
        static LLCachedControl<F32> drop_bulge(gSavedSettings, "SSAtmoDropBulge", 0.35f);
        static LLCachedControl<F32> drop_core(gSavedSettings, "SSAtmoDropCore", 2.0f);
        static LLCachedControl<F32> drop_sparkle(gSavedSettings, "SSAtmoDropSparkle", 6.0f);

        static LLStaticHashedString ssDropBulge("ss_drop_bulge");
        static LLStaticHashedString ssDropCore("ss_drop_core");
        static LLStaticHashedString ssDropSparkle("ss_drop_sparkle");

        shader->uniform1f(ssDropBulge, llclamp((F32)drop_bulge, 0.f, 4.f));
        shader->uniform1f(ssDropCore, llclamp((F32)drop_core, 0.f, 8.f));
        shader->uniform1f(ssDropSparkle, llclamp((F32)drop_sparkle, 0.f, 64.f));
    };

    // Whether the ring art bound for the decal pass is the one that is baked
    // here, which carries the wave's tangent-space normal in its colour
    // channels. A developer-set ripple texture is ordinary art whose colour is
    // a colour, so the shaders have to be told which they are looking at, and
    // since every decal in the buffer is a ripple that is one flag for the
    // whole pass rather than one per batch.
    const F32 decal_normals = atmo->rippleTexture() ? 0.f : 1.f;
    static LLStaticHashedString ssDecalNormals("ss_decal_normals");

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
    //
    // Ripples go through the same shader with the decal flag set, which adds
    // the light already sitting on the surface they are lying on. Two draws
    // rather than one so the flag can differ; the program stays bound across
    // both, so it is a uniform change and a second submission, not a rebind.
    {
        static LLStaticHashedString ssDecal("ss_decal");
        static LLStaticHashedString ssSceneLit("ss_scene_lit");
        LLGLSLShader* lit = &gSSPrecipLitProgram;
        if (lit->isComplete())
        {
            // Force the full bind. The fast path re-binds the light function,
            // the shadow maps and the probes but leaves the environment
            // uniforms - sun direction, shadow matrices, shadow bias - at
            // whatever the last full bind left on the program, and those go
            // stale the moment the camera or the sun moves. Sampling the
            // shadow map through last frame's matrices is what left ripples
            // looking the same in sun and in shade. LLDrawPoolAlpha clears
            // the flag for its own deferred-environment shaders for exactly
            // this reason.
            lit->mCanBindFast = false;
            gPipeline.bindDeferredShaderFast(*lit);
            lit->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);

            // The scene map is only allocated with HDR on; without it there is
            // no lit scene to read back off the ground
            lit->uniform1f(ssSceneLit, (gPipeline.mSceneMap.getWidth() > 0) ? 1.f : 0.f);

            lit->uniform1f(ssDecalNormals, decal_normals);

            lit->uniform1f(ssDecal, 0.f);
            drawMaterial(sim, MAT_LIT);

            lit->uniform1f(ssDecal, 1.f);
            drawMaterial(sim, MAT_DECAL);
            lit->uniform1f(ssDecal, 0.f);
        }
        else
        {
            // Fallback for a lit program that would not compile. The ring's
            // colour channels are a normal here and the fullbright path has no
            // way to be told that, so it multiplies them through as a tint and
            // the ripples come out pale blue. Left as it is rather than baking
            // and tracking a second flat ring for a path that has already given
            // up every other part of the shading; the log says why it is on.
            bind_fullbright();
            drawMaterial(sim, MAT_LIT);
            drawMaterial(sim, MAT_DECAL);
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
            bind_drop_shading(rain);

            gPipeline.bindReflectionProbes(*rain);
            drawMaterial(sim, MAT_WATER);
            gPipeline.unbindReflectionProbes(*rain);
        }
        else
        {
            drawMaterial(sim, MAT_WATER);
        }
    }

    // Projector pass: one additive draw of everything per nearby projected
    // light, so a spotlight picks itself out against heavy rain the way it
    // does against fog. Batched particles have no per-object light list to run
    // a forward light loop over - one buffer spans the whole visible scene -
    // so the light is iterated instead of the geometry, which is the same
    // trade the deferred pipeline makes for opaque surfaces.
    //
    // Bounded hard: it is a full redraw of the precipitation buffer per light,
    // and heavy rain is tens of thousands of quads.
    {
        static LLCachedControl<bool> use_projectors(gSavedSettings, "SSAtmoProjectorLights", true);
        static LLCachedControl<U32> max_projectors(gSavedSettings, "SSAtmoProjectorLightCount", 2);
        static LLCachedControl<F32> scatter_gain(gSavedSettings, "SSAtmoProjectorGain", 0.1f);
        static LLCachedControl<F32> scatter_aniso(gSavedSettings, "SSAtmoProjectorAnisotropy", 0.3f);

        LLGLSLShader* proj = &gSSPrecipProjProgram;
        const U32 want = llclamp((U32)max_projectors, 0u, 8u);

        if (use_projectors && want > 0 && proj->isComplete())
        {
            std::vector<LLDrawable*> projectors;
            gPipeline.getNearbyProjectors(projectors, want);

            if (!projectors.empty())
            {
                LL_PROFILE_GPU_ZONE("atmo precip projectors");

                // Additive: a beam adds light to the drops, it does not
                // replace what they were already shaded with. Glow is left
                // alone - rain lit by a spotlight is not itself a bloom
                // source, and writing it would haze the whole beam.
                gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE,
                              LLRender::BF_ZERO, LLRender::BF_ONE);

                proj->bind();
                proj->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)gGLViewport[2], (F32)gGLViewport[3]);

                static LLStaticHashedString ssScatterGain("ss_scatter_gain");
                static LLStaticHashedString ssScatterAniso("ss_scatter_aniso");
                static LLStaticHashedString ssProjDecal("ss_decal");
                proj->uniform1f(ssScatterGain, llclamp((F32)scatter_gain, 0.f, 2.f));
                proj->uniform1f(ssScatterAniso, llclamp((F32)scatter_aniso, 0.f, 0.95f));
                bind_drop_shading(proj);
                proj->uniform1f(ssDecalNormals, decal_normals);

                const glm::mat4 view = get_current_modelview();

                for (LLDrawable* drawablep : projectors)
                {
                    LLVOVolume* volume = drawablep->getVOVolume();
                    if (!volume) continue;

                    // Everything the projector maths needs: matrix, near plane,
                    // range, ambiance and the projection texture. Shared with
                    // the deferred spot pass rather than reimplemented, so the
                    // beam lands in the same place on the rain as it does on
                    // the wall behind it.
                    gPipeline.setupSpotLight(*proj, drawablep);

                    // The deferred pass carries the light centre as a varying
                    // off the light volume it draws; there is no volume here,
                    // so it goes in already transformed.
                    const LLVector3 center_agent = drawablep->getPositionAgent();
                    const glm::vec3 c = mul_mat4_vec3(view, glm::vec3(center_agent.mV[VX],
                                                                      center_agent.mV[VY],
                                                                      center_agent.mV[VZ]));
                    const LLVector3 center_view(c.x, c.y, c.z);

                    const LLColor3 col = volume->getLightLinearColor();

                    proj->uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, center_view.mV);
                    proj->uniform1f(LLShaderMgr::LIGHT_SIZE, volume->getLightRadius() * 1.5f);
                    proj->uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, col.mV);
                    // 0.5 is the DEFERRED_LIGHT_FALLOFF the deferred passes
                    // use; it is a file-local constant over in pipeline.cpp,
                    // so it is spelled out rather than shared
                    proj->uniform1f(LLShaderMgr::LIGHT_FALLOFF, volume->getLightFalloff(0.5f));

                    // Emissive particles are light sources in their own
                    // right; shining a spotlight on one adds nothing.
                    //
                    // The ripples go in with the decal flag up: they are wet
                    // ground being lit, not drops scattering the beam on the
                    // way through, and the shader shades them accordingly.
                    proj->uniform1f(ssProjDecal, 0.f);
                    drawMaterial(sim, MAT_LIT);
                    drawMaterial(sim, MAT_WATER);

                    proj->uniform1f(ssProjDecal, 1.f);
                    drawMaterial(sim, MAT_DECAL);
                }

                proj->disableTexture(LLShaderMgr::DEFERRED_PROJECTION);
                gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                              LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
            }
        }
    }

    LLGLSLShader::unbind();
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
    }
}

// </SS:Nexii>
