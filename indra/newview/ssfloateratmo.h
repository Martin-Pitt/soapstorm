/**
 * @file ssfloateratmo.h
 * @brief Atmo Magic floater: local testing / user override panel driving the
 *        weather parameters, standing in for EEP-style shared environments
 *        until parameters can arrive from LSL/notecards.
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

#ifndef SS_FLOATERATMO_H
#define SS_FLOATERATMO_H

// <SS:Nexii> Atmo Magic floater

#include "llfloater.h"

class SSFloaterAtmoMagic : public LLFloater
{
public:
    SSFloaterAtmoMagic(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void onClickRecapture();
    void refreshPresets();
};

// </SS:Nexii>

#endif // SS_FLOATERATMO_H
