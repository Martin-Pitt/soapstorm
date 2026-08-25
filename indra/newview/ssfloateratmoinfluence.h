/**
 * @file ssfloateratmoinfluence.h
 * @brief Atmo Magic: the Weather Influence sub-floater - per-mapping
 *        enable + strength for how far the weather cube may bend the
 *        authored sky, plus a live readout of what each mapping is doing
 *        right now. See doc/atmo_magic_environment.md.
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

#ifndef SS_FLOATERATMOINFLUENCE_H
#define SS_FLOATERATMOINFLUENCE_H

// <SS:Nexii> Atmo Magic weather influence editor

#include "llfloater.h"

#include <functional>
#include <string>
#include <vector>

struct SSAtmoEnvWeatherInfluence;

class SSFloaterAtmoInfluence : public LLFloater
{
public:
    SSFloaterAtmoInfluence(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // Which track's influence is being edited. Called by the main floater when its own selection moves (and when a track sort renumbers everything), for the same reason the System Designer is: one
    // track is one world, and an editor pointed at a track the author is no longer looking at is two views quietly disagreeing.
    void setTrack(S32 index);

private:
    //-------------------------------------------------------------------
    // One mapping's row. The accessors reach into the selected track's SSAtmoEnvWeatherInfluence rather than caching a pointer to it: the asset can be replaced wholesale by a load or a revert
    // between frames, and a cached reference would outlive the struct it named. mEffect reads the applier's last modulation, so what the readout shows is what the renderer actually did on the
    // previous frame, not a second opinion computed here.
    //-------------------------------------------------------------------
    struct Row
    {
        std::string mPrefix;                        // widget name stem
        std::function<bool&(SSAtmoEnvWeatherInfluence&)> mEnabled;
        std::function<F32&(SSAtmoEnvWeatherInfluence&)> mStrength;
        std::function<F32()> mEffect;               // 0..1, what it is doing now
    };

    // Built once in postBuild; the list IS the wiring, so adding a mapping to the model means adding one entry here and one row to the XML.
    std::vector<Row> mRows;

    void buildRows();

    // Nothing loaded, or the track index no longer exists: every control is disabled and the readouts blank rather than editing whatever now sits at that index.
    bool influence(SSAtmoEnvWeatherInfluence** out) const;

    void refreshAll();
    void refreshReadouts();

    void onCommitMaster();
    void onCommitRow(const Row& row);
    void onClickReset();

    S32 mTrackIndex = 0;
    F64 mLastPoll = 0.0;
};

// </SS:Nexii>

#endif // SS_FLOATERATMOINFLUENCE_H
