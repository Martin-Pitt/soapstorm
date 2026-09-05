/**
 * @file ssdeckmacrocore.h
 * @brief Atmo Magic: the far deck's merged macro-puff tier (B) - macro-cell occupancy from the fine gate, coverage-conserving bodies, tier crossfade. Header-only core. CONTRACT.
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

#ifndef SS_DECKMACROCORE_H
#define SS_DECKMACROCORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssdecklodcore.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_far_clouds.md section 2 step 2, Tier B (phase 6d). Beyond TIER_B_M the deck stops placing per-fine-cell puffs and places ONE large body per occupied MACRO cell (a 2x2 block of fine cells, MACRO_CELLS = 2): the macro cell's occupancy is the SHARE of its fine cells that pass the ordinary gate - the SAME gate the fine tier, the shader's ss_cell_occupied and the shadow bake evaluate, so the tiers agree about where cloud exists without a fourth gate implementation; the body's radius covers the block and its alpha is scaled by that share so aggregate coverage is conserved. Tier A and Tier B crossfade over TIER_BLEND_M (A's alpha down, B's up) so body count changes but coverage does not pop - and BOTH halves read ONE scalar, the block's horizontal, drift-only centre distance computed once in the fine loop and carried to the body (the 6d review found the body, at deck altitude, reading its own 3-D post-shift distance and so wB ~ 1 while its fine cells still read wA ~ 1: double coverage at the band's inner edge; a crossfade whose two sides read different distances is not a crossfade). The macro body is placed at the block centre + the block's mean displacement (drift + hero shift + O(z) of its altitude - the producer rule holds per block). The review's earlier rejection of a stride-2 decimation near the squash knee is answered by DISTANCE: at >= 5 km the residual disagreement between a 260 m veil carve and a 520 m body is below what the eye resolves through haze and squash; the fine gate is still what decides occupancy. LOD only: camera distance selects the tier and alphas, never the field. Bodies marked STUB are the implementer's; the invariants are the test author's spec.
#include "lldefs.h"
#include "ssdecklodcore.h"

#include <cmath>
#include <cstdint>

namespace SSDeckMacro
{
    constexpr F32 CELL_M        = 260.f;   // LOCKSTEP ssvolcloud.cpp CELL_M
    constexpr S32 MACRO_CELLS   = 2;       // fine cells per macro cell side (2x2 blocks)
    constexpr F32 MACRO_M       = CELL_M * (F32)MACRO_CELLS;
    constexpr F32 TIER_B_M      = 5000.f;  // macro tier begins here (LOCKSTEP: must be >= SSDeckLod::THIN_START_M so thinning and merging never overlap in confusing ways - the fine tier thins, the macro tier does not)
    constexpr F32 TIER_BLEND_M  = 600.f;   // A -> B crossfade band straddling TIER_B_M
    constexpr F32 BODY_RADIUS_FRAC = 0.85f * 0.5f; // LOCKSTEP the fine puff's base_radius = CELL_M * 0.85 * 0.5; the macro body uses MACRO_M * this
    constexpr F32 BODY_ALPHA_MAX = 1.f;

    // The macro cell containing fine cell (cx, cy): floor division that is exact for negatives. Invariants: cells 0..1 map
    // to 0, -1..-2 map to -1; pure.
    inline S32 macroIndex(S32 fine)
    {
        return (fine >= 0) ? (fine / MACRO_CELLS) : -(((-fine) + MACRO_CELLS - 1) / MACRO_CELLS);
    }

    // Occupancy share of a macro cell from its MACRO_CELLS x MACRO_CELLS fine-gate verdicts (0/1 each): the count / total.
    // Invariants: in [0,1]; 0 when no fine cell passes; 1 when all do; equals count/4 for 2x2.
    inline F32 occupancy(const U8* fineVerdicts, S32 count)
    {
        if (count <= 0) return 0.f;
        S32 hit = 0;
        for (S32 i = 0; i < count; ++i)
        {
            if (fineVerdicts[i]) ++hit;
        }
        return (F32)hit / (F32)count;
    }

    // Body radius for a macro cell: MACRO_M * BODY_RADIUS_FRAC. Body alpha: fineAlpha * occupancy, capped at BODY_ALPHA_MAX.
    // Coverage conservation: radius^2 * alpha of one body equals the sum over the occupied fine cells of their radius^2 *
    // alpha within 5% for occupancy in {0.25, 0.5, 0.75, 1} (documented approximation - the areas overlap differently).
    inline F32 bodyRadiusM()
    {
        return MACRO_M * BODY_RADIUS_FRAC;
    }
    inline F32 bodyAlpha(F32 fineAlpha, F32 occupancy)
    {
        return llmin(BODY_ALPHA_MAX, fineAlpha * occupancy);
    }

    // The tier weights at camera distance dist: wA = 1 - smoothstep(TIER_B_M - TIER_BLEND_M/2, TIER_B_M + TIER_BLEND_M/2,
    // dist), wB = 1 - wA. Invariants: wA + wB == 1 within 1e-6; wA 1 well inside; wB 1 well beyond; continuous; monotone.
    inline void tierWeights(F32 dist, F32& wA, F32& wB)
    {
        const F32 lo = TIER_B_M - TIER_BLEND_M * 0.5f;
        const F32 hi = TIER_B_M + TIER_BLEND_M * 0.5f;
        const F32 t = llclamp((dist - lo) / (hi - lo), 0.f, 1.f);
        wB = t * t * (3.f - 2.f * t); // smoothstep
        wA = 1.f - wB;
    }

    // The macro body's altitude fraction: the mean of the occupied fine cells' up_cell hashes (the caller supplies them)
    // so the body sits where its block's mass sits. Invariants: in [0,1]; equals the single value when one cell is occupied;
    // pure.
    inline F32 bodyUp(const F32* ups, const U8* verdicts, S32 count)
    {
        F32 sum = 0.f;
        S32 hit = 0;
        for (S32 i = 0; i < count; ++i)
        {
            if (verdicts[i])
            {
                sum += ups[i];
                ++hit;
            }
        }
        return (hit > 0) ? (sum / (F32)hit) : 0.5f;
    }

    // Whether the macro tier is even eligible for a macro cell: its block centre must be within the walk (SSDeckLod::cellInWalk
    // semantics on the block centre) and at least one fine cell occupied. Pure.
    inline bool macroEligible(F32 blockCentreX, F32 blockCentreY, F32 camX, F32 camY, F32 occupancy)
    {
        return SSDeckLod::cellInWalk(blockCentreX, blockCentreY, camX, camY) && occupancy > 0.f;
    }
}

#endif
