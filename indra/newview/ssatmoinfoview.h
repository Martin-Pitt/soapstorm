/**
 * @file ssatmoinfoview.h
 * @brief Atmo Magic: the info-view framework (dim, legend, graph widget, mode plumbing) and V1 Wind Profile.
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

#ifndef SS_ATMOINFOVIEW_H
#define SS_ATMOINFOVIEW_H

#include "llview.h"
#include "sswindprofilecore.h"

#include <boost/signals2.hpp>
#include <string>
#include <utility>
#include <vector>

// <SS:Nexii> Cities: Skylines-style info views for Atmo Magic (doc/atmo_magic_debug_views.md). One exclusive mode at a time (the SSAtmoInfoView setting), the world dims under a translucent quad at the top of the UI stage, a shared legend explains the colours, a graph widget on the debug floater's Views tab draws what a 3D overlay cannot, and the mode's in-world layer is drawn post-deferred beside the engineering overlays. READ-ONLY by contract: every view reads counters and fields the systems already hold - it never ticks, builds or reorders anything, and says "not built" when the data is not resident. Nothing drawn here positions world content, so the camera is free to shape it.
class SSAtmoInfoView
{
public:
    // Creates the dim quad (drawn first, under every other debug overlay) and the legend (drawn last) as children of the debug view, and starts driving the engineering masks off the mode setting. Called once from LLDebugView::init.
    static void attach(LLView* debug_view);

    // The live mode, 0 when off (SSAtmoInfoViewCore::MODE_*).
    static U32 mode();

    // The active mode's in-world layer (V1: the wind mast), from LLPipeline::renderDebug. Draws nothing when off.
    static void renderWorld();

    // <SS:Nexii> V1's data, gathered once per draw from the systems' resident state: the applier's wind profile and floors, the deck's built band, the flowmap's region exponent. mValid is false with no applied weather cube; mDeckBuilt is false when the volumetric field has no puffs (the rails then read "not built" instead of forcing one). Read-only by construction - every accessor it touches is a const getter. [interaction: SSAtmoEnvApplier windProfile/windAt] [interaction: SSVolCloud cloudBaseZ/cloudTopZ] [interaction: SSWindFlowMap windAlpha]
    struct WindProfileData
    {
        bool mValid = false;
        bool mDeckBuilt = false;
        bool mFlowSolved = false;
        SSWindProfile::Params mParams;
        F32 mGroundZ = 0.f;      // the track floor windAt measures AGL from
        F32 mBaseZ = 0.f;        // the deck base the drift is integrated at
        F32 mLidZ = 0.f;         // the primary deck's top (valid with mDeckBuilt)
        F32 mCirrusZ = 0.f;      // the dome/cirrus band's CURRENT altitude (moves with the anvil ramp)
        F32 mTopAgl = 1.f;       // chart ceiling AGL: the cirrus altitude, floored above the boundary layer
        F32 mMaxSpeed = 1.f;     // nice axis ceiling over the profile's fastest sample
        F32 mFlowAlpha = 0.f;    // the flowmap's camera-region exponent (or its fallback)
        F32 mGradientSetting = 0.f; // the SSAtmoWindFlowGradient fallback
    };
    static WindProfileData windProfileData();

private:
    static void onModeChanged(U32 previous, U32 now);

    static U32 sLastMode;
    static bool sFlowMaskWasOn;
    static boost::signals2::scoped_connection sModeConnection;
};

// <SS:Nexii> The world dimmer: one translucent dark quad over the whole debug-view rect, alpha from SSAtmoInfoViewDim, drawn as the debug view's FIRST child so every other overlay and console sits on top of it. Post-tonemap UI stage: no glow/alpha hazard, no shader touched, and a mode of 0 costs one integer compare.
class SSAtmoDimView : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    SSAtmoDimView(const Params& p);
    void draw() override;
};

// <SS:Nexii> The one shared legend (SSStatsView idiom: translucent, monospace, read-only, sized to content), docked bottom-left of the debug view: mode title, a gradient bar with min/max labels in real units, and the mode's icon key. Every mode pushes rows through the same interface; none invents its own panel.
class SSAtmoLegendView : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    struct KeyRow
    {
        LLColor4 mColor;
        std::string mLabel;
        bool mSwatchIsLine = false;
    };

    struct Spec
    {
        std::string mTitle;
        std::string mSubtitle;
        std::string mRampLabel;   // what the gradient bar measures, e.g. "wind speed"
        std::string mRampMin;     // left-end label in real units
        std::string mRampMax;     // right-end label in real units
        // Ramp sampler: unit t in [0,1] -> colour. Null hides the bar.
        LLColor4 (*mRamp)(F32 t) = nullptr;
        std::vector<KeyRow> mKeys;
    };

    SSAtmoLegendView(const Params& p);
    void draw() override;

private:
    static void buildWindProfileSpec(Spec& spec);
};

// <SS:Nexii> The reusable chart widget (XUI tag ss_atmo_graph_view): an LLView drawing polylines, rails and monospace labels with gGL in 2D. Dispatches on the live mode - V1 draws the altitude-vs-speed boundary-layer curve with annotated rails and a hodograph inset. Read-only like everything else here; it never asks any system to build.
class SSAtmoGraphView : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params()
        {
            changeDefault(mouse_opaque, false);
        }
    };

    SSAtmoGraphView(const Params& p);
    void draw() override;

private:
    void drawWindProfile();
    void drawMessage(const std::string& msg);

    // 2D primitives in local view pixels.
    static void polyline(const std::vector<std::pair<S32, S32> >& pts, const LLColor4& color, bool dashed = false);
    static void text(const std::string& s, S32 x, S32 y, const LLColor4& color, bool right_align = false);
};

#endif
