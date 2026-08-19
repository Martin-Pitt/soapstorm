/**
 * @file ssrunoff.h
 * @brief Atmo Magic runoff: rain landing on a roof does not stay there. The
 *        rain shadow surface is traced downhill into a drainage network, so
 *        every sloped roof knows how much water reaches each of its edges,
 *        and sheds it off the eaves into the street below.
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

#ifndef SS_RUNOFF_H
#define SS_RUNOFF_H

// <SS:Nexii> Atmo Magic roof runoff

#include "llsingleton.h"
#include "v3math.h"

#include <map>
#include <vector>

// Where water leaves a surface: the lip of a roof, the end of an awning, the
// edge of a bridge deck. Positions are region-local so a region crossing
// leaves the network alone.
struct SSRunoffSite
{
    LLVector3 mLip;         // last point on the surface, region-local
    LLVector3 mLand;        // where the drop it sheds comes down, region-local
    LLVector3 mOut;         // horizontal direction the water was travelling
    F32 mCatchment = 0.f;   // square metres draining through here
    bool mOnWater = false;  // it lands on the water plane
};

class SSRunoff : public LLSingleton<SSRunoff>
{
    LLSINGLETON_EMPTY_CTOR(SSRunoff);

public:
    // Per-frame driver, from SSAtmoMagic::idle after the sim has stepped.
    // Refreshes at most one region's network per call and sheds water from
    // the sites near the camera.
    void idle(F32 dt);

    void clear();

    // Render Metadata > Roof Runoff: the drainage network drawn as flow
    // arrows over the surface, with the eaves marked by catchment.
    void renderDebug();

    // Simulation floater / info overlay
    S32 siteCount() const;
    S32 networkCount() const { return (S32)mNetworks.size(); }
    F32 lastBuildMS() const { return mLastBuildMS; }
    F32 dripRate() const { return mDripRate; }

private:
    struct Network
    {
        U64 mRegionHandle = 0;
        F64 mBuiltFrom = -1.0;          // capture time of the tile it came from
        S32 mBuiltRes = 0;
        std::vector<SSRunoffSite> mSites;   // sorted by catchment, largest first
        std::vector<F32> mAccum;            // fractional drips owed per site

        // Kept for the debug draw only: the traced surface and where each
        // cell sends its water, which is the interesting part to look at and
        // far too much to hold for every region otherwise.
        S32 mDebugN = 0;
        F32 mDebugCell = 0.f;
        std::vector<LLVector3> mDebugPos;
        std::vector<S32> mDebugFlow;    // downstream cell index, -1 terminal
        std::vector<F32> mDebugCatch;
    };

    void refreshNetworks(bool want_debug);
    void buildNetwork(Network& net, S32 res, bool want_debug);
    void shed(F32 dt);

    std::map<U64, Network> mNetworks;
    F64 mLastRefresh = 0.0;
    F32 mLastBuildMS = 0.f;
    F32 mDripRate = 0.f;        // drips per second currently being shed
    bool mDebugBuilt = false;   // networks carry their debug fields
};

// </SS:Nexii>

#endif // SS_RUNOFF_H
