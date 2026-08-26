/**
 * @file ssfarsea.h
 * @brief Atmo Magic: the far sea - a square frame lattice hung on the
 *        stock water footprint (hole tiles + edge patches) that carries
 *        the water plane from where stock water ends out to a
 *        planet-curved horizon kilometres past the projection far plane,
 *        drawn through the stock water shader's SS_ATMO variant. The mesh
 *        is a canonical annulus built once and never rebuilt: the vertex
 *        shader hangs it on the footprint rect from uniforms and blends
 *        its outer rings 0->100% onto the camera-centred rim circle.
 *        Exists only while an Atmo environment owns the water; stock
 *        water never learns it is there. See
 *        doc/atmo_magic_interactions.md.
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

// <SS:Nexii> Atmo Magic: far sea disc

#include "llsingleton.h"
#include "llpointer.h"
#include "llvertexbuffer.h"

class LLGLSLShader;

class SSFarSea : public LLSingleton<SSFarSea>
{
    LLSINGLETON_EMPTY_CTOR(SSFarSea);

public:
    // Binds the shared squash band for the STOCK water planes - called by LLDrawPoolWater just before pushWaterPlanes, only when the far sea will draw afterwards. The stock footprint's edge sits
    // beyond the knee, so the seam only stays closed if stock and frame move through the same squash; it also carries stock's far-plane-crossing edge stretch back inside the cap. render() zeroes
    // the band again on every path out, so callers pair the two.
    void bindSquash(LLGLSLShader* shader);

    // Draws the frame with the given water shader already bound and fully set up - called by LLDrawPoolWater at the end of its above-water pass, after the stock planes, so depth testing hides
    // the tucked apron wherever real water covers it. Binds the sea's ss_squash band and ss_sea expansion uniforms around the draw and zeroes both after (on refusal paths too).
    void render(LLGLSLShader* shader);

    // For the debug overlay: the rim radius actually bound this frame (0 when the disc did not draw - a live refusal must read as one, not as the last good value) and the squash knee in use.
    F32 lastRimRadius() const { return mLastRSea; }
    F32 lastKnee() const { return mLastKnee; }

private:
    void build();
    // The shared band (knee, cap) plus this frame's rim radius and planet radius - one formula, computed identically for bindSquash and render so stock planes and frame agree per frame.
    void band(F32& knee, F32& cap, F32& rim, F32& planet_r) const;

    LLPointer<LLVertexBuffer> mVB;
    U32 mVertCount = 0;
    U32 mIndexCount = 0;
    F32 mLastRSea = 0.f;
    F32 mLastKnee = 0.f;
};

#endif // SS_FARSEA_H
