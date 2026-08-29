/**
 * @file ssatmoenvcloudfieldstate.cpp
 * @brief See ssatmoenvcloudfieldstate.h.
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

// Derives the live cloud field (coverage, height, gloom, churn) from the authored tunables and the moisture/convection cube.
SSAtmoEnvCloudFieldState SSAtmoEnvCloudFieldResolver::resolve(const SSAtmoEnvCloudField& field,
                                                             F32 moisture, F32 convection, F64 phase)
{
    SSAtmoEnvCloudFieldState state;

    F32 base_height, thickness, coverage_scale;
    F32 auto_darkening = -1.f;
    if (field.mAuto)
    {
        deriveAutoBaseline(moisture, convection, base_height, thickness, coverage_scale, auto_darkening);
    }
    else
    {
        base_height    = field.mBaseHeightM.valueAt(phase);
        thickness      = field.mBaseThicknessM.valueAt(phase);
        coverage_scale = field.mCoverageScale.valueAt(phase);
    }

    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 dry = 1.f - m;
    state.mCoverage = (1.f - dry * dry * dry) * llmax(0.f, coverage_scale);

    const F32 height_factor = 1.f + llclamp(convection, 0.f, 1.f) * 4.f;
    state.mBaseHeightM = base_height;
    state.mThicknessM = llmax(0.f, thickness) * height_factor;

    state.mBaseTexture = field.mBaseTexture.valueAt(phase);
    state.mDetailTexture = field.mDetailTexture.valueAt(phase);
    state.mNoiseTexture = field.mNoiseTexture.valueAt(phase);

    state.mTextureMix = llclamp(field.mTextureMix.valueAt(phase), 0.f, 1.f);
    state.mPuffDensity = llclamp(field.mPuffDensity.valueAt(phase), 0.f, 1.f);
    state.mDetailScale = llmax(0.01f, field.mDetailScale.valueAt(phase));
    state.mNoiseScale = llmax(0.05f, field.mNoiseScale.valueAt(phase));
    state.mDriftRate = llmax(0.f, field.mDriftRate.valueAt(phase));

    const F32 darkening = (auto_darkening >= 0.f)
        ? auto_darkening
        : llclamp(field.mStormDarkening.valueAt(phase), 0.f, 2.f);
    state.mGloom = llmax(0.03f, expf(-1.7f * darkening * llclamp(convection, 0.f, 1.f)));

    state.mChurn = llclamp(convection, 0.f, 1.f);
    state.mHasAnvil = convection >= 0.75f;

    state.mAnvil = llclamp((convection - 0.6f) / 0.3f, 0.f, 1.f);

    return state;
}

// Auto mode: plausible base height, thickness and darkening straight from moisture and convection when nothing is authored.
void SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(F32 moisture, F32 convection,
                                                     F32& out_base_height, F32& out_thickness,
                                                     F32& out_coverage_scale, F32& out_darkening)
{
    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 c = llclamp(convection, 0.f, 1.f);

    out_darkening = 0.45f + 1.25f * c + 0.3f * m * c;

    out_base_height = 1400.f - 700.f * m - 200.f * c;
    out_thickness   = 150.f + 350.f * m;

    out_coverage_scale = 1.f;
}
