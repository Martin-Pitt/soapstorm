/**
 * @file ssdeckshadecore.h
 * @brief Atmo Magic: the puff deck's structural shading (facing / through-layer shade / form / buried) - the ONE formula site the veil sheet, every fine puff and every Tier B macro body share. Header-only core.
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

#ifndef SS_DECKSHADECORE_H
#define SS_DECKSHADECORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath> only). Phase 6d review finding 3: the deck's structural shading block (facing term, exponential shade through the layer, beam gate, rim ease, buried depth) had been spelled three times in ssvolcloud.cpp - the base veil, the fine puff loop, the Tier B macro body - as shell arithmetic the harness could not compile (PLAN.md lesson 2). It lives here once, VERBATIM from the fine puff loop's copy, and the three call sites pass their own inputs: the fine puff and the macro body pass (up, max(cellHeight - up, 0), up, coreness, rim); the veil passes its fixed representative depths (facingUp 0.15, aboveDepth 0.65, belowDepth 0.65, coreness 1, rim 0). V:/Scratch/atmo/tests/deckshadecore.cpp transliterates the three OLD shell blocks and pins the fine/macro results bit-identical and the veil's within 1e-6 (the veil's old facing term associated its multiplications differently, so a last-bit difference is possible; measured 0 over the test grid). Pure functions of their arguments - no camera, no settings, no time.
#include "lldefs.h"

#include <cmath>

namespace SSDeckShade
{
    // The layer's optical thickness term th: (0.5 + clamp(thicknessM / 500)) * (0.35 + 0.65 * coverage). Invariants: in
    // [0.175, 1.5] for coverage in [0,1]; monotone in both arguments.
    inline F32 thicknessTerm(F32 thicknessM, F32 coverage)
    {
        return (0.5f + llclamp(thicknessM / 500.f, 0.f, 1.f)) * (0.35f + 0.65f * coverage);
    }

    // The facing term: clamp(0.5 + 0.2 * (sunZ * (facingUp - 0.5) * 2)). A puff high in its column facing a high sun
    // brightens, one low in it darkens. Invariants: in [0.3, 0.7]; 0.5 at sunZ 0 or facingUp 0.5.
    inline F32 facing(F32 sunZ, F32 facingUp)
    {
        return llclamp(0.5f + 0.2f * (sunZ * (facingUp - 0.5f) * 2.f), 0.f, 1.f);
    }

    // The exponential shade through the layer. Sun above the horizon: the column standing ABOVE the point (aboveDepth,
    // in layer fractions) attenuates along the slant 1/max(sunZ, 0.35) plus a coreness-weighted low-sun term; sun below:
    // the column BELOW the point (belowDepth) does, plus a flat coreness term. Invariants: in (0, 1]; 1 when both depths
    // and coreness are 0; monotone non-increasing in each depth, in coreness and in th.
    inline F32 shade(F32 sunZ, F32 th, F32 aboveDepth, F32 belowDepth, F32 coreness)
    {
        if (sunZ >= 0.f)
        {
            return expf(-(aboveDepth / llmax(sunZ, 0.35f) * 0.9f + coreness * (1.f - sunZ) * 1.5f) * th);
        }
        return expf(-(belowDepth / llmax(-sunZ, 0.35f) * 0.9f + coreness * 1.5f) * th);
    }

    // The form the fragment stage multiplies in: lerp(0.65, lerp(facing, 0.65, rim), beam) * lerp(1, shade, beam) - the
    // beam gates both terms (a sunless sky wears the flat 0.65), the rim (cubic-eased edge fraction, 0 inside the field)
    // flattens the facing toward 0.65 so the last rows meet the dome band's flat painting. Invariants: 0.65 at beam 0;
    // equals facing * shade at beam 1, rim 0; in (0, 1].
    inline F32 form(F32 facingTerm, F32 shadeTerm, F32 rim, F32 beam)
    {
        return std::lerp(0.65f, std::lerp(facingTerm, 0.65f, rim), beam) * std::lerp(1.f, shadeTerm, beam);
    }

    // The buried depth (Puff::mBuried): how much of the column stands over the point, normalised by the column's own
    // height and NOT beam-gated (a storm deck is dark underneath at midnight too), eased to 0.5 at the rim. Invariants:
    // in [0,1]; 0.5 at rim 1; 0 at up >= cellHeight with rim 0; 1 at up 0 with rim 0.
    inline F32 buried(F32 cellHeight, F32 up, F32 rim)
    {
        return std::lerp(llclamp((cellHeight - up) / llmax(cellHeight, 0.01f), 0.f, 1.f), 0.5f, rim);
    }

    // The fine-puff / macro-body composite: form at (sunZ, th) for a point at height fraction up in a column of height
    // cellHeight with the given coreness, rim and beam. aboveDepth = max(cellHeight - up, 0), belowDepth = up.
    inline F32 puffForm(F32 sunZ, F32 th, F32 up, F32 cellHeight, F32 coreness, F32 rim, F32 beam)
    {
        const F32 above = llmax(cellHeight - up, 0.f);
        return form(facing(sunZ, up), shade(sunZ, th, above, up, coreness), rim, beam);
    }

    // The base veil's composite: the shade a puff at the deck's floor would wear - a representative low facing height
    // (0.15), a fixed 0.65 of layer over and under it, full coreness, no rim.
    constexpr F32 VEIL_FACING_UP = 0.15f;
    constexpr F32 VEIL_DEPTH     = 0.65f;
    inline F32 veilForm(F32 sunZ, F32 th, F32 beam)
    {
        return form(facing(sunZ, VEIL_FACING_UP), shade(sunZ, th, VEIL_DEPTH, VEIL_DEPTH, 1.f), 0.f, beam);
    }
}

#endif
