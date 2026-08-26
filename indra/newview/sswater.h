/**
 * @file sswater.h
 * @brief Atmo Magic: the SSWater plane family - Atmo-owned duplicates of the stock water planes.
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

#ifndef SS_WATER_H
#define SS_WATER_H

#include "llsingleton.h"
#include "llpointer.h"
#include "llvowater.h"

#include <vector>

// <SS:Nexii> Atmo Magic renders its own water plane objects instead of repurposing LLVOWater, keeping the stock planes pristine - future Atmo water work (tides, per-tile waterlines, horizon
// geometry) lands here without touching viewer water. The family reuses the stock water pcodes, pool, partitions and shaders wholesale, so every pcode-keyed check treats both families
// identically; the LLVOWater mIsAtmoWater flag is the only discriminator and SSWaterWorld below swaps which family draws. Because the pcodes are shared, the pcode factory cannot build these -
// SSWaterWorld news them directly and hands them to gObjectList.adoptViewerObject. Rationale and geometry conventions in doc/atmo_magic_water.md.

// One plane per connected region, at that region's own water height - the Atmo twin of the LLSurface region water object.
class SSWater : public LLVOWater
{
public:
    SSWater(const LLUUID& id, LLViewerRegion* regionp);
};

// A 256x256m void water tile - the grid ringing the regions replaces the stock nine-slab hole/skirt arrangement so each tile can later carry its own waterline.
class SSEdgeWater : public LLVOVoidWater
{
public:
    SSEdgeWater(const LLUUID& id, LLViewerRegion* regionp);
};

// Placeholder for the band from the tile ring to the true horizon, past MAX_FAR_CLIP - wired up but never instantiated yet (doc/atmo_magic_water.md, deferred).
class SSFarWater : public LLVOVoidWater
{
public:
    SSFarWater(const LLUUID& id, LLViewerRegion* regionp);
};

// Builds and tears down the SS family as the Atmo environment gains and loses the scene, and answers, per water face, which family owns the frame.
class SSWaterWorld : public LLSingleton<SSWaterWorld>
{
    LLSINGLETON_EMPTY_CTOR(SSWaterWorld);
    ~SSWaterWorld() = default;

public:
    void update();

    void clearWaterObjects();

    // The one gate both water draw call sites (pushWaterPlanes and the haze re-push) ask - exactly one of the stock or Atmo plane families renders on any frame.
    static bool drawsThisFrame(const LLVOWater* vo);

private:
    void rebuild(bool active);
    void rebuildFarWater();

    bool anyDead() const;

    // The world signature a rebuild is keyed on - when any field moves the whole SS set is recreated, which only fires on region changes, water height changes or the Atmo toggle.
    struct State
    {
        bool mActive = false;
        bool mTransparentWater = true;
        U64  mAgentHandle = 0;
        U32  mRegionCount = 0;
        U64  mHandleHash = 0;
        F32  mHeightSum = 0.f;
        F32  mAgentWaterHeight = 0.f;

        bool operator==(const State& o) const
        {
            return mActive == o.mActive && mTransparentWater == o.mTransparentWater && mAgentHandle == o.mAgentHandle && mRegionCount == o.mRegionCount
                && mHandleHash == o.mHandleHash && mHeightSum == o.mHeightSum && mAgentWaterHeight == o.mAgentWaterHeight;
        }
    };

    State mState;

    std::vector<LLPointer<LLVOWater> > mRegionWater;
    std::vector<LLPointer<LLVOWater> > mEdgeWater;

    static bool sAtmoWaterLive;
};

#endif // SS_WATER_H
