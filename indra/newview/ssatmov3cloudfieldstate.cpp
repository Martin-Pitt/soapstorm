/**
 * @file ssatmov3cloudfieldstate.cpp
 * @brief Atmo Magic v3 volumetric cloud field derivation implementation.
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

#include "ssatmov3cloudfieldstate.h"

// <SS:Nexii> Atmo Magic v3: volumetric cloud field derivation

// static
SSAtmoV3CloudFieldState SSAtmoV3CloudFieldResolver::resolve(const SSAtmoV3CloudField& field,
                                                             F32 moisture, F32 convection)
{
    SSAtmoV3CloudFieldState state;

    state.mCoverage = llclamp(moisture, 0.f, 1.f) * llmax(0.f, field.mCoverageScale);

    // Height and thickness climb with convection - Stable sits at the
    // field's own authored base ("minimal cloud height, flat overcast" per
    // the design doc), Severe reaches several times taller, toward the
    // iconic cumulonimbus anvil.
    const F32 height_factor = 1.f + llclamp(convection, 0.f, 1.f) * 4.f;
    state.mBaseHeightM = field.mBaseHeightM;
    state.mThicknessM = field.mBaseThicknessM * height_factor;

    state.mChurn = llclamp(convection, 0.f, 1.f);
    state.mHasAnvil = convection >= 0.75f;

    return state;
}

// </SS:Nexii>
