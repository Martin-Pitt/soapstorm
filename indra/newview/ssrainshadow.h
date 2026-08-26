/**
 * @file ssrainshadow.h
 * @brief Atmo Magic: rain shadow depth maps.
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
    void capture();

    void markDirty(const LLVector3& pos_agent, F32 radius);
    void clearCache();

    S32 tileCount() const { return (S32)mTiles.size(); }
    U32 resolution() const;

    U32 captureCount() const { return mCaptureCount; }
    U32 dirtyCaptureCount() const { return mDirtyCaptures; }
    U32 dirtyTileCount() const;
    F32 lastCaptureMS() const { return mLastCaptureMS; }
    F64 lastCaptureAge() const;

    void renderDebug();

    enum
    {
        SURF_MAPPED   = 0x01,
        SURF_WATER    = 0x02,
        SURF_FALLBACK = 0x04
    };

    struct SurfaceGrid
    {
        U64 mRegionHandle = 0;
        S32 mN = 0;
        F32 mCell = 0.f;
        std::vector<F32> mZ;
        std::vector<U8> mFlags;
        U32 mGeomSerial = 0;

        F32 axis(S32 i) const { return ((F32)i + 0.5f) * mCell; }
    };

    bool buildSurfaceGrid(U64 region_handle, S32 n, SurfaceGrid& out);

    bool refineEdge(U64 region_handle, const LLVector3& from_agent, const LLVector3& out_dir,
                    F32 max_dist, F32 tolerance, LLVector3& refined_agent) const;

    void validTiles(std::vector<std::pair<U64, U32> >& out) const;

    bool resolveColumn(const LLVector3& pos_agent, LLVector3& hit_pos_agent, bool& on_water,
                       LLVector3* hit_normal = nullptr);

private:
    struct Tile
    {
        U64 mRegionHandle = 0;
        U32 mRes = 0;
        std::vector<F32> mDepth;

        LLVector3 mEyeRegion;
        LLVector3 mDir, mRight, mUp;
        F32 mHalfW = 0.f, mHalfH = 0.f;
        F32 mNear = 0.f, mFar = 0.f;
        F32 mBandTop = 0.f, mBandBottom = 0.f;

        F64 mCaptureTime = 0.0;
        bool mDirty = false;
        bool mValid = false;
        F64 mLastTouched = 0.0;

        U32 mGeomSerial = 1;
        U32 mCapturedSerial = 0;
    };

    struct ShadowMesh
    {
        S32 mN = 0;
        std::vector<LLVector3> mPos;
        std::vector<F32> mShade;

        F64 mBuiltFrom = -1.0;
        LLVector3 mBuiltDir;
        F32 mBuiltStep = 0.f;
        F32 mBuiltFloor = 0.f;
        bool mBuiltSky = false;
    };

    void buildShadowMesh(const Tile& tile, ShadowMesh& mesh);

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    bool needsCapture(const Tile& tile) const;
    bool captureTile(Tile& tile);
    void evict();

    std::map<U64, Tile> mTiles;
    std::map<U64, ShadowMesh> mDebugMesh;
    LLRenderTarget mTarget;
    F64 mLastCapture = 0.0;

    U32 mCaptureCount = 0;
    U32 mDirtyCaptures = 0;
    F32 mLastCaptureMS = 0.f;
};

#endif
