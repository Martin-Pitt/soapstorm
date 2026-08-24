/**
 * @file ssvolcloud.h
 * @brief Atmo Magic: the volumetric cloud field, drawn as camera-facing
 *        puffs at the altitude the field state describes.
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

// <SS:Nexii> Atmo Magic volumetric cloud field

#include "llsingleton.h"
#include "lluuid.h"
#include "v3color.h"
#include "v3math.h"

#include <vector>

// The volumetric layer, as opposed to the cirrus dome EEP already draws.
//
// The dome is a texture on the inside of the sky: it has no altitude, no
// thickness and no position in the world, so you cannot fly through it, it
// does not sit below a mountain top, and a storm cannot tower over you in
// it. This is the layer that can - SSAtmoEnvCloudField authors a base
// height, a thickness, a coverage and a churn, and this is what finally
// puts them on screen.
//
// It is a puff field rather than a raymarched volume: a deterministic
// scatter of camera-facing textured quads at the field's altitude, sized by
// its thickness and thinned by its coverage. That is a real simplification
// and worth naming - a raymarch would light correctly from inside and this
// cannot - but it draws in one pass on any hardware the viewer already
// runs on, it sits at true world altitudes, and it travels with the wind.
class SSVolCloud : public LLSingleton<SSVolCloud>
{
    LLSINGLETON_EMPTY_CTOR(SSVolCloud);

public:
    // From SSAtmoMagic::idle. Rebuilds the puff list from the active
    // track's field state; cheap enough to do every frame, and doing it
    // every frame is what lets coverage and altitude be keyframed.
    void update(F32 dt);

    // From the sky pass, after the dome clouds. Draws nothing at all when
    // the field is empty, which is the usual case for a clear sky.
    void render();

    void clear();

    // Simulation floater / info overlay
    S32 puffCount() const { return (S32)mPuffs.size(); }
    F32 lastBuildMS() const { return mLastBuildMS; }

private:
    struct Puff
    {
        LLVector3 mPosAgent;
        F32 mRadius = 0.f;
        F32 mAlpha = 0.f;
        LLColor3 mColor;
        F32 mCamDistSq = 0.f;   // sort key, back to front
    };

    std::vector<Puff> mPuffs;
    LLUUID mTexture;
    F32 mLastBuildMS = 0.f;
};

// </SS:Nexii>

#endif // SS_VOLCLOUD_H
