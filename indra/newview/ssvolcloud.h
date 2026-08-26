/**
 * @file ssvolcloud.h
 * @brief Atmo Magic: volumetric cloud puff field.
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

#ifndef SS_VOLCLOUD_H
#define SS_VOLCLOUD_H

#include "llsingleton.h"
#include "llpointer.h"
#include "lluuid.h"
#include "llrendertarget.h"
#include "llviewertexture.h"
#include "v3color.h"
#include "v3math.h"
#include "v4math.h"

#include <vector>
#include <unordered_map>

static const S32 SS_MAX_STRIKE_LIGHTS = 4;

// <SS:Nexii> The far-field squash cap as a fraction of MAX_FAR_CLIP: where the cloud field's drawn depth tops out, just short of the projection far plane so nothing rasterises against it. The
// knee (0.8 of the cap, set in update()) is where the compression starts; between them the whole remaining field is folded into the last fifth of drawn depth. Shared with lightning so bolt and
// cloud agree about drawn depth. [interaction: SSLightning] </SS:Nexii>
constexpr F32 SS_SQUASH_CAP_FRAC = 0.98f;

class SSVolCloud : public LLSingleton<SSVolCloud>
{
    LLSINGLETON_EMPTY_CTOR(SSVolCloud);

public:
    void update(F32 dt);

    void render();

    void clear();

    F32 cloudBaseZ() const { return mBaseZ; }
    F32 cloudTopZ() const { return mBaseZ + mThicknessM; }
    bool empty() const { return mPuffs.empty(); }

    F32 transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength);

    S32 puffCount() const { return (S32)mPuffs.size(); }
    F32 lastBuildMS() const { return mLastBuildMS; }

private:
    struct Puff
    {
        LLVector3 mPosAgent;
        F32 mRadius = 0.f;
        F32 mAlpha = 0.f;
        LLColor3 mColor;
        F32 mCamDistSq = 0.f;
    };

    std::vector<Puff> mPuffs;
    LLUUID mTexture;

    LLPointer<LLViewerFetchedTexture> mTextureRef;

    LLUUID mAuthoredDetail;

    LLUUID mDomeTexture;
    LLPointer<LLViewerFetchedTexture> mDomeTexRef;

    F32 mChurn = 0.f;

    F32 mBaseZ = 0.f;
    F32 mThicknessM = 1.f;

    F32 mAnvil = 0.f;
    F32 mTextureMix = 0.f;
    F32 mPuffDensity = 0.8f;
    F32 mDetailScale = 1.f;
    F32 mDriftRate = 1.f;

    LLVector3 mLightDir;
    LLColor3 mSunColor;

    F32 mBeam = 1.f;

    F32 mEffRadius = 5000.f;
    F32 mSquashKnee = 1600.f;
    F32 mSquashCap = 2000.f;

public:
    F32 squashScale(F32 true_dist) const;
    F32 lastCoverage() const { return mLastCoverage; }
    F32 squashKnee() const { return mSquashKnee; }
    F32 squashCap() const { return mSquashCap; }
    F32 virtualRadius() const { return mEffRadius; }

private:

    LLRenderTarget mDepthCopy;

    std::vector<LLVector4> mStrikeLights;

    std::unordered_map<U64, std::vector<S32>> mOccGrid;
    std::vector<U32> mOccStamp;
    U32 mOccQuery = 0;
    F32 mMaxPuffR = 0.f;
    bool mOccGridDirty = true;

    F32 mLastCoverage = 0.f;
    F32 mLastBuildMS = 0.f;
};

#endif
