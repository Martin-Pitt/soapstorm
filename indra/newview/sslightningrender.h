/**
 * @file sslightningrender.h
 * @brief Atmo Magic lightning - drawing the strikes the model built. The
 *        channel ribbons, and the gathering-charge sparks. Reads SSLightning
 *        and owns nothing about when or where anything strikes.
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

// <SS:Nexii> Atmo Magic lightning rendering

#include "llsingleton.h"
#include "lluuid.h"
#include "llpointer.h"

class LLViewerFetchedTexture;

class SSLightningRender : public LLSingleton<SSLightningRender>
{
    LLSINGLETON_EMPTY_CTOR(SSLightningRender);

public:
    // From renderGeomPostDeferred, BEFORE the volumetric cloud pass. The
    // order is the whole in-cloud compositing story: every puff in front of
    // a channel segment alpha-blends over it, so inside the cloud the bolt
    // is veiled into a glow, through a gap it shows bare, and below cloud
    // base it is fully visible. The field itself is the diffuser; nothing
    // here tests occlusion at all.
    void render();

private:
    // The electric-line texture, held resident the same way the puff field
    // holds its noise maps and for the same reason.
    LLUUID mTexture;
    LLPointer<LLViewerFetchedTexture> mTextureRef;
};

// </SS:Nexii>

#endif // SS_LIGHTNINGRENDER_H
