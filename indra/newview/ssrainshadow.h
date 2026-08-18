/**
 * @file ssrainshadow.h
 * @brief Atmo Magic rain shadow maps: per-region depth captures along the
 *        precipitation fall direction, cached and lazily refreshed, so drops
 *        land on roofs and bridges and sheltered spots stay dry.
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

#ifndef SS_RAINSHADOW_H
#define SS_RAINSHADOW_H

// <SS:Nexii> Atmo Magic rain shadow maps

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3math.h"
#include "v3dmath.h"

#include <map>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

class SSRainShadowMap : public LLSingleton<SSRainShadowMap>
{
    LLSINGLETON_EMPTY_CTOR(SSRainShadowMap);

public:
    // Called from the display loop right after generateSunShadow, where the
    // pipeline is in a known state. Captures at most one tile per call,
    // throttled, picking the stalest/dirtiest tile near the camera.
    void capture();

    // Object update hook: cheap filter, then marks the covering tile for a
    // lazy recapture. Static so the call site stays a single line.
    static void onObjectUpdate(LLViewerObject* objectp);

    void markDirty(const LLVector3& pos_agent, F32 radius);
    void clearCache();

    // Find where the precipitation column through pos_agent first hits
    // something, walking along the current fall direction. Falls back to
    // terrain/water height when no map covers the point; the result is
    // always usable. Returns whether map data was involved. When requested,
    // hit_normal receives the surface normal at the hit, derived from the
    // depth map's gradients (terrain/water normal on fallback).
    bool resolveColumn(const LLVector3& pos_agent, LLVector3& hit_pos_agent, bool& on_water,
                       LLVector3* hit_normal = nullptr);

private:
    struct Tile
    {
        U64 mRegionHandle = 0;
        U32 mRes = 0;
        std::vector<F32> mDepth;        // window-space depth, linear over [mNear, mFar]

        // Ortho basis in region-local coordinates so tiles survive the
        // agent-origin shift on region crossings
        LLVector3 mEyeRegion;           // top-plane center
        LLVector3 mDir, mRight, mUp;
        F32 mHalfW = 0.f, mHalfH = 0.f;
        F32 mNear = 0.f, mFar = 0.f;
        F32 mBandTop = 0.f, mBandBottom = 0.f;

        F64 mCaptureTime = 0.0;
        bool mDirty = false;
        bool mValid = false;
        F64 mLastTouched = 0.0;         // for LRU eviction
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    bool needsCapture(const Tile& tile) const;
    bool captureTile(Tile& tile);
    void evict();

    std::map<U64, Tile> mTiles;
    LLRenderTarget mTarget;
    F64 mLastCapture = 0.0;
};

// </SS:Nexii>

#endif // SS_RAINSHADOW_H
