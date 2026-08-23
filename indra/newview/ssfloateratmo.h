/**
 * @file ssfloateratmo.h
 * @brief Legacy Atmo Magic weather floater - deprecated, trimmed. See the
 *        header comment in floater_ss_atmo.xml and
 *        doc/atmo_magic_environment.md for what superseded it and what's
 *        still here on purpose (the preset picker/editor, impact effects,
 *        the master switch, the sub-floater launchers).
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

// <SS:Nexii> Legacy Atmo Magic weather floater

#include "llfloater.h"

class SSFloaterAtmoMagic : public LLFloater
{
public:
    SSFloaterAtmoMagic(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void onClickEditPreset();

    // Presets can change while this is open (the preset editor is its own
    // floater, reachable from here) - refreshed on a poll rather than a
    // change signal, same idiom the rest of this floater used to use for
    // its own now-removed per-track polling.
    void refreshPresets();

    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMO_H
