/**
 * @file ssfloatersim.h
 * @brief Atmo Magic simulation settings: the two maps the weather is solved
 *        against - the rain shadow depth capture and the wind flowmap - with
 *        live status and explicit rebuild buttons.
 *
 *        Both maps are built once and then left alone, so the tuning here has
 *        no effect until something asks for a rebuild. Every control that
 *        changes what a solve produces triggers one on commit.
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

#ifndef SS_FLOATERSIM_H
#define SS_FLOATERSIM_H

// <SS:Nexii> Atmo Magic simulation settings

#include "llfloater.h"

#include <boost/signals2.hpp>
#include <vector>

class SSFloaterSimulation : public LLFloater
{
public:
    SSFloaterSimulation(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void onClickRecaptureShadow();
    void onClickRebuildFlow();

    void refreshStatus();

    // Which derived thing a watched setting invalidates when it changes
    enum class EInvalidate { SHADOW, FLOW };

    // A tuning change only matters once something rebuilds, so watching the settings themselves rather than the widgets catches the slider, the spinner, the reset button and anyone typing into the
    // debug console, all through one path.
    void watch(const std::string& control, EInvalidate what);

    std::vector<boost::signals2::scoped_connection> mConnections;
    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERSIM_H
