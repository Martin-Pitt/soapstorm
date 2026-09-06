/**
 * @file ssscreenfx.h
 * @brief Atmo Magic: the screen-space presentation shell - thermal shock and mirage strength driving a heat-shimmer
 *        post pass, and lens-drops wet/condensation state driving a rain-on-glass post pass.
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

#ifndef SS_SCREENFX_H
#define SS_SCREENFX_H

#include "llsingleton.h"
#include "ssscreenfxcore.h"

class LLRenderTarget;

// <SS:Nexii> NAMING NOTE: doc/atmo_magic_surface_weather.md section 8 spells this shell "class SSScreenFX", but
// ssscreenfxcore.h (DO NOT EDIT, harness-green) already claims that identifier as `namespace SSScreenFX` - a class
// and a namespace cannot share one name in the same scope (a hard redefinition error, not a style choice), and this
// shell's own .cpp needs both the core namespace (to call its free functions) and this class' own declaration
// visible in the same translation unit. Named SSScreenFXPost instead - the shell that drives the SSScreenFX core's
// two post-processing passes - mirroring the established sibling pattern of a differently-named shell over a core
// namespace (SSVortex core / SSVortices shell, SSStormCell core / SSStormCells shell). Report this rename up: any
// call site written against the literal "SSScreenFX::getInstance()" text needs "SSScreenFXPost::getInstance()".
class SSScreenFXPost : public LLSingleton<SSScreenFXPost>
{
    LLSINGLETON_EMPTY_CTOR(SSScreenFXPost);

public:
    // One frame of every screen-space source: thermal shock, lens wet/condensation, and the screen-space slide
    // direction (gravity plus wind) the lens shader drifts drops along and the heat shader's ripple reads. Wall-clock
    // dt; touches no world state; every closed form is the core's.
    void idle(F32 dt);

    // The heat-shimmer pass: draws src into dst through gSSPostHeatProgram. Returns false (nothing drawn, caller
    // does not swap) when SSAtmoHeatShimmer is off or the dialed mirage strength is below the shader's noise floor.
    bool renderHeat(LLRenderTarget* src, LLRenderTarget* dst);

    // The lens-drops pass: draws src into dst through gSSPostLensProgram. Returns false when SSAtmoLensDrops is off
    // or there is nothing on the lens worth drawing (wet plus dialed condensation at or below the floor).
    bool renderLens(LLRenderTarget* src, LLRenderTarget* dst);

    F32 shock() const { return mThermal.mShock; }
    F32 mirage() const { return mMirage; }
    F32 lensWet() const { return mLens.mWet; }
    F32 lensFog() const { return mLens.mFog; }

private:
    SSScreenFX::Thermal mThermal;
    SSScreenFX::Lens mLens;

    F32 mMirage = 0.f;            // this frame's undialed mirageStrength(), cached for the render gate and the info overlay
    bool mWasUnderwater = false;  // last frame's underwater state, so stepLens sees the surfacing frame

    // Screen-space projections refreshed once per idle() from the rotation part of the current modelview: the slide
    // direction the lens drops fall along (unit length), and the raw wind projection the heat shimmer ripples with.
    F32 mSlideDX = 0.f;
    F32 mSlideDY = -1.f;
    F32 mHeatWindX = 0.f;
    F32 mHeatWindY = 0.f;
};

#endif
