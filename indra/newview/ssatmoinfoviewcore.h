/**
 * @file ssatmoinfoviewcore.h
 * @brief Atmo Magic: the info-view chart/legend geometry helpers, header-only core.
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

#ifndef SS_ATMOINFOVIEWCORE_H
#define SS_ATMOINFOVIEWCORE_H

// <SS:Nexii> A CORE header (lldefs.h + <cmath> + <cstdint> only; NO llmath.h). Design: doc/atmo_magic_debug_views.md sections 1 and 3. The pure geometry the info views share: the colour ramps of the one colour language (section 1 point 4), the altitude ladder the wind-profile chart samples windAt(z) on, the rung table the in-world wind mast stacks its arrows on, and the pixel mappings of the chart and its hodograph inset. Everything here is a pure function of its arguments so the scratch harness can pin it; nothing here reads a setting, the camera or a system - the shell (ssatmoinfoview.cpp) does the reading and hands the numbers in. None of it positions world content, so it is free of the determinism contract - but it must never be used to.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSAtmoInfoViewCore
{
    // The info-view modes of the SSAtmoInfoView setting. 0 is off; the catalogue numbers follow the design doc's V-numbers.
    constexpr U32 MODE_OFF          = 0;
    constexpr U32 MODE_WIND_PROFILE = 1;

    struct RGB
    {
        F32 r = 0.f;
        F32 g = 0.f;
        F32 b = 0.f;
    };

    struct PixelPoint
    {
        S32 x = 0;
        S32 y = 0;
    };

    // A chart's plotting rectangle in view pixels: left/bottom corner plus size. Axes run right (x) and up (y).
    struct ChartBox
    {
        S32 left   = 0;
        S32 bottom = 0;
        S32 w      = 1;
        S32 h      = 1;
    };

    // Unit fraction of value in [0, max], NaN-safe: a non-positive max reads any positive value as full scale and everything else as zero.
    inline F32 unitOf(F32 value, F32 max)
    {
        if (!(max > 0.f))
        {
            return value > 0.f ? 1.f : 0.f;
        }
        return llclamp(value / max, 0.f, 1.f);
    }

    // Wind speed ramp: dark -> bright teal (design colour language). Invariants: every channel monotone non-decreasing in speed; speed 0 gives the dark end exactly; speed >= max gives the bright end exactly; all channels in [0,1].
    inline RGB speedRamp(F32 speedMS, F32 maxMS)
    {
        const F32 t = unitOf(speedMS, maxMS);
        RGB c;
        c.r = std::lerp(0.02f, 0.55f, t);
        c.g = std::lerp(0.20f, 0.96f, t);
        c.b = std::lerp(0.24f, 0.92f, t);
        return c;
    }

    // Presence / coverage ramp: grey -> white. Same invariants as speedRamp.
    inline RGB presenceRamp(F32 value, F32 max)
    {
        const F32 t = unitOf(value, max);
        RGB c;
        c.r = std::lerp(0.30f, 1.f, t);
        c.g = c.r;
        c.b = c.r;
        return c;
    }

    // Energy / convection ramp: orange -> red. Same invariants as speedRamp (r is constant-bright, g falls, so "monotone" is per channel in either direction: r non-decreasing, g non-increasing, b non-increasing).
    inline RGB energyRamp(F32 value, F32 max)
    {
        const F32 t = unitOf(value, max);
        RGB c;
        c.r = 1.f;
        c.g = std::lerp(0.62f, 0.08f, t);
        c.b = std::lerp(0.10f, 0.05f, t);
        return c;
    }

    // The smallest "nice" axis ceiling (1, 2 or 5 times a power of ten) that is >= v, never below 1. Invariants: result >= v; result >= 1; for v >= 1 result < 2.5 * v; idempotent (niceMax(niceMax(v)) == niceMax(v)); NaN or negative v gives 1.
    inline F32 niceMax(F32 v)
    {
        if (!(v > 1.f))
        {
            return 1.f;
        }
        const F32 e = std::floor(std::log10(v));
        const F32 p = std::pow(10.f, e);
        const F32 m = v / p;
        F32 step;
        if (m <= 1.f)      step = 1.f;
        else if (m <= 2.f) step = 2.f;
        else if (m <= 5.f) step = 5.f;
        else               step = 10.f;
        F32 r = step * p;
        // Guard the floating seam: pow/log10 rounding can land a hair under v.
        if (r < v)
        {
            r = (step >= 10.f) ? 20.f * p : (step >= 5.f ? 10.f * p : (step >= 2.f ? 5.f * p : 2.f * p));
        }
        return r;
    }

    // The chart's altitude ladder: sample i of n from 0 to topAgl, quadratically biased toward the ground where the power law does all its bending. Invariants: i=0 gives 0; i=n-1 gives topAgl exactly; strictly increasing in i for topAgl > 0; n < 2 gives topAgl.
    inline F32 altitudeLadder(F32 topAgl, S32 n, S32 i)
    {
        if (n < 2)
        {
            return topAgl;
        }
        const S32 k = llclamp(i, 0, n - 1);
        if (k == n - 1)
        {
            return topAgl;
        }
        const F32 t = (F32)k / (F32)(n - 1);
        return topAgl * t * t;
    }

    // The in-world mast's rung altitudes AGL: a fixed low table where shear is steep, then MAST_STEP_M steps, then the top itself. mastRungCount(top) rungs, mastRungAgl(i, top) the i-th. Invariants: every rung in (0, top]; strictly increasing in i; the last rung == top; top <= 0 gives zero rungs; rung 0 == MAST_LOW_RUNGS[0] whenever top exceeds it.
    constexpr S32 MAST_LOW_RUNG_COUNT = 10;
    constexpr F32 MAST_LOW_RUNGS[MAST_LOW_RUNG_COUNT] = { 10.f, 25.f, 50.f, 100.f, 200.f, 350.f, 500.f, 750.f, 1000.f, 1500.f };
    constexpr F32 MAST_STEP_M = 500.f;

    inline S32 mastRungCount(F32 topAgl)
    {
        if (!(topAgl > 0.f))
        {
            return 0;
        }
        S32 n = 0;
        for (S32 i = 0; i < MAST_LOW_RUNG_COUNT; ++i)
        {
            if (MAST_LOW_RUNGS[i] < topAgl) ++n;
        }
        const F32 last_low = MAST_LOW_RUNGS[MAST_LOW_RUNG_COUNT - 1];
        if (topAgl > last_low)
        {
            // Steps strictly between the last low rung and the top.
            const S32 k_first = (S32)floor(last_low / MAST_STEP_M) + 1;          // 4 -> 2000m
            const S32 k_last  = (S32)std::ceil(topAgl / MAST_STEP_M) - 1;         // largest k with k*step < top
            if (k_last >= k_first) n += k_last - k_first + 1;
        }
        return n + 1; // the top itself
    }

    inline F32 mastRungAgl(S32 i, F32 topAgl)
    {
        const S32 n = mastRungCount(topAgl);
        if (n <= 0)
        {
            return 0.f;
        }
        const S32 k = llclamp(i, 0, n - 1);
        if (k == n - 1)
        {
            return topAgl;
        }
        S32 low = 0;
        for (S32 j = 0; j < MAST_LOW_RUNG_COUNT; ++j)
        {
            if (MAST_LOW_RUNGS[j] < topAgl) ++low;
        }
        if (k < low)
        {
            return MAST_LOW_RUNGS[k];
        }
        const F32 last_low = MAST_LOW_RUNGS[MAST_LOW_RUNG_COUNT - 1];
        const S32 k_first = (S32)floor(last_low / MAST_STEP_M) + 1;
        return (F32)(k_first + (k - low)) * MAST_STEP_M;
    }

    // Arrow length for a mast rung: a fraction of the gap to the next rung, scaled by speed but floored so a calm rung still shows its heading. Invariants: in [0.08, 1] * gapM * 0.45; monotone in speed.
    inline F32 mastArrowLen(F32 speedMS, F32 maxMS, F32 gapM)
    {
        const F32 t = llclamp(unitOf(speedMS, maxMS), 0.08f, 1.f);
        return llmax(gapM, 0.f) * 0.45f * t;
    }

    // Pixel x of a speed on the chart's horizontal axis. Invariants: 0 -> box.left; >= max -> box.left + box.w; monotone.
    inline S32 chartX(F32 speedMS, F32 maxMS, const ChartBox& box)
    {
        return box.left + (S32)floor(unitOf(speedMS, maxMS) * (F32)box.w + 0.5f);
    }

    // Pixel y of an altitude on the chart's vertical axis. Invariants: 0 -> box.bottom; >= top -> box.bottom + box.h; monotone.
    inline S32 chartY(F32 agl, F32 topAgl, const ChartBox& box)
    {
        return box.bottom + (S32)floor(unitOf(agl, topAgl) * (F32)box.h + 0.5f);
    }

    // Hodograph inset: a wind vector (x east, y north, m/s) as a pixel about (cx, cy) with |v| == maxMS on the radius. Invariants: the zero vector lands on the centre exactly; a vector of length maxMS lands within one pixel of the radius; east is +x, north is +y (up).
    inline PixelPoint hodoPoint(F32 vx, F32 vy, F32 maxMS, S32 cx, S32 cy, S32 radius)
    {
        PixelPoint p;
        const F32 k = (maxMS > 0.f) ? (F32)radius / maxMS : 0.f;
        p.x = cx + (S32)floor(vx * k + 0.5f);
        p.y = cy + (S32)floor(vy * k + 0.5f);
        return p;
    }

    // Compass heading (degrees, 0 north, clockwise, in [0, 360)) the vector points TOWARD. Invariants: (0,1)->0, (1,0)->90, (0,-1)->180, (-1,0)->270 (to float precision); the zero vector gives 0; never NaN.
    inline F32 headingOfVec(F32 vx, F32 vy)
    {
        if (vx == 0.f && vy == 0.f)
        {
            return 0.f;
        }
        constexpr F32 kRadToDeg = 57.29577951308232f;
        F32 h = std::atan2(vx, vy) * kRadToDeg;
        if (h < 0.f) h += 360.f;
        if (h >= 360.f) h -= 360.f;
        return h;
    }
}

#endif
