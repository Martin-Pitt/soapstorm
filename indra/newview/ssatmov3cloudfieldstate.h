/**
 * @file ssatmov3cloudfieldstate.h
 * @brief Atmo Magic v3: derives the storm-cloud volumetric field's actual
 *        coverage/height/churn from a track's SSAtmoV3CloudField tunables
 *        plus the weather cube - phase 7 of doc/atmo_magic_v3_environment.md.
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

#ifndef SS_ATMOV3CLOUDFIELDSTATE_H
#define SS_ATMOV3CLOUDFIELDSTATE_H

// <SS:Nexii> Atmo Magic v3: volumetric cloud field derivation

#include "ssatmov3asset.h"

// One evaluation of a track's storm-cloud shape. The legacy Windlight cloud
// layer (now cirrus-only, per the design doc) is untouched by any of this -
// this is only the new storm-capable layer.
struct SSAtmoV3CloudFieldState
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
};

class SSAtmoV3CloudFieldResolver
{
public:
    // moisture drives coverage (how much sky the cloud claims, per the
    // Weather tab: 0 is "clear skies", 1 is "thick, black clouds");
    // convection drives height/thickness/churn/anvil (the shape and
    // instability side, kept separate from how much of it there is).
    static SSAtmoV3CloudFieldState resolve(const SSAtmoV3CloudField& field, F32 moisture, F32 convection);
};

// </SS:Nexii>

#endif // SS_ATMOV3CLOUDFIELDSTATE_H
