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

// Order-coupled: renders BETWEEN SSLightningRender's two passes - after renderFlash (the discs stay veiled under the puffs) and before render (the bolts draw over the puffs, dimmed per node by transmittance()) - and updates AFTER SSLightning::idle (same-frame flicker). Band exposed to lightning for channel altitude. doc/atmo_magic_interactions.md

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

// The volumetric layer, as opposed to the cirrus dome EEP already draws. The dome is a texture on the inside of the sky: it has no altitude, no thickness and no position in the world, so you cannot
// fly through it, it does not sit below a mountain top, and a storm cannot tower over you in it. This is the layer that can - SSAtmoEnvCloudField authors a base height, a thickness, a coverage and a
// churn, and this is what finally puts them on screen. It is a puff field rather than a raymarched volume: a deterministic scatter of camera-facing textured quads at the field's altitude, sized by
// its thickness and thinned by its coverage. That is a real simplification and worth naming - a raymarch would light correctly from inside and this cannot - but it draws in one pass on any hardware
// the viewer already runs on, it sits at true world altitudes, and it travels with the wind. As many strikes as can light the deck at once. Past a few they stop being separable anyway, and each one
// costs every cloud fragment a distance and a dot product.
static const S32 SS_MAX_STRIKE_LIGHTS = 4;

class SSVolCloud : public LLSingleton<SSVolCloud>
{
    LLSINGLETON_EMPTY_CTOR(SSVolCloud);

public:
    // From SSAtmoMagic::idle. Rebuilds the puff list from the active track's field state; cheap enough to do every frame, and doing it every frame is what lets coverage and altitude be keyframed.
    void update(F32 dt);

    // From the sky pass, after the dome clouds. Draws nothing at all when the field is empty, which is the usual case for a clear sky.
    void render();

    void clear();

    // The band the rendered layer actually occupies, for anything that has to put something inside it - lightning starts its channels here rather than at a guessed altitude. Base and top of the last
    // built field; meaningless while empty() is true.
    F32 cloudBaseZ() const { return mBaseZ; }
    F32 cloudTopZ() const { return mBaseZ + mThicknessM; }
    bool empty() const { return mPuffs.empty(); }

    // How much of a light at `to` survives the cloud between it and `from`, 0..1, measured against the ACTUAL rendered puffs rather than any field function - each puff the segment crosses multiplies
    // visibility down by its own soft alpha profile at the crossing offset, so the answer agrees with what is on screen by construction: 1 through a gap, fractional through thin deck, ~0 behind a
    // dense core. `strength` scales each puff's bite (the SSAtmoLightningOcclusion knob). Cheap per call via an XY cell grid rebuilt with the puff list [interaction: SSLightning -> bolt occlusion].
    F32 transmittance(const LLVector3& from_agent, const LLVector3& to_agent, F32 strength);

    // Simulation floater / info overlay
    S32 puffCount() const { return (S32)mPuffs.size(); }
    F32 lastBuildMS() const { return mLastBuildMS; }

private:
    struct Puff
    {
        LLVector3 mPosAgent;    // TRUE position always - the far-field squash happens per vertex in the shaders (ss_squash), so the CPU only ever reasons about where things really are
        F32 mRadius = 0.f;
        F32 mAlpha = 0.f;
        LLColor3 mColor;
        F32 mCamDistSq = 0.f;   // sort key, back to front (the squash is monotonic, so true order IS drawn order)
    };

    std::vector<Puff> mPuffs;
    LLUUID mTexture;

    // Held rather than looked up per frame, and kept resident. Re-fetching by UUID every frame let the texture be evicted and re-streamed between frames, and a texture partway back up its mip chain
    // is a flat average of itself - which for a noise map is featureless grey. That was the field going blank every so often.
    LLPointer<LLViewerFetchedTexture> mTextureRef;

    // The sky dome's OWN cloud map, held the same way. Shared with the dome deliberately: the two systems are meant to be the same weather seen at two ranges, and nothing sells that like being cut
    // from the same cloth. Whatever the author puts on the dome shows up in the volume. The track's own choice for the detail octaves, if it made one.
    LLUUID mAuthoredDetail;

    LLUUID mDomeTexture;
    LLPointer<LLViewerFetchedTexture> mDomeTexRef;

    // What the field's convection was on the last build, for the boil.
    F32 mChurn = 0.f;

    // Where the layer's underside sits, and how deep it is - the flat base, and the height each fragment sits at within the layer.
    F32 mBaseZ = 0.f;
    F32 mThicknessM = 1.f;

    // The authored look, straight through to the shader.
    F32 mAnvil = 0.f;
    F32 mTextureMix = 0.f;
    F32 mPuffDensity = 0.8f;
    F32 mDetailScale = 1.f;
    F32 mDriftRate = 1.f;

    // The lighting the field was built under, kept for the fragment shader: a puff needs shape, and shape needs a light direction per fragment rather than one colour per quad. Haze is no longer
    // carried here at all - aerial perspective is the windlight atmospheric module in the fragment shader.
    LLVector3 mLightDir;
    LLColor3 mSunColor;

    // How much direct celestial light exists right now, 0..1, from the sunlit magnitude itself - the gate on every directional shading term, CPU and shader both. See the note where it is set.
    F32 mBeam = 1.f;

    // The virtual field radius of the last build, and the squash band that fits it inside the projection: [knee, radius] compresses linearly into [knee, cap] per VERTEX in ssVolCloudV and
    // ssLightningV (both bound the same ss_squash uniform), so clouds and bolts share one far-field mapping and stay depth-consistent with each other. squashScale is the CPU-side mirror for the
    // few places that must reason about drawn positions (the lightning cull).
    F32 mEffRadius = 5000.f;
    F32 mSquashKnee = 1600.f;
    F32 mSquashCap = 2000.f;

public:
    F32 squashScale(F32 true_dist) const;
    F32 lastCoverage() const { return mLastCoverage; }    // the field coverage of the last build, 0 while empty - the dome layer's altitude merge reads it
    F32 squashKnee() const { return mSquashKnee; }
    F32 squashCap() const { return mSquashCap; }
    F32 virtualRadius() const { return mEffRadius; }

private:

    // A copy of the scene's depth, taken just before the puffs are drawn. A copy and not the buffer itself: this pass draws into a target that has the scene depth attached to it, and a shader that
    // samples the very texture its own pass has bound as depth is undefined - which on a GPU means flicker. Blitting it out first costs one depth blit and makes the read legal.
    LLRenderTarget mDepthCopy;

    // Strikes lighting the field this frame: xyz agent position, w brightness. Gathered in update(), bound in render().
    std::vector<LLVector4> mStrikeLights;

    // XY cell grid over mPuffs for transmittance(): each puff's index sits in every cell its disc overlaps, so a query only tests puffs near its own ray. Rebuilt with the puff list; the stamp
    // array dedupes a puff seen from several cells of one query without clearing anything between queries.
    std::unordered_map<U64, std::vector<S32>> mOccGrid;
    std::vector<U32> mOccStamp;
    U32 mOccQuery = 0;
    F32 mMaxPuffR = 0.f;
    bool mOccGridDirty = true;    // set by every rebuild, built lazily on the first query - most frames have no bright bolt and pay nothing

    F32 mLastCoverage = 0.f;
    F32 mLastBuildMS = 0.f;
};

// </SS:Nexii>

#endif // SS_VOLCLOUD_H
