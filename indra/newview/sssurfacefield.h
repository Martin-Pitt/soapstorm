/**
 * @file sssurfacefield.h
 * @brief Atmo Magic surface field: what the weather has left on the world.
 *        Wetness, settled snow and standing water, integrated over time on
 *        the drainage network the runoff trace already solved, so a shower
 *        darkens a street over a minute and it stays damp long after.
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

#ifndef SS_SURFACEFIELD_H
#define SS_SURFACEFIELD_H

// <SS:Nexii> Atmo Magic surface field

#include "llrendertarget.h"
#include "llsingleton.h"
#include "ssrunoff.h"
#include "v3math.h"

#include <map>
#include <vector>

struct SSPrecipPreset;
class LLGLSLShader;

// The state the weather has worked the surface into, one value per drainage
// cell. Everything here is slow: it is measured in minutes of weather, not in
// frames, which is the whole point of keeping it rather than deriving it from
// the rain shadow every time it is asked for.
//
// The field is laid out on - and only exists for - regions the runoff trace
// has solved. That is deliberate rather than convenient: the trace already
// answers where the surface is, which way water runs off it and where it
// stands, and every one of those is something this needs. Improving the trace
// improves what sits on top of it for free.
class SSSurfaceField : public LLSingleton<SSSurfaceField>
{
    LLSINGLETON_EMPTY_CTOR(SSSurfaceField);

public:
    // From SSAtmoMagic::idle, after the runoff network has been refreshed so a
    // region traced this frame is dressed this frame rather than next.
    void idle(F32 dt);

    void clear();

    // Render Metadata > Surface Field (Atmo Magic): the cells drawn as a
    // coloured wash over the surface they belong to.
    void renderDebug();

    // What the weather has left at a point. Agent space, and answered from
    // whichever region covers it; invalid where nothing has been traced.
    struct Sample
    {
        F32 mWet = 0.f;         // 0 dry, 1 saturated
        F32 mSnow = 0.f;        // settled depth, metres
        F32 mPuddle = 0.f;      // standing water depth, metres
        F32 mSurfaceZ = 0.f;    // the height all of the above belongs to
        bool mValid = false;
    };
    Sample sample(const LLVector3& pos_agent) const;

    // The field as a shading pass sees it: one camera-centred window stitched
    // from every region in range, rather than a layer per region. A shader
    // then has a single texture and a single origin to reason about, region
    // borders stitch instead of stepping, and nothing in the fragment path has
    // to search a list of regions to find out which one it is standing in.
    //
    // False when there is nothing worth binding, in which case the caller has
    // no work to do either.
    bool bindForShader(LLGLSLShader& shader, S32 channel);
    bool hasWindow() const { return mWindowTex != 0 && mWindowValid; }
    void releaseGL();

    // The gbuffer pass. From renderDeferredLighting, before anything has read
    // the specular buffer, so every lighting pass downstream - sun, local
    // lights, projectors, probes - sees one consistent set of values rather
    // than each having to be taught about the weather separately.
    void renderWetPass();

    // Simulation floater / info overlay
    S32 fieldCount() const { return (S32)mFields.size(); }
    F32 lastTickMS() const { return mLastTickMS; }
    F32 peakWet() const { return mPeakWet; }
    F32 peakSnow() const { return mPeakSnow; }
    F32 peakPuddle() const { return mPeakPuddle; }

private:
    struct Field
    {
        U64 mRegionHandle = 0;
        S32 mN = 0;
        F32 mCell = 0.f;

        // The height each cell's values were accumulated against. Geometry
        // moving under the field is the one thing that has to reset it: a roof
        // rezzed into a downpour has not been standing in it, so it starts dry
        // rather than inheriting whatever the ground below it had soaked up.
        std::vector<F32> mZ;

        std::vector<F32> mWet;
        std::vector<F32> mSnow;
        std::vector<F32> mPuddle;

        F64 mLastTouched = 0.0;
    };

    Field* fieldFor(U64 region_handle, const SSRunoffField& src, F64 now);
    void updateWindow();
    void tick(Field& fld, const SSRunoffField& src, F32 dt,
              const SSPrecipPreset& preset, F32 intensity);
    void evict(F64 now);

    std::map<U64, Field> mFields;

    // Ticked well below frame rate. Nothing here moves fast enough to see the
    // difference, and a quarter of a million cells a region is not something
    // to walk every frame for a value that changes over minutes.
    F32 mTickAccum = 0.f;

    // Where the modified specular buffer is built before it goes back over the
    // gbuffer's own. Reading and writing one texture in a single pass is not
    // something the driver promises anything about, and a full resolution RGBA
    // is cheap next to finding out which drivers get away with it.
    LLRenderTarget mScratch;

    // Same idea, for the flattened normal. Its own target rather than a
    // second attachment on mScratch: the wetness shader's early returns are
    // all proven correct as a single-output shader, and giving it a second
    // output to carry through unchanged on every one of those paths would
    // have meant touching every one of them to add it. A second pass over a
    // second target costs one more full-screen triangle and touches none of
    // that.
    LLRenderTarget mScratchNormal;

    // The stitched window. Held as RGBA32F - the height channel is an agent
    // space Z that runs to a few thousand metres and is compared against
    // fragment positions to within a few centimetres, which is well past what
    // a half float would carry.
    U32 mWindowTex = 0;
    S32 mWindowRes = 0;
    F32 mWindowCell = 0.f;
    LLVector3 mWindowOrigin;            // agent-space corner of texel (0, 0)
    std::vector<F32> mWindowData;       // staging, RGBA per texel
    bool mWindowValid = false;

    F32 mLastTickMS = 0.f;
    F32 mPeakWet = 0.f;
    F32 mPeakSnow = 0.f;
    F32 mPeakPuddle = 0.f;
};

// </SS:Nexii>

#endif // SS_SURFACEFIELD_H
