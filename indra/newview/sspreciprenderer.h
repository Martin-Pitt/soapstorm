/**
 * @file sspreciprenderer.h
 * @brief Atmo Magic precipitation renderer: draws the SSPrecipSim particle
 *        pools with its own vertex buffer as a late translucent pass.
 *        Velocity-stretched streaks, camera-facing flakes, distant shower
 *        sheets and surface-aligned ripple rings, cross-faded between LOD
 *        tiers by live camera distance. Rain-family particles run through a
 *        dedicated refraction/env/specular shader when available.
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

#ifndef SS_PRECIPRENDERER_H
#define SS_PRECIPRENDERER_H

// <SS:Nexii> Atmo Magic precipitation renderer

#include "ssprecipitation.h"

#include "llpointer.h"
#include "llsingleton.h"
#include "llvertexbuffer.h"

#include <vector>

class SSPrecipRenderer : public LLSingleton<SSPrecipRenderer>
{
    LLSINGLETON_EMPTY_CTOR(SSPrecipRenderer);

public:
    // Called at the end of LLPipeline::renderGeomPostDeferred for the main
    // world camera; draws nothing when the system is idle
    void render();

    void cleanupGL() { mVB = nullptr; }

private:
    // Particles surviving the fade math this frame, bucketed by shading
    // material and texture so each texture is one draw call
    struct Item
    {
        const SSPrecipParticle* mPart;
        F32 mAlpha;
    };
    std::vector<Item> mBuckets[MAT_COUNT][SS_PRECIP_MAX_TEXTURES];

    struct Range
    {
        U32 mStartQuad = 0;
        U32 mQuads = 0;
    };
    Range mRanges[MAT_COUNT][SS_PRECIP_MAX_TEXTURES];

    bool ensureBuffer(U32 quads);
    void drawMaterial(class SSPrecipSim* sim, S32 material);

    // Expands one KIND_STREAM particle into its ribbon of quads. Templated on
    // the emitter so the strider capture stays local to render(); returns how
    // many quads it actually wrote, which is at most SS_STREAM_SEGMENTS.
    // Needs no camera: a stream hangs in the plane of its own fall rather than
    // facing the view.
    template <typename EmitFn>
    U32 emitStream(const SSPrecipParticle& p, F32 alpha, F32 stretch, EmitFn& emit);

    LLPointer<LLVertexBuffer> mVB;
    U32 mVBQuads = 0;
};

// </SS:Nexii>

#endif // SS_PRECIPRENDERER_H
