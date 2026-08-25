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

    // Auto swaps the authored baseline for one derived from the cube - the same instant's moisture/convection this is already evaluating, so the two sources can never disagree about "when".
    F32 base_height, thickness, coverage_scale;
    F32 auto_darkening = -1.f;    // < 0 means "not derived, use the authored value"
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

    // The >= 0 clamps sit here rather than in the schema's fromLLSD because the generic keyframe container has no notion of a field's valid range - the same deferral the weather cube documents on
    // its own fields. Coverage climbs much faster than moisture does, and saturates early. Straight proportion was wrong against the rest of the system. The weather resolver calls moisture 0.35-0.65
    // MODERATE rain and 0.65-0.85 HEAVY (classifyIntensity), so a linear map had moderate rain falling out of a sky half full of holes - and heavy rain out of a sky with a third of it still open.
    // Rain does not work that way round: by the time a deck is precipitating properly it is overcast, and what rises after that is how hard it falls, not how much of the sky is left. 1 - (1-m)^3
    // gives that shape against those same bands: about 0.49 covered where drizzle is turning into light rain, 0.73 at the start of moderate, 0.96 by heavy, and effectively solid past that. The
    // author's coverage_scale still multiplies through, so a track can still ask for a broken sky in the wet if it wants one.
    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 dry = 1.f - m;
    state.mCoverage = (1.f - dry * dry * dry) * llmax(0.f, coverage_scale);

    // Height and thickness climb with convection - Stable sits at the baseline ("minimal cloud height, flat overcast" per the design doc), Severe reaches several times taller, toward the iconic
    // cumulonimbus anvil.
    const F32 height_factor = 1.f + llclamp(convection, 0.f, 1.f) * 4.f;
    state.mBaseHeightM = base_height;
    state.mThicknessM = llmax(0.f, thickness) * height_factor;

    state.mBaseTexture = field.mBaseTexture.valueAt(phase);
    state.mDetailTexture = field.mDetailTexture.valueAt(phase);

    state.mTextureMix = llclamp(field.mTextureMix.valueAt(phase), 0.f, 1.f);
    state.mPuffDensity = llclamp(field.mPuffDensity.valueAt(phase), 0.f, 1.f);
    state.mDetailScale = llmax(0.01f, field.mDetailScale.valueAt(phase));
    state.mDriftRate = llmax(0.f, field.mDriftRate.valueAt(phase));

    // The author says how dark a storm gets; convection says how much of a storm this is. Allowed past 1, and clamped at the other end instead. Darkening the body while the rim stays lit is what
    // gives a puff its form - the two together are the whole of its shape - so the top of the range turned out to be where the field looks most solid rather than merely dim. Capping darkening at 1
    // capped that. The floor is on the RESULT: however hard a storm is pushed, cloud that reaches pure black has stopped being cloud.
    // Auto derives darkening from the cube like the rest of the baseline; authored only when Auto is off. Exponential rather than 1 - d*c, which saturated at its floor by darkening ~1/convection -
    // the top half of the slider literally did nothing [was: "above 1.0 does nothing"]. exp keeps responding across the whole 0..2 range and reaches genuinely stormy black-bottomed gloom at the top.
    const F32 darkening = (auto_darkening >= 0.f)
        ? auto_darkening
        : llclamp(field.mStormDarkening.valueAt(phase), 0.f, 2.f);
    state.mGloom = llmax(0.03f, expf(-1.7f * darkening * llclamp(convection, 0.f, 1.f)));

    state.mChurn = llclamp(convection, 0.f, 1.f);
    state.mHasAnvil = convection >= 0.75f;

    // Anvils are not a switch that flips at 0.75. A tower spreads at its top as it approaches the inversion and flattens harder the harder it is driven, so this ramps in either side of that
    // threshold and the shape arrives gradually.
    state.mAnvil = llclamp((convection - 0.6f) / 0.3f, 0.f, 1.f);

    return state;
}

// static
void SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(F32 moisture, F32 convection,
                                                     F32& out_base_height, F32& out_thickness,
                                                     F32& out_coverage_scale, F32& out_darkening)
{
    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 c = llclamp(convection, 0.f, 1.f);

    // A stable sky keeps a light deck; a severe one goes near-black underneath. Moisture adds a little on top - a rain-heavy deck is optically thicker at any convection.
    out_darkening = 0.45f + 1.25f * c + 0.3f * m * c;

    // A dry, stable sky carries a high thin deck; heavy, unstable weather brings the ceiling down and fattens it. Numbers are artistic - picked to sweep the same 600..1400m / 150..500m band the
    // authored defaults (800/300) sit comfortably inside - not meteorology.
    out_base_height = 1400.f - 700.f * m - 200.f * c;
    out_thickness   = 150.f + 350.f * m;

    // Coverage already tracks moisture inside resolve(); Auto has nothing to add on top of that, so the scale stays neutral.
    out_coverage_scale = 1.f;
}

// </SS:Nexii>
