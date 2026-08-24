/**
 * @file lldrawpoolwlsky.h
 * @brief LLDrawPoolWLSky class definition
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_DRAWPOOLWLSKY_H
#define LL_DRAWPOOLWLSKY_H

#include "lldrawpool.h"

class LLGLSLShader;

class LLDrawPoolWLSky : public LLDrawPool {
public:

    static const U32 SKY_VERTEX_DATA_MASK = LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_TEXCOORD0;
    static const U32 STAR_VERTEX_DATA_MASK =    LLVertexBuffer::MAP_VERTEX |
        LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXCOORD0;
    static const U32 ADV_ATMO_SKY_VERTEX_DATA_MASK = LLVertexBuffer::MAP_VERTEX
                                                   | LLVertexBuffer::MAP_TEXCOORD0;
    LLDrawPoolWLSky(void);
    /*virtual*/ ~LLDrawPoolWLSky();

    /*virtual*/ bool isDead() { return false; }

    /*virtual*/ S32 getNumDeferredPasses() { return 1; }
    /*virtual*/ void beginDeferredPass(S32 pass);
    /*virtual*/ void endDeferredPass(S32 pass);
    /*virtual*/ void renderDeferred(S32 pass);

    // <SS:Nexii> Render JUST the sky into `target`, looking along a given
    // heading, covering a band of elevation.
    //
    // For the photo matcher: it needs to score a candidate sky without
    // showing it, and reading the main framebuffer meant mutating the live
    // sky on screen (the whole view flashing through every candidate) and
    // demanding the camera be aimed at open sky. A narrow offscreen strip
    // costs a fraction of a frame, can be run several times per frame, and
    // is looking wherever it wants regardless of the user's camera.
    //
    // Only the haze dome and the cloud layer are drawn - no bodies, no
    // stars - because those are what the fit is fitting.
    bool renderSkyProbe(class LLRenderTarget& target, const LLVector3& heading,
                        F32 min_elev_deg, F32 max_elev_deg);

    /*virtual*/ LLViewerTexture *getDebugTexture();
    /*virtual*/ U32 getVertexDataMask() { return SKY_VERTEX_DATA_MASK; }
    /*virtual*/ bool verify() const { return true; }        // Verify that all data in the draw pool is correct!
    /*virtual*/ S32 getShaderLevel() const { return mShaderLevel; }

    //static LLDrawPool* createPool(const U32 type, LLViewerTexture *tex0 = NULL);

    /*virtual*/ LLViewerTexture* getTexture();
    /*virtual*/ bool isFacePool() { return false; }
    /*virtual*/ void resetDrawOrders();

    static void cleanupGL();
    static void restoreGL();
private:
    // scale shrinks the dome toward the camera. Stock is 0.333 for both the
    // haze backdrop and the cloud layer drawn on it; the cloud pass alone
    // passes a slightly smaller one - see its call site.
    void renderDome(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader * shader,
                    F32 scale = 0.333f) const;

    void renderSkyHazeDeferred(const LLVector3& camPosLocal, F32 camHeightLocal) const;
    void renderSkyCloudsDeferred(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader* cloudshader) const;

    void renderStarsDeferred(const LLVector3& camPosLocal) const;
    void renderHeavenlyBodies();
};

#endif // LL_DRAWPOOLWLSKY_H
