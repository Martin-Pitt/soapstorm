/**
 * @file sscombatcamera.h
 * @brief Combat Log camera vantage: flies the third-person camera to frame a DEATH or DAMAGE event when the
 *        officer selects it from a floater. The store (SSCombatLog) never calls this and never depends on the
 *        camera; SSCombatOverlay is the only caller, and only for a selection it did not itself just make.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATCAMERA_H
#define SS_COMBATCAMERA_H

#include "stdtypes.h"

// Static interface: there is exactly one agent camera.
class SSCombatCamera
{
public:
    // Flies to a vantage of the given event: frames attacker and target at the event's own time. Prefers an
    // elevated slanted angle with a clear line of sight through static geometry to both; failing that (typically
    // indoors), a composed low-angle shot that frames the victim about 80% down the screen and the attacker
    // about 20% down; failing that, straight above; and as a last resort an over-the-shoulder shot on the
    // victim/target (or the only party known) when nothing at all is clear.
    static void flyTo(U32 eventId);
};

#endif // SS_COMBATCAMERA_H
