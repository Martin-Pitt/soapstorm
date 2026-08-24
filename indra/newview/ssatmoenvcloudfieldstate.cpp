/**
 * @file ssatmoenvcloudfieldstate.cpp
 * @brief Atmo Magic volumetric cloud field derivation implementation.
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

#include "ssatmoenvcloudfieldstate.h"

// <SS:Nexii> Atmo Magic: volumetric cloud field derivation

// static
SSAtmoEnvCloudFieldState SSAtmoEnvCloudFieldResolver::resolve(const SSAtmoEnvCloudField& field,
                                                             F32 moisture, F32 convection, F64 phase)
{
    SSAtmoEnvCloudFieldState state;

    // Auto swaps the authored baseline for one derived from the cube - the
    // same instant's moisture/convection this is already evaluating, so the
    // two sources can never disagree about "when".
    F32 base_height, thickness, coverage_scale;
    if (field.mAuto)
    {
        deriveAutoBaseline(moisture, convection, base_height, thickness, coverage_scale);
    }
    else
    {
        base_height    = field.mBaseHeightM.valueAt(phase);
        thickness      = field.mBaseThicknessM.valueAt(phase);
        coverage_scale = field.mCoverageScale.valueAt(phase);
    }

    // The >= 0 clamps sit here rather than in the schema's fromLLSD because
    // the generic keyframe container has no notion of a field's valid range
    // - the same deferral the weather cube documents on its own fields.
    state.mCoverage = llclamp(moisture, 0.f, 1.f) * llmax(0.f, coverage_scale);

    // Height and thickness climb with convection - Stable sits at the
    // baseline ("minimal cloud height, flat overcast" per the design doc),
    // Severe reaches several times taller, toward the iconic cumulonimbus
    // anvil.
    const F32 height_factor = 1.f + llclamp(convection, 0.f, 1.f) * 4.f;
    state.mBaseHeightM = base_height;
    state.mThicknessM = llmax(0.f, thickness) * height_factor;

    state.mChurn = llclamp(convection, 0.f, 1.f);
    state.mHasAnvil = convection >= 0.75f;

    return state;
}

// static
void SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(F32 moisture, F32 convection,
                                                     F32& out_base_height, F32& out_thickness,
                                                     F32& out_coverage_scale)
{
    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 c = llclamp(convection, 0.f, 1.f);

    // A dry, stable sky carries a high thin deck; heavy, unstable weather
    // brings the ceiling down and fattens it. Numbers are artistic - picked
    // to sweep the same 600..1400m / 150..500m band the authored defaults
    // (800/300) sit comfortably inside - not meteorology.
    out_base_height = 1400.f - 700.f * m - 200.f * c;
    out_thickness   = 150.f + 350.f * m;

    // Coverage already tracks moisture inside resolve(); Auto has nothing
    // to add on top of that, so the scale stays neutral.
    out_coverage_scale = 1.f;
}

// </SS:Nexii>
