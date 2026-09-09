/**
 * @file sscombaticons.h
 * @brief Names of the Combat Log UI icons declared in textures.xml (the owner's damage-type atlas), keyed by
 *        Combat 2.0 damage type and by combat event kind, so lists, buttons and the overlay share one lookup.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Soapstorm Viewer Source Code
 * Copyright (C) 2026, Nexii Malthus
 * This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser
 * General Public License as published by the Free Software Foundation; version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef SS_COMBATICONS_H
#define SS_COMBATICONS_H

#include "stdtypes.h"
#include <string>

namespace SSCombatIcons
{
    // Combat-event icons beyond the damage types, numbered like the owner's LSL table.
    enum EEventIcon : S32
    {
        ICON_DEATH = -100, ICON_ENEMY_DEATH = -101, ICON_FRIENDLY_DEATH = -102, ICON_OBJECT_DEATH = -103,
        ICON_VEHICLE_DEATH = -104, ICON_AIRCRAFT_DEATH = -105, ICON_DOWNED = -106, ICON_REVIVE_REQUESTED = -107,
        ICON_BEING_REVIVED = -108
    };

    // UI image name for a damage type (-1..14, 100..106) or an EEventIcon; variant picks among alternates for impact, sonic, medical and repair. Empty when unknown.
    inline std::string forType(S32 type, U32 variant = 0)
    {
        switch (type)
        {
            case -1:  return "SS_Dmg_Impact_" + std::to_string(variant % 4);
            case 0:   return "SS_Dmg_Generic";
            case 1:   return "SS_Dmg_Acid";
            case 2:   return "SS_Dmg_Bludgeoning";
            case 3:   return "SS_Dmg_Cold";
            case 4:   return "SS_Dmg_Electric";
            case 5:   return "SS_Dmg_Fire";
            case 6:   return "SS_Dmg_Force";
            case 7:   return "SS_Dmg_Necrotic";
            case 8:   return "SS_Dmg_Piercing";
            case 9:   return "SS_Dmg_Poison";
            case 10:  return "SS_Dmg_Psychic";
            case 11:  return "SS_Dmg_Radiant";
            case 12:  return "SS_Dmg_Slashing";
            case 13:  return "SS_Dmg_Sonic_" + std::to_string(variant % 2);
            case 14:  return "SS_Dmg_Emotional";
            case 100: return "SS_Dmg_Medical_" + std::to_string(variant % 2);
            case 101: return "SS_Dmg_Repair_" + std::to_string(variant % 2);
            case 102: return "SS_Dmg_Explosive";
            case 103: return "SS_Dmg_Crushing";
            case 104: return "SS_Dmg_AntiArmor";
            case 105: return "SS_Dmg_Suffocation";
            case 106: return "SS_Dmg_Redeploy";
            case ICON_DEATH:            return "SS_Combat_Death";
            case ICON_ENEMY_DEATH:      return "SS_Combat_EnemyDeath";
            case ICON_FRIENDLY_DEATH:   return "SS_Combat_FriendlyDeath";
            case ICON_OBJECT_DEATH:     return "SS_Combat_ObjectDeath";
            case ICON_VEHICLE_DEATH:    return "SS_Combat_VehicleDeath";
            case ICON_AIRCRAFT_DEATH:   return "SS_Combat_AircraftDeath";
            case ICON_DOWNED:           return "SS_Combat_Downed";
            case ICON_REVIVE_REQUESTED: return "SS_Combat_ReviveRequested";
            case ICON_BEING_REVIVED:    return "SS_Combat_BeingRevived";
            default: return std::string();
        }
    }

    // Human name for a damage type, matching the community list; "Unknown" otherwise.
    inline const char* typeName(S32 type)
    {
        switch (type)
        {
            case -1: return "Impact";      case 0: return "Generic";     case 1: return "Acid";
            case 2: return "Bludgeoning";  case 3: return "Cold";        case 4: return "Electric";
            case 5: return "Fire";         case 6: return "Force";       case 7: return "Necrotic";
            case 8: return "Piercing";     case 9: return "Poison";      case 10: return "Psychic";
            case 11: return "Radiant";     case 12: return "Slashing";   case 13: return "Sonic";
            case 14: return "Emotional";   case 100: return "Medical";   case 101: return "Repair";
            case 102: return "Explosive";  case 103: return "Crushing";  case 104: return "Anti-Armor";
            case 105: return "Suffocation"; case 106: return "Redeploy";
            default: return "Unknown";
        }
    }
}

#endif // SS_COMBATICONS_H
