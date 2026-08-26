/**
 * @file sslightningrender.h
 * @brief Atmo Magic: lightning rendering.
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

#ifndef SS_LIGHTNINGRENDER_H
#define SS_LIGHTNINGRENDER_H

#include "llsingleton.h"
#include "lluuid.h"
#include "llpointer.h"

class LLViewerFetchedTexture;

class SSLightningRender : public LLSingleton<SSLightningRender>
{
    LLSINGLETON_EMPTY_CTOR(SSLightningRender);

public:
    void renderFlash();
    void render();

    struct DrawStats
    {
        bool mShaderOk = false;
        bool mGuarded = false;
        S32 mStrikes = 0;
        S32 mBright = 0;
        S32 mOffScreen = 0;
        S32 mSegments = 0;
    };
    const DrawStats& stats() const { return mStats; }

private:
    LLUUID mTexture;
    LLPointer<LLViewerFetchedTexture> mTextureRef;
    DrawStats mStats;
};

#endif
