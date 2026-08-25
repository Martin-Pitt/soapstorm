/**
 * @file ssfloateratmoinfluence.cpp
 * @brief Atmo Magic: the Weather Influence sub-floater. See the header.
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

#include "llviewerprecompiledheaders.h"

#include "ssfloateratmoinfluence.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvmanager.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfocusmgr.h"
#include "llsliderctrl.h"
#include "lltextbox.h"

// <SS:Nexii> Atmo Magic weather influence editor

static const F64 STATUS_POLL_INTERVAL = 0.25;

SSFloaterAtmoInfluence::SSFloaterAtmoInfluence(const LLSD& key) :
    LLFloater(key)
{
}

void SSFloaterAtmoInfluence::buildRows()
{
    // The applier's most recent modulation is the readout's only source - see the header. Fetched per call rather than held, since the applier recomputes it every frame.
    auto effect = [](std::function<F32(const SSAtmoEnvSkyModulation&)> pick)
    {
        return [pick]() -> F32
        {
            return pick(SSAtmoEnvApplier::instance().lastModulation());
        };
    };

    mRows = {
        { "cover",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mCloudCoverEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mCloudCoverStrength; },
          // Coverage is the one mapping whose effect is not a single drive: it is "how much cover is being asked for" times "how far toward it we go", which multiplied together is what the dome
          // actually receives.
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mCoverTarget * m.mCoverBlend; }) },

        { "wind",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mWindScrollEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mWindScrollStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mWind; }) },

        { "haze",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mHazeEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mHazeStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mHaze; }) },

        { "storm",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mStormDarkeningEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mStormDarkeningStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mDarkening; }) },

        { "cold",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mColdSkyEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mColdSkyStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mCold; }) },

        { "rainbow",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mRainbowEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mRainbowStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mRainbow; }) },
    };
}

bool SSFloaterAtmoInfluence::postBuild()
{
    buildRows();

    getChild<LLUICtrl>("master_enabled")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitMaster(); });

    for (const Row& row : mRows)
    {
        // Captured by value: mRows is built once and never resized, but copying two std::functions and a string is cheap enough that the callback need not depend on that staying true.
        const Row captured = row;
        getChild<LLUICtrl>(row.mPrefix + "_enabled")->setCommitCallback(
            [this, captured](LLUICtrl*, const LLSD&) { onCommitRow(captured); });
        getChild<LLUICtrl>(row.mPrefix + "_strength")->setCommitCallback(
            [this, captured](LLUICtrl*, const LLSD&) { onCommitRow(captured); });
    }

    getChild<LLUICtrl>("reset_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickReset(); });

    refreshAll();
    return true;
}

void SSFloaterAtmoInfluence::onOpen(const LLSD& key)
{
    setTrack(key.asInteger());
}

void SSFloaterAtmoInfluence::setTrack(S32 index)
{
    mTrackIndex = index;
    refreshAll();
}

bool SSFloaterAtmoInfluence::influence(SSAtmoEnvWeatherInfluence** out) const
{
    *out = nullptr;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mTrackIndex < 0 || mTrackIndex >= (S32)asset.mTracks.size()) return false;

    *out = &asset.mTracks[(size_t)mTrackIndex].mWeatherInfluence;
    return true;
}

void SSFloaterAtmoInfluence::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;

        // Same capture guard the other Atmo floaters poll behind: writing a slider's value back while it is being dragged makes the drag fight itself. The readouts are safe either way - they are
        // display-only text - so they refresh regardless.
        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            refreshAll();
        }
        else
        {
            refreshReadouts();
        }
    }

    LLFloater::draw();
}

void SSFloaterAtmoInfluence::refreshAll()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    const bool have = influence(&infl);

    LLCheckBoxCtrl* master = getChild<LLCheckBoxCtrl>("master_enabled");
    master->setEnabled(have);
    master->set(have && infl->mEnabled);

    // Rows follow the master switch as well as the asset: with weather influence off wholesale, a per-mapping strength has nothing to say, and leaving them live would invite tuning dials that do
    // nothing.
    const bool rows_live = have && infl->mEnabled;

    for (const Row& row : mRows)
    {
        LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(row.mPrefix + "_enabled");
        LLSliderCtrl* slider = getChild<LLSliderCtrl>(row.mPrefix + "_strength");

        check->setEnabled(rows_live);
        slider->setEnabled(rows_live && row.mEnabled(*infl));

        if (have)
        {
            check->set(row.mEnabled(*infl));
            slider->setValue(row.mStrength(*infl));
        }
    }

    getChild<LLUICtrl>("reset_button")->setEnabled(have);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    getChild<LLTextBox>("track_text")->setText(
        have ? ("Editing: " + mgr->asset().mTracks[(size_t)mTrackIndex].mName)
             : std::string("No environment loaded."));

    refreshReadouts();
}

void SSFloaterAtmoInfluence::refreshReadouts()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    const bool have = influence(&infl);

    for (const Row& row : mRows)
    {
        LLTextBox* readout = getChild<LLTextBox>(row.mPrefix + "_readout");
        if (!have || !infl->mEnabled || !row.mEnabled(*infl))
        {
            // A disabled mapping reads as off rather than as 0%: zero is a thing the weather can legitimately be asking for, and the two states should not look alike.
            readout->setText(std::string("off"));
            continue;
        }

        const F32 effect = llclamp(row.mEffect(), 0.f, 1.f);
        readout->setText(llformat("%d%% now", (S32)(effect * 100.f + 0.5f)));
    }
}

void SSFloaterAtmoInfluence::onCommitMaster()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    if (!influence(&infl)) return;

    infl->mEnabled = getChild<LLCheckBoxCtrl>("master_enabled")->get();
    refreshAll(); // the rows' enabled state hangs off this
}

void SSFloaterAtmoInfluence::onCommitRow(const Row& row)
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    if (!influence(&infl)) return;

    row.mEnabled(*infl) = getChild<LLCheckBoxCtrl>(row.mPrefix + "_enabled")->get();
    row.mStrength(*infl) = llclamp(
        (F32)getChild<LLSliderCtrl>(row.mPrefix + "_strength")->getValueF32(), 0.f, 1.f);

    // Only this row's own slider can have changed enablement, so the whole panel does not need rebuilding - and rebuilding here would stamp on a slider the author is still dragging.
    getChild<LLSliderCtrl>(row.mPrefix + "_strength")->setEnabled(row.mEnabled(*infl));
    refreshReadouts();
}

void SSFloaterAtmoInfluence::onClickReset()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    if (!influence(&infl)) return;

    // The struct's own constructed defaults, so "defaults" here and "what a new environment gets" cannot drift apart.
    *infl = SSAtmoEnvWeatherInfluence();
    refreshAll();
}

// </SS:Nexii>
