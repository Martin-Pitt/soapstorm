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
#include "llpointer.h"
#include "lluuid.h"
#include "llrendertarget.h"
#include "llviewertexture.h"
#include "v3color.h"
#include "v3math.h"
#include "v4math.h"

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
// As many strikes as can light the deck at once. Past a few they stop
// being separable anyway, and each one costs every cloud fragment a
// distance and a dot product.
static const S32 SS_MAX_STRIKE_LIGHTS = 4;

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

    // The band the rendered layer actually occupies, for anything that has
    // to put something inside it - lightning starts its channels here rather
    // than at a guessed altitude. Base and top of the last built field;
    // meaningless while empty() is true.
    F32 cloudBaseZ() const { return mBaseZ; }
    F32 cloudTopZ() const { return mBaseZ + mThicknessM; }
    bool empty() const { return mPuffs.empty(); }

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

    // Held rather than looked up per frame, and kept resident. Re-fetching
    // by UUID every frame let the texture be evicted and re-streamed between
    // frames, and a texture partway back up its mip chain is a flat average
    // of itself - which for a noise map is featureless grey. That was the
    // field going blank every so often.
    LLPointer<LLViewerFetchedTexture> mTextureRef;

    // The sky dome's OWN cloud map, held the same way.
    //
    // Shared with the dome deliberately: the two systems are meant to be the
    // same weather seen at two ranges, and nothing sells that like being cut
    // from the same cloth. Whatever the author puts on the dome shows up in
    // the volume.
    // The track's own choice for the detail octaves, if it made one.
    LLUUID mAuthoredDetail;

    LLUUID mDomeTexture;
    LLPointer<LLViewerFetchedTexture> mDomeTexRef;

    // What the field's convection was on the last build, for the boil.
    F32 mChurn = 0.f;

    // Where the layer's underside sits, and how deep it is - the flat base,
    // and the height each fragment sits at within the layer.
    F32 mBaseZ = 0.f;
    F32 mThicknessM = 1.f;

    // The authored look, straight through to the shader.
    F32 mAnvil = 0.f;
    F32 mTextureMix = 0.f;
    F32 mPuffDensity = 0.8f;
    F32 mDetailScale = 1.f;
    F32 mDriftRate = 1.f;

    // The lighting the field was built under, kept for the fragment shader:
    // a puff needs shape, and shape needs a light direction per fragment
    // rather than one colour per quad.
    LLVector3 mLightDir;
    LLColor3 mSunColor;
    LLColor3 mHaze;

    // A copy of the scene's depth, taken just before the puffs are drawn.
    //
    // A copy and not the buffer itself: this pass draws into a target that
    // has the scene depth attached to it, and a shader that samples the very
    // texture its own pass has bound as depth is undefined - which on a GPU
    // means flicker. Blitting it out first costs one depth blit and makes
    // the read legal.
    LLRenderTarget mDepthCopy;

    // Strikes lighting the field this frame: xyz agent position, w
    // brightness. Gathered in update(), bound in render().
    std::vector<LLVector4> mStrikeLights;

    F32 mLastBuildMS = 0.f;
};

// </SS:Nexii>

#endif // SS_VOLCLOUD_H
