/**
 * @file sslufs.cpp
 * @brief ITU-R BS.1770-4 integrated loudness (LUFS) measurement and gain for 16-bit PCM. <SS:Nexii>
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Soapstorm Viewer Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "sslufs.h"
#include "llmath.h"

#include <cmath>
#include <vector>

namespace
{

struct Biquad
{
    F64 b0, b1, b2, a1, a2;
    F64 x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    inline F64 process(F64 x)
    {
        F64 y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

// K-weighting stage 1: high-shelf; de-quantized BS.1770 parameters (libebur128 derivation) so any sample rate works
Biquad make_shelf(F64 fs)
{
    const F64 f0 = 1681.974450955533;
    const F64 G  = 3.999843853973347;
    const F64 Q  = 0.7071752369554196;

    F64 K  = tan(F_PI * f0 / fs);
    F64 Vh = pow(10.0, G / 20.0);
    F64 Vb = pow(Vh, 0.4996667741545416);
    F64 a0 = 1.0 + K / Q + K * K;

    Biquad bq;
    bq.b0 = (Vh + Vb * K / Q + K * K) / a0;
    bq.b1 = 2.0 * (K * K - Vh) / a0;
    bq.b2 = (Vh - Vb * K / Q + K * K) / a0;
    bq.a1 = 2.0 * (K * K - 1.0) / a0;
    bq.a2 = (1.0 - K / Q + K * K) / a0;
    return bq;
}

// K-weighting stage 2: RLB high-pass
Biquad make_highpass(F64 fs)
{
    const F64 f0 = 38.13547087602444;
    const F64 Q  = 0.5003270373238773;

    F64 K  = tan(F_PI * f0 / fs);
    F64 a0 = 1.0 + K / Q + K * K;

    Biquad bq;
    bq.b0 = 1.0;
    bq.b1 = -2.0;
    bq.b2 = 1.0;
    bq.a1 = 2.0 * (K * K - 1.0) / a0;
    bq.a2 = (1.0 - K / Q + K * K) / a0;
    return bq;
}

inline F64 block_loudness(F64 power)
{
    return -0.691 + 10.0 * log10(power);
}

} // namespace

F32 ss_lufs_measure_mono16(const S16* samples, size_t count, U32 sample_rate)
{
    if (!samples || !count || !sample_rate) return SS_LUFS_SILENCE;

    Biquad shelf = make_shelf((F64)sample_rate);
    Biquad hp = make_highpass((F64)sample_rate);

    // accumulate K-weighted energy per 100ms sub-block; a 400ms gating block is 4 consecutive sub-blocks (75% overlap)
    size_t sub_len = sample_rate / 10;
    std::vector<F64> sub_energy;
    sub_energy.reserve(count / sub_len + 2);
    F64 acc = 0.0;
    size_t n = 0;
    F64 total = 0.0;
    for (size_t i = 0; i < count; i++)
    {
        F64 x = (F64)samples[i] / 32768.0;
        x = hp.process(shelf.process(x));
        acc += x * x;
        total += x * x;
        if (++n == sub_len)
        {
            sub_energy.push_back(acc);
            acc = 0.0;
            n = 0;
        }
    }

    std::vector<F64> powers;
    if (sub_energy.size() >= 4)
    {
        F64 block_div = (F64)(4 * sub_len);
        for (size_t j = 0; j + 4 <= sub_energy.size(); j++)
        {
            powers.push_back((sub_energy[j] + sub_energy[j + 1] + sub_energy[j + 2] + sub_energy[j + 3]) / block_div);
        }
    }
    else
    {
        // clip shorter than a gating block: best effort, measure the whole clip as one block
        powers.push_back(total / (F64)count);
    }

    const F64 ABS_GATE = -70.0;
    F64 sum = 0.0;
    size_t cnt = 0;
    for (F64 p : powers)
    {
        if (p > 0.0 && block_loudness(p) > ABS_GATE) { sum += p; cnt++; }
    }
    if (!cnt) return SS_LUFS_SILENCE;

    F64 rel_gate = block_loudness(sum / (F64)cnt) - 10.0;
    F64 sum2 = 0.0;
    size_t cnt2 = 0;
    for (F64 p : powers)
    {
        if (p > 0.0)
        {
            F64 l = block_loudness(p);
            if (l > ABS_GATE && l > rel_gate) { sum2 += p; cnt2++; }
        }
    }
    if (!cnt2) return SS_LUFS_SILENCE;

    return (F32)block_loudness(sum2 / (F64)cnt2);
}

F32 ss_lufs_gain_for_target(F32 measured_lufs, F32 target_lufs)
{
    if (measured_lufs <= SS_LUFS_SILENCE) return 1.f;
    return powf(10.f, (target_lufs - measured_lufs) / 20.f);
}

void ss_lufs_apply_gain16(S16* samples, size_t count, F32 gain)
{
    for (size_t i = 0; i < count; i++)
    {
        S32 v = ll_round((F32)samples[i] * gain);
        samples[i] = (S16)llclamp(v, -32768, 32767);
    }
}
