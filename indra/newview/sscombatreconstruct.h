/**
 * @file sscombatreconstruct.h
 * @brief Combat Log Reconstruction: the ghosted replay of one death (doc/combat_log_ux.md 3.10) plus its
 *        bottom-centre transport panel. A mode, not a level: R0 given a window instead of an instant, driven
 *        by its own transport rather than the session cursor, and running zero raycasts.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATRECONSTRUCT_H
#define SS_COMBATRECONSTRUCT_H

#include "stdtypes.h"

class LLPanel;

// Static interface, because there is exactly one world and at most one death being replayed in it.
class SSCombatReconstruct
{
public:
    // True while the shared View is in reconstruction and a prepared frame ring exists for it.
    static bool active();

    // Prepares the frame ring for one DEATH and shows the transport panel. Idempotent: entering the death
    // that is already loaded only re-shows the panel. SSCombatLog::enterReconstruction sets the View; this
    // reads it, so calling either one alone still leaves the two in step.
    static void enter(U32 death_event);
    // Drops the frame ring, hides the panel and returns the View to wherever it came from.
    static void leave();

    // Called from LLPipeline::renderDebug after SSCombatOverlay::render(), with gUIProgram already bound.
    static void render();

    // Space toggles play, ',' and '.' step one event, Esc leaves. Returns true when it consumed the key.
    static bool handleKey(KEY key, MASK mask);

    // Builds the docked control panel once, at world-UI time; safe to call again (it does nothing).
    static void createPanel();
    // Drops the panel, for viewer shutdown.
    static void cleanup();
    // The panel, or null before createPanel() has run.
    static LLPanel* getPanel();
};

#endif // SS_COMBATRECONSTRUCT_H
