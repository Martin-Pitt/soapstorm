/**
 * @file ssfloaterassets.h
 * @brief Atmo Magic: the global assets floater - the few things that are not
 *        tied to a precipitation preset.
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

#ifndef SS_FLOATERASSETS_H
#define SS_FLOATERASSETS_H

// <SS:Nexii> Atmo Magic global assets

#include "ssprecippreset.h"

#include "llfloater.h"

// The wind loops, which are global because wind is not a precipitation type -
// it blows whether or not anything is falling.
//
// This floater used to be plain LLFloater with the two fields bound straight
// to their settings by control_name. A sound list cannot be bound that way:
// control_name copies an LLSD value in and out of a control, and what this
// control holds is a vector the settings store as a comma separated string.
// So the conversion happens here, which is the only thing this class exists
// to do.
class SSFloaterAssets : public LLFloater
{
public:
    SSFloaterAssets(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    static std::string stepWidgetName(SSStepSurface surface, SSStepAction action);

    void onCommitWind();
    void onCommitThunder();
    void onCommitSteps();
};

// </SS:Nexii>

#endif
