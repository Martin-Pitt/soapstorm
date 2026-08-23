/**
 * @file ssatmov3trackstate.cpp
 * @brief Atmo Magic v3 track resolution implementation.
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

#include "ssatmov3trackstate.h"

#include <cmath>

// <SS:Nexii> Atmo Magic v3: track resolution and crossing behaviour

// static
S32 SSAtmoV3TrackResolver::trackContaining(const SSAtmoV3Asset& asset, F32 world_z)
{
    // Ground (index 0) is deliberately checked last: it is whatever isn't
    // claimed by an optional track's own band, not a band with a floor/
    // ceiling of its own worth trusting literally.
    for (size_t i = 1; i < asset.mTracks.size(); ++i)
    {
        const SSAtmoV3Track& t = asset.mTracks[i];
        if (world_z >= t.mFloorZ && world_z < t.mCeilingZ) return (S32)i;
    }
    return 0;
}

// static
bool SSAtmoV3TrackResolver::nearestBoundary(const SSAtmoV3Asset& asset, S32 primary, F32 world_z,
                                             S32& out_neighbor, F32& out_distance)
{
    bool found = false;
    F32 best = FLT_MAX;
    S32 best_idx = -1;

    for (size_t i = 0; i < asset.mTracks.size(); ++i)
    {
        if ((S32)i == primary) continue;
        const SSAtmoV3Track& t = asset.mTracks[i];

        const F32 floor_dist = std::fabs(world_z - t.mFloorZ);
        if (floor_dist < best)
        {
            best = floor_dist;
            best_idx = (S32)i;
            found = true;
        }

        // FLT_MAX is "open ended" (the ground track's stored default, and
        // any optional track's own default before it's given a real
        // ceiling) - not a boundary that's actually there to cross.
        if (t.mCeilingZ < FLT_MAX)
        {
            const F32 ceiling_dist = std::fabs(world_z - t.mCeilingZ);
            if (ceiling_dist < best)
            {
                best = ceiling_dist;
                best_idx = (S32)i;
                found = true;
            }
        }
    }

    if (found)
    {
        out_neighbor = best_idx;
        out_distance = best;
    }
    return found;
}

// static
SSAtmoV3TrackBlend SSAtmoV3TrackResolver::resolve(const SSAtmoV3Asset& asset, F32 world_z,
                                                   F32 prev_world_z, bool teleported)
{
    SSAtmoV3TrackBlend result;
    result.mPrimaryTrack = trackContaining(asset, world_z);

    // Checked ahead of the teleported flag so a teleport that happens to
    // land across the water plane and a walk that crosses it land on the
    // same instant-cut path, rather than two separate ones that could
    // disagree.
    F32 water_height = 0.f;
    const bool has_water = asset.visibleWaterHeight(water_height);
    const bool crossed_water = has_water &&
        ((prev_world_z >= water_height) != (world_z >= water_height));

    if (teleported || crossed_water)
    {
        result.mInstantCut = true;
        return result;
    }

    S32 neighbor = -1;
    F32 distance = 0.f;
    const SSAtmoV3Track& primary_track = asset.mTracks[result.mPrimaryTrack];
    if (primary_track.mTransitionBuffer > 0.f &&
        nearestBoundary(asset, result.mPrimaryTrack, world_z, neighbor, distance) &&
        distance < primary_track.mTransitionBuffer)
    {
        result.mNeighborTrack = neighbor;
        result.mNeighborWeight = 1.f - (distance / primary_track.mTransitionBuffer);
    }

    return result;
}

// </SS:Nexii>
