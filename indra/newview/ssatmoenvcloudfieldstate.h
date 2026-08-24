/**
 * @file ssatmoenvcloudfieldstate.h
 * @brief Atmo Magic: derives the storm-cloud volumetric field's actual
 *        coverage/height/churn from a track's SSAtmoEnvCloudField tunables
 *        plus the weather cube - phase 7 of doc/atmo_magic_environment.md.
 *        Pure logic only, same discipline as every resolver before it: no
 *        noise field, no shader, no rendering hookup. This is what a future
 *        SSCloudField (rain-shaft placement, lightning-fork generation)
 *        would read, not that field itself.
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

#ifndef SS_ATMOENVCLOUDFIELDSTATE_H
#define SS_ATMOENVCLOUDFIELDSTATE_H

// <SS:Nexii> Atmo Magic: volumetric cloud field derivation

#include "ssatmoenvasset.h"

// One evaluation of a track's storm-cloud shape. The legacy Windlight cloud
// layer (now cirrus-only, per the design doc) is untouched by any of this -
// this is only the new storm-capable layer.
struct SSAtmoEnvCloudFieldState
{
    F32 mCoverage = 0.f;     // 0..1, fraction of sky the storm cloud covers
    F32 mBaseHeightM = 800.f;
    F32 mThicknessM = 0.f;

    // 0..1: how much the cloud shape boils/churns, directly the convection
    // phase - Stable is a flat, static overcast, Severe is "rapid cloud
    // panning, max churning" per the design doc's convection table.
    F32 mChurn = 0.f;

    // The iconic cumulonimbus anvil top, only once convection reaches the
    // Severe phase - a shape detail, not a physical simulation of one.
    bool mHasAnvil = false;

    // How far into anvil the tower has gone, 0..1 - a ramp rather than the
    // flag above, which is a threshold and cannot express "nearly".
    F32 mAnvil = 0.f;

    // What the field is drawn with, resolved at this phase. Null means "the
    // sensible default for this slot" - see SSAtmoEnvCloudField.
    LLUUID mBaseTexture;
    LLUUID mDetailTexture;

    // Authored look, resolved at this phase - see SSAtmoEnvCloudField.
    F32 mTextureMix = 0.f;
    F32 mPuffDensity = 0.8f;
    F32 mDetailScale = 1.f;
    F32 mDriftRate = 1.f;

    // Already folded together: how dark the cloud is drawn, authored
    // darkening applied to this instant's convection.
    F32 mGloom = 1.f;
};

class SSAtmoEnvCloudFieldResolver
{
public:
    // moisture drives coverage (how much sky the cloud claims, per the
    // Weather tab: 0 is "clear skies", 1 is "thick, black clouds");
    // convection drives height/thickness/churn/anvil (the shape and
    // instability side, kept separate from how much of it there is).
    //
    // phase is a position in the day cycle, [0, 1) - same convention as
    // SSAtmoEnvWeatherResolver::resolve. The field's own tunables are
    // keyframable, and they must be evaluated at the same instant the
    // caller derived its moisture/convection from, or a keyframed baseline
    // would drift against the cube it modulates.
    static SSAtmoEnvCloudFieldState resolve(const SSAtmoEnvCloudField& field, F32 moisture, F32 convection, F64 phase);

    // What SSAtmoEnvCloudField::mAuto actually computes: an artistic
    // baseline from the same two cube inputs resolve() consumes. Public and
    // separate from resolve() because the floater shows these numbers in
    // the (disabled) baseline rows while Auto owns them - the user should
    // see what Auto decided, not a parked authored value it is ignoring.
    // Heavier weather pulls the cloud base down and thickens the deck
    // (thickness is the pre-convection baseline; resolve() still applies
    // the convective height factor on top); coverage scale stays neutral
    // because coverage already tracks moisture inside resolve().
    static void deriveAutoBaseline(F32 moisture, F32 convection,
                                   F32& out_base_height, F32& out_thickness, F32& out_coverage_scale);
};

// </SS:Nexii>

#endif // SS_ATMOENVCLOUDFIELDSTATE_H
