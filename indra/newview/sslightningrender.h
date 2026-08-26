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
    // The two halves of the in-cloud compositing, either side of SSVolCloud::render(). renderFlash draws the sky-flash discs BEFORE the puffs - a disc is scattered light, and the deck veiling it
    // is the correct look. render() draws the channel ribbons and sparks AFTER the puffs, each node dimmed by SSVolCloud::transmittance() along its own camera ray - so a fork crawling the deck
    // reads THROUGH the cloud the way a real one does: dimmed where the deck is thin, bare through gaps, swallowed outright behind a dense core. Painter's order alone (the first design) buried a
    // deep channel under stacked alpha completely; the analytic transmittance is what lets structure survive the veil [interaction: SSVolCloud -> bolt occlusion].
    void renderFlash();
    void render();

    // Last frame's draw accounting, for the info overlay - "the bolt is invisible" has half a dozen silent causes and this names which stage went quiet.
    struct DrawStats
    {
        bool mShaderOk = false;
        bool mGuarded = false;      // skipped by the HUD/impostor/shadow/snapshot guard this frame
        S32 mStrikes = 0;           // live strikes seen
        S32 mBright = 0;            // with channel brightness worth drawing
        S32 mOffScreen = 0;         // culled by the frustum test
        S32 mSegments = 0;          // ribbon segments actually emitted
    };
    const DrawStats& stats() const { return mStats; }

private:
    // The electric-line texture, held resident the same way the puff field holds its noise maps and for the same reason.
    LLUUID mTexture;
    LLPointer<LLViewerFetchedTexture> mTextureRef;
    DrawStats mStats;
};

// </SS:Nexii>

#endif // SS_LIGHTNINGRENDER_H
