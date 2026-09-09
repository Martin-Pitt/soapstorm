/**
 * @file sscombatoverlay.cpp
 * @brief Combat Log in-world overlay: the gate, the world figure, and the pick model.
 *        The world mirrors the Events pane: one icon per event in SSCombatLog::visibleEvents(), plus whatever
 *        is hovered or selected even when scrolled away. The caller (LLPipeline::renderDebug) binds gUIProgram
 *        and owns the shader; everything here only emits immediate-mode geometry in agent space.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "sscombatoverlay.h"

#include "fscombathitmarker.h"
#include "sscombatcamera.h"
#include "sscombaticons.h"
#include "sscombatreconstruct.h"

#include "indra_constants.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglstates.h"
#include "llhudrender.h"
#include "llimagegl.h"
#include "llmenugl.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsd.h"
#include "lluictrlfactory.h"
#include "llui.h"
#include "lluiimage.h"
#include "lluuid.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

extern bool gSnapshot;
extern bool gCubeSnapshot;

// ---------------------------------------------------------------------------------------------------------
// Palette. One named colour per meaning: direction is carried by geometry (arrowheads), never by hue, so
// every entry below names a *kind of thing* and never a direction.
// ---------------------------------------------------------------------------------------------------------

// Whatever the officer last clicked; the one colour that outranks everything else.
static const LLColor4 COL_SELECTION(1.00f, 0.95f, 0.38f, 1.f);
// Whatever the pointer is over right now. Transient, so it is dimmer than selection at rest.
static const LLColor4 COL_HOVER(0.85f, 0.92f, 1.00f, 1.f);
// A hovered world icon's ring: brighter than COL_HOVER so a highlighted icon is unmistakable at a glance.
static const LLColor4 COL_HOVER_BRIGHT(1.00f, 1.00f, 1.00f, 1.f);
// 3D-anchored label text.
static const LLColor4 COL_LABEL(0.93f, 0.93f, 0.96f, 1.f);
// A damage type the palette below has no entry for.
static const LLColor4 COL_DAMAGE_UNKNOWN(0.80f, 0.80f, 0.84f, 1.f);
// A measured projectile flight from the ghost-projectile store: solid, because it was seen.
static const LLColor4 COL_FLIGHT(0.96f, 0.92f, 0.72f, 1.f);
// The killer/hit line in a detective-view freeze-frame.
static const LLColor4 COL_KILLER_LINE(1.00f, 0.58f, 0.22f, 1.f);
// The attacker's party colour: warm orange, used consistently for the attacker's ghost, label and trail
// everywhere the Combat Log draws one (this overlay's detective view and Reconstruction's replay both call
// ghostColor(), so the two never drift apart).
static const LLColor4 COL_ATTACKER(1.00f, 0.62f, 0.20f, 1.f);
// The victim's party colour: cool cyan-blue, the attacker's warm orange complement.
static const LLColor4 COL_VICTIM(0.35f, 0.75f, 1.00f, 1.f);
// A ghost that is neither party: present, but not the focus (Reconstruction's wider cast, mostly).
static const LLColor4 COL_GHOST(0.78f, 0.80f, 0.86f, 1.f);

// Damage-type hues, keyed by the display name FSCombatHitMarker already curates, so the overlay and the
// hitmarker's symbol table can never drift apart on which number means which type.
namespace
{
    struct DamagePaletteEntry { const char* mName; F32 mR, mG, mB; };
}
static const DamagePaletteEntry DAMAGE_PALETTE[] = {
    { "Impact",      0.78f, 0.78f, 0.80f },
    { "Generic",     0.90f, 0.86f, 0.80f },
    { "Acid",        0.55f, 0.90f, 0.35f },
    { "Bludgeoning", 0.72f, 0.55f, 0.35f },
    { "Cold",        0.55f, 0.85f, 1.00f },
    { "Electric",    0.60f, 0.75f, 1.00f },
    { "Fire",        1.00f, 0.55f, 0.20f },
    { "Force",       0.72f, 0.55f, 1.00f },
    { "Necrotic",    0.40f, 0.62f, 0.42f },
    { "Piercing",    0.80f, 0.86f, 0.92f },
    { "Poison",      0.70f, 0.95f, 0.30f },
    { "Psychic",     0.95f, 0.45f, 0.90f },
    { "Radiant",     1.00f, 0.95f, 0.60f },
    { "Slashing",    1.00f, 0.42f, 0.42f },
    { "Sonic",       0.40f, 0.90f, 0.85f },
    { "Emotional",   1.00f, 0.62f, 0.80f },
    { "Medical",     0.45f, 0.95f, 0.60f },
    { "Repair",      0.60f, 0.90f, 0.75f },
    { "Explosive",   1.00f, 0.68f, 0.25f },
    { "Crushing",    0.80f, 0.62f, 0.40f },
    { "Anti-Armor",  1.00f, 0.80f, 0.35f },
    { "Suffocation", 0.62f, 0.68f, 0.75f },
    { "Redeploy",    0.55f, 0.80f, 1.00f },
};

// Damage type number to overlay colour, via the hitmarker's curated name for that number.
LLColor4 SSCombatDraw::damageTypeColor(S16 type)
{
    const char* name = NULL;
    const S32 count = FSCombatHitMarker::getDamageTypeCount();
    for (S32 i = 0; i < count; ++i)
    {
        const FSCombatHitMarker::DamageTypeInfo& info = FSCombatHitMarker::getDamageTypeInfo(i);
        if (info.mType == (S32)type)
        {
            name = info.mName;
            break;
        }
    }
    if (name)
    {
        for (const DamagePaletteEntry& entry : DAMAGE_PALETTE)
        {
            if (strcmp(entry.mName, name) == 0)
            {
                return LLColor4(entry.mR, entry.mG, entry.mB, 1.f);
            }
        }
    }
    return COL_DAMAGE_UNKNOWN;
}

// The shared ghost hue: subject (victim/target, cool blue) or source (killer/attacker, warm orange) or
// neither, alpha baked in.
LLColor4 SSCombatDraw::ghostColor(bool subject, bool source)
{
    LLColor4 color = subject ? COL_VICTIM : (source ? COL_ATTACKER : COL_GHOST);
    color.mV[VALPHA] = (subject || source) ? 0.60f : 0.25f;
    return color;
}

// ---------------------------------------------------------------------------------------------------------
// SSCombatDraw: the shared draw list.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    struct DrawSeg
    {
        LLVector3 mA, mB;
        LLColor4  mCA, mCB;
        U8        mWidth = SSCombatDraw::WIDTH_THIN;
    };
    struct DrawTri
    {
        LLVector3 mV[3];
        LLColor4  mC;
    };
    struct DrawLabel
    {
        std::string mText;
        LLVector3   mPos;
        LLColor4    mColor;
    };
    struct DrawPick
    {
        LLRect              mRect;
        SSCombat::NounRef   mRef;
    };
    struct IconQuad
    {
        LLPointer<LLUIImage> mImage;
        LLVector3            mCentre;
        F32                  mHalf = 0.f;
        LLColor4             mColor;
    };

    std::vector<DrawSeg>    sSegs[SSCombatDraw::LAYER_COUNT];
    std::vector<DrawTri>    sTris;
    std::vector<DrawLabel>  sLabels;
    std::vector<DrawPick>   sPicks;
    std::vector<IconQuad>   sIcons;
    U32                     sPickFrame = 0xFFFFFFFFu;

    const F32 LINE_WIDTHS[3] = { 1.f, 3.f, 6.f };
}

// Region-local store coordinates become agent space through the region the agent is standing in.
LLVector3 SSCombatDraw::agentFromRegion(const LLVector3& region_pos)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return region_pos;
    }
    return region->getPosAgentFromRegion(region_pos);
}

// One screen pixel in metres at that world point; the camera's ratio is pixels per metre at unit distance.
F32 SSCombatDraw::metresPerPixelAt(const LLVector3& pos_agent)
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const F32 ratio = camera->getPixelMeterRatio();
    if (ratio <= 0.f)
    {
        return 0.f;
    }
    return llmax(0.f, cameraDistance(pos_agent)) / ratio;
}

// Distance from the render camera to a point.
F32 SSCombatDraw::cameraDistance(const LLVector3& pos_agent)
{
    return (pos_agent - LLViewerCamera::getInstance()->getOrigin()).magVec();
}

// Segment count for a ring, falling with apparent size and clamped to [8,64] so a distant ring is cheap.
S32 SSCombatDraw::ringSegments(const LLVector3& centre_agent, F32 radius)
{
    const F32 mpp = metresPerPixelAt(centre_agent);
    const F32 px = (mpp > 0.f) ? (radius / mpp) : 64.f;
    return (S32)llclamp((S32)(px * 0.5f), 8, 64);
}

// Drops last frame's geometry; pick rects survive until the frame counter moves so every module that draws
// this frame (overlay first, reconstruction second) accumulates into one pick list.
void SSCombatDraw::clear()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != sPickFrame)
    {
        sPickFrame = frame;
        sPicks.clear();
    }
    sSegs[LAYER_LINE].clear();
    sSegs[LAYER_MARKER].clear();
    sTris.clear();
    sLabels.clear();
    sIcons.clear();
}

// One line segment with its own end colours, so per-vertex alpha can carry age along a trail.
void SSCombatDraw::seg(ELayer layer, const LLVector3& a, const LLVector3& b, const LLColor4& ca, const LLColor4& cb, U8 width)
{
    DrawSeg s;
    s.mA = a;
    s.mB = b;
    s.mCA = ca;
    s.mCB = cb;
    s.mWidth = (U8)llclamp((S32)width, 0, 2);
    sSegs[layer].push_back(s);
}

// A dashed run between two points; used for the reconstructed (as opposed to measured) geometry.
void SSCombatDraw::dashedSeg(ELayer layer, const LLVector3& a, const LLVector3& b, const LLColor4& ca, const LLColor4& cb, U8 width)
{
    const S32 dashes = 7;
    for (S32 i = 0; i < dashes; ++i)
    {
        const F32 t0 = (F32)i / (F32)dashes;
        const F32 t1 = t0 + 0.55f / (F32)dashes;
        LLColor4 c0 = lerp(ca, cb, t0);
        LLColor4 c1 = lerp(ca, cb, t1);
        seg(layer, lerp(a, b, t0), lerp(a, b, t1), c0, c1, width);
    }
}

// A horizontal ring on the ground plane.
void SSCombatDraw::ring(ELayer layer, const LLVector3& centre_agent, F32 radius, const LLColor4& color, U8 width)
{
    const S32 segments = ringSegments(centre_agent, radius);
    LLVector3 prev(centre_agent.mV[VX] + radius, centre_agent.mV[VY], centre_agent.mV[VZ]);
    for (S32 i = 1; i <= segments; ++i)
    {
        const F32 angle = F_TWO_PI * (F32)i / (F32)segments;
        const LLVector3 cur(centre_agent.mV[VX] + radius * cosf(angle),
                            centre_agent.mV[VY] + radius * sinf(angle),
                            centre_agent.mV[VZ]);
        seg(layer, prev, cur, color, color, width);
        prev = cur;
    }
}

// A camera-facing ring outline: the icon selection/hover halo, built with the same axes as disc().
void SSCombatDraw::ringFacing(const LLVector3& centre_agent, F32 radius, const LLColor4& color, U8 width)
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLVector3 right = camera->getLeftAxis() * -1.f;
    LLVector3 up = camera->getUpAxis();
    const S32 segments = llclamp(ringSegments(centre_agent, radius), 8, 32);
    LLVector3 prev = centre_agent + right * radius;
    for (S32 i = 1; i <= segments; ++i)
    {
        const F32 angle = F_TWO_PI * (F32)i / (F32)segments;
        const LLVector3 cur = centre_agent + right * (radius * cosf(angle)) + up * (radius * sinf(angle));
        seg(LAYER_MARKER, prev, cur, color, color, width);
        prev = cur;
    }
}

// A three-axis cross.
void SSCombatDraw::cross(ELayer layer, const LLVector3& centre_agent, F32 size, const LLColor4& color, U8 width)
{
    const LLVector3& c = centre_agent;
    seg(layer, LLVector3(c.mV[VX] - size, c.mV[VY], c.mV[VZ]), LLVector3(c.mV[VX] + size, c.mV[VY], c.mV[VZ]), color, color, width);
    seg(layer, LLVector3(c.mV[VX], c.mV[VY] - size, c.mV[VZ]), LLVector3(c.mV[VX], c.mV[VY] + size, c.mV[VZ]), color, color, width);
    seg(layer, LLVector3(c.mV[VX], c.mV[VY], c.mV[VZ] - size), LLVector3(c.mV[VX], c.mV[VY], c.mV[VZ] + size), color, color, width);
}

// The facing arrow. Direction is geometry, so this is the only thing that says which way somebody was looking.
void SSCombatDraw::arrow(ELayer layer, const LLVector3& base_agent, F32 yaw, F32 length, const LLColor4& color, U8 width)
{
    const LLVector3 dir(cosf(yaw), sinf(yaw), 0.f);
    const LLVector3 side(-dir.mV[VY], dir.mV[VX], 0.f);
    const LLVector3 tip = base_agent + dir * length;
    seg(layer, base_agent, tip, color, color, width);
    seg(layer, tip, tip - dir * (length * 0.32f) + side * (length * 0.20f), color, color, width);
    seg(layer, tip, tip - dir * (length * 0.32f) - side * (length * 0.20f), color, color, width);
}

// One filled triangle; the ghost capsules are the only thing that needs fill.
void SSCombatDraw::tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color)
{
    DrawTri t;
    t.mV[0] = a;
    t.mV[1] = b;
    t.mV[2] = c;
    t.mC = color;
    sTris.push_back(t);
}

// A camera-facing filled disc, built as a fan of triangles so it reads at any alt-cam angle.
void SSCombatDraw::disc(const LLVector3& centre_agent, F32 radius, const LLColor4& color)
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLVector3 right = camera->getLeftAxis() * -1.f;
    LLVector3 up = camera->getUpAxis();
    const S32 segments = llclamp(ringSegments(centre_agent, radius), 8, 24);
    LLVector3 prev = centre_agent + right * radius;
    for (S32 i = 1; i <= segments; ++i)
    {
        const F32 angle = F_TWO_PI * (F32)i / (F32)segments;
        const LLVector3 cur = centre_agent + right * (radius * cosf(angle)) + up * (radius * sinf(angle));
        tri(centre_agent, prev, cur, color);
        prev = cur;
    }
}

// A translucent capsule standing on its feet: a cylinder of ten segments with both caps, no depth writes.
void SSCombatDraw::capsule(const LLVector3& feet, F32 radius, F32 height, const LLColor4& color)
{
    const S32 segments = 10;
    const LLVector3 top(feet.mV[VX], feet.mV[VY], feet.mV[VZ] + height);
    LLVector3 prev_b(feet.mV[VX] + radius, feet.mV[VY], feet.mV[VZ]);
    LLVector3 prev_t(prev_b.mV[VX], prev_b.mV[VY], top.mV[VZ]);
    for (S32 i = 1; i <= segments; ++i)
    {
        const F32 angle = F_TWO_PI * (F32)i / (F32)segments;
        const LLVector3 cur_b(feet.mV[VX] + radius * cosf(angle), feet.mV[VY] + radius * sinf(angle), feet.mV[VZ]);
        const LLVector3 cur_t(cur_b.mV[VX], cur_b.mV[VY], top.mV[VZ]);
        tri(prev_b, cur_b, cur_t, color);
        tri(prev_b, cur_t, prev_t, color);
        tri(feet, cur_b, prev_b, color);
        tri(top, prev_t, cur_t, color);
        prev_b = cur_b;
        prev_t = cur_t;
    }
}

// One ghost: capsule, outline ring, yaw arrow, name label, and (when pickable) a pick rect. The single body
// shared by the overlay's detective view and by Reconstruction's replay. alpha_scale multiplies every colour
// so a hover preview can draw the identical body ghosted without the caller re-deriving colours. flags is kept
// in the signature for whichever ESampleFlags visual comes next; the mouselook eye that used to read it here
// is gone (owner correction 2026-09-09: it was never actually a line-of-sight cue, and the real one now lives
// on the kill line's own midpoint instead -- see sscombatreconstruct.cpp's draw_killer_line()).
void SSCombatDraw::ghostBody(const LLVector3& feet_agent, F32 yaw, U16 flags, const LLColor4& color,
                             const std::string& label, const SSCombat::NounRef& pickRef, bool pickable, F32 alpha_scale)
{
    (void)flags;
    LLColor4 body = color;
    body.mV[VALPHA] *= alpha_scale;
    capsule(feet_agent, 0.32f, 1.80f, body);

    LLColor4 outline = body;
    outline.mV[VALPHA] = llmin(1.f, body.mV[VALPHA] + 0.30f);
    ring(LAYER_MARKER, feet_agent, 0.42f, outline, WIDTH_THIN);
    arrow(LAYER_MARKER, feet_agent + LLVector3(0.f, 0.f, 0.05f), yaw, 1.1f, outline, WIDTH_MID);
    LLColor4 label_color = COL_LABEL;
    label_color.mV[VALPHA] *= alpha_scale;
    pushLabel(label, feet_agent + LLVector3(0.f, 0.f, 2.05f), label_color);
    if (pickable)
    {
        pushPick(feet_agent + LLVector3(0.f, 0.f, 1.f), 12, pickRef);
    }
}

// A camera-facing textured quad sampling the given UI atlas image; queued because each icon binds its own
// texture and cannot share a batch with the untextured lists.
void SSCombatDraw::icon(const LLVector3& centre_agent, LLPointer<LLUIImage> image, F32 half_size, const LLColor4& color)
{
    if (!image)
    {
        return;
    }
    IconQuad i;
    i.mImage = image;
    i.mCentre = centre_agent;
    i.mHalf = half_size;
    i.mColor = color;
    sIcons.push_back(i);
}

// Queues a 3D-anchored label; labels are emitted last because the font path rebinds textures and matrices.
void SSCombatDraw::pushLabel(const std::string& text, const LLVector3& pos_agent, const LLColor4& color)
{
    if (text.empty())
    {
        return;
    }
    DrawLabel l;
    l.mText = text;
    l.mPos = pos_agent;
    l.mColor = color;
    sLabels.push_back(l);
}

// Projects a marker anchor to a screen rect and files it for the pick model.
void SSCombatDraw::pushPick(const LLVector3& pos_agent, S32 half_px, const SSCombat::NounRef& ref)
{
    if (!ref.valid())
    {
        return;
    }
    LLCoordGL screen;
    if (!LLViewerCamera::getInstance()->projectPosAgentToScreen(pos_agent, screen, true))
    {
        return;
    }
    DrawPick p;
    p.mRect = LLRect(screen.mX - half_px, screen.mY + half_px, screen.mX + half_px, screen.mY - half_px);
    p.mRef = ref;
    sPicks.push_back(p);
}

// Emits one width bucket of one layer; the caller owns depth state, this only touches line width and colour.
static void emit_bucket(const std::vector<DrawSeg>& segs, U8 bucket, F32 alpha_scale)
{
    bool any = false;
    for (const DrawSeg& s : segs)
    {
        if (s.mWidth == bucket)
        {
            any = true;
            break;
        }
    }
    if (!any)
    {
        return;
    }
    gGL.setLineWidth(LINE_WIDTHS[bucket]);
    gGL.begin(LLRender::LINES);
    for (const DrawSeg& s : segs)
    {
        if (s.mWidth != bucket)
        {
            continue;
        }
        gGL.color4f(s.mCA.mV[VRED], s.mCA.mV[VGREEN], s.mCA.mV[VBLUE], s.mCA.mV[VALPHA] * alpha_scale);
        gGL.vertex3fv(s.mA.mV);
        gGL.color4f(s.mCB.mV[VRED], s.mCB.mV[VGREEN], s.mCB.mV[VBLUE], s.mCB.mV[VALPHA] * alpha_scale);
        gGL.vertex3fv(s.mB.mV);
    }
    gGL.end();
}

// Draws one line layer at a global alpha multiplier, restoring the line width to 1 when it is done.
void SSCombatDraw::emitLines(ELayer layer, F32 alpha_scale)
{
    const std::vector<DrawSeg>& segs = sSegs[layer];
    if (segs.empty())
    {
        return;
    }
    emit_bucket(segs, WIDTH_THIN, alpha_scale);
    emit_bucket(segs, WIDTH_MID, alpha_scale);
    emit_bucket(segs, WIDTH_THICK, alpha_scale);
    gGL.setLineWidth(1.f);
}

// Draws the filled list (the ghost capsules) at a global alpha multiplier.
void SSCombatDraw::emitTris(F32 alpha_scale)
{
    if (sTris.empty())
    {
        return;
    }
    gGL.begin(LLRender::TRIANGLES);
    for (const DrawTri& t : sTris)
    {
        gGL.color4f(t.mC.mV[VRED], t.mC.mV[VGREEN], t.mC.mV[VBLUE], t.mC.mV[VALPHA] * alpha_scale);
        gGL.vertex3fv(t.mV[0].mV);
        gGL.vertex3fv(t.mV[1].mV);
        gGL.vertex3fv(t.mV[2].mV);
    }
    gGL.end();
}

// Draws the queued icons: one bind and one quad per icon, camera-facing, sampling gUIProgram's diffuse map.
void SSCombatDraw::emitIcons(F32 alpha_scale)
{
    if (sIcons.empty())
    {
        return;
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 right = camera->getLeftAxis() * -1.f;
    const LLVector3 up = camera->getUpAxis();
    for (const IconQuad& i : sIcons)
    {
        LLPointer<LLTexture> tex = i.mImage->getImage();
        if (!tex)
        {
            continue;
        }
        // Skip a quad whose texture has not actually uploaded any mip yet (discard level still -1): binding it
        // would sample whatever placeholder GL leaves bound rather than show nothing, which reads as a stray
        // symbol hanging in the world instead of an icon that has not loaded.
        LLImageGL* gl_tex = tex->getGLTexture();
        if (!gl_tex || gl_tex->getDiscardLevel() < 0)
        {
            continue;
        }
        gGL.getTexUnit(0)->bind(tex);
        const LLRectf& uv = i.mImage->getClipRegion();
        const LLVector3 tl = i.mCentre - right * i.mHalf + up * i.mHalf;
        const LLVector3 tr = i.mCentre + right * i.mHalf + up * i.mHalf;
        const LLVector3 bl = i.mCentre - right * i.mHalf - up * i.mHalf;
        const LLVector3 br = i.mCentre + right * i.mHalf - up * i.mHalf;
        gGL.color4f(i.mColor.mV[VRED], i.mColor.mV[VGREEN], i.mColor.mV[VBLUE], i.mColor.mV[VALPHA] * alpha_scale);
        gGL.begin(LLRender::TRIANGLES);
        gGL.texCoord2f(uv.mLeft, uv.mTop);      gGL.vertex3fv(tl.mV);
        gGL.texCoord2f(uv.mLeft, uv.mBottom);   gGL.vertex3fv(bl.mV);
        gGL.texCoord2f(uv.mRight, uv.mTop);     gGL.vertex3fv(tr.mV);
        gGL.texCoord2f(uv.mRight, uv.mTop);     gGL.vertex3fv(tr.mV);
        gGL.texCoord2f(uv.mLeft, uv.mBottom);   gGL.vertex3fv(bl.mV);
        gGL.texCoord2f(uv.mRight, uv.mBottom);  gGL.vertex3fv(br.mV);
        gGL.end();
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// Draws the queued labels; hud_render_utf8text pushes its own matrices and must run outside a begin/end pair.
void SSCombatDraw::emitLabels()
{
    if (sLabels.empty())
    {
        return;
    }
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (!font)
    {
        return;
    }
    for (const DrawLabel& l : sLabels)
    {
        hud_render_utf8text(l.mText, l.mPos, *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW,
                            -0.5f * font->getWidthF32(l.mText), 3.f, l.mColor, false);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// True when nothing at all was queued this frame, so the caller can skip its state changes.
bool SSCombatDraw::empty()
{
    return sSegs[LAYER_LINE].empty() && sSegs[LAYER_MARKER].empty() && sTris.empty() && sLabels.empty() && sIcons.empty();
}

// ---------------------------------------------------------------------------------------------------------
// Gate.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // Reset every frame the camera is in mouselook or over-the-shoulder; the overlay only draws once this has
    // been running for the grace period, so a firefight never has an analysis figure hanging in front of it.
    LLFrameTimer sSinceMouselook;
    bool         sTimersStarted = false;
    // What wantsDraw() last returned, so drawLegend() (a separate 2D-pass call) knows without recomputing the
    // gate a second time this frame.
    bool         sWantedDrawLastFrame = false;
}

// Forward declaration: defined below in the figure-building section, but polled from wantsDraw() (here) on
// every frame regardless of the gate, so a floater-driven selection flies the camera immediately.
static void check_camera_vantage(const SSCombatLog& store);

// Polls the mouselook/OTS posture timer. Called from wantsDraw(), which the render block calls every frame.
static void poll_posture_timers()
{
    if (!sTimersStarted)
    {
        sSinceMouselook.start();
        sTimersStarted = true;
    }

    if (gAgentCamera.cameraMouselook() || gAgentCamera.getCameraMode() == CAMERA_MODE_OTS)
    {
        sSinceMouselook.reset();
    }
}

// The draw gate proper, split out so wantsDraw() can cache its result for drawLegend().
static bool compute_wants_draw()
{
    static LLCachedControl<bool> overlay_on(gSavedSettings, "SSCombatLogOverlay", true);
    if (!overlay_on)
    {
        return false;
    }
    if (gSnapshot || gCubeSnapshot)
    {
        return false;
    }
    if (!SSCombatLog::instanceExists())
    {
        return false;
    }
    // Either pane is enough: the Events list and a Combatant page both want the world figure underneath them.
    if (!LLFloaterReg::instanceVisible("ss_combat_events") && !LLFloaterReg::instanceVisible("ss_combat_combatant"))
    {
        return false;
    }
    // No stationary or focus-off-avatar requirement: the officer can be standing in ordinary third person, or
    // walking, and still see the figure; only mouselook/OTS (and its grace tail) hide it.
    if (gAgentCamera.cameraMouselook() || gAgentCamera.getCameraMode() == CAMERA_MODE_OTS)
    {
        return false;
    }
    static LLCachedControl<F32> grace(gSavedSettings, "SSCombatLogMouselookGrace", 5.f);
    if (sSinceMouselook.getElapsedTimeF32() < llmax(0.f, (F32)grace))
    {
        return false;
    }
    return gAgent.getRegion() != NULL;
}

// static
bool SSCombatOverlay::wantsDraw()
{
    // The timer is polled before every early-out, because a gate that only samples while it is already open
    // would let a single mouselook frame slip through unnoticed.
    poll_posture_timers();

    // Camera flight reacts to a floater-driven selection change on every frame this runs, which is every
    // rendered frame regardless of what this function goes on to return: the flight itself is what puts the
    // officer into alt-cam, so it cannot wait for the figure's own (much narrower) draw gate to open first.
    if (SSCombatLog::instanceExists())
    {
        check_camera_vantage(SSCombatLog::instance());
    }

    sWantedDrawLastFrame = compute_wants_draw();
    return sWantedDrawLastFrame;
}

// ---------------------------------------------------------------------------------------------------------
// Figure building: one icon per visible/selected/hovered event, plus the detective-view freeze-frame.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // The three distance bands, used only to cap trail cost and reach in the detective view.
    enum EBand : U8 { BAND_NEAR = 0, BAND_MID = 1, BAND_FAR = 2 };

    // Two rendering strengths for the detective freeze-frame: the officer's pinned selection always reads at
    // full strength; a differing hover is a preview and reads ghostly, so the two are never confused when both
    // show at once (a pinned death while the pointer rests on some other icon, say).
    const F32 DETECTIVE_ALPHA_PINNED = 1.00f;
    const F32 DETECTIVE_ALPHA_HOVER  = 0.35f;
    const U8  DETECTIVE_LINE_PINNED  = SSCombatDraw::WIDTH_MID;   // 3 px bucket
    const U8  DETECTIVE_LINE_HOVER   = SSCombatDraw::WIDTH_THIN;  // 1 px bucket
    // A multi-attacker death: the recorded killing blow draws at full alpha, every other attributed attacker
    // dims to this fraction so the one that actually landed the kill still reads first.
    const F32 ATTACKER_BLOW_ALPHA  = 1.00f;
    const F32 ATTACKER_OTHER_ALPHA = 0.60f;
    // Where an arrow from an attacker ghost toward its victim is drawn, and how far above the feet a ghost's
    // "head" is for the secondary event icon floated over a victim.
    const F32 GHOST_SHOULDER_HEIGHT = 1.5f;
    const F32 GHOST_HEAD_HEIGHT     = 1.8f;

    // The pinned view's full hit/flight enumeration (owner addition 2026-09-09): a DEATH's related hits and
    // flights read at slightly less than full strength so the victim/attacker ghosts and kill line still stand
    // out, and a DAMAGE's sibling volley hits dim further still since they are context, not the selected hit.
    const F32 RELATED_HIT_ALPHA    = 0.85f;
    const F32 VOLLEY_SIBLING_ALPHA = 0.45f;
    // Per-pinned-view caps, nearest to the event's own time first.
    const size_t MAX_RELATED_HITS    = 40;
    const size_t MAX_RELATED_FLIGHTS = 40;
    // Unrelated icons dim to this while something is pinned, so the pinned detective view stands out; they
    // keep their pick rects so the officer can still click past them onto something else.
    const F32 UNRELATED_ICON_ALPHA = 0.30f;

    // Mouse-down state for the 4 px / 350 ms pick rule.
    bool sArmed = false;
    S32  sDownX = 0, sDownY = 0;
    F64  sDownTime = 0.0;
    S32  sHoverX = 0, sHoverY = 0;

    // Camera-vantage bookkeeping: a selection this module itself just made never flies the camera.
    SSCombat::NounRef sLastSeenSelection;
    SSCombat::NounRef sLastWorldClickSelection;
    LLFrameTimer      sSinceFly;
    bool              sFlyTimerStarted = false;

    LLHandle<LLContextMenu> sPickStackHandle;
}

// Which LOD band a point falls in; used only to cap the detective view's trail reach and cost.
static EBand band_for(const LLVector3& pos_agent)
{
    static LLCachedControl<F32> lod_near(gSavedSettings, "SSCombatLogLodNear", 40.f);
    static LLCachedControl<F32> lod_far(gSavedSettings, "SSCombatLogLodFar", 120.f);
    const F32 d = SSCombatDraw::cameraDistance(pos_agent);
    if (d <= llmax(1.f, (F32)lod_near))
    {
        return BAND_NEAR;
    }
    if (d <= llmax(2.f, (F32)lod_far))
    {
        return BAND_MID;
    }
    return BAND_FAR;
}

// Evidence quality to one of the {1,3,6} width buckets: bridge > viewer > coarse.
static U8 width_for_sample(const SSCombat::Sample& sample)
{
    if (sample.mSource == SSCombat::SAMPLE_BRIDGE)
    {
        return SSCombatDraw::WIDTH_THICK;
    }
    if (sample.mSource == SSCombat::SAMPLE_COARSE || sample.mQuality < 128)
    {
        return SSCombatDraw::WIDTH_THIN;
    }
    return SSCombatDraw::WIDTH_MID;
}

// Draws one combatant's trail over the window, alpha ramped old to new and width bucketed by sample quality
// (unless force_thin narrows every segment to the 1 px bucket, for the hover-ghosted pass). Shared by the
// DEATH detective view (victim/killer over the previous 6 s).
static void draw_trail(const SSCombatLog& store, const LLUUID& id, const LLColor4& team, F64 from, F64 to,
                       F32 alpha_scale = 1.f, bool force_thin = false)
{
    if (to <= from)
    {
        return;
    }
    SSCombat::Sample probe;
    if (!store.sampleAt(id, to, probe))
    {
        return;
    }
    const EBand band = band_for(SSCombatDraw::agentFromRegion(probe.mPos));
    if (band == BAND_FAR)
    {
        return;
    }
    const S32 steps = (band == BAND_NEAR) ? 48 : 24;

    bool have_prev = false;
    LLVector3 prev;
    LLColor4 prev_color = team;
    for (S32 i = 0; i <= steps; ++i)
    {
        const F64 t = from + (to - from) * ((F64)i / (F64)steps);
        SSCombat::Sample sample;
        if (!store.sampleAt(id, t, sample))
        {
            have_prev = false; // a gap is absent, not bridged; sliding across it would be a lie
            continue;
        }
        const LLVector3 pos = SSCombatDraw::agentFromRegion(sample.mPos) + LLVector3(0.f, 0.f, 0.08f);
        LLColor4 color = team;
        color.mV[VALPHA] = (0.15f + 0.85f * ((F32)i / (F32)steps)) * alpha_scale;
        // A teleport-flagged sample starts a new polyline segment; the edge into it is never drawn, so the
        // trail never shows the officer walking from the death spot to the respawn point.
        const bool breaks = (sample.mFlags & SSCombat::FLAG_TELEPORT) != 0;
        if (have_prev && !breaks)
        {
            const U8 width = force_thin ? SSCombatDraw::WIDTH_THIN : width_for_sample(sample);
            SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, prev, pos, prev_color, color, width);
        }
        prev = pos;
        prev_color = color;
        have_prev = true;
    }
}

// The measured polyline of one observed projectile flight, solid because it was seen, terminal tick and all.
static void draw_flight(const SSCombat::Flight& flight, F32 alpha_scale = 1.f)
{
    if (flight.mPath.size() < 2)
    {
        return;
    }
    for (size_t i = 1; i < flight.mPath.size(); ++i)
    {
        const LLVector3 a = SSCombatDraw::agentFromRegion(flight.mPath[i - 1].second);
        const LLVector3 b = SSCombatDraw::agentFromRegion(flight.mPath[i].second);
        LLColor4 color = COL_FLIGHT;
        color.mV[VALPHA] = (0.30f + 0.70f * ((F32)i / (F32)flight.mPath.size())) * alpha_scale;
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, a, b, color, color, SSCombatDraw::WIDTH_THIN);
    }
    if (flight.mHitGeometry)
    {
        LLColor4 color = COL_FLIGHT;
        color.mV[VALPHA] *= alpha_scale;
        SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, SSCombatDraw::agentFromRegion(flight.mPath.back().second),
                            0.2f, color, SSCombatDraw::WIDTH_THIN);
    }
}

// The flight (if any) whose measured hit was this DAMAGE event.
static const SSCombat::Flight* find_flight_for_damage(const SSCombatLog& store, U32 damageEventId)
{
    if (damageEventId == 0)
    {
        return NULL;
    }
    for (const SSCombat::Flight& flight : store.flights())
    {
        if (flight.mDamageEvent == damageEventId)
        {
            return &flight;
        }
    }
    return NULL;
}

// A two-barb arrowhead at tip, pointing back along -dir; flights are not always horizontal, so this cannot
// reuse SSCombatDraw::arrow()'s ground-plane yaw version.
static void draw_arrowhead_3d(const LLVector3& tip, LLVector3 dir, const LLColor4& color, U8 width)
{
    if (!dir.normalize())
    {
        return;
    }
    LLVector3 side = dir % LLVector3(0.f, 0.f, 1.f);
    if (side.magVecSquared() < 0.0001f)
    {
        side = LLVector3(1.f, 0.f, 0.f);
    }
    side.normalize();
    const F32 BARB_LEN = 0.35f;
    const LLVector3 back = dir * -1.f;
    SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, tip, tip + (back + side) * (BARB_LEN * 0.7071f), color, color, width);
    SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, tip, tip + (back - side) * (BARB_LEN * 0.7071f), color, color, width);
}

// A related flight in the pinned detective view: solid (no per-segment fade, unlike draw_flight's ramp) with
// an arrowhead at the end, so a dense cluster of flights still reads as arrows rather than as fading streaks.
static void draw_flight_pinned(const SSCombat::Flight& flight, F32 alpha_scale)
{
    if (flight.mPath.size() < 2)
    {
        return;
    }
    LLColor4 color = COL_FLIGHT;
    color.mV[VALPHA] *= alpha_scale;
    for (size_t i = 1; i < flight.mPath.size(); ++i)
    {
        const LLVector3 a = SSCombatDraw::agentFromRegion(flight.mPath[i - 1].second);
        const LLVector3 b = SSCombatDraw::agentFromRegion(flight.mPath[i].second);
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, a, b, color, color, SSCombatDraw::WIDTH_THIN);
    }
    const LLVector3 tip = SSCombatDraw::agentFromRegion(flight.mPath.back().second);
    const LLVector3 prev = SSCombatDraw::agentFromRegion(flight.mPath[flight.mPath.size() - 2].second);
    draw_arrowhead_3d(tip, tip - prev, color, SSCombatDraw::WIDTH_THIN);
    if (flight.mHitGeometry)
    {
        SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, tip, 0.2f, color, SSCombatDraw::WIDTH_THIN);
    }
}

// One related hit's own evidence in the pinned view: the attacker->victim line at the hit's own time (not the
// selected event's time), tinted by damage type, a small impact disc, and the damage number. Shared by the
// DEATH pinned view's full hit list and the DAMAGE pinned view's dimmer volley siblings.
static void draw_hit_line(const SSCombatLog& store, const SSCombat::Event& hit, F32 alpha_scale)
{
    SSCombat::Sample attacker_sample, target_sample;
    const bool have_attacker = hit.mOwner.notNull() && store.sampleAt(hit.mOwner, hit.mTime, attacker_sample);
    const bool have_target = store.sampleAt(hit.mTarget, hit.mTime, target_sample);
    if (!have_attacker || !have_target)
    {
        return;
    }
    const LLVector3 a = SSCombatDraw::agentFromRegion(attacker_sample.mPos) + LLVector3(0.f, 0.f, 1.6f);
    const LLVector3 b = SSCombatDraw::agentFromRegion(target_sample.mPos) + LLVector3(0.f, 0.f, 1.2f);
    LLColor4 color = SSCombatDraw::damageTypeColor(hit.mType);
    color.mV[VALPHA] *= alpha_scale;
    SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, a, b, color, color, SSCombatDraw::WIDTH_THIN);
    SSCombatDraw::disc(b, 0.12f, color);
    SSCombatDraw::pushLabel(llformat("%.1f", hit.mDamage), b + LLVector3(0.f, 0.f, 0.18f), color);
}

// Every DAMAGE against the DEATH's own target inside the attribution window, nearest to the death's own time
// first and capped at MAX_RELATED_HITS: the raw evidence attribution()'s mShares was aggregated from.
static std::vector<const SSCombat::Event*> gather_death_hits(const SSCombatLog& store, const SSCombat::Event& ev, F32 window)
{
    std::vector<const SSCombat::Event*> hits;
    const F64 from = ev.mTime - (F64)window;
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = store.eventIndexAt(from); i < events.size(); ++i)
    {
        const SSCombat::Event& hit = events[i];
        if (hit.mTime > ev.mTime)
        {
            break;
        }
        if (hit.mKind != SSCombat::EVENT_DAMAGE || hit.mTarget != ev.mTarget)
        {
            continue;
        }
        hits.push_back(&hit);
    }
    std::stable_sort(hits.begin(), hits.end(), [&ev](const SSCombat::Event* a, const SSCombat::Event* b)
    {
        return fabs(a->mTime - ev.mTime) < fabs(b->mTime - ev.mTime);
    });
    if (hits.size() > MAX_RELATED_HITS)
    {
        hits.resize(MAX_RELATED_HITS);
    }
    return hits;
}

// Every flight that is evidence for this DEATH: its mDamageEvent is one of the hits already gathered above, or
// (a flight the log's own damage-match missed) its end falls inside the window within 3 m of the victim.
// Nearest to the death's own time first, capped at MAX_RELATED_FLIGHTS.
static std::vector<const SSCombat::Flight*> gather_death_flights(const SSCombatLog& store, const SSCombat::Event& ev,
                                                                  const std::vector<const SSCombat::Event*>& hits,
                                                                  F32 window, const LLVector3& victim_region_pos)
{
    std::vector<const SSCombat::Flight*> out;
    const F64 from = ev.mTime - (F64)window;
    for (const SSCombat::Flight& flight : store.flights())
    {
        bool matched = false;
        for (const SSCombat::Event* hit : hits)
        {
            if (flight.mDamageEvent != 0 && flight.mDamageEvent == hit->mId)
            {
                matched = true;
                break;
            }
        }
        if (!matched && flight.mEnd >= from && flight.mEnd <= ev.mTime && !flight.mPath.empty())
        {
            matched = ((flight.mPath.back().second - victim_region_pos).magVec() <= 3.f);
        }
        if (matched)
        {
            out.push_back(&flight);
        }
    }
    std::stable_sort(out.begin(), out.end(), [&ev](const SSCombat::Flight* a, const SSCombat::Flight* b)
    {
        return fabs(a->mEnd - ev.mTime) < fabs(b->mEnd - ev.mTime);
    });
    if (out.size() > MAX_RELATED_FLIGHTS)
    {
        out.resize(MAX_RELATED_FLIGHTS);
    }
    return out;
}

// The other hits of the same volley as a DAMAGE event: same owner, rezzer and target, within 2 s either side.
static std::vector<const SSCombat::Event*> gather_damage_siblings(const SSCombatLog& store, const SSCombat::Event& ev)
{
    std::vector<const SSCombat::Event*> siblings;
    const F64 from = ev.mTime - 2.0;
    const F64 to = ev.mTime + 2.0;
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = store.eventIndexAt(from); i < events.size(); ++i)
    {
        const SSCombat::Event& sib = events[i];
        if (sib.mTime > to)
        {
            break;
        }
        if (sib.mId == ev.mId || sib.mKind != SSCombat::EVENT_DAMAGE)
        {
            continue;
        }
        if (sib.mOwner != ev.mOwner || sib.mRezzer != ev.mRezzer || sib.mTarget != ev.mTarget)
        {
            continue;
        }
        siblings.push_back(&sib);
    }
    std::stable_sort(siblings.begin(), siblings.end(), [&ev](const SSCombat::Event* a, const SSCombat::Event* b)
    {
        return fabs(a->mTime - ev.mTime) < fabs(b->mTime - ev.mTime);
    });
    if (siblings.size() > MAX_RELATED_HITS)
    {
        siblings.resize(MAX_RELATED_HITS);
    }
    return siblings;
}

// The event ids the pinned selection makes "related" for icon fading: itself, plus a DEATH's gathered hits or
// a DAMAGE's volley siblings. Empty when nothing is selected or the selection is not a DEATH/DAMAGE.
static std::unordered_set<U32> compute_related_event_ids(const SSCombatLog& store, const SSCombat::NounRef& selection)
{
    std::unordered_set<U32> ids;
    if (selection.mType != SSCombat::NOUN_DEATH && selection.mType != SSCombat::NOUN_DAMAGE)
    {
        return ids;
    }
    const SSCombat::Event* ev = store.event(selection.mIndex);
    if (!ev)
    {
        return ids;
    }
    ids.insert(ev->mId);
    if (ev->mKind == SSCombat::EVENT_DEATH || ev->mKind == SSCombat::EVENT_OBJECT_DEATH)
    {
        const SSCombat::Attribution attribution = store.attribution(ev->mId);
        for (const SSCombat::Event* hit : gather_death_hits(store, *ev, attribution.mWindow))
        {
            ids.insert(hit->mId);
        }
    }
    else if (ev->mKind == SSCombat::EVENT_DAMAGE)
    {
        for (const SSCombat::Event* sib : gather_damage_siblings(store, *ev))
        {
            ids.insert(sib->mId);
        }
    }
    return ids;
}

// "12s ago" / "3m ago" / "1h 4m ago": the near-context relative-time label.
static std::string format_relative_age(F64 seconds)
{
    const S64 s = (S64)(llmax(0.0, seconds) + 0.5);
    if (s < 60)
    {
        return llformat("%llds ago", (long long)s);
    }
    if (s < 3600)
    {
        return llformat("%lldm ago", (long long)(s / 60));
    }
    return llformat("%lldh %lldm ago", (long long)(s / 3600), (long long)((s % 3600) / 60));
}

// The UI atlas image for a name, cached: LLUI::getUIImage() is a lookup the icon figure would otherwise
// repeat every event, every frame.
static LLPointer<LLUIImage> icon_image(const std::string& name)
{
    static std::unordered_map<std::string, LLPointer<LLUIImage> > cache;
    if (name.empty())
    {
        return NULL;
    }
    std::unordered_map<std::string, LLPointer<LLUIImage> >::iterator it = cache.find(name);
    if (it != cache.end())
    {
        return it->second;
    }
    LLPointer<LLUIImage> image = LLUI::getUIImage(name);
    cache[name] = image;
    return image;
}

// The atlas icon name for one event: damage type for DAMAGE, the death glyphs otherwise. Mirrors the Events
// list's own icon column so the world and the pane never disagree on which glyph means what.
static std::string icon_name_for(const SSCombat::Event& ev)
{
    switch (ev.mKind)
    {
        case SSCombat::EVENT_DAMAGE:       return SSCombatIcons::forType(ev.mType);
        case SSCombat::EVENT_DEATH:        return SSCombatIcons::forType(SSCombatIcons::ICON_DEATH);
        case SSCombat::EVENT_OBJECT_DEATH: return SSCombatIcons::forType(SSCombatIcons::ICON_OBJECT_DEATH);
        default:                           return std::string();
    }
}

// The event's world position: DEATH/object death at mTargetPos, DAMAGE at the target's (else the attacker's)
// track position at mTime. False when none of those exist, in which case the event is skipped, not smeared.
// out_id carries whichever combatant the position belongs to, so the caller can special-case its own avatar.
static bool event_icon_pos(const SSCombatLog& store, const SSCombat::Event& ev, LLVector3& out_region, LLUUID& out_id)
{
    if (ev.mKind == SSCombat::EVENT_DEATH || ev.mKind == SSCombat::EVENT_OBJECT_DEATH)
    {
        if (ev.mHasPositions && !ev.mTargetPos.isExactlyZero())
        {
            out_region = ev.mTargetPos;
            out_id = ev.mTarget;
            return true;
        }
        return false;
    }
    if (ev.mKind == SSCombat::EVENT_DAMAGE)
    {
        SSCombat::Sample sample;
        if (store.sampleAt(ev.mTarget, ev.mTime, sample))
        {
            out_region = sample.mPos;
            out_id = ev.mTarget;
            return true;
        }
        if (ev.mOwner.notNull() && store.sampleAt(ev.mOwner, ev.mTime, sample))
        {
            out_region = sample.mPos;
            out_id = ev.mOwner;
            return true;
        }
        return false;
    }
    return false;
}

// One event's icon: screen-constant size, always visible, lifted with a stem to the ground, tinted white,
// ringed on selection/hover, plus the near-context label inside 24 m. While something is pinned, an icon that
// is neither the selection nor one of its related hits/flights/parties (is_related false) dims to
// UNRELATED_ICON_ALPHA and skips its label, so the pinned detective view stands out; it keeps its pick rect
// so the officer can still click past it onto something else.
static void draw_event_icon(const SSCombatLog& store, const SSCombat::Event& ev, S32& label_budget, bool is_related)
{
    LLVector3 ground_region;
    LLUUID position_owner;
    if (!event_icon_pos(store, ev, ground_region, position_owner))
    {
        return;
    }
    LLPointer<LLUIImage> image = icon_image(icon_name_for(ev));
    if (!image)
    {
        return;
    }

    const LLVector3 ground = SSCombatDraw::agentFromRegion(ground_region);
    // The synthetic FINAL_DAMAGE cue legitimately targets the local avatar; floating its icon at head height
    // instead of the feet keeps it from sitting inside the officer's own avatar mesh.
    const bool at_local_avatar = position_owner.notNull() && position_owner == gAgent.getID();
    const LLVector3 centre = ground + LLVector3(0.f, 0.f, at_local_avatar ? (GHOST_HEAD_HEIGHT + 0.3f) : 0.3f);

    SSCombat::NounRef ref;
    ref.mType = (ev.mKind == SSCombat::EVENT_DAMAGE) ? SSCombat::NOUN_DAMAGE : SSCombat::NOUN_DEATH;
    ref.mIndex = ev.mId;
    ref.mTime = ev.mTime;
    const SSCombat::View& view = store.view();
    const bool selected = (view.mSelection == ref);
    const bool hovered = (view.mHover == ref);
    // Selected and hovered icons are always related to themselves; fading only ever dims some *other* icon
    // while the officer has one pinned.
    const bool faded = !is_related && !selected && !hovered;

    const F32 mpp = SSCombatDraw::metresPerPixelAt(centre);
    F32 half = llmax(0.02f, 11.f * mpp); // ~22 px square, screen-constant
    if (hovered)
    {
        half *= 1.3f; // the hovered icon reads larger so the pointer's target is unmistakable
    }

    LLColor4 stem_color = COL_LABEL;
    if (faded)
    {
        stem_color.mV[VALPHA] = UNRELATED_ICON_ALPHA;
    }
    SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, ground, centre, stem_color, stem_color, SSCombatDraw::WIDTH_THIN);
    const F32 icon_alpha = faded ? UNRELATED_ICON_ALPHA : ((selected || hovered) ? 1.f : 0.90f);
    SSCombatDraw::icon(centre, image, half, LLColor4(1.f, 1.f, 1.f, icon_alpha));
    if (selected)
    {
        SSCombatDraw::ringFacing(centre, half * 1.5f, COL_SELECTION, SSCombatDraw::WIDTH_MID);
    }
    else if (hovered)
    {
        // Brighter and a wider bucket than the resting hover tint, so the highlighted icon never reads the
        // same as an icon nobody is pointing at.
        SSCombatDraw::ringFacing(centre, half * 1.5f, COL_HOVER_BRIGHT, SSCombatDraw::WIDTH_MID);
    }
    // Pushed even when faded: a dimmed icon is still clickable, so the officer can pick past the pinned view
    // onto something else without having to clear the selection first.
    SSCombatDraw::pushPick(centre, 13, ref);

    // The hovered icon's context label always shows, ignoring both the label budget and the 24 m cutoff, since
    // it is the one icon the officer is actively pointing at right now. A faded icon never shows one.
    if (!faded && (hovered || (label_budget > 0 && SSCombatDraw::cameraDistance(centre) <= 24.f)))
    {
        if (!hovered)
        {
            --label_budget;
        }
        const F64 age = LLFrameTimer::getElapsedSeconds() - ev.mTime;
        SSCombatDraw::pushLabel(format_relative_age(age), centre - LLVector3(0.f, 0.f, 0.35f), COL_LABEL);
        if (ev.mKind == SSCombat::EVENT_DEATH || ev.mKind == SSCombat::EVENT_OBJECT_DEATH)
        {
            const std::string line2 = store.displayName(ev.mTarget) + " <- " + store.displayName(ev.mOwner);
            SSCombatDraw::pushLabel(line2, centre - LLVector3(0.f, 0.f, 0.62f), COL_LABEL);
        }
    }
}

// A short arrow from an attacker ghost's shoulder toward the victim's, so a multi-attacker death still reads
// which ghost is aimed at whom without relying on colour alone.
static void draw_attacker_arrow(const LLVector3& attacker_feet_agent, const LLVector3& victim_feet_agent,
                                const LLColor4& color, U8 width)
{
    LLVector3 to_victim = victim_feet_agent - attacker_feet_agent;
    to_victim.mV[VZ] = 0.f;
    const F32 dist = to_victim.magVec();
    if (dist < 0.05f)
    {
        return;
    }
    const F32 yaw = atan2f(to_victim.mV[VY], to_victim.mV[VX]);
    const LLVector3 base = attacker_feet_agent + LLVector3(0.f, 0.f, GHOST_SHOULDER_HEIGHT);
    SSCombatDraw::arrow(SSCombatDraw::LAYER_MARKER, base, yaw, llmin(1.2f, dist * 0.5f), color, width);
}

// The event's own icon (death glyph or damage-type glyph), floated 0.4 m above a victim ghost's head; distinct
// from the world icon draw_event_icon plants at the event's recorded ground position, since the ghost's
// resampled position at the event's time can differ slightly from it.
static void draw_victim_event_icon(const SSCombatLog& store, const SSCombat::Event& ev,
                                   const LLVector3& victim_feet_agent, F32 alpha_scale)
{
    LLPointer<LLUIImage> image = icon_image(icon_name_for(ev));
    if (!image)
    {
        return;
    }
    const LLVector3 centre = victim_feet_agent + LLVector3(0.f, 0.f, GHOST_HEAD_HEIGHT + 0.4f);
    const F32 half = llmax(0.02f, 11.f * SSCombatDraw::metresPerPixelAt(centre));
    SSCombatDraw::icon(centre, image, half, LLColor4(1.f, 1.f, 1.f, alpha_scale));
}

// The DEATH freeze-frame: victim and every attacker from attribution(), the kill line, an attacker->victim
// arrow per attacker, and both parties' trails over the previous 6 s. When full_detail is false (the hover
// preview) this is the whole picture, plus just the fatal blow's own flight if one was measured. When
// full_detail is true (the pinned selection) it additionally draws every DAMAGE against the same target inside
// the attribution window as its own hit line (colour by damage type, impact disc, damage number) and every
// flight that is evidence for one of those hits or ends near the victim in the window, superseding the single
// blow-flight call since the full flight list already includes it when it matches. alpha_scale/line_width/
// pickable let the caller draw this pinned (full strength) or hover-ghosted.
static void draw_death_detective(const SSCombatLog& store, const SSCombat::Event& ev, F32 alpha_scale, U8 line_width, bool pickable, bool full_detail)
{
    SSCombat::Sample victim_sample;
    bool have_victim = store.sampleAt(ev.mTarget, ev.mTime, victim_sample);
    if (ev.mHasPositions)
    {
        // The DEATH event's own target position is the simulator's, not a resampled/held track guess: it is
        // where the victim actually died, never where a respawn later moved them.
        victim_sample.mPos = ev.mTargetPos;
        have_victim = true;
    }
    LLVector3 victim_feet;
    if (have_victim)
    {
        victim_feet = SSCombatDraw::agentFromRegion(victim_sample.mPos);
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = ev.mTarget;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(victim_feet, victim_sample.mYaw, victim_sample.mFlags, SSCombatDraw::ghostColor(true, false), store.displayName(ev.mTarget), ref, pickable, alpha_scale);
        draw_victim_event_icon(store, ev, victim_feet, alpha_scale);
    }

    std::vector<LLUUID> attackers;
    if (ev.mOwner.notNull())
    {
        attackers.push_back(ev.mOwner);
    }
    const SSCombat::Attribution attribution = store.attribution(ev.mId);
    for (const SSCombat::VolleyShare& share : attribution.mShares)
    {
        if (share.mAttacker.notNull() && std::find(attackers.begin(), attackers.end(), share.mAttacker) == attackers.end())
        {
            attackers.push_back(share.mAttacker);
        }
    }
    for (size_t i = 0; i < attackers.size(); ++i)
    {
        const LLUUID& attacker = attackers[i];
        // attackers[0] is the DEATH event's own recorded owner, i.e. the killing blow, and draws at full
        // alpha; every other attributed attacker dims so the one that actually landed the kill reads first.
        const bool is_killer = (i == 0 && ev.mOwner.notNull());
        // A hitscan killing blow (weapon attached, or its own rezzer) is placed at the DEATH event's own
        // mSourcePos, the simulator's ground truth; anything else falls back to the attacker's resampled track.
        const bool killer_hitscan = is_killer && ev.mHasPositions && !ev.mSource.isNull()
                                  && (ev.mSource == ev.mRezzer || ev.mSource == ev.mOwner);

        SSCombat::Sample sample;
        const bool have_sample = store.sampleAt(attacker, ev.mTime, sample);
        if (!killer_hitscan && !have_sample)
        {
            continue;
        }
        const LLVector3 attacker_feet = killer_hitscan ? SSCombatDraw::agentFromRegion(ev.mSourcePos)
                                                        : SSCombatDraw::agentFromRegion(sample.mPos);
        const F32 attacker_yaw = have_sample ? sample.mYaw : 0.f;
        const U16 attacker_flags = have_sample ? sample.mFlags : 0;

        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = attacker;
        ref.mTime = ev.mTime;
        const F32 blow_alpha = is_killer ? ATTACKER_BLOW_ALPHA : ATTACKER_OTHER_ALPHA;
        SSCombatDraw::ghostBody(attacker_feet, attacker_yaw, attacker_flags, SSCombatDraw::ghostColor(false, true), store.displayName(attacker), ref, pickable, alpha_scale * blow_alpha);
        if (have_victim)
        {
            LLColor4 arrow_color = COL_ATTACKER;
            arrow_color.mV[VALPHA] = 0.85f * alpha_scale * blow_alpha;
            draw_attacker_arrow(attacker_feet, victim_feet, arrow_color, line_width);
        }
    }

    if (have_victim && ev.mOwner.notNull())
    {
        const bool killer_hitscan = ev.mHasPositions && !ev.mSource.isNull()
                                  && (ev.mSource == ev.mRezzer || ev.mSource == ev.mOwner);
        SSCombat::Sample killer_sample;
        const bool have_killer = killer_hitscan || store.sampleAt(ev.mOwner, ev.mTime, killer_sample);
        if (have_killer)
        {
            const LLVector3 killer_pos = killer_hitscan ? SSCombatDraw::agentFromRegion(ev.mSourcePos)
                                                         : SSCombatDraw::agentFromRegion(killer_sample.mPos);
            LLColor4 line = COL_KILLER_LINE;
            line.mV[VALPHA] *= alpha_scale;
            SSCombatDraw::seg(SSCombatDraw::LAYER_LINE,
                              killer_pos + LLVector3(0.f, 0.f, 1.6f),
                              victim_feet + LLVector3(0.f, 0.f, 1.2f),
                              line, line, line_width);
        }
    }

    if (full_detail && have_victim)
    {
        const std::vector<const SSCombat::Event*> hits = gather_death_hits(store, ev, attribution.mWindow);
        for (const SSCombat::Event* hit : hits)
        {
            draw_hit_line(store, *hit, alpha_scale * RELATED_HIT_ALPHA);
        }
        for (const SSCombat::Flight* flight : gather_death_flights(store, ev, hits, attribution.mWindow, victim_sample.mPos))
        {
            draw_flight_pinned(*flight, alpha_scale);
        }
    }
    else if (const SSCombat::Flight* flight = find_flight_for_damage(store, attribution.mBlow))
    {
        draw_flight(*flight, alpha_scale);
    }

    const F64 trail_from = ev.mTime - 6.0;
    const bool force_thin = (line_width == DETECTIVE_LINE_HOVER);
    draw_trail(store, ev.mTarget, SSCombatDraw::ghostColor(true, false), trail_from, ev.mTime, alpha_scale, force_thin);
    if (ev.mOwner.notNull())
    {
        draw_trail(store, ev.mOwner, SSCombatDraw::ghostColor(false, true), trail_from, ev.mTime, alpha_scale, force_thin);
    }
}

// The DAMAGE freeze-frame: attacker and target, the hit line tinted by damage type, an attacker->victim arrow,
// and the matched flight; that is the whole picture for the hover preview. When full_detail is true (the
// pinned selection) it additionally draws the other hits of the same volley (same owner, rezzer and target,
// within 2 s either side) as their own dimmer hit lines. alpha_scale/line_width/pickable let the caller draw
// this pinned or hover-ghosted.
static void draw_damage_detective(const SSCombatLog& store, const SSCombat::Event& ev, F32 alpha_scale, U8 line_width, bool pickable, bool full_detail)
{
    SSCombat::Sample attacker_sample, target_sample;
    const bool have_attacker = ev.mOwner.notNull() && store.sampleAt(ev.mOwner, ev.mTime, attacker_sample);
    const bool have_target = store.sampleAt(ev.mTarget, ev.mTime, target_sample);
    LLVector3 attacker_feet, target_feet;

    if (have_attacker)
    {
        attacker_feet = SSCombatDraw::agentFromRegion(attacker_sample.mPos);
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = ev.mOwner;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(attacker_feet, attacker_sample.mYaw, attacker_sample.mFlags, SSCombatDraw::ghostColor(false, true), store.displayName(ev.mOwner), ref, pickable, alpha_scale);
    }
    if (have_target)
    {
        target_feet = SSCombatDraw::agentFromRegion(target_sample.mPos);
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = ev.mTarget;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(target_feet, target_sample.mYaw, target_sample.mFlags, SSCombatDraw::ghostColor(true, false), store.displayName(ev.mTarget), ref, pickable, alpha_scale);
        draw_victim_event_icon(store, ev, target_feet, alpha_scale);
    }
    if (have_attacker && have_target)
    {
        LLColor4 line = SSCombatDraw::damageTypeColor(ev.mType);
        line.mV[VALPHA] *= alpha_scale;
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE,
                          attacker_feet + LLVector3(0.f, 0.f, 1.6f),
                          target_feet + LLVector3(0.f, 0.f, 1.2f),
                          line, line, line_width);
        LLColor4 arrow_color = COL_ATTACKER;
        arrow_color.mV[VALPHA] = 0.85f * alpha_scale;
        draw_attacker_arrow(attacker_feet, target_feet, arrow_color, line_width);
    }
    if (const SSCombat::Flight* flight = find_flight_for_damage(store, ev.mId))
    {
        draw_flight(*flight, alpha_scale);
    }
    if (full_detail)
    {
        for (const SSCombat::Event* sib : gather_damage_siblings(store, ev))
        {
            draw_hit_line(store, *sib, alpha_scale * VOLLEY_SIBLING_ALPHA);
        }
    }
}

// One pass of the detective view for one noun, at the given strength; nothing draws unless it is a DEATH/DAMAGE.
// full_detail is true only for the pinned selection; the hover preview always stays the light version.
static void draw_one_detective(const SSCombatLog& store, const SSCombat::NounRef& target, F32 alpha_scale, U8 line_width, bool pickable, bool full_detail)
{
    if (target.mType != SSCombat::NOUN_DEATH && target.mType != SSCombat::NOUN_DAMAGE)
    {
        return;
    }
    const SSCombat::Event* ev = store.event(target.mIndex);
    if (!ev)
    {
        return;
    }
    if (ev->mKind == SSCombat::EVENT_DEATH || ev->mKind == SSCombat::EVENT_OBJECT_DEATH)
    {
        draw_death_detective(store, *ev, alpha_scale, line_width, pickable, full_detail);
    }
    else if (ev->mKind == SSCombat::EVENT_DAMAGE)
    {
        draw_damage_detective(store, *ev, alpha_scale, line_width, pickable, full_detail);
    }
}

// The detective view: the officer's pinned selection always shows at full strength and stays pickable, and it
// never disappears on its own (ux clarification: only a new selection or a bare-ground click clears it); it is
// also the only pass that gets the full composed view (every related hit and flight, owner addition
// 2026-09-09). A hover that differs from the pinned selection draws ghosted on top of it, at
// DETECTIVE_ALPHA_HOVER and DETECTIVE_LINE_HOVER and always the light version, so the two states are never
// confused when both show at once.
static void draw_detective_view(const SSCombatLog& store)
{
    const SSCombat::View& view = store.view();
    // While a reconstruction is active for the pinned selection, SSCombatReconstruct::render() already draws
    // that event's full cast as a moving ghost ring at the scrub cursor; drawing the static pinned freeze-frame
    // (fixed at the event's own instant) on top of it here would double the ghosts and desync from the scrub
    // position. Selecting an event now always enters its reconstruction (rework 2026-09-09), so this branch
    // only still fires for a selection that predates that state, which no longer happens in practice but costs
    // nothing to guard against.
    if (view.mSelection.valid() && !view.mReconstruct)
    {
        draw_one_detective(store, view.mSelection, DETECTIVE_ALPHA_PINNED, DETECTIVE_LINE_PINNED, true, true);
    }
    // Hover keeps the ghosted freeze-frame regardless: it never enters reconstruction (ux rework 2026-09-09),
    // so it is the only preview left once a selection is reconstructing.
    if (view.mHover.valid() && !(view.mHover == view.mSelection))
    {
        draw_one_detective(store, view.mHover, DETECTIVE_ALPHA_HOVER, DETECTIVE_LINE_HOVER, false, false);
    }
}

// Flies the camera to a vantage of a newly selected event, but only when the officer picked it from a floater;
// a world click already recorded its own selection above so it never re-triggers this. Called from wantsDraw()
// on every frame regardless of the draw gate: the flight must happen immediately on a floater click in any
// camera mode except mouselook/OTS (it is the flight itself that puts the officer into alt-cam, so it cannot
// require alt-cam, and it must not wait out the figure's own mouselook-grace or floater-visibility checks).
static void check_camera_vantage(const SSCombatLog& store)
{
    const SSCombat::NounRef& sel = store.view().mSelection;
    if (sel == sLastSeenSelection)
    {
        return;
    }
    const bool from_world_click = (sel == sLastWorldClickSelection);
    sLastSeenSelection = sel;
    if (from_world_click || !sel.valid())
    {
        return;
    }
    if (sel.mType != SSCombat::NOUN_DEATH && sel.mType != SSCombat::NOUN_DAMAGE)
    {
        return;
    }
    if (gAgentCamera.cameraMouselook() || gAgentCamera.getCameraMode() == CAMERA_MODE_OTS)
    {
        return;
    }
    if (sFlyTimerStarted && sSinceFly.getElapsedTimeF32() < 0.5f)
    {
        return;
    }
    SSCombatCamera::flyTo(sel.mIndex);
    sSinceFly.start();
    sFlyTimerStarted = true;
}

// Builds this frame's figure: one icon per visible/selected/hovered event, then the detective-view overlay.
static void build_figure()
{
    SSCombatLog& store = SSCombatLog::instance();
    const SSCombat::View& view = store.view();

    static LLCachedControl<S32> max_labels(gSavedSettings, "SSCombatLogMaxLabels", 12);
    S32 label_budget = llmax(1, (S32)max_labels);

    std::vector<U32> ids = store.visibleEvents();
    auto add_extra = [&ids](const SSCombat::NounRef& ref)
    {
        if ((ref.mType == SSCombat::NOUN_DEATH || ref.mType == SSCombat::NOUN_DAMAGE) &&
            std::find(ids.begin(), ids.end(), ref.mIndex) == ids.end())
        {
            ids.push_back(ref.mIndex);
        }
    };
    add_extra(view.mSelection);
    add_extra(view.mHover);

    // While something is pinned, every icon that is not the selection itself or one of its related hits fades
    // (draw_event_icon), so the pinned detective view stands out against the rest of the figure.
    const bool any_selection = view.mSelection.valid();
    const std::unordered_set<U32> related_ids = any_selection ? compute_related_event_ids(store, view.mSelection)
                                                               : std::unordered_set<U32>();
    for (U32 id : ids)
    {
        if (const SSCombat::Event* ev = store.event(id))
        {
            const bool is_related = !any_selection || related_ids.count(id) != 0;
            draw_event_icon(store, *ev, label_budget, is_related);
        }
    }

    draw_detective_view(store);
    // Camera flight no longer lives here: it is polled from wantsDraw() every frame regardless of the draw
    // gate, so a floater click flies the camera even on a frame this figure is not allowed to draw.
}

// ---------------------------------------------------------------------------------------------------------
// Render.
// ---------------------------------------------------------------------------------------------------------

// static
void SSCombatOverlay::render()
{
    SSCombatDraw::clear();
    build_figure();
    if (SSCombatDraw::empty())
    {
        return;
    }

    static LLCachedControl<bool> xray_setting(gSavedSettings, "SSCombatLogOverlayXray", false);
    const bool xray = SSCombatLog::instance().view().mXray || (bool)xray_setting;

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);

    // Connective geometry carries the through-wall cue: depth-tested at full alpha, then depth-off at 30 %,
    // so a line ghosted through its middle went through geometry and you can see the wall doing it.
    if (xray)
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        SSCombatDraw::emitLines(SSCombatDraw::LAYER_LINE, 1.f);
    }
    else
    {
        {
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            SSCombatDraw::emitLines(SSCombatDraw::LAYER_LINE, 1.f);
        }
        {
            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            SSCombatDraw::emitLines(SSCombatDraw::LAYER_LINE, 0.30f);
        }
    }

    // Markers, icons and labels never hide: a marker you cannot see is a marker you cannot click.
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        SSCombatDraw::emitTris(1.f);
        SSCombatDraw::emitLines(SSCombatDraw::LAYER_MARKER, 1.f);
        SSCombatDraw::emitIcons(1.f);
        SSCombatDraw::emitLabels();
    }
    gGL.flush();
}

// ---------------------------------------------------------------------------------------------------------
// Pick model.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // The noun this overlay itself last wrote into SSCombatLog::view().mHover, so handleHover() only ever
    // clears a hover it set: a list-row hover the Events floater wrote is not this module's to touch.
    SSCombat::NounRef sLastHoverSet;

    // Screen distance from a point to a pick rect; inside the rect is zero.
    F32 pick_distance(const DrawPick& pick, S32 x, S32 y)
    {
        const S32 dx = (x < pick.mRect.mLeft) ? (pick.mRect.mLeft - x) : ((x > pick.mRect.mRight) ? (x - pick.mRect.mRight) : 0);
        const S32 dy = (y < pick.mRect.mBottom) ? (pick.mRect.mBottom - y) : ((y > pick.mRect.mTop) ? (y - pick.mRect.mTop) : 0);
        return sqrtf((F32)(dx * dx + dy * dy));
    }

    // Every candidate within the 12 px radius, nearest first, deduplicated by noun.
    std::vector<DrawPick> candidates_at(S32 x, S32 y)
    {
        const F32 RADIUS = 12.f;
        std::vector<std::pair<F32, DrawPick> > scored;
        for (const DrawPick& pick : sPicks)
        {
            const F32 d = pick_distance(pick, x, y);
            if (d <= RADIUS)
            {
                scored.push_back(std::make_pair(d, pick));
            }
        }
        std::stable_sort(scored.begin(), scored.end(),
                         [](const std::pair<F32, DrawPick>& a, const std::pair<F32, DrawPick>& b) { return a.first < b.first; });
        std::vector<DrawPick> out;
        for (const std::pair<F32, DrawPick>& s : scored)
        {
            bool seen = false;
            for (const DrawPick& kept : out)
            {
                if (kept.mRef == s.second.mRef)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
            {
                out.push_back(s.second);
            }
        }
        return out;
    }
}

// Writes (or clears) the world-icon hover into the shared View, without ever clobbering a hover some other
// source (a floater's list-row hover, say) wrote there: an empty ref only clears the field when it still holds
// exactly the noun this overlay itself last set.
static void set_overlay_hover(const SSCombat::NounRef& ref)
{
    if (!SSCombatLog::instanceExists())
    {
        return;
    }
    SSCombat::View& view = SSCombatLog::instance().view();
    if (!ref.valid())
    {
        if (view.mHover == sLastHoverSet)
        {
            view.mHover = SSCombat::NounRef();
        }
        sLastHoverSet = SSCombat::NounRef();
        return;
    }
    view.mHover = ref;
    sLastHoverSet = ref;
}

// Acts on one resolved noun: a ghost opens the Combatant floater, everything else selects. Never moves the
// camera on its own; SSCombatCamera::flyTo is reserved for a selection that came from a floater.
static void resolve_pick(const SSCombat::NounRef& ref)
{
    if (ref.mType == SSCombat::NOUN_COMBATANT)
    {
        LLFloaterReg::showInstance("ss_combat_combatant", LLSD(ref.mId.asString()));
        return;
    }
    SSCombatLog& store = SSCombatLog::instance();
    store.select(ref, true);
    sLastWorldClickSelection = ref;
    sLastSeenSelection = ref; // pre-empt check_camera_vantage: this click already knows where it came from
}

// Builds the pick-stack popup for overlapping candidates; nothing is selected until the officer chooses.
static void show_pick_stack(const std::vector<DrawPick>& candidates, S32 x, S32 y)
{
    if (LLContextMenu* old_menu = sPickStackHandle.get())
    {
        old_menu->die();
        sPickStackHandle.markDead();
    }

    LLContextMenu::Params params;
    params.name("ss_combat_pick_stack");
    params.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(params);
    if (!menu)
    {
        return;
    }

    S32 index = 0;
    for (const DrawPick& pick : candidates)
    {
        const SSCombat::NounRef ref = pick.mRef;
        LLMenuItemCallGL::Params item;
        item.name(llformat("ss_pick_%d", index++));
        item.label(ref.label());
        item.on_click.function([ref](LLUICtrl*, const LLSD&) { resolve_pick(ref); });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(item));
    }

    sPickStackHandle = menu->getHandle();
    if (gMenuHolder)
    {
        gMenuHolder->addChild(menu);
    }
    menu->show(x, y);
}

// static
bool SSCombatOverlay::handleMouseDown(S32 x, S32 y, MASK mask)
{
    sArmed = false;
    if (mask & (MASK_ALT | MASK_CONTROL | MASK_SHIFT))
    {
        return false; // modified presses belong to alt-cam and the pick tool, always
    }
    if (!wantsDraw())
    {
        return false;
    }
    // Every unmodified press is remembered here, hit or not, so the matching release can tell a real
    // bare-ground click (the idle/deselect click, ux 4.6) from a drag or a held press that merely ended over
    // empty space. Only recording this on an armed (pick-hitting) press left sDownX/Y/Time stale on a bare
    // press, so a quick click on open ground was scored against the previous armed click's position/time and
    // the deselect never fired; that was the missing idle-click bug.
    sDownX = x;
    sDownY = y;
    sDownTime = LLFrameTimer::getTotalSeconds();
    if (sPicks.empty() || candidates_at(x, y).empty())
    {
        return false; // bare ground: let the press through, the up-click clears the selection
    }
    sArmed = true;
    return true;
}

// static
bool SSCombatOverlay::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool armed = sArmed;
    sArmed = false;
    if (mask & (MASK_ALT | MASK_CONTROL | MASK_SHIFT))
    {
        return false;
    }
    if (!wantsDraw())
    {
        return false;
    }

    const S32 dx = x - sDownX;
    const S32 dy = y - sDownY;
    const bool travelled = (dx * dx + dy * dy) > (4 * 4);
    const bool slow = (LLFrameTimer::getTotalSeconds() - sDownTime) > 0.350;

    const std::vector<DrawPick> candidates = candidates_at(x, y);
    if (candidates.empty())
    {
        // Bare ground clears the selection, which leaves its reconstruction too (rework 2026-09-09: the two
        // are now the same state) and does nothing else. leaveReconstruction() is idempotent, so this needs
        // no valid()-guard the way the old direct mSelection clear did.
        if (armed || (!travelled && !slow))
        {
            SSCombatLog::instance().leaveReconstruction();
        }
        return false;
    }
    if (!armed || travelled || slow)
    {
        return false; // any drag, or a held press, cancels the pick
    }

    if (candidates.size() > 1)
    {
        show_pick_stack(candidates, x, y);
        return true;
    }

    // Selecting any event now opens its reconstruction directly (rework 2026-09-09: the old double-click-to-
    // reconstruct gesture is redundant, a single click already does it via SSCombatLog::select()). Clicking
    // the already-selected icon again deselects it instead, which leaves the reconstruction too. Combatant
    // pins never reach here selected (resolve_pick opens their floater instead of selecting), so they are
    // exempt and just reopen/refocus that floater below.
    const SSCombat::NounRef ref = candidates.front().mRef;
    if (ref.mType != SSCombat::NOUN_COMBATANT && SSCombatLog::instance().view().mSelection == ref)
    {
        SSCombatLog::instance().leaveReconstruction();
    }
    else
    {
        resolve_pick(ref);
    }
    return true;
}

// static
void SSCombatOverlay::handleHover(S32 x, S32 y)
{
    sHoverX = x;
    sHoverY = y;
    if (!wantsDraw())
    {
        set_overlay_hover(SSCombat::NounRef());
        return;
    }
    const std::vector<DrawPick> candidates = candidates_at(x, y);
    set_overlay_hover(candidates.empty() ? SSCombat::NounRef() : candidates.front().mRef);
}

// Esc no longer clears the Combat Log selection here: a pinned detective view is meant to persist until the
// officer picks something else or clicks bare ground (handleMouseUp above), never on a keypress or a timeout.
// SSCombatOverlay::handleKey has been removed; llviewerwindow.cpp's call to it must go too.

// static
const SSCombat::NounRef& SSCombatOverlay::hovered()
{
    static const SSCombat::NounRef none;
    return SSCombatLog::instanceExists() ? SSCombatLog::instance().view().mHover : none;
}

// ---------------------------------------------------------------------------------------------------------
// Legend.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // A swatch draws whichever shape the row's own thing actually is in world, same idiom as SSAtmoLegendView
    // (ssatmoinfoview.cpp, doc/atmo_magic_debug_views.md): a line row got drawn as a filled box before this,
    // which read as "this is a coloured area" for things that are really a segment or a ring (owner report
    // 2026-09-09: "the legend and visualisation is a bit confusing").
    enum ESwatchKind : U8 { SWATCH_FILL = 0, SWATCH_LINE, SWATCH_RING };

    // One legend row: either an atlas icon (mIconName non-empty, drawn at 12 px) or a colour swatch shaped by
    // mSwatch (ignored when mIconName is set).
    struct LegendRow
    {
        std::string mIconName;
        LLColor4    mColor;
        std::string mLabel;
        U8          mSwatch = SWATCH_FILL;
    };
}

// static
void SSCombatOverlay::drawLegend()
{
    // Only when this frame's draw gate actually opened; a legend explaining a figure that is not drawing would
    // be its own kind of stray symbol.
    if (!sWantedDrawLastFrame || !SSCombatLog::instanceExists() || !gViewerWindow)
    {
        return;
    }
    if (gSnapshot || gCubeSnapshot)
    {
        return;
    }

    const SSCombatLog& store = SSCombatLog::instance();

    LLColor4 hover_swatch = COL_HOVER;
    hover_swatch.mV[VALPHA] = DETECTIVE_ALPHA_HOVER;

    std::vector<LegendRow> rows;
    rows.push_back({ SSCombatIcons::forType(SSCombatIcons::ICON_DEATH), LLColor4::white, "Death", SWATCH_FILL });
    rows.push_back({ SSCombatIcons::forType(-1), LLColor4::white, "Damage", SWATCH_FILL });
    // Attacker/victim are the ghost capsule's own fill, so a solid box is exactly what they look like in world.
    rows.push_back({ std::string(), COL_ATTACKER, "Attacker", SWATCH_FILL });
    rows.push_back({ std::string(), COL_VICTIM, "Victim", SWATCH_FILL });
    // These three are all segments in world (the kill/hit line, a flight's polyline, a trail), so they get a
    // line swatch rather than a box that implied a filled area nothing actually draws.
    rows.push_back({ std::string(), COL_KILLER_LINE, "Kill / hit line", SWATCH_LINE });
    rows.push_back({ std::string(), COL_FLIGHT, "Bullet path", SWATCH_LINE });
    rows.push_back({ std::string(), COL_GHOST, "Trail", SWATCH_LINE });
    // Reconstruction's kill line is dashed and carries its own eye icon at the midpoint (draw_killer_line(),
    // sscombatreconstruct.cpp); named here too since the legend is the one place both modules' figures explain
    // themselves together.
    rows.push_back({ "Profile_Group_Visibility_On", LLColor4::white, "Kill line: line of sight clear", SWATCH_FILL });
    rows.push_back({ "Profile_Group_Visibility_Off", LLColor4::white, "Kill line: line of sight blocked", SWATCH_FILL });
    // Hover/selection are ringFacing()'s hollow halo around an icon, never a filled or straight-line mark.
    rows.push_back({ std::string(), hover_swatch, "Hovered (ghosted ring)", SWATCH_RING });
    rows.push_back({ std::string(), COL_SELECTION, "Selected (ring)", SWATCH_RING });

    const LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font)
    {
        return;
    }

    // Fixed-width subtitle only: appending the hovered or selected line resized the panel every time the pointer moved, which made the legend jump.
    std::string subtitle = llformat("%d event%s in view", (int)store.visibleEvents().size(),
                                    store.visibleEvents().size() == 1 ? "" : "s");
    static const std::string TITLE("Combat Log");

    const S32 pad = 6;
    const S32 row_h = 14;
    const S32 line_h = (S32)font->getLineHeight() + 2;
    const S32 content_h = line_h * 2 + (S32)rows.size() * row_h;

    S32 content_w = llmax((S32)font->getWidthF32(TITLE), (S32)font->getWidthF32(subtitle));
    for (const LegendRow& row : rows)
    {
        content_w = llmax(content_w, (S32)font->getWidthF32(row.mLabel) + 20);
    }
    const S32 width = content_w + pad * 2;
    const S32 height = content_h + pad * 2;

    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    S32 bottom = world.mBottom + 24;
    if (LLPanel* recon_panel = SSCombatReconstruct::getPanel())
    {
        if (recon_panel->getVisible())
        {
            bottom += recon_panel->getRect().getHeight();
        }
    }
    const S32 left = world.mLeft + (world.getWidth() - width) / 2;
    const S32 top = bottom + height;

    LLGLEnable blend(GL_BLEND);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(left, top, left + width, bottom, LLColor4(0.f, 0.f, 0.f, 0.35f));

    S32 y = top - pad;
    font->renderUTF8(TITLE, 0, left + pad, y, COL_LABEL, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::BOLD, LLFontGL::DROP_SHADOW_SOFT);
    y -= line_h;
    font->renderUTF8(subtitle, 0, left + pad, y, COL_LABEL, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
    y -= line_h;

    for (const LegendRow& row : rows)
    {
        const S32 swatch_cy = y - row_h / 2;
        if (!row.mIconName.empty())
        {
            LLPointer<LLUIImage> image = icon_image(row.mIconName);
            if (image)
            {
                image->draw(left + pad, swatch_cy - 6, 12, 12, LLColor4::white);
            }
        }
        else if (row.mSwatch == SWATCH_LINE)
        {
            gl_line_2d(left + pad, swatch_cy, left + pad + 10, swatch_cy, row.mColor);
        }
        else if (row.mSwatch == SWATCH_RING)
        {
            gGL.color4fv(row.mColor.mV);
            gl_circle_2d((F32)(left + pad + 5), (F32)swatch_cy, 5.f, 16, false);
        }
        else
        {
            gl_rect_2d(left + pad, swatch_cy + 5, left + pad + 10, swatch_cy - 5, row.mColor);
        }
        font->renderUTF8(row.mLabel, 0, left + pad + 18, y, COL_LABEL, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        y -= row_h;
    }
}
