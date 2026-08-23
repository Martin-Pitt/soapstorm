/**
 * @file ssatmoenvtrackstate.h
 * @brief Atmo Magic: which track is active at a given altitude, and how
 *        a crossing between two of them should be presented. Phase 4 of
 *        doc/atmo_magic_environment.md - deliberately still not wired to
 *        a live camera/agent position; this is pure resolution logic over
 *        an asset and a couple of altitudes, testable on its own, the same
 *        way phases 1-3 stayed schema/logic before anything rendered.
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

#ifndef SS_ATMOENVTRACKSTATE_H
#define SS_ATMOENVTRACKSTATE_H

// <SS:Nexii> Atmo Magic: track resolution and crossing behaviour

#include "ssatmoenvasset.h"

// One evaluation's result: which track is "home" right now, and whether a
// neighbour is being blended in for a soft crossing.
struct SSAtmoEnvTrackBlend
{
    S32 mPrimaryTrack   = 0;
    S32 mNeighborTrack  = -1;   // -1 when not currently blending with anything
    F32 mNeighborWeight = 0.f;  // 0 = fully mPrimaryTrack, 1 = fully mNeighborTrack

    // True for exactly the one evaluation right after a jump - teleport,
    // sit-teleport, region-position-change, or a crossing that happened to
    // pass through the visible water plane all collapse to the same "cut,
    // don't blend" rule per the design doc, rather than three separate
    // flags a caller would have to remember to check.
    bool mInstantCut = false;
};

class SSAtmoEnvTrackResolver
{
public:
    // world_z / prev_world_z: this evaluation's altitude and the previous
    // one, agent-space. teleported covers every already-instant case that
    // isn't a plain crossing (teleport, sit-teleport, region-position-
    // change) - the caller knows which kind of movement just happened, this
    // function only needs to know whether it counts as one.
    //
    // A crossing that happens to pass through the asset's one globally
    // visible water plane also cuts rather than blends, per the design
    // doc's water-crossing rule - detected here from prev_world_z/world_z
    // straddling SSAtmoEnvAsset::visibleWaterHeight(), not passed in by the
    // caller, since it is a property of the crossing itself rather than
    // something the caller would otherwise have a reason to know.
    static SSAtmoEnvTrackBlend resolve(const SSAtmoEnvAsset& asset, F32 world_z,
                                       F32 prev_world_z, bool teleported);

private:
    // Ground (index 0) is the catch-all: whatever altitude isn't claimed by
    // an optional track's own [floor, ceiling) band. Optional tracks are
    // expected to be authored contiguously - addTrack() keeps new ones that
    // way - so this does not try to adjudicate an author's deliberately
    // overlapping or gapped tracks beyond "first one that contains it wins,
    // ground otherwise": a stricter rule can be added once real content
    // shows whether that case actually comes up.
    static S32 trackContaining(const SSAtmoEnvAsset& asset, F32 world_z);

    // The nearest *other* track's boundary to world_z, and how far away it
    // is - the input to the transition-buffer blend. False if there is no
    // other track to blend toward at all (a single-track asset).
    static bool nearestBoundary(const SSAtmoEnvAsset& asset, S32 primary, F32 world_z,
                                S32& out_neighbor, F32& out_distance);
};

// </SS:Nexii>

#endif // SS_ATMOENVTRACKSTATE_H
