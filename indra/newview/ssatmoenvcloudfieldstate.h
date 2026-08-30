/**
 * @file ssatmoenvcloudfieldstate.h
 * @brief Atmo Magic: cloud field state resolver.
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

#include "ssatmoenvasset.h"

struct SSAtmoEnvCloudFieldState
{
    F32 mCoverage = 0.f;

    // <SS:Nexii> The deck's base in WORLD metres: the authored offset above the track floor
    // (negative below it) with the floor already added. Everything downstream - puff placement,
    // the dome altitude merge, precipitation's tilt maths - is world-frame and reads this.
    // </SS:Nexii>
    F32 mBaseHeightM = 800.f;
    F32 mThicknessM = 0.f;

    F32 mChurn = 0.f;

    bool mHasAnvil = false;

    F32 mAnvil = 0.f;

    LLUUID mBaseTexture;
    LLUUID mDetailTexture;

    // <SS:Nexii> The convection noise map and its metres-per-tile multiplier, straight off the
    // authored keyframes - the deck does its own shaping (see ssvolcloud). mProfileTexture is the
    // vertical profile ramp: none runs the built-in vertical curves.
    LLUUID mNoiseTexture;
    F32 mNoiseScale = 1.f;
    LLUUID mProfileTexture;

    F32 mTextureMix = 0.f;
    F32 mPuffDensity = 0.8f;
    F32 mDetailScale = 1.f;
    F32 mDriftRate = 1.f;

    F32 mGloom = 1.f;
};

class SSAtmoEnvCloudFieldResolver
{
public:
    // <SS:Nexii> track_floor_z is the owning track's vertical position (SSAtmoEnvTrack::mFloorZ):
    // the authored base height - and the auto derivation's answer - are offsets above it, so the
    // whole deck rides the track. The ground track sits at 0, where nothing changes numerically.
    // </SS:Nexii>
    static SSAtmoEnvCloudFieldState resolve(const SSAtmoEnvCloudField& field, F32 moisture,
                                            F32 convection, F64 phase, F32 track_floor_z);

    // Floor-relative: out_base_height is metres ABOVE the track floor the deck should sit at.
    static void deriveAutoBaseline(F32 moisture, F32 convection,
                                   F32& out_base_height, F32& out_thickness, F32& out_coverage_scale, F32& out_darkening);
};

#endif
