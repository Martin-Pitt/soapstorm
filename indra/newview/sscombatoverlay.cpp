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
#include "llmenugl.h"
#include "llrender.h"
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

extern bool gSnapshot;
extern bool gCubeSnapshot;

// ---------------------------------------------------------------------------------------------------------
// Palette. One named colour per meaning: direction is carried by geometry (arrowheads), never by hue, so
// every entry below names a *kind of thing* and never a direction.
// ---------------------------------------------------------------------------------------------------------

// Whatever the officer last clicked; the one colour that outranks everything else.
static const LLColor4 COL_SELECTION(1.00f, 0.95f, 0.38f, 1.f);
// Whatever the pointer is over right now. Transient, so it is dimmer than selection.
static const LLColor4 COL_HOVER(0.85f, 0.92f, 1.00f, 1.f);
// 3D-anchored label text.
static const LLColor4 COL_LABEL(0.93f, 0.93f, 0.96f, 1.f);
// The eye glyph for FLAG_MOUSELOOK.
static const LLColor4 COL_MOUSELOOK(0.96f, 0.86f, 0.36f, 1.f);
// A damage type the palette below has no entry for.
static const LLColor4 COL_DAMAGE_UNKNOWN(0.80f, 0.80f, 0.84f, 1.f);
// A measured projectile flight from the ghost-projectile store: solid, because it was seen.
static const LLColor4 COL_FLIGHT(0.96f, 0.92f, 0.72f, 1.f);
// The killer/hit line in a detective-view freeze-frame.
static const LLColor4 COL_KILLER_LINE(1.00f, 0.58f, 0.22f, 1.f);
// The ghost that is the subject of the freeze-frame: a death's victim, or a damage event's target.
static const LLColor4 COL_GHOST_VICTIM(1.00f, 0.42f, 0.36f, 1.f);
// The ghost that is the source of the freeze-frame: a death's killer, or a damage event's attacker.
static const LLColor4 COL_GHOST_KILLER(1.00f, 0.72f, 0.30f, 1.f);
// A ghost that is neither subject nor source: present, but not the focus.
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

// The shared ghost hue: subject (victim/target) or source (killer/attacker) or neither, alpha baked in.
LLColor4 SSCombatDraw::ghostColor(bool subject, bool source)
{
    LLColor4 color = subject ? COL_GHOST_VICTIM : (source ? COL_GHOST_KILLER : COL_GHOST);
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
    struct DrawIcon
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
    std::vector<DrawIcon>   sIcons;
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

// The mouselook eye: a lens outline with a pupil ring, floated above the head.
void SSCombatDraw::eyeGlyph(const LLVector3& centre_agent, F32 size, const LLColor4& color)
{
    LLVector3 up(0.f, 0.f, 1.f);
    LLVector3 right = LLViewerCamera::getInstance()->getLeftAxis() * -1.f;
    right.mV[VZ] = 0.f;
    if (right.magVecSquared() < 0.0001f)
    {
        right.setVec(1.f, 0.f, 0.f);
    }
    right.normalize();

    const LLVector3 l = centre_agent - right * size;
    const LLVector3 r = centre_agent + right * size;
    const LLVector3 t = centre_agent + up * (size * 0.55f);
    const LLVector3 b = centre_agent - up * (size * 0.55f);
    seg(LAYER_MARKER, l, t, color, color, WIDTH_THIN);
    seg(LAYER_MARKER, t, r, color, color, WIDTH_THIN);
    seg(LAYER_MARKER, r, b, color, color, WIDTH_THIN);
    seg(LAYER_MARKER, b, l, color, color, WIDTH_THIN);

    const S32 pupil = 6;
    LLVector3 prev = centre_agent + right * (size * 0.22f);
    for (S32 i = 1; i <= pupil; ++i)
    {
        const F32 angle = F_TWO_PI * (F32)i / (F32)pupil;
        const LLVector3 cur = centre_agent + right * (size * 0.22f * cosf(angle)) + up * (size * 0.22f * sinf(angle));
        seg(LAYER_MARKER, prev, cur, color, color, WIDTH_THIN);
        prev = cur;
    }
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

// One ghost: capsule, outline ring, yaw arrow, mouselook eye, name label, and (when pickable) a pick rect.
// The single body shared by the overlay's detective view and by Reconstruction's replay.
void SSCombatDraw::ghostBody(const LLVector3& feet_agent, F32 yaw, U16 flags, const LLColor4& color,
                             const std::string& label, const SSCombat::NounRef& pickRef, bool pickable)
{
    capsule(feet_agent, 0.32f, 1.80f, color);

    LLColor4 outline = color;
    outline.mV[VALPHA] = llmin(1.f, color.mV[VALPHA] + 0.30f);
    ring(LAYER_MARKER, feet_agent, 0.42f, outline, WIDTH_THIN);
    arrow(LAYER_MARKER, feet_agent + LLVector3(0.f, 0.f, 0.05f), yaw, 1.1f, outline, WIDTH_MID);
    if (flags & SSCombat::FLAG_MOUSELOOK)
    {
        eyeGlyph(feet_agent + LLVector3(0.f, 0.f, 2.35f), 0.22f, COL_MOUSELOOK);
    }
    pushLabel(label, feet_agent + LLVector3(0.f, 0.f, 2.05f), COL_LABEL);
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
    DrawIcon i;
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
    for (const DrawIcon& i : sIcons)
    {
        LLPointer<LLTexture> tex = i.mImage->getImage();
        if (!tex)
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
    // Reset every frame the agent is moving; the overlay wants a standing officer, not a running one.
    LLFrameTimer sSinceMoved;
    bool         sTimersStarted = false;
}

// Polls the two posture timers. Called from wantsDraw(), which the render block calls every frame.
static void poll_posture_timers()
{
    if (!sTimersStarted)
    {
        sSinceMouselook.start();
        sSinceMoved.start();
        sTimersStarted = true;
    }

    if (gAgentCamera.cameraMouselook() || gAgentCamera.getCameraMode() == CAMERA_MODE_OTS)
    {
        sSinceMouselook.reset();
    }

    const U32 move_flags = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
                           AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
                           AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG;
    const bool pressing = (gAgent.getControlFlags() & move_flags) != 0;
    if (pressing || gAgent.getVelocity().length() >= 0.05f)
    {
        sSinceMoved.reset();
    }
}

// static
bool SSCombatOverlay::wantsDraw()
{
    // The timers are polled before every early-out, because a gate that only samples while it is already open
    // would let a single mouselook frame slip through unnoticed.
    poll_posture_timers();

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
    if (gAgentCamera.getCameraMode() != CAMERA_MODE_THIRD_PERSON)
    {
        return false;
    }
    if (gAgentCamera.getFocusOnAvatar())
    {
        return false; // not alt-cammed; the officer is playing, not reading
    }
    static LLCachedControl<F32> grace(gSavedSettings, "SSCombatLogMouselookGrace", 5.f);
    if (sSinceMouselook.getElapsedTimeF32() < llmax(0.f, (F32)grace))
    {
        return false;
    }
    if (sSinceMoved.getElapsedTimeF32() < 0.5f)
    {
        return false;
    }
    return gAgent.getRegion() != NULL;
}

// ---------------------------------------------------------------------------------------------------------
// Figure building: one icon per visible/selected/hovered event, plus the detective-view freeze-frame.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // The three distance bands, used only to cap trail cost and reach in the detective view.
    enum EBand : U8 { BAND_NEAR = 0, BAND_MID = 1, BAND_FAR = 2 };

    // The pointer's current noun, refreshed on hover and consumed by the figure for its highlight.
    SSCombat::NounRef sHovered;

    // Mouse-down state for the 4 px / 350 ms pick rule.
    bool sArmed = false;
    S32  sDownX = 0, sDownY = 0;
    F64  sDownTime = 0.0;
    S32  sHoverX = 0, sHoverY = 0;

    // Double-click state: the same icon clicked twice inside 400 ms opens Reconstruction.
    SSCombat::NounRef sLastClickRef;
    F64               sLastClickTime = 0.0;

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

// Draws one combatant's trail over the window, alpha ramped old to new and width bucketed by sample quality.
// Shared by the DEATH detective view (victim/killer over the previous 6 s).
static void draw_trail(const SSCombatLog& store, const LLUUID& id, const LLColor4& team, F64 from, F64 to)
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
        color.mV[VALPHA] = 0.15f + 0.85f * ((F32)i / (F32)steps);
        if (have_prev)
        {
            SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, prev, pos, prev_color, color, width_for_sample(sample));
        }
        prev = pos;
        prev_color = color;
        have_prev = true;
    }
}

// The measured polyline of one observed projectile flight, solid because it was seen, terminal tick and all.
static void draw_flight(const SSCombat::Flight& flight)
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
        color.mV[VALPHA] = 0.30f + 0.70f * ((F32)i / (F32)flight.mPath.size());
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, a, b, color, color, SSCombatDraw::WIDTH_THIN);
    }
    if (flight.mHitGeometry)
    {
        SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, SSCombatDraw::agentFromRegion(flight.mPath.back().second),
                            0.2f, COL_FLIGHT, SSCombatDraw::WIDTH_THIN);
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
static bool event_icon_pos(const SSCombatLog& store, const SSCombat::Event& ev, LLVector3& out_region)
{
    if (ev.mKind == SSCombat::EVENT_DEATH || ev.mKind == SSCombat::EVENT_OBJECT_DEATH)
    {
        if (ev.mHasPositions && !ev.mTargetPos.isExactlyZero())
        {
            out_region = ev.mTargetPos;
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
            return true;
        }
        if (ev.mOwner.notNull() && store.sampleAt(ev.mOwner, ev.mTime, sample))
        {
            out_region = sample.mPos;
            return true;
        }
        return false;
    }
    return false;
}

// One event's icon: screen-constant size, always visible, lifted with a stem to the ground, tinted white,
// ringed on selection/hover, plus the near-context label inside 24 m.
static void draw_event_icon(const SSCombatLog& store, const SSCombat::Event& ev, S32& label_budget)
{
    LLVector3 ground_region;
    if (!event_icon_pos(store, ev, ground_region))
    {
        return;
    }
    LLPointer<LLUIImage> image = icon_image(icon_name_for(ev));
    if (!image)
    {
        return;
    }

    const LLVector3 ground = SSCombatDraw::agentFromRegion(ground_region);
    const LLVector3 centre = ground + LLVector3(0.f, 0.f, 0.3f);

    SSCombat::NounRef ref;
    ref.mType = (ev.mKind == SSCombat::EVENT_DAMAGE) ? SSCombat::NOUN_DAMAGE : SSCombat::NOUN_DEATH;
    ref.mIndex = ev.mId;
    ref.mTime = ev.mTime;
    const SSCombat::View& view = store.view();
    const bool selected = (view.mSelection == ref);
    const bool hovered = (sHovered == ref);

    const F32 mpp = SSCombatDraw::metresPerPixelAt(centre);
    const F32 half = llmax(0.02f, 11.f * mpp); // ~22 px square, screen-constant

    SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, ground, centre, COL_LABEL, COL_LABEL, SSCombatDraw::WIDTH_THIN);
    SSCombatDraw::icon(centre, image, half, LLColor4(1.f, 1.f, 1.f, selected ? 1.f : 0.90f));
    if (selected)
    {
        SSCombatDraw::ringFacing(centre, half * 1.5f, COL_SELECTION, SSCombatDraw::WIDTH_MID);
    }
    else if (hovered)
    {
        SSCombatDraw::ringFacing(centre, half * 1.5f, COL_HOVER, SSCombatDraw::WIDTH_THIN);
    }
    SSCombatDraw::pushPick(centre, 13, ref);

    if (label_budget > 0 && SSCombatDraw::cameraDistance(centre) <= 24.f)
    {
        --label_budget;
        const F64 age = LLFrameTimer::getElapsedSeconds() - ev.mTime;
        SSCombatDraw::pushLabel(format_relative_age(age), centre - LLVector3(0.f, 0.f, 0.35f), COL_LABEL);
        if (ev.mKind == SSCombat::EVENT_DEATH || ev.mKind == SSCombat::EVENT_OBJECT_DEATH)
        {
            const std::string line2 = store.displayName(ev.mTarget) + " <- " + store.displayName(ev.mOwner);
            SSCombatDraw::pushLabel(line2, centre - LLVector3(0.f, 0.f, 0.62f), COL_LABEL);
        }
    }
}

// The DEATH freeze-frame: victim and every attacker from attribution(), the kill line, the fatal blow's
// flight if one was measured, and both parties' trails over the previous 6 s.
static void draw_death_detective(const SSCombatLog& store, const SSCombat::Event& ev, bool pinned)
{
    SSCombat::Sample victim_sample;
    const bool have_victim = store.sampleAt(ev.mTarget, ev.mTime, victim_sample);
    if (have_victim)
    {
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = ev.mTarget;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(SSCombatDraw::agentFromRegion(victim_sample.mPos), victim_sample.mYaw, victim_sample.mFlags,
                                SSCombatDraw::ghostColor(true, false), store.displayName(ev.mTarget), ref, pinned);
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
    for (const LLUUID& attacker : attackers)
    {
        SSCombat::Sample sample;
        if (!store.sampleAt(attacker, ev.mTime, sample))
        {
            continue;
        }
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = attacker;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(SSCombatDraw::agentFromRegion(sample.mPos), sample.mYaw, sample.mFlags,
                                SSCombatDraw::ghostColor(false, true), store.displayName(attacker), ref, pinned);
    }

    if (have_victim && ev.mOwner.notNull())
    {
        SSCombat::Sample killer_sample;
        if (store.sampleAt(ev.mOwner, ev.mTime, killer_sample))
        {
            SSCombatDraw::seg(SSCombatDraw::LAYER_LINE,
                              SSCombatDraw::agentFromRegion(killer_sample.mPos) + LLVector3(0.f, 0.f, 1.6f),
                              SSCombatDraw::agentFromRegion(victim_sample.mPos) + LLVector3(0.f, 0.f, 1.2f),
                              COL_KILLER_LINE, COL_KILLER_LINE, SSCombatDraw::WIDTH_MID);
        }
    }

    if (const SSCombat::Flight* flight = find_flight_for_damage(store, attribution.mBlow))
    {
        draw_flight(*flight);
    }

    const F64 trail_from = ev.mTime - 6.0;
    draw_trail(store, ev.mTarget, SSCombatDraw::ghostColor(true, false), trail_from, ev.mTime);
    if (ev.mOwner.notNull())
    {
        draw_trail(store, ev.mOwner, SSCombatDraw::ghostColor(false, true), trail_from, ev.mTime);
    }
}

// The DAMAGE freeze-frame: attacker and target, the hit line tinted by damage type, and the matched flight.
static void draw_damage_detective(const SSCombatLog& store, const SSCombat::Event& ev, bool pinned)
{
    SSCombat::Sample attacker_sample, target_sample;
    const bool have_attacker = ev.mOwner.notNull() && store.sampleAt(ev.mOwner, ev.mTime, attacker_sample);
    const bool have_target = store.sampleAt(ev.mTarget, ev.mTime, target_sample);

    if (have_attacker)
    {
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = ev.mOwner;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(SSCombatDraw::agentFromRegion(attacker_sample.mPos), attacker_sample.mYaw, attacker_sample.mFlags,
                                SSCombatDraw::ghostColor(false, true), store.displayName(ev.mOwner), ref, pinned);
    }
    if (have_target)
    {
        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_COMBATANT;
        ref.mId = ev.mTarget;
        ref.mTime = ev.mTime;
        SSCombatDraw::ghostBody(SSCombatDraw::agentFromRegion(target_sample.mPos), target_sample.mYaw, target_sample.mFlags,
                                SSCombatDraw::ghostColor(true, false), store.displayName(ev.mTarget), ref, pinned);
    }
    if (have_attacker && have_target)
    {
        const LLColor4 line = SSCombatDraw::damageTypeColor(ev.mType);
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE,
                          SSCombatDraw::agentFromRegion(attacker_sample.mPos) + LLVector3(0.f, 0.f, 1.6f),
                          SSCombatDraw::agentFromRegion(target_sample.mPos) + LLVector3(0.f, 0.f, 1.2f),
                          line, line, SSCombatDraw::WIDTH_MID);
    }
    if (const SSCombat::Flight* flight = find_flight_for_damage(store, ev.mId))
    {
        draw_flight(*flight);
    }
}

// The detective view: hovered event first, else the pinned selection; nothing when neither is a DEATH/DAMAGE.
static void draw_detective_view(const SSCombatLog& store)
{
    const SSCombat::View& view = store.view();
    const SSCombat::NounRef& target = view.mHover.valid() ? view.mHover : view.mSelection;
    if (target.mType != SSCombat::NOUN_DEATH && target.mType != SSCombat::NOUN_DAMAGE)
    {
        return;
    }
    const SSCombat::Event* ev = store.event(target.mIndex);
    if (!ev)
    {
        return;
    }
    const bool pinned = !view.mHover.valid();
    if (ev->mKind == SSCombat::EVENT_DEATH || ev->mKind == SSCombat::EVENT_OBJECT_DEATH)
    {
        draw_death_detective(store, *ev, pinned);
    }
    else if (ev->mKind == SSCombat::EVENT_DAMAGE)
    {
        draw_damage_detective(store, *ev, pinned);
    }
}

// Flies the camera to a vantage of a newly selected event, but only when the officer picked it from a floater;
// a world click already recorded its own selection above so it never re-triggers this.
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

    for (U32 id : ids)
    {
        if (const SSCombat::Event* ev = store.event(id))
        {
            draw_event_icon(store, *ev, label_budget);
        }
    }

    draw_detective_view(store);
    check_camera_vantage(store);
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
    if (!wantsDraw() || sPicks.empty())
    {
        return false;
    }
    if (candidates_at(x, y).empty())
    {
        return false; // bare ground: let the press through, the up-click clears the selection
    }
    sArmed = true;
    sDownX = x;
    sDownY = y;
    sDownTime = LLFrameTimer::getTotalSeconds();
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
        // Bare ground clears the selection and does nothing else.
        if (armed || (!travelled && !slow))
        {
            SSCombatLog& store = SSCombatLog::instance();
            if (store.view().mSelection.valid())
            {
                // Cleared in place rather than through select(), because clearing is not a navigation and
                // must not push the View chain.
                store.view().mSelection = SSCombat::NounRef();
                store.notifyViewChanged();
            }
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

    // A second click on the same icon inside 400 ms opens Reconstruction instead of just selecting it.
    const SSCombat::NounRef ref = candidates.front().mRef;
    const F64 click_now = LLFrameTimer::getTotalSeconds();
    const bool double_click = (ref == sLastClickRef) && ((click_now - sLastClickTime) <= 0.4);
    sLastClickRef = ref;
    sLastClickTime = click_now;
    if (double_click && ref.mType == SSCombat::NOUN_DEATH)
    {
        SSCombatLog::instance().enterReconstruction(ref.mIndex);
        SSCombatReconstruct::enter(ref.mIndex);
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
        sHovered = SSCombat::NounRef();
        return;
    }
    const std::vector<DrawPick> candidates = candidates_at(x, y);
    sHovered = candidates.empty() ? SSCombat::NounRef() : candidates.front().mRef;
}

// static
bool SSCombatOverlay::handleKey(KEY key, MASK mask)
{
    if (key != KEY_ESCAPE || (mask & (MASK_CONTROL | MASK_ALT | MASK_SHIFT)))
    {
        return false;
    }
    if (!wantsDraw())
    {
        return false;
    }
    SSCombatLog& store = SSCombatLog::instance();
    if (!store.view().mSelection.valid())
    {
        return false;
    }
    store.view().mSelection = SSCombat::NounRef();
    store.notifyViewChanged();
    return true;
}

// static
const SSCombat::NounRef& SSCombatOverlay::hovered()
{
    return sHovered;
}
