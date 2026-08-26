/**
 * @file ssfarsea.h
 * @brief Atmo Magic: the far sea - ocean out to a planet-curved horizon.
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

#ifndef SS_FARSEA_H
#define SS_FARSEA_H

#include "llsingleton.h"
#include "llpointer.h"
#include "llvertexbuffer.h"

class LLGLSLShader;

// <SS:Nexii> The water's far-field squash band as fractions of MAX_FAR_CLIP. The cap is shared with the cloud field + lightning (SSVolCloud), but the KNEE is deliberately the sea's own: 0.95
// keeps the drawn water TRUE out to 95% of the cap and compresses the whole knee-to-rim sweep into the last ~100 drawn metres - a distant, near-vertical wall at the bubble rim - while clouds
// and lightning keep their original 0.8 knee so their volume geometry is untouched. The accepted cost: inside the 0.8-0.95 sliver the water is unsquashed while clouds are squashed, so water
// and cloud depth ordering can disagree there - vertical separation makes real overlap rare. </SS:Nexii>
constexpr F32 SS_SQUASH_CAP_FRAC = 0.98f;
constexpr F32 SS_SEA_SQUASH_KNEE_FRAC = 0.95f;

class SSFarSea : public LLSingleton<SSFarSea>
{
    LLSINGLETON_EMPTY_CTOR(SSFarSea);

public:
    void bindSquash(LLGLSLShader* shader);

    void render(LLGLSLShader* shader);

    F32 lastRimRadius() const { return mLastRSea; }
    F32 lastKnee() const { return mLastKnee; }

private:
    void build();
    void band(F32& knee, F32& cap, F32& rim, F32& planet_r) const;
    LLVector3 anchorEye() const;

    LLPointer<LLVertexBuffer> mVB;
    U32 mVertCount = 0;
    U32 mIndexCount = 0;
    F32 mLastRSea = 0.f;
    F32 mLastKnee = 0.f;
    mutable LLVector3 mFrozenEye;
    mutable bool mEyeFrozen = false;
};

#endif
