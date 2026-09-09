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
    // Flies to a vantage of the given event: frames attacker and target at the event's own time, prefers an
    // elevated angle with a clear line of sight through static geometry to both, falls back to straight above,
    // and as a last resort to an over-the-shoulder shot on the victim/target when nothing is clear (indoors).
    static void flyTo(U32 eventId);
};

#endif // SS_COMBATCAMERA_H
