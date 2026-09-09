/**
 * @file sscombatoverlay.cpp
 * @brief Combat Log in-world overlay: the gate, the level-indexed world figure, and the pick model.
 *        Design and rationale: doc/combat_log_ux.md section 4. The caller (LLPipeline::renderDebug) binds
 *        gUIProgram and owns the shader; everything here only emits immediate-mode geometry in agent space.
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
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern bool gSnapshot;
extern bool gCubeSnapshot;

// ---------------------------------------------------------------------------------------------------------
// Palette. One named colour per meaning, per the storm-view convention: direction is carried by geometry
// (arrowheads), never by hue, so every entry below names a *kind of thing* and never a direction.
// ---------------------------------------------------------------------------------------------------------

// A combatant the team solve has not placed yet; deliberately colourless so an unassigned avatar never reads as a side.
static const LLColor4 COL_UNASSIGNED(0.72f, 0.72f, 0.76f, 1.f);
// A death: the cross and the ring at target_pos. Measured, so it is drawn solid at full alpha (ux 4.5).
static const LLColor4 COL_DEATH(1.00f, 0.24f, 0.20f, 1.f);
// The killer line hanging off a death marker, from source_pos to target_pos.
static const LLColor4 COL_KILLER_LINE(1.00f, 0.58f, 0.22f, 1.f);
// Whatever the officer last clicked; the one colour that outranks team colour.
static const LLColor4 COL_SELECTION(1.00f, 0.95f, 0.38f, 1.f);
// Whatever the pointer is over right now. Transient, so it is dimmer than selection.
static const LLColor4 COL_HOVER(0.85f, 0.92f, 1.00f, 1.f);
// 3D-anchored label text.
static const LLColor4 COL_LABEL(0.93f, 0.93f, 0.96f, 1.f);
// The faint per-engagement density ring drawn at the coarse levels.
static const LLColor4 COL_ENGAGEMENT(0.45f, 0.62f, 0.78f, 1.f);
// The eye glyph for FLAG_MOUSELOOK.
static const LLColor4 COL_MOUSELOOK(0.96f, 0.86f, 0.36f, 1.f);
// A damage type the palette below has no entry for.
static const LLColor4 COL_DAMAGE_UNKNOWN(0.80f, 0.80f, 0.84f, 1.f);
// A measured projectile flight from the ghost-projectile store: solid, because it was seen.
static const LLColor4 COL_FLIGHT(0.96f, 0.92f, 0.72f, 1.f);

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

    std::vector<DrawSeg>    sSegs[SSCombatDraw::LAYER_COUNT];
    std::vector<DrawTri>    sTris;
    std::vector<DrawLabel>  sLabels;
    std::vector<DrawPick>   sPicks;
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

// Distance from the render camera to a point, the input to every LOD decision in ux 4.3.
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

// A dashed run between two points; ux 4.5 draws reconstructed geometry dashed and measured geometry solid.
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

// A horizontal ring on the ground plane, the feet marker and the uncertainty disc of ux 4.5.
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

// A three-axis cross; the death mark, and the terminal tick on anything that stopped where it stopped.
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

// The mouselook eye: a lens outline with a pupil ring, floated above the head. Glyphs carry state (ux 4.4).
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

// One filled triangle; the ghost capsules and hit discs are the only things that need fill.
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

// Projects a marker anchor to a screen rect and files it for the pick model of ux 4.6.
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

// Draws the filled list (capsules and discs) at a global alpha multiplier.
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
    return sSegs[LAYER_LINE].empty() && sSegs[LAYER_MARKER].empty() && sTris.empty() && sLabels.empty();
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
    if (!LLFloaterReg::instanceVisible("ss_combat_events"))
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
// Figure building.
// ---------------------------------------------------------------------------------------------------------

namespace
{
    // The three distance bands of ux 4.3; the thresholds are hidden debug settings, not preferences.
    enum EBand : U8 { BAND_NEAR = 0, BAND_MID = 1, BAND_FAR = 2 };

    // The pointer's current noun, refreshed on hover and consumed by the figure for its highlight.
    SSCombat::NounRef sHovered;

    // Mouse-down state for the 4 px / 350 ms pick rule.
    bool sArmed = false;
    S32  sDownX = 0, sDownY = 0;
    F64  sDownTime = 0.0;
    S32  sHoverX = 0, sHoverY = 0;

    LLHandle<LLContextMenu> sPickStackHandle;
}

// Which LOD band a point falls in.
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

// Labels are culled by distance before they are culled by count, so a far label never costs a near one.
static bool label_allowed(const LLVector3& pos_agent, S32& budget)
{
    static LLCachedControl<F32> cull(gSavedSettings, "SSCombatLogLabelCullDistance", 256.f);
    if (SSCombatDraw::cameraDistance(pos_agent) > llmax(8.f, (F32)cull))
    {
        return false;
    }
    if (budget <= 0)
    {
        return false;
    }
    --budget;
    return true;
}

// Team colour for a combatant, falling back to the colourless "no verdict yet" grey.
static LLColor4 combatant_color(const SSCombatLog& store, const LLUUID& id)
{
    const SSCombat::TeamAssignment assignment = store.team(id);
    if (assignment.mTeam < 0)
    {
        return COL_UNASSIGNED;
    }
    LLColor4 color = store.teamColor(assignment.mTeam);
    color.mV[VALPHA] = 1.f;
    return color;
}

// Evidence quality to one of the {1,3,6} width buckets: bridge > viewer > coarse (ux 4.4).
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

// The foot ring's radius: the coarse-location sigma when the sample is coarse, otherwise max(0.6 m, 8 px).
static F32 foot_ring_radius(const SSCombat::Sample& sample, const LLVector3& pos_agent)
{
    if (sample.mSource == SSCombat::SAMPLE_COARSE)
    {
        return 4.f; // a coarse-located avatar visibly sits inside its own uncertainty (ux 4.5)
    }
    const F32 mpp = SSCombatDraw::metresPerPixelAt(pos_agent);
    return llmax(0.6f, 8.f * mpp);
}

// Draws one combatant's head marker: ring (or seated square), yaw arrow, eye glyph, label, pick rect.
static void draw_combatant_marker(const SSCombatLog& store, const LLUUID& id, const SSCombat::Sample& sample,
                                  const LLColor4& team, F64 cursor, S32& label_budget)
{
    const LLVector3 feet = SSCombatDraw::agentFromRegion(sample.mPos);
    const EBand band = band_for(feet);
    if (band == BAND_FAR)
    {
        return; // > 120 m the body layer is not drawn at all; the ground tint is what carries that view
    }

    SSCombat::NounRef ref;
    ref.mType = SSCombat::NOUN_COMBATANT;
    ref.mId = id;
    ref.mTime = cursor;

    const bool selected = (store.view().mSelection == ref);
    const bool hovered = (sHovered == ref);
    LLColor4 color = selected ? COL_SELECTION : (hovered ? COL_HOVER : team);
    const U8 width = selected ? SSCombatDraw::WIDTH_THICK : width_for_sample(sample);

    if (band == BAND_MID)
    {
        // 40-120 m collapses the wedge ring to a small dominant-colour disc; the two-tone edge is the
        // uncertainty cue, so a coarse sample keeps a second, wider ring around the dot.
        const F32 dot = llmax(0.25f, 5.f * SSCombatDraw::metresPerPixelAt(feet));
        SSCombatDraw::disc(feet + LLVector3(0.f, 0.f, 0.05f), dot, color);
        if (sample.mSource == SSCombat::SAMPLE_COARSE)
        {
            LLColor4 faint = color;
            faint.mV[VALPHA] = 0.35f;
            SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, feet, 4.f, faint, SSCombatDraw::WIDTH_THIN);
        }
    }
    else
    {
        SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, feet, foot_ring_radius(sample, feet), color, width);
        SSCombatDraw::arrow(SSCombatDraw::LAYER_MARKER, feet + LLVector3(0.f, 0.f, 0.05f), sample.mYaw, 1.2f, color, width);
        if (sample.mParent.notNull())
        {
            // Seated: the square glyph of ux 4.4, drawn where the vehicle marker would be.
            const F32 s = 0.45f;
            const LLVector3 c = feet + LLVector3(0.f, 0.f, 0.15f);
            SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, c + LLVector3(-s, -s, 0.f), c + LLVector3(s, -s, 0.f), color, color, width);
            SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, c + LLVector3(s, -s, 0.f), c + LLVector3(s, s, 0.f), color, color, width);
            SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, c + LLVector3(s, s, 0.f), c + LLVector3(-s, s, 0.f), color, color, width);
            SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, c + LLVector3(-s, s, 0.f), c + LLVector3(-s, -s, 0.f), color, color, width);
        }
        if (sample.mFlags & SSCombat::FLAG_MOUSELOOK)
        {
            SSCombatDraw::eyeGlyph(feet + LLVector3(0.f, 0.f, 2.35f), 0.22f, COL_MOUSELOOK);
        }
    }

    if (label_allowed(feet, label_budget))
    {
        SSCombatDraw::pushLabel(store.displayName(id), feet + LLVector3(0.f, 0.f, 2.05f), selected ? COL_SELECTION : COL_LABEL);
    }
    SSCombatDraw::pushPick(feet + LLVector3(0.f, 0.f, 1.f), 12, ref);
}

// Draws one combatant's trail over the window, alpha ramped old to new and width bucketed by sample quality.
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
        return; // trails are not drawn past the far threshold (ux 4.3)
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

// The attacker's world position for a DAMAGE event; measured when the event carried one, reconstructed from
// the owner's (or the rezzer's owner's) track otherwise. The caller draws the reconstructed case dashed.
static bool damage_attacker_pos(const SSCombatLog& store, const SSCombat::Event& ev, LLVector3& out, bool& measured)
{
    if (ev.mHasPositions && !ev.mSourcePos.isExactlyZero())
    {
        out = ev.mSourcePos;
        measured = true;
        return true;
    }
    measured = false;
    SSCombat::Sample sample;
    if (ev.mOwner.notNull() && store.sampleAt(ev.mOwner, ev.mTime, sample))
    {
        out = sample.mPos;
        return true;
    }
    const SSCombat::Equipment* rezzer = store.equipmentFor(ev.mRezzer);
    if (rezzer && rezzer->mOwner.notNull() && store.sampleAt(rezzer->mOwner, ev.mTime, sample))
    {
        out = sample.mPos;
        return true;
    }
    if (rezzer && rezzer->mHasLastPos)
    {
        out = rezzer->mLastPos;
        return true;
    }
    return false;
}

// The target's world position for an event, measured when carried and reconstructed from its track otherwise.
static bool event_target_pos(const SSCombatLog& store, const SSCombat::Event& ev, LLVector3& out, bool& measured)
{
    if (ev.mHasPositions && !ev.mTargetPos.isExactlyZero())
    {
        out = ev.mTargetPos;
        measured = true;
        return true;
    }
    measured = false;
    SSCombat::Sample sample;
    if (store.sampleAt(ev.mTarget, ev.mTime, sample))
    {
        out = sample.mPos;
        return true;
    }
    return false;
}

// One damage line: attacker to target at the event's own time, coloured by type, brighter with damage, and
// pulsing for a second after it landed so a fresh hit reads as fresh.
static void draw_damage_line(const SSCombatLog& store, const SSCombat::Event& ev, F64 now)
{
    LLVector3 from_region, to_region;
    bool from_measured = false, to_measured = false;
    if (!damage_attacker_pos(store, ev, from_region, from_measured))
    {
        return; // no usable attacker position; ux 4.5 wants this in the unattributed tally, not smeared
    }
    if (!event_target_pos(store, ev, to_region, to_measured))
    {
        return;
    }

    const LLVector3 from = SSCombatDraw::agentFromRegion(from_region) + LLVector3(0.f, 0.f, 1.55f);
    const LLVector3 to = SSCombatDraw::agentFromRegion(to_region) + LLVector3(0.f, 0.f, 1.15f);
    if (band_for(to) == BAND_FAR)
    {
        return;
    }

    LLColor4 color = SSCombatDraw::damageTypeColor(ev.mType);
    const F32 brightness = llclamp(0.35f + ev.mDamage / 60.f, 0.35f, 1.f);
    const F64 age = now - ev.mTime;
    const F32 pulse = (age >= 0.0 && age < 1.0) ? (1.f + 0.6f * (1.f - (F32)age)) : 1.f;
    color.mV[VRED] = llclamp(color.mV[VRED] * brightness * pulse, 0.f, 1.f);
    color.mV[VGREEN] = llclamp(color.mV[VGREEN] * brightness * pulse, 0.f, 1.f);
    color.mV[VBLUE] = llclamp(color.mV[VBLUE] * brightness * pulse, 0.f, 1.f);

    const bool reconstructed = !from_measured || !to_measured;
    color.mV[VALPHA] = reconstructed ? 0.60f : 1.f;
    const U8 width = (ev.mDamage >= 40.f) ? SSCombatDraw::WIDTH_THICK
                                          : ((ev.mDamage >= 12.f) ? SSCombatDraw::WIDTH_MID : SSCombatDraw::WIDTH_THIN);

    if (reconstructed)
    {
        SSCombatDraw::dashedSeg(SSCombatDraw::LAYER_LINE, from, to, color, color, width);
    }
    else
    {
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE, from, to, color, color, width);
    }

    // Sub-TRACK attacker positions draw hollow, never solid; here that is the small diamond at the origin.
    if (!from_measured)
    {
        const F32 s = 0.18f;
        SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, from + LLVector3(-s, 0.f, 0.f), from + LLVector3(0.f, 0.f, s), color, color, SSCombatDraw::WIDTH_THIN);
        SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, from + LLVector3(0.f, 0.f, s), from + LLVector3(s, 0.f, 0.f), color, color, SSCombatDraw::WIDTH_THIN);
        SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, from + LLVector3(s, 0.f, 0.f), from + LLVector3(0.f, 0.f, -s), color, color, SSCombatDraw::WIDTH_THIN);
        SSCombatDraw::seg(SSCombatDraw::LAYER_MARKER, from + LLVector3(0.f, 0.f, -s), from + LLVector3(-s, 0.f, 0.f), color, color, SSCombatDraw::WIDTH_THIN);
    }

    LLColor4 hit = color;
    hit.mV[VALPHA] = 1.f;
    SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, to, 0.16f * pulse, hit, SSCombatDraw::WIDTH_THIN);

    SSCombat::NounRef ref;
    ref.mType = SSCombat::NOUN_DAMAGE;
    ref.mIndex = ev.mId;
    ref.mTime = ev.mTime;
    SSCombatDraw::pushPick(to, 10, ref);
}

// One death marker: cross plus ring at target_pos, the killer line from source_pos, and the standing label.
static void draw_death_marker(const SSCombatLog& store, const SSCombat::Event& ev, F32 alpha, S32& label_budget)
{
    if (!ev.mHasPositions && ev.mTargetPos.isExactlyZero())
    {
        return;
    }
    const LLVector3 target = SSCombatDraw::agentFromRegion(ev.mTargetPos);

    SSCombat::NounRef ref;
    ref.mType = SSCombat::NOUN_DEATH;
    ref.mIndex = ev.mId;
    ref.mTime = ev.mTime;
    const bool selected = (store.view().mSelection == ref);
    const bool hovered = (sHovered == ref);

    LLColor4 cross_color = selected ? COL_SELECTION : (hovered ? COL_HOVER : COL_DEATH);
    cross_color.mV[VALPHA] = alpha;
    SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, target + LLVector3(0.f, 0.f, 0.9f), 0.55f, cross_color, SSCombatDraw::WIDTH_MID);
    SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, target, 1.1f, cross_color, SSCombatDraw::WIDTH_MID);

    if (!ev.mSourcePos.isExactlyZero())
    {
        LLColor4 killer = COL_KILLER_LINE;
        killer.mV[VALPHA] = alpha;
        SSCombatDraw::seg(SSCombatDraw::LAYER_LINE,
                          SSCombatDraw::agentFromRegion(ev.mSourcePos) + LLVector3(0.f, 0.f, 1.55f),
                          target + LLVector3(0.f, 0.f, 1.15f), killer, killer, SSCombatDraw::WIDTH_MID);
    }

    if (label_allowed(target, label_budget))
    {
        // "victim <- killer (family)": the killer is the weapon's owner, not the weapon.
        std::string text = store.displayName(ev.mTarget) + " <- " + store.displayName(ev.mOwner);
        const SSCombat::Equipment* equip = store.equipmentFor(ev.mSource);
        if (equip && !equip->mFamily.empty())
        {
            text += " (" + equip->mFamily + ")";
        }
        LLColor4 label = COL_LABEL;
        label.mV[VALPHA] = alpha;
        SSCombatDraw::pushLabel(text, target + LLVector3(0.f, 0.f, 1.85f), label);
    }

    // Deaths stay pickable for the whole persistence window whatever their alpha; that is the promise of 4.1
    // that an officer who alt-cams anywhere always has something to click.
    SSCombatDraw::pushPick(target + LLVector3(0.f, 0.f, 0.9f), 12, ref);
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
        // A bullet that stopped in a wall keeps its terminal tick there; that is what "shot into an occluder"
        // looks like in the world.
        SSCombatDraw::cross(SSCombatDraw::LAYER_MARKER, SSCombatDraw::agentFromRegion(flight.mPath.back().second),
                            0.2f, COL_FLIGHT, SSCombatDraw::WIDTH_THIN);
    }
}

// The coarse levels' figure: a faint ring per engagement, tinted by whichever team has the most combatants.
static void draw_engagement_rings(const SSCombatLog& store, S32& label_budget)
{
    for (const SSCombat::Engagement& eng : store.engagements())
    {
        const LLVector3 centre = SSCombatDraw::agentFromRegion(eng.mCentre);
        LLColor4 color = COL_ENGAGEMENT;
        color.mV[VALPHA] = 0.35f;
        SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, centre, llmax(2.f, eng.mRadius), color, SSCombatDraw::WIDTH_THIN);

        // Density is carried by a second, tighter ring in the dominant team's colour rather than by hue on
        // the first, so the outline always means "engagement" and never "side".
        std::vector<S32> counts((size_t)llmax(1, store.teamCount()), 0);
        for (const LLUUID& id : eng.mCombatants)
        {
            const SSCombat::TeamAssignment assignment = store.team(id);
            if (assignment.mTeam >= 0 && (size_t)assignment.mTeam < counts.size())
            {
                ++counts[(size_t)assignment.mTeam];
            }
        }
        S32 best = -1, best_count = 0;
        for (size_t i = 0; i < counts.size(); ++i)
        {
            if (counts[i] > best_count)
            {
                best_count = counts[i];
                best = (S32)i;
            }
        }
        if (best >= 0)
        {
            LLColor4 team = store.teamColor((S8)best);
            team.mV[VALPHA] = 0.30f;
            SSCombatDraw::ring(SSCombatDraw::LAYER_MARKER, centre, llmax(1.5f, eng.mRadius * 0.85f), team, SSCombatDraw::WIDTH_THIN);
        }

        SSCombat::NounRef ref;
        ref.mType = SSCombat::NOUN_ENGAGEMENT;
        ref.mIndex = eng.mId;
        ref.mTime = eng.mStart;
        if (label_allowed(centre, label_budget))
        {
            SSCombatDraw::pushLabel(eng.mLabel, centre + LLVector3(0.f, 0.f, 1.2f), COL_LABEL);
        }
        SSCombatDraw::pushPick(centre, 12, ref);
    }
}

// Builds this frame's figure into the draw list: base layer first, then the current level's focus layer.
static void build_figure()
{
    const SSCombatLog& store = SSCombatLog::instance();
    const SSCombat::View& view = store.view();

    static LLCachedControl<F32> trail_seconds(gSavedSettings, "SSCombatLogTrailSeconds", 20.f);
    static LLCachedControl<S32> max_labels(gSavedSettings, "SSCombatLogMaxLabels", 12);
    static LLCachedControl<S32> max_damage_lines(gSavedSettings, "SSCombatLogMaxDamageLines", 40);

    const F32 window = (view.mWindow > 0.f) ? view.mWindow : llmax(1.f, (F32)trail_seconds);
    const F64 cursor = view.mCursor;
    const F64 from = cursor - (F64)window;
    S32 label_budget = llmax(1, (S32)max_labels);

    const bool bodies = (view.mLevel <= SSCombat::LEVEL_ENGAGEMENT);

    // Layer 1, the permanent base layer: every death in the session at ~25 %, minus the ones the focus layer
    // is about to draw at full strength, so nothing is drawn (or picked) twice.
    const F64 persist_from = cursor - 3.0 * (F64)window;
    for (const SSCombat::Event& ev : store.events())
    {
        if (ev.mKind != SSCombat::EVENT_DEATH)
        {
            continue;
        }
        if (ev.mTime >= persist_from && ev.mTime <= cursor)
        {
            continue; // the focus/persistence pass below owns this one
        }
        draw_death_marker(store, ev, 0.25f, label_budget);
    }

    // Deaths inside the persistence window: full alpha inside the trail window, fading to a quarter over the
    // remaining two windows, and pickable throughout.
    for (const SSCombat::Event& ev : store.events())
    {
        if (ev.mKind != SSCombat::EVENT_DEATH || ev.mTime < persist_from || ev.mTime > cursor)
        {
            continue;
        }
        const F64 age = cursor - ev.mTime;
        F32 alpha = 1.f;
        if (age > (F64)window)
        {
            alpha = 1.f - 0.75f * (F32)((age - (F64)window) / (2.0 * (F64)window));
        }
        draw_death_marker(store, ev, llclamp(alpha, 0.25f, 1.f), label_budget);
    }

    if (!bodies)
    {
        // Sweep, Comparison and Session: deaths and density only, so alt-camming stays useful up here.
        draw_engagement_rings(store, label_budget);
        return;
    }

    // Moment and Engagement: the body layer.
    const std::vector<LLUUID> combatants = store.combatants();
    for (const LLUUID& id : combatants)
    {
        const LLColor4 team = combatant_color(store, id);
        draw_trail(store, id, team, from, cursor);
        SSCombat::Sample head;
        if (store.sampleAt(id, cursor, head))
        {
            draw_combatant_marker(store, id, head, team, cursor, label_budget);
        }
    }

    // Damage lines in the window. Past the near band the design keeps only the top N by damage, so the count
    // is honest rather than the view silently dropping glyphs.
    std::vector<const SSCombat::Event*> damage;
    const size_t first = store.eventIndexAt(from);
    const std::vector<SSCombat::Event>& events = store.events();
    for (size_t i = first; i < events.size(); ++i)
    {
        const SSCombat::Event& ev = events[i];
        if (ev.mTime > cursor)
        {
            break;
        }
        if (ev.mKind == SSCombat::EVENT_DAMAGE)
        {
            damage.push_back(&ev);
        }
    }
    const S32 budget = llmax(1, (S32)max_damage_lines);
    if ((S32)damage.size() > budget)
    {
        std::partial_sort(damage.begin(), damage.begin() + budget, damage.end(),
                          [](const SSCombat::Event* a, const SSCombat::Event* b) { return a->mDamage > b->mDamage; });
        damage.resize((size_t)budget);
    }
    for (const SSCombat::Event* ev : damage)
    {
        draw_damage_line(store, *ev, cursor);
    }

    // Observed flights whose measured span overlaps the window.
    for (const SSCombat::Flight& flight : store.flights())
    {
        if (flight.mEnd < from || flight.mStart > cursor)
        {
            continue;
        }
        draw_flight(flight);
    }
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
    // so a shot ghosted through its middle went through geometry and you can see the wall doing it (ux 4.5).
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

    // Markers and labels never hide: a marker you cannot see is a marker you cannot click.
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        SSCombatDraw::emitTris(1.f);
        SSCombatDraw::emitLines(SSCombatDraw::LAYER_MARKER, 1.f);
        SSCombatDraw::emitLabels();
    }
    gGL.flush();
}

// ---------------------------------------------------------------------------------------------------------
// Pick model (ux 4.6).
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

// Acts on one resolved noun: a death opens its reconstruction, everything else selects and navigates.
static void resolve_pick(const SSCombat::NounRef& ref)
{
    SSCombatLog& store = SSCombatLog::instance();
    if (ref.mType == SSCombat::NOUN_DEATH)
    {
        // In the world the replay is the article; the text is one button away on the reconstruction panel.
        store.enterReconstruction(ref.mIndex);
        SSCombatReconstruct::enter(ref.mIndex);
        return;
    }
    store.select(ref, true);
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
        // Bare ground clears the selection and does nothing else; it is prims and mesh and knows nothing.
        if (armed || (!travelled && !slow))
        {
            SSCombatLog& store = SSCombatLog::instance();
            if (store.view().mSelection.valid())
            {
                // Cleared in place rather than through select(), because clearing is not a navigation and
                // must not push the View chain (ux 4.6: it does nothing else).
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
    resolve_pick(candidates.front().mRef);
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
const SSCombat::NounRef& SSCombatOverlay::hovered()
{
    return sHovered;
}
