/**
 * @file sslufs.h
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

#ifndef SS_LUFS_H
#define SS_LUFS_H

#include "stdtypes.h"
#include <cstddef>

const F32 SS_LUFS_SILENCE = -100.f; // returned when nothing rises above the absolute gate
const F32 SS_LUFS_TOLERANCE = 0.5f; // skip re-processing when already within this many LU of target

// gated integrated loudness of mono 16-bit PCM per BS.1770-4; clips under 400ms measure as one block
F32 ss_lufs_measure_mono16(const S16* samples, size_t count, U32 sample_rate);

// linear gain that moves measured_lufs onto target_lufs; 1.0 when measurement was silence
F32 ss_lufs_gain_for_target(F32 measured_lufs, F32 target_lufs);

// in-place linear gain with hard clamp to 16-bit range
void ss_lufs_apply_gain16(S16* samples, size_t count, F32 gain);

#endif
