/**
 * @file ssfloaterfx.h
 * @brief Atmo Magic: effects and LOD floater.
 *
 *        Almost everything in here binds straight to a setting through
 *        control_name and needs no code at all. This class exists for the
 *        things XUI cannot bind: the render-debug overlay toggles (cloud
 *        field, rain shadow, roof runoff), which live in the pipeline's
 *        render-debug mask rather than in settings, the same way the
 *        Simulation floater drives its overlays.
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

#ifndef SS_FLOATERFX_H
#define SS_FLOATERFX_H

#include "llfloater.h"

class SSFloaterEffects : public LLFloater
{
public:
    SSFloaterEffects(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void syncOverlayChecks();
};

#endif
