/**
 * @file sscombatoverlay.h
 * @brief Combat Log in-world overlay: the gate, the world figure that mirrors the Events pane one icon per
 *        event, the screen-rect pick model, and the shared immediate-mode draw list the Reconstruction module
 *        also emits into. The owner replaced the old level-indexed figure with this model 2026-09-09: the
 *        overlay no longer knows about navigation levels at all, it draws exactly SSCombatLog::visibleEvents()
 *        plus whatever is hovered or selected.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATOVERLAY_H
#define SS_COMBATOVERLAY_H

#include "sscombatlog.h"

#include "llpointer.h"
#include "llrect.h"
#include "stdtypes.h"
#include "v3math.h"
#include "v4color.h"

#include <string>
#include <vector>

class LLUIImage;

// The draw list every world figure emits into. Two line layers exist because they want different depth state:
// LAYER_LINE is the connective geometry (killer/hit lines, trails, flights) which is drawn twice when x-ray is
// off so a wall between two points is visible as a ghosted middle; LAYER_MARKER is the glyph geometry (rings,
// stems, arrows, eye glyphs) which is always depth-off because a marker you cannot see is a marker you cannot
// click. Triangles are a third list for the filled things (ghost capsules); icons are a fourth because each one
// binds its own atlas page and cannot share a batch with the untextured lists.
namespace SSCombatDraw
{
    enum ELayer : U8 { LAYER_LINE = 0, LAYER_MARKER = 1, LAYER_COUNT = 2 };

    // Width buckets are exactly {1,3,6}, indexed 0/1/2, and the emitter restores 1 when it is done.
    enum EWidth : U8 { WIDTH_THIN = 0, WIDTH_MID = 1, WIDTH_THICK = 2 };

    // Region-local store positions are meaningless to the renderer; everything drawn is agent space.
    LLVector3 agentFromRegion(const LLVector3& region_pos);
    // Metres subtended by a given number of screen pixels at that point; the input to every screen-constant size.
    F32 metresPerPixelAt(const LLVector3& pos_agent);
    // Distance from the render camera.
    F32 cameraDistance(const LLVector3& pos_agent);
    // Ring tessellation falls with distance and is clamped to [8,64].
    S32 ringSegments(const LLVector3& centre_agent, F32 radius);
    // The overlay palette for a Combat 2.0 damage type, keyed through the names FSCombatHitMarker curates.
    LLColor4 damageTypeColor(S16 type);
    // The shared ghost hue: subject (a victim or a damage target) or source (a killer or an attacker) or
    // neither; alpha is baked in (0.60 for subject/source, 0.25 otherwise) so callers never re-derive it.
    LLColor4 ghostColor(bool subject, bool source);

    // ----- draw list building (agent space, colours carry their own alpha) -----
    void clear();
    void seg(ELayer layer, const LLVector3& a, const LLVector3& b, const LLColor4& ca, const LLColor4& cb, U8 width);
    void dashedSeg(ELayer layer, const LLVector3& a, const LLVector3& b, const LLColor4& ca, const LLColor4& cb, U8 width);
    void ring(ELayer layer, const LLVector3& centre_agent, F32 radius, const LLColor4& color, U8 width);
    // A camera-facing ring outline (as opposed to ring()'s ground-plane one); the icon selection/hover halo.
    void ringFacing(const LLVector3& centre_agent, F32 radius, const LLColor4& color, U8 width);
    void cross(ELayer layer, const LLVector3& centre_agent, F32 size, const LLColor4& color, U8 width);
    // A ground arrow of the given length pointing along the world-Z yaw, with two barbs; direction is geometry.
    void arrow(ELayer layer, const LLVector3& base_agent, F32 yaw, F32 length, const LLColor4& color, U8 width);
    // The mouselook eye: a small lens outline with a pupil tick, floated above a head.
    void eyeGlyph(const LLVector3& centre_agent, F32 size, const LLColor4& color);
    // One filled triangle; used only by the ghost capsules.
    void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLColor4& color);
    // A camera-facing filled disc.
    void disc(const LLVector3& centre_agent, F32 radius, const LLColor4& color);
    // A translucent capsule standing on its feet: the ghost body, shared by the overlay's detective view and
    // by Reconstruction's replay.
    void capsule(const LLVector3& feet_agent, F32 radius, F32 height, const LLColor4& color);
    // One ghost: capsule, outline ring, yaw arrow, mouselook eye, name label, and (when pickable) a pick rect
    // addressed at pickRef. The single body shared by every ghost drawn anywhere in the Combat Log.
    void ghostBody(const LLVector3& feet_agent, F32 yaw, U16 flags, const LLColor4& color,
                  const std::string& label, const SSCombat::NounRef& pickRef, bool pickable);
    // A camera-facing textured quad sampling the given UI atlas image; half_size is agent-space metres.
    void icon(const LLVector3& centre_agent, LLPointer<LLUIImage> image, F32 half_size, const LLColor4& color);

    // ----- labels and picks -----
    void pushLabel(const std::string& text, const LLVector3& pos_agent, const LLColor4& color);
    // Every marker that can be clicked pushes its screen rect here; half_px is half the square's side.
    void pushPick(const LLVector3& pos_agent, S32 half_px, const SSCombat::NounRef& ref);

    // ----- emission (caller owns the shader; these only emit geometry) -----
    void emitLines(ELayer layer, F32 alpha_scale);
    void emitTris(F32 alpha_scale);
    void emitIcons(F32 alpha_scale);
    void emitLabels();
    bool empty();
}

// The overlay proper: gate, figure, picking. Everything is static because there is exactly one world.
class SSCombatOverlay
{
public:
    // The posture gate: floater open (Events or Combatant), setting on, not snapshotting, third person,
    // alt-cammed, past the mouselook/OTS grace, and standing still. Polls its own timers every frame.
    static bool wantsDraw();
    // Called from LLPipeline::renderDebug with gUIProgram already bound; emits geometry and labels only.
    static void render();

    // ----- pick model -----
    static bool handleMouseDown(S32 x, S32 y, MASK mask);
    static bool handleMouseUp(S32 x, S32 y, MASK mask);
    static void handleHover(S32 x, S32 y);
    // Esc clears a pinned selection (the detective view's freeze-frame). Wiring note: llviewerwindow.cpp does
    // not call this today; it would need one more line beside its SSCombatReconstruct::handleKey call.
    static bool handleKey(KEY key, MASK mask);

    // The noun the pointer is currently over, so the figure can highlight it; invalid when nothing is under it.
    static const SSCombat::NounRef& hovered();
};

#endif // SS_COMBATOVERLAY_H
