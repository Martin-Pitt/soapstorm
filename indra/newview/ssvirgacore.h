/**
 * @file ssvirgacore.h
 * @brief Atmo Magic: distant rain shafts (virga curtains) - qualification, card-stack geometry, alpha profile, particle-rain handoff, budget trim. Header-only core. CONTRACT.
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

#ifndef SS_VIRGACORE_H
#define SS_VIRGACORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, <cmath>, <cstdint> only; keepHash below calls SSAtmoNoise::hash01/combine directly). The emitter's edge fade and placement come from ssdecklodcore.h / ssdeckframecore.h, which the SHELL includes - this core references neither. Design: doc/atmo_magic_far_clouds.md section 3 (phase 6c). Rain falling from distant cells is drawn as fog-like curtains: per qualifying cell a vertical stack of Z-axis-billboarded cards no taller than CELL_M from the deck base down to the ground reference, emitted INTO the deck's puff vector (flagged on the spare b vertex channel) so the existing farthest-first painter's sort interleaves them, shaded by the puff shade/form formulas with buried pinned near 1, REAL alpha in the sky forward pass only. Driver: precipitation intensity x cell presence (the storm coupling's per-cell tower feeds intensity when active). Handoff: shafts skip inside r2 x HANDOFF_SKIP of the particle rain's TIER_SHEETS radius and ramp in over HANDOFF_BAND_M so one shared radius owns the boundary. Budget: a STABLE HASH TRIM (keepHash) - each qualifying cell keeps iff hash01(cell) < p, p = MAX_SHAFTS / quantised(n) - so when the candidate count n moves (camera walk crossing, precipitation easing) only the cells whose hash sits between the old and new p change, never the whole kept set (the phase-6c review found an index-based trim re-picked all 64 curtains on every count change). The walk that bounds n is camera-centred (LOD), but each cell's qualify AND keep decision is a function of (cell, weather, the quantised n) only - with ONE stated exception: the hard-ceiling backstop (hardCap), a rank cut on a fixed per-cell hash key that fires only when the hash trim's random excursion keeps more than HARD_CAP_FRAC x MAX_SHAFTS (about a 4-sigma event at n >> cap, since E[kept] <= cap); when it fires, the dropped cells are those with the LARGEST keys, so growing or shrinking the kept set by one moves at most one cell across the cut - an order statistic, not a reshuffle. Card count is NOT distance LOD - it is the span / CARD_MAX_M (stated plainly; only alpha/handoff/edge and the far ground lift depend on distance). Cards in a stack OVERLAP by OVERLAP_FRAC of a card so the shader's two-sided soft ends crossfade instead of punching a transparent band at each seam. Beyond the squash knee the curtain's bottom LIFTS off the ground (groundLiftZ) - physically virga, precipitation evaporating before it lands - which also keeps ground-level geometry out of the far-squash depth fold that would otherwise draw it over terrain standing in front of it; stated residual: the fold still affects a curtain's mid-air cards against tall terrain, as it does far puffs. The shaft is base-anchored in the O(z) sense (reads gate_air, like the sheet) and is placed by SSDeckFrame::placeWorld with the hero shift, never a respelled sum.
#include "lldefs.h"
#include "ssatmonoisecore.h"

#include <cmath>
#include <cstdint>

namespace SSVirga
{
    constexpr F32 CELL_M          = 260.f;   // LOCKSTEP ssvolcloud.cpp CELL_M
    constexpr F32 THRESHOLD       = 0.18f;   // precip * presence must clear this (scaled inversely by the Weather Influence "Distant rain" strength)
    constexpr F32 CARD_MAX_M      = 260.f;   // max card height (<= CELL_M so the per-vertex squash stays accurate, see the veil's own comment)
    constexpr F32 WIDTH_FRAC      = 0.62f;   // shaft half-width at the deck base as a share of CELL_M (7d: > 0.5 so neighbouring qualifying cells' curtains OVERLAP into a sheet instead of standing as isolated pillars - the user's 'blurry rectangles')
    constexpr F32 TAPER_GROUND    = 0.6f;    // half-width at the ground as a share of the base half-width (virga narrows down)
    constexpr F32 ALPHA_TOP       = 0.35f;   // alpha at the deck base at full drive (7d: 0.55 read as solid panels)
    constexpr F32 ALPHA_GROUND    = 0.03f;   // alpha at the ground at full drive (7d: virga all but vanishes before it lands)
    constexpr F32 HANDOFF_SKIP    = 1.1f;    // no shaft cards inside r2 * HANDOFF_SKIP of the camera
    constexpr F32 HANDOFF_BAND_M  = 400.f;   // ramp from 0 at r2 * HANDOFF_SKIP to full over this band
    constexpr S32 MAX_SHAFTS      = 64;
    constexpr F32 HARD_CAP_FRAC   = 1.3f;    // hardCap: the hash trim's kept count may not exceed this share of MAX_SHAFTS (rank-cut backstop, see header)
    constexpr F32 BURIED          = 1.f;     // LOCKSTEP ssvolcloud.h Deck::SHEET_BURIED - a curtain is the deck's underside like the veil
    constexpr F32 OVERLAP_FRAC    = 0.22f;   // LOCKSTEP ssVolCloudF.glsl SS_SHAFT_V_SOFT: consecutive cards overlap by this share of a card so the soft ends crossfade
    constexpr F32 LIFT_START_FRAC = 1.0f;    // ground lift begins at this multiple of the squash knee
    constexpr F32 LIFT_FULL_FRAC  = 2.0f;    // ... and reaches LIFT_MAX_FRAC of the span at this multiple
    constexpr F32 LIFT_MAX_FRAC   = 0.55f;   // the far curtain ends this share of the way up from the ground (virga)
    constexpr F32 N_QUANT_STEP    = 1.25f;   // keepHash quantises n to the next power of this so p moves in coarse steps

    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    // Drive strength of a cell: precipIntensity * presence * (0.85 + 0.30 * tower), clamped to [0,1] - the same shape
    // precipitation's own spawn-rate gate uses, so a curtain hangs exactly where rain would fall. Invariants: 0 when either
    // precipIntensity or presence is 0; monotone in all three; <= 1.
    inline F32 drive(F32 precipIntensity, F32 presence, F32 tower)
    {
        return llclampf(precipIntensity * presence * (0.85f + 0.30f * tower));
    }

    // Whether a cell qualifies: drive >= THRESHOLD / max(strength, 0.05) where strength is the influence row's 0..1 dial
    // (higher strength -> lower effective threshold). Invariants: strength 0 never qualifies; drive 1 always qualifies for
    // strength >= 0.18; monotone in drive and in strength; pure (no distance term - qualification is world state).
    inline bool qualifies(F32 drive, F32 strength)
    {
        return drive >= (THRESHOLD / llmax(strength, 0.05f));
    }

    // Card count for a shaft spanning [groundZ, baseZ]: ceil(span / CARD_MAX_M), at least 1; 0 when baseZ <= groundZ.
    inline S32 cardCount(F32 baseZ, F32 groundZ)
    {
        const F32 span = baseZ - groundZ;
        if (span <= 0.f) return 0;
        return llmax(1, (S32)std::ceil(span / CARD_MAX_M));
    }

    // Half-width at height fraction h01 (0 ground, 1 deck base): CELL_M * WIDTH_FRAC * lerp(TAPER_GROUND, 1, h01).
    // Invariants: monotone non-decreasing in h01; at h01 1 equals CELL_M * WIDTH_FRAC.
    inline F32 halfWidthM(F32 h01)
    {
        return CELL_M * WIDTH_FRAC * (TAPER_GROUND + (1.f - TAPER_GROUND) * h01);
    }

    // Alpha at h01 for a drive d: d * lerp(ALPHA_GROUND, ALPHA_TOP, smoothstep(0, 1, h01)). Invariants: 0 at drive 0;
    // monotone in h01 and d; <= ALPHA_TOP.
    inline F32 alphaAt(F32 h01, F32 drive)
    {
        const F32 t = llclampf(h01);
        const F32 s = t * t * (3.f - 2.f * t); // smoothstep
        return drive * (ALPHA_GROUND + (ALPHA_TOP - ALPHA_GROUND) * s);
    }

    // The handoff weight against the particle rain's sheets radius r2 (metres): 0 for dist <= r2 * HANDOFF_SKIP, rising
    // with smoothstep to 1 at r2 * HANDOFF_SKIP + HANDOFF_BAND_M. Camera-distance LOD only (never changes which cells
    // qualify). Invariants: 0 inside; 1 beyond the band; monotone; continuous.
    inline F32 handoff(F32 dist, F32 r2)
    {
        const F32 skipR = r2 * HANDOFF_SKIP;
        if (dist <= skipR) return 0.f;
        const F32 bandEnd = skipR + HANDOFF_BAND_M;
        if (dist >= bandEnd) return 1.f;
        const F32 t = (dist - skipR) / HANDOFF_BAND_M;
        return t * t * (3.f - 2.f * t); // smoothstep
    }

    // Quantise the candidate count upward to the next power of N_QUANT_STEP (>= 1). Invariants: >= n; >= 1; monotone
    // non-decreasing in n; a change of n within one quantum returns the same value.
    inline F32 quantiseCount(S32 n)
    {
        const F32 nf = (F32)llmax(0, n);
        if (nf <= 1.f) return 1.f;
        F32 q = 1.f;
        while (q < nf) q *= N_QUANT_STEP;
        return q;
    }

    // STABLE budget trim: keep iff SSAtmoNoise::hash01(combine(cellId, salt)) < min(1, cap / quantiseCount(n)). Invariants:
    // pure in (cellId, salt, n, cap); keeps everything when n <= cap; over 20000 hashed cells the kept share is within 0.03 of
    // min(1, cap / quantiseCount(n)); for two counts n1 < n2 inside one quantum the kept sets are IDENTICAL; across a
    // quantum boundary only cells whose hash lies between the two thresholds change (measured share within 0.03 of the
    // threshold difference).
    //
    // <SS:Nexii> The 64-bit cellId is folded into the 32-bit hash the same way the shell folds any 64-bit lattice id
    // elsewhere: low word and (high word XOR salt) through SSAtmoNoise::combine, so salt perturbs the fold without a
    // second combine call. STATED RESIDUAL: the "keeps everything when n <= cap" invariant is exact only when
    // quantiseCount(n) itself does not overshoot cap - N_QUANT_STEP's 25% ceiling means a narrow band of n just below
    // cap (n in (cap/N_QUANT_STEP, cap]) quantises past cap and trims slightly (p as low as ~0.8, never lower); the
    // min(1, ...) clamp still makes it exact everywhere else. No test asserts the literal "everything" claim for that
    // band; the "kept share within 0.03 of p" test covers the true behaviour there instead.
    inline bool keepHash(U64 cellId, U32 salt, S32 n, S32 cap)
    {
        const U32 lo = (U32)cellId;
        const U32 hi = (U32)(cellId >> 32) ^ salt;
        const F32 h = SSAtmoNoise::hash01(SSAtmoNoise::combine(lo, hi));
        const F32 p = llmin(1.f, (F32)cap / quantiseCount(n));
        return h < p;
    }

    // The hard ceiling the shell's rank-cut backstop enforces on the hash-kept set: floor(cap * HARD_CAP_FRAC).
    // Invariants: >= cap for cap >= 0; monotone in cap; hardCap(MAX_SHAFTS) == 83.
    inline S32 hardCap(S32 cap)
    {
        return (S32)((F32)llmax(0, cap) * HARD_CAP_FRAC);
    }

    // The k-th card's slab (k = 0 at the deck base) for a stack spanning [groundZ, baseZ]: cards are CARD_MAX_M tall and
    // step down by CARD_MAX_M * (1 - OVERLAP_FRAC), the last card clamped to end at groundZ; h01Mid is the card centre's
    // height fraction (0 ground, 1 base). Invariants: card 0's top == baseZ; consecutive cards overlap by >= OVERLAP_FRAC *
    // CARD_MAX_M (except the last, which may be shorter); the last card's bottom == groundZ; zTop > zBot for every card;
    // cardCountOverlapped(baseZ, groundZ) cards cover the span with no gap.
    struct CardSpan
    {
        F32 zTop = 0.f;
        F32 zBot = 0.f;
        F32 h01Mid = 0.f;
    };
    inline S32 cardCountOverlapped(F32 baseZ, F32 groundZ)
    {
        const F32 span = baseZ - groundZ;
        if (span <= 0.f) return 0;
        if (span <= CARD_MAX_M) return 1;
        const F32 stride = CARD_MAX_M * (1.f - OVERLAP_FRAC);
        const F32 remain = span - CARD_MAX_M;
        return 1 + (S32)std::ceil(remain / stride);
    }
    inline CardSpan cardSpan(S32 k, F32 baseZ, F32 groundZ)
    {
        const F32 stride = CARD_MAX_M * (1.f - OVERLAP_FRAC);
        const S32 n = cardCountOverlapped(baseZ, groundZ);
        const F32 top = baseZ - (F32)k * stride;
        F32 bot = top - CARD_MAX_M;
        if (k >= n - 1 || bot <= groundZ)
        {
            bot = groundZ; // last card (or one that would undershoot) clamps to the ground reference
        }
        CardSpan cs;
        cs.zTop = top;
        cs.zBot = bot;
        const F32 span = llmax(1e-6f, baseZ - groundZ);
        cs.h01Mid = llclamp(((top + bot) * 0.5f - groundZ) / span, 0.f, 1.f);
        return cs;
    }

    // The far ground lift: the curtain's effective ground altitude at camera distance dist, given the squash knee: groundZ +
    // (baseZ - groundZ) * LIFT_MAX_FRAC * smoothstep(knee * LIFT_START_FRAC, knee * LIFT_FULL_FRAC, dist). Cards whose whole
    // slab lies below the lifted ground are not emitted; the lowest emitted card's bottom is the lifted ground. Invariants:
    // == groundZ at or inside the knee; monotone in dist; never above groundZ + LIFT_MAX_FRAC * span; continuous.
    inline F32 groundLiftZ(F32 groundZ, F32 baseZ, F32 dist, F32 kneeM)
    {
        const F32 span = baseZ - groundZ;
        const F32 t0 = kneeM * LIFT_START_FRAC;
        const F32 t1 = kneeM * LIFT_FULL_FRAC;
        F32 s;
        if (t1 <= t0)
        {
            s = (dist >= t0) ? 1.f : 0.f; // degenerate knee (<= 0): step instead of divide by zero
        }
        else
        {
            const F32 t = llclamp((dist - t0) / (t1 - t0), 0.f, 1.f);
            s = t * t * (3.f - 2.f * t); // smoothstep
        }
        return groundZ + span * LIFT_MAX_FRAC * s;
    }

    // The k-th card's slab after the far ground lift: false (not emitted) when the whole slab lies at or below liftZ;
    // otherwise out = cardSpan(k, ...) with zBot raised to liftZ. h01Mid is left at the card's TRUE (unlifted) height
    // fraction - the lift truncates what is drawn, it does not change the physical curtain's vertical profile.
    // Invariants: returns false iff cardSpan(k).zTop <= liftZ; when true, zBot >= liftZ, zTop > zBot, zTop and h01Mid
    // equal cardSpan(k)'s; with liftZ <= groundZ it is cardSpan(k) unchanged.
    inline bool cardSpanLifted(S32 k, F32 baseZ, F32 groundZ, F32 liftZ, CardSpan& out)
    {
        const CardSpan cs = cardSpan(k, baseZ, groundZ);
        if (cs.zTop <= liftZ) return false;
        out = cs;
        out.zBot = llmax(cs.zBot, liftZ);
        return out.zTop > out.zBot;
    }

    // Edge fade: the emitter calls SSDeckLod::edgeFade directly (one formula site); this core deliberately owns no copy.
}

#endif
