/**
 * @file ssatmoinfoview.cpp
 * @brief See ssatmoinfoview.h.
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

#include "ssatmoinfoview.h"
#include "ssatmoinfoviewcore.h"
#include "ssdecklodcore.h"
#include "ssdeckmacrocore.h"
#include "ssvirgacore.h"

#include "llappviewer.h" // <SS:Nexii> S12: gFrameCount - the per-frame memo key for stormCellsData()
#include "llfontgl.h"
#include "llgl.h"
#include "llhudrender.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvcloudfieldstate.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvweatherstate.h"
#include "ssatmomagic.h"
#include "ssstormcells.h"
#include "sssquallcore.h"
#include "ssvolcloud.h"
#include "ssvortexcore.h"
#include "ssvortices.h"
#include "sswindflow.h"

static LLDefaultChildRegistry::Register<SSAtmoGraphView> r_ss_atmo_graph_view("ss_atmo_graph_view");

U32  SSAtmoInfoView::sLastMode = 0;
bool SSAtmoInfoView::sFlowMaskWasOn = false;
boost::signals2::scoped_connection SSAtmoInfoView::sModeConnection;

namespace
{
    using namespace SSAtmoInfoViewCore;

    const S32 PAD = 6;
    const S32 CURVE_SAMPLES = 64;
    const S32 CUBE_SAMPLES = 96; // V5 Weather Cube: samples per lane across one full day cycle

    // <SS:Nexii> CHART (design section 6.2): the debug HUD's chart, docked to the legend's right edge - fixed size,
    // 8px gap, bottom-aligned. sLegendHandle is set once by SSAtmoInfoView::attach and read fresh by
    // SSAtmoGraphView::draw() every frame (an LLHandle, not a raw LLView*, so a future teardown of the debug view
    // makes it silently return null rather than dangle). [interaction: SSAtmoLegendView]
    const S32 CHART_W = 440;
    const S32 CHART_H = 280;
    const S32 CHART_GAP = 8;
    LLHandle<LLView> sLegendHandle;

    // Rail palette: geometry annotations, one colour each, shared by the chart, the legend and the mast so a rail reads the same in every place it appears.
    const LLColor4 RAIL_REF     (0.85f, 0.85f, 0.85f, 0.9f);   // 10 m reference
    const LLColor4 RAIL_BL      (1.00f, 1.00f, 1.00f, 0.9f);   // boundary-layer top
    const LLColor4 RAIL_BASE    (0.35f, 0.55f, 1.00f, 0.9f);   // deck base (moisture blue)
    const LLColor4 RAIL_LID     (0.60f, 0.78f, 1.00f, 0.9f);   // deck lid
    const LLColor4 RAIL_CIRRUS  (1.00f, 0.85f, 0.45f, 0.95f);  // the moving rail
    const LLColor4 CURVE_LIVE   (0.45f, 0.95f, 0.90f, 1.0f);
    const LLColor4 CURVE_FLOW   (0.75f, 0.75f, 0.75f, 0.8f);
    const LLColor4 CURVE_GRAD   (0.50f, 0.50f, 0.50f, 0.8f);
    const LLColor4 AXIS         (0.70f, 0.70f, 0.70f, 0.8f);
    const LLColor4 GRID         (1.00f, 1.00f, 1.00f, 0.10f);
    const LLColor4 TEXT_NORMAL  (1.00f, 1.00f, 1.00f, 1.0f);
    const LLColor4 TEXT_DIM     (0.65f, 0.65f, 0.65f, 1.0f);

    LLColor4 toColor(const RGB& c, F32 a = 1.f)
    {
        return LLColor4(c.r, c.g, c.b, a);
    }

    LLColor4 rampSpeed(F32 t)
    {
        return toColor(speedRamp(t, 1.f));
    }

    LLColor4 rampEnergy(F32 t)
    {
        return toColor(energyRamp(t, 1.f));
    }

    LLColor4 rampGrey(F32 t)
    {
        return toColor(presenceRamp(t, 1.f));
    }

    // V3 palette: one colour per LOD rail, shared by the in-world rings, the legend and the chart.
    const LLColor4 RING_SUBS_FULL  (0.45f, 0.95f, 0.90f, 0.9f);  // SUBS_FULL_M
    const LLColor4 RING_SUBS_TWO   (0.55f, 0.85f, 1.00f, 0.9f);  // SUBS_TWO_M
    const LLColor4 RING_THIN_START (1.00f, 0.85f, 0.45f, 0.9f);  // THIN_START_M
    const LLColor4 RING_FADE_START (1.00f, 0.60f, 0.30f, 0.9f);  // FIELD_FADE_START_M
    const LLColor4 RING_DECK_EDGE  (1.00f, 0.30f, 0.30f, 0.9f);  // DECK_EDGE_M

    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): Tier B's own swatch - the macro grid tint beyond
    // TIER_B_M and the legend's "Tier B macro bodies" line - a violet distinct from every V3 rail so the merged
    // tier reads as its own thing rather than a sixth shade of the same ramp.
    const LLColor4 RING_TIER_B     (0.70f, 0.45f, 1.00f, 0.9f);

    // V4 palette: the fall-tilt comparison line reuses the deck-base rail's own "moisture blue" (RAIL_BASE above)
    // rather than a new swatch - one blue means one thing across V1 and V4. The handoff ring/band get their own
    // white, the same idiom V2's hero-pass rings use for the anchor below.
    const LLColor4 VIRGA_HANDOFF (1.00f, 1.00f, 1.00f, 0.55f);

    // V2 palette: the anchor and hero marks that are not a ramp colour.
    const LLColor4 ANCHOR_WHITE (1.00f, 1.00f, 1.00f, 0.9f);
    const LLColor4 HERO_ORIGIN  (1.00f, 0.72f, 0.30f, 1.0f);
    const LLColor4 HERO_DEATH   (0.60f, 0.15f, 0.10f, 1.0f);
    const LLColor4 TILE_EDGE    (1.00f, 1.00f, 1.00f, 0.12f);
    const LLColor4 TILE_ALIVE   (1.00f, 1.00f, 1.00f, 0.55f);

    // <SS:Nexii> SQUALL/FORCED palette (doc/atmo_magic_storm_dynamics.md sections 5-6): a bar-and-marker language
    // distinct from every ring/glyph colour already in use, so a line reads as its own thing crossing several
    // cells rather than a property of any one of them. The forced outline reuses HERO_DEATH's dark red at full
    // alpha - an authored cue and a dying hero are both "this cell's fate was decided elsewhere", and the two never
    // appear on the same cell (a forced candidate is never also a line member, per ssstormcellcore.h's own comment).
    const LLColor4 SQUALL_LINE      (0.85f, 0.85f, 1.00f, 0.85f);  // the bar through a line's own members
    const LLColor4 SQUALL_JUNCTION  (1.00f, 0.95f, 0.35f, 0.95f);  // leading-edge QLCS spin-up points
    const LLColor4 FORCED_OUTLINE   (1.00f, 0.20f, 0.75f, 0.95f);  // the authored/forced pin's own outline

    // <SS:Nexii> V5 Weather Cube palette: one colour per curve, shared by the top lane's day-cycle curves and the
    // bottom lane's derived gates, plus the "now" cursor and the two cue-marker colours (storm cue vs precipitation
    // cue) - never reusing a V1-V4 swatch, so a colour means one thing on this chart without also meaning "deck
    // lid" or "hero origin" a scroll away.
    const LLColor4 CUBE_MOISTURE    (0.35f, 0.55f, 1.00f, 0.95f);
    const LLColor4 CUBE_CONVECTION  (1.00f, 0.55f, 0.15f, 0.95f);
    const LLColor4 CUBE_TEMP        (1.00f, 0.85f, 0.35f, 0.95f);
    const LLColor4 CUBE_WIND        (0.45f, 0.95f, 0.90f, 0.95f);
    const LLColor4 CUBE_SHEAR       (0.80f, 0.55f, 1.00f, 0.95f);
    const LLColor4 CUBE_CONSOLIDATE (0.90f, 0.90f, 0.90f, 0.95f);
    const LLColor4 CUBE_GLOOM       (0.45f, 0.45f, 0.55f, 0.95f);
    const LLColor4 CUBE_ANVIL       (1.00f, 0.60f, 0.20f, 0.95f);
    const LLColor4 CUBE_LIGHTNING   (0.62f, 0.55f, 1.00f, 0.95f);
    const LLColor4 CUBE_STORM_SCORE (1.00f, 0.30f, 0.30f, 0.95f);
    const LLColor4 CUBE_NOW         (1.00f, 1.00f, 1.00f, 0.9f);
    const LLColor4 CUBE_CUE_STORM   (1.00f, 0.20f, 0.75f, 0.9f);   // matches FORCED_OUTLINE: same authored pin
    const LLColor4 CUBE_CUE_PRECIP  (0.35f, 0.85f, 1.00f, 0.9f);

    const char* stageLabel(S32 stage)
    {
        switch (stage)
        {
            case SSStormCell::STAGE_TCU:      return "TCU";
            case SSStormCell::STAGE_MATURING: return "MATURING";
            case SSStormCell::STAGE_MATURE:   return "MATURE";
            case SSStormCell::STAGE_ANVIL:    return "ANVIL";
            case SSStormCell::STAGE_DECAY:    return "DECAY";
        }
        return "?";
    }

    std::string shortId(U64 id)
    {
        return llformat("%08x", (U32)(id & 0xffffffffu));
    }

    // SSVortex::Vec2 and SSStormCell::Vec2 are the same shape in two core namespaces (neither core may include the
    // other's Vec2 - see each core's own file-top include list); every crossing at a read site is this copy, not a
    // cast, same as ssvortices.cpp's own toVortexVec2 does it the other way.
    SSStormCell::Vec2 toStormVec2(const SSVortex::Vec2& v)
    {
        SSStormCell::Vec2 out;
        out.x = v.x;
        out.y = v.y;
        return out;
    }

    const LLColor4 DUST_COLOR (0.85f, 0.70f, 0.35f, 0.9f); // dust devil marker: dusty tan, distinct from the funnel kind colours

    // Orbit period of the rotation glyphs, seconds of WALL CLOCK per revolution (display only).
    const F32 ORBIT_PERIOD_S = 12.f;
    const S32 ORBIT_GLYPHS = 6;

    F32 speedOf(const SSWindProfile::Vec2& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y);
    }

    // A named horizontal rail of the V1 chart and mast: altitude AGL, label, colour, and whether the data behind it is resident.
    struct Rail
    {
        F32 mAgl;
        const char* mLabel;
        const LLColor4* mColor;
        bool mPresent;
    };

    void collectRails(const SSAtmoInfoView::WindProfileData& d, std::vector<Rail>& out)
    {
        out.push_back({ SSWindProfile::REF_M, "10 m ref", &RAIL_REF, true });
        out.push_back({ SSWindProfile::BL_TOP_M, "1500 m BL top", &RAIL_BL, true });
        out.push_back({ d.mBaseZ - d.mGroundZ, "deck base", &RAIL_BASE, d.mDeckBuilt });
        out.push_back({ d.mLidZ - d.mGroundZ, "deck lid", &RAIL_LID, d.mDeckBuilt });
        out.push_back({ d.mCirrusZ - d.mGroundZ, "cirrus (now)", &RAIL_CIRRUS, true });
    }
}

// ---------------------------------------------------------------------------
// SSAtmoInfoView: mode plumbing
// ---------------------------------------------------------------------------

void SSAtmoInfoView::attach(LLView* debug_view)
{
    if (!debug_view) return;

    LLRect full = debug_view->getLocalRect();

    SSAtmoDimView::Params dp;
    dp.name("ss_atmo_info_dim");
    dp.rect(full);
    dp.follows.flags(FOLLOWS_ALL);
    dp.visible(true);
    dp.mouse_opaque(false);
    SSAtmoDimView* dim = LLUICtrlFactory::create<SSAtmoDimView>(dp);
    // Under everything else the debug view holds: the consoles and the stats overlay must stay legible over a dimmed world.
    debug_view->addChildInBack(dim);

    LLRect lr;
    lr.set(PAD, PAD + 140, PAD + 280, PAD);
    SSAtmoLegendView::Params lp;
    lp.name("ss_atmo_info_legend");
    lp.rect(lr);
    lp.follows.flags(FOLLOWS_BOTTOM | FOLLOWS_LEFT);
    lp.visible(true);
    lp.mouse_opaque(false);
    SSAtmoLegendView* legend = LLUICtrlFactory::create<SSAtmoLegendView>(lp);
    debug_view->addChild(legend);
    sLegendHandle = legend->getHandle();

    // <SS:Nexii> CHART (design section 6.2): the placeholder rect below is only ever visible for one frame - it
    // reads the legend's own placeholder rect (lr) before the legend's first draw() has sized it to its real
    // content, but SSAtmoGraphView::draw() re-docks itself off the legend's LIVE rect every frame after that, so
    // the fixed starting numbers here never matter beyond that first frame.
    LLRect gr;
    gr.set(lr.mRight + CHART_GAP, lr.mBottom + CHART_H, lr.mRight + CHART_GAP + CHART_W, lr.mBottom);
    SSAtmoGraphView::Params gp;
    gp.name("ss_atmo_info_chart");
    gp.rect(gr);
    gp.follows.flags(FOLLOWS_BOTTOM | FOLLOWS_LEFT);
    gp.visible(true);
    gp.mouse_opaque(false);
    SSAtmoGraphView* chart = LLUICtrlFactory::create<SSAtmoGraphView>(gp);
    debug_view->addChild(chart);

    // The mode drives the engineering masks underneath it: activating a view flips the overlays it hands off to, deactivating restores what was there. The setting does not persist, so a fresh session always starts with the masks untouched.
    LLControlVariable* var = gSavedSettings.getControl("SSAtmoInfoView");
    if (var)
    {
        sLastMode = (U32)var->getValue().asInteger();
        sModeConnection = var->getSignal()->connect(
            [](LLControlVariable*, const LLSD& now, const LLSD&)
            {
                const U32 m = (U32)now.asInteger();
                if (m != sLastMode)
                {
                    onModeChanged(sLastMode, m);
                    sLastMode = m;
                }
            });
    }
}

U32 SSAtmoInfoView::mode()
{
    static LLCachedControl<U32> mode_setting(gSavedSettings, "SSAtmoInfoView", 0);
    return (U32)mode_setting;
}

namespace
{
    // <SS:Nexii> S12 (phase-2b audit): V2's stake in the storm scheduler running, claimed/released as the mode
    // switches to/from MODE_STORM_CELLS. File-local: only onModeChanged touches it.
    SSStormCells::Interest sStormInterest;
}

// Snapshot the flow-arrow mask on the way from off, then hold it where the active mode wants it; back to off restores the snapshot. V1 hands its near-ground layer to the flowmap arrows, so it wants them on; V2 wants them off under its lattice.
void SSAtmoInfoView::onModeChanged(U32 previous, U32 now)
{
    // S12: claim the storm scheduler only while V2 is the active mode - it should not pay its per-frame pipeline
    // for a view nobody has open. [interaction: SSStormCells::claim]
    sStormInterest = (now == MODE_STORM_CELLS) ? SSStormCells::getInstance()->claim() : SSStormCells::Interest();

    const U64 flow = LLPipeline::RENDER_DEBUG_WIND_FLOW;
    if (previous == MODE_OFF && now != MODE_OFF)
    {
        sFlowMaskWasOn = gPipeline.hasRenderDebugMask(flow);
    }
    const bool desired = (now == MODE_OFF) ? sFlowMaskWasOn : (now == MODE_WIND_PROFILE);
    if (gPipeline.hasRenderDebugMask(flow) != desired)
    {
        LLPipeline::toggleRenderDebug(flow);
    }
}

// Every read here is a const getter on state the systems already hold; the instanceExists guards keep a view from even constructing a singleton, let alone building one.
SSAtmoInfoView::WindProfileData SSAtmoInfoView::windProfileData()
{
    WindProfileData d;
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoMagic::instanceExists())
    {
        return d;
    }

    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    d.mValid   = SSAtmoMagic::getInstance()->hasWeather();
    d.mParams  = applier.windProfile();
    d.mGroundZ = applier.windProfileGroundZ();
    d.mBaseZ   = applier.windProfileBaseZ();
    d.mCirrusZ = applier.cirrusAltitudeMetres();

    if (SSVolCloud::instanceExists())
    {
        const SSVolCloud* clouds = SSVolCloud::getInstance();
        d.mDeckBuilt = !clouds->empty();
        if (d.mDeckBuilt)
        {
            d.mLidZ = clouds->cloudTopZ();
        }
    }

    d.mTopAgl = llmax(d.mCirrusZ - d.mGroundZ, SSWindProfile::BL_TOP_M + SSWindProfile::CELL_M);

    F32 vmax = 0.f;
    for (S32 i = 0; i < CURVE_SAMPLES; ++i)
    {
        const F32 agl = altitudeLadder(d.mTopAgl, CURVE_SAMPLES, i);
        vmax = llmax(vmax, speedOf(SSWindProfile::windAt(agl, d.mParams)));
    }
    d.mMaxSpeed = niceMax(vmax);

    if (SSWindFlowMap::instanceExists())
    {
        const SSWindFlowMap* flow = SSWindFlowMap::getInstance();
        d.mFlowAlpha  = flow->windAlpha();
        d.mFlowSolved = flow->windAlphaSolved();
    }
    static LLCachedControl<F32> gradient(gSavedSettings, "SSAtmoWindFlowGradient", 0.16f);
    d.mGradientSetting = llclamp((F32)gradient, SSWindProfile::EXPONENT_MIN, SSWindProfile::EXPONENT_MAX);
    return d;
}

// <SS:Nexii> V3's data: read-only off SSVolCloud's own resident counters (puffCount(), the primary deck's
// mLodSubsTally/mLodCellTally via the accessors added alongside ssdecklodcore.h's wiring) plus the two dials the
// LOD ramp depends on, read straight off gSavedSettings for display exactly as windProfileData() reads
// SSAtmoWindFlowGradient above - UNCLAMPED (SSVolCloud keeps the actual MIN/MAX_PUFF_BUDGET and
// MIN/MAX_PUFFS_PER_CELL bounds to itself; this is a diagnostic readout, not a placement decision, so it is not
// this view's place to duplicate that clamp range).
SSAtmoInfoView::DeckLodData SSAtmoInfoView::deckLodData()
{
    DeckLodData d;
    if (!SSVolCloud::instanceExists())
    {
        return d;
    }
    const SSVolCloud* clouds = SSVolCloud::getInstance();
    d.mDeckBuilt = !clouds->empty();
    d.mLayerZ = d.mDeckBuilt ? clouds->cloudBaseZ()
              : (SSAtmoEnvApplier::instanceExists() ? SSAtmoEnvApplier::instance().windProfileBaseZ() : 0.f);
    d.mPuffsPlaced = clouds->puffCount();
    d.mLodPredicted = clouds->primaryLodPredictedSubs();
    d.mCellsWalked = clouds->primaryLodCellsWalked();
    d.mTierBCount = clouds->primaryTierBCount();

    static LLCachedControl<U32> budget_setting(gSavedSettings, "SSAtmoCloudPuffBudget", 2520);
    d.mBudget = (S32)budget_setting;
    static LLCachedControl<U32> dial_setting(gSavedSettings, "SSAtmoCloudPuffsPerCell", 3);
    d.mPuffsPerCell = (S32)dial_setting;
    return d;
}

// <SS:Nexii> V4's data: SSVolCloud::virgaDebug()'s snapshot copied straight across (read-only - nothing here
// re-derives qualification or the trim order), plus the ground-level wind and the active precip preset's fall
// speed the fall-tilt comparison line needs (SSAtmoInfoViewCore::fallTiltOffsetM). mKept/mTrimmed are counted
// off the snapshot's own mKept flags, never recomputed from SSVirga::keepHash - the flags already ARE that
// verdict.
SSAtmoInfoView::VirgaData SSAtmoInfoView::virgaData()
{
    VirgaData d;
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoMagic::instanceExists() || !SSVolCloud::instanceExists())
    {
        return d;
    }

    d.mValid = SSAtmoMagic::getInstance()->hasWeather();

    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    d.mWindGround = SSWindProfile::windAt(0.f, applier.windProfile());
    d.mFallSpeedMS = SSAtmoMagic::getInstance()->preset().mFallSpeed;

    const SSVolCloud* clouds = SSVolCloud::getInstance();
    d.mDeckBuilt = !clouds->empty();

    const SSVolCloud::SSVirgaDebug& vd = clouds->virgaDebug();
    d.mActive = vd.mActive;
    d.mGroundZ = vd.mGroundZ;
    d.mBaseZ = vd.mBaseZ;
    d.mR2 = vd.mR2;
    d.mHandoffRadius = vd.mR2 * SSVirga::HANDOFF_SKIP;

    d.mCells.reserve(vd.mCells.size());
    S32 kept = 0;
    for (const SSVolCloud::SSVirgaDebugCell& c : vd.mCells)
    {
        VirgaData::Cell out;
        out.mX = c.mX;
        out.mY = c.mY;
        out.mDrive = c.mDrive;
        out.mKept = c.mKept;
        if (c.mKept) ++kept;
        d.mCells.push_back(out);
    }
    d.mCandidates = (S32)vd.mCells.size();
    d.mKept = kept;
    d.mTrimmed = d.mCandidates - kept;

    return d;
}

// <SS:Nexii> V5's data: the applied track's cube sampled at CUBE_SAMPLES phases across one day cycle through the
// SAME resolvers the sky reads (SSAtmoEnvWeatherResolver::resolve for wind/shear/lightning, SSAtmoEnvCloudFieldResolver::resolve
// for gloom/anvil, SSWindProfile::consolidation, SSStormCell::gateScore for the storm-spawn curve) - every field is
// a pure function call on the track's own curves at that phase, never a respelled formula. The applied track comes
// off SSAtmoEnvApplier::appliedTrackIndex() (the SAME track the sky itself is drawn from this frame, camera-band
// selection and all), and mNowPhase off SSAtmoEnvApplier::appliedPhase() (carries the editor's preview override),
// so the cursor this view draws never disagrees with the sky beside it. The storm-spawn curve reads the anchor's
// own lattice cell (SSStormCells::anchorNow() - callable without an Interest claim, see its own comment) at the
// CURRENT epoch only (an epoch is a wall-clock bucket, not a day-cycle phase, so it is never swept per sample) with
// just the weather term varied per sample: "what would this candidate's score be if it were born at this time of
// day". [interaction: SSAtmoEnvWeatherResolver] [interaction: SSAtmoEnvCloudFieldResolver] [interaction: SSStormCell::gateScore]
SSAtmoInfoView::WeatherCubeData SSAtmoInfoView::weatherCubeData()
{
    WeatherCubeData d;
    if (!SSAtmoEnvApplier::instanceExists() || !SSAtmoEnvManager::instanceExists() || !SSAtmoMagic::instanceExists())
    {
        return d;
    }

    const SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();
    const S32 track_idx = applier.appliedTrackIndex();
    const SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (track_idx < 0 || !mgr->hasAsset() || track_idx >= (S32)mgr->asset().mTracks.size())
    {
        return d;
    }
    if (!SSAtmoMagic::getInstance()->hasWeather())
    {
        return d;
    }

    const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)track_idx];
    d.mValid = true;
    d.mTrackName = track.mName;
    d.mDayLengthS = track.mDayLengthSeconds;
    d.mNowPhase = applier.appliedPhase();

    // The anchor's own lattice cell, current epoch: fixed across every sample, only the weather term is swept.
    // SSStormCells::anchorNow() is callable with no Interest claim (same idiom SSVortices' own dust-devil block
    // uses, ssvortices.cpp) and reads Vec2() (world origin) only when the agent has no region - already unlikely
    // here since hasWeather() above implies a live sky is actually applied - so this is trusted directly rather
    // than guarded on a zero-vector heuristic that would also misread a genuine anchor at the world origin.
    SSStormCell::Candidate anchor_candidate;
    d.mHaveStormScore = true;
    {
        const SSStormCell::Vec2 anchor = SSStormCells::getInstance()->anchorNow();
        const S32 lx = (S32)std::floor(anchor.x / SSStormCell::LATTICE_M);
        const S32 ly = (S32)std::floor(anchor.y / SSStormCell::LATTICE_M);
        const S64 epoch = SSStormCell::epochOf(SSAtmoMagic::getInstance()->sharedTime());
        anchor_candidate = SSStormCell::candidate(SSAtmoMagic::getInstance()->seed(), lx, ly, epoch);
    }

    d.mSamples.reserve((size_t)CUBE_SAMPLES);
    for (S32 i = 0; i < CUBE_SAMPLES; ++i)
    {
        const F64 phase = (F64)cubePhaseSample(CUBE_SAMPLES, i);
        WeatherCubeData::Sample s;
        s.mPhase = (F32)phase;
        s.mMoisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
        s.mConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
        s.mTemperatureC = track.mWeather.mTemperatureC.valueAt(phase);

        const SSAtmoEnvWeatherState ws = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);
        s.mWindSpeed = ws.mWindSpeed;
        s.mWindHeading = ws.mWindHeading;
        s.mShearStrength = ws.mShearStrength;
        s.mVeerDeg = ws.mVeerDeg;
        s.mLightningIntensity = llclamp(ws.mLightningIntensity, 0.f, 1.f);
        s.mLightningActive = ws.mLightningEnabled && ws.mLightningIntervalMaxSeconds > 0.f;

        s.mConsolidation = SSWindProfile::consolidation(s.mMoisture, s.mConvection);

        const SSAtmoEnvCloudFieldState fs = SSAtmoEnvCloudFieldResolver::resolve(
            track.mCloudField, track.mWeatherInfluence, s.mMoisture, s.mConvection, s.mTemperatureC, phase, track.mFloorZ);
        s.mGloom = fs.mGloom;
        s.mAnvilRamp = fs.mAnvil;

        if (d.mHaveStormScore)
        {
            SSStormCell::WeatherAtBirth w;
            w.mMoisture = s.mMoisture;
            w.mConvection = s.mConvection;
            w.mShearStrength = s.mShearStrength;
            s.mStormScore = SSStormCell::gateScore(anchor_candidate, w);
        }

        d.mSamples.push_back(s);
    }

    // <SS:Nexii> Authored override cues (doc/atmo_magic_debug_views.md V5's own marker ask): every keyframe of
    // mStormOverride/mPrecipitationOverride whose authored value is active, at THAT keyframe's own phase - the
    // instant the author placed the cue on the timeline, not SSSquall::forcedCandidate's derived cueTime (which is
    // a wall-clock instant near "now", not a fixed point on this phase axis).
    for (const auto& kf : track.mWeather.mStormOverride.keyframes())
    {
        if (kf.mValue.empty() || kf.mValue == "none") continue;
        WeatherCubeData::Cue c;
        c.mPhase = kf.mTime;
        c.mLabel = "storm: " + kf.mValue;
        c.mIsStorm = true;
        d.mCues.push_back(c);
    }
    for (const auto& kf : track.mWeather.mPrecipitationOverride.keyframes())
    {
        if (kf.mValue.empty()) continue;
        WeatherCubeData::Cue c;
        c.mPhase = kf.mTime;
        c.mLabel = "precip: " + kf.mValue;
        c.mIsStorm = false;
        d.mCues.push_back(c);
    }

    return d;
}

// ---------------------------------------------------------------------------
// The in-world layer: V1's wind mast
// ---------------------------------------------------------------------------

// Dispatch on the live mode; each layer guards its own data.
void SSAtmoInfoView::renderWorld()
{
    switch (mode())
    {
        case MODE_WIND_PROFILE: renderWindMast(); break;
        case MODE_STORM_CELLS:  renderStormCells(); break;
        case MODE_DECK_LOD:     renderDeckLod(); break;
        case MODE_PRECIP_VIRGA: renderVirga(); break;
        default: break;
    }
}

// A vertical stack of arrows from the track floor to the cirrus band, each rotated and scaled by windAt(z) and coloured by the speed ramp, so shear reads as the stack twisting with height. Stood a little ahead of the camera (SSAtmoInfoViewMastOffset, 0 for the camera column itself) and pulled through the cloud field's far squash so the upper rungs land beside the deck they describe. [interaction: SSVolCloud squashScale]
void SSAtmoInfoView::renderWindMast()
{
    const WindProfileData d = windProfileData();
    if (!d.mValid) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;

    static LLCachedControl<F32> offset_setting(gSavedSettings, "SSAtmoInfoViewMastOffset", 80.f);

    const LLVector3 cam = camera->getOrigin();
    LLVector3 forward = camera->getAtAxis();
    forward.mV[VZ] = 0.f;
    if (forward.magVecSquared() < 1.0e-6f)
    {
        forward.setVec(1.f, 0.f, 0.f);
    }
    forward.normVec();

    const F32 offset = llclamp((F32)offset_setting, 0.f, 2000.f);
    const F32 mx = cam.mV[VX] + forward.mV[VX] * offset;
    const F32 my = cam.mV[VY] + forward.mV[VY] * offset;
    const F32 ground = d.mGroundZ;
    const F32 top_agl = d.mTopAgl;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this now draws AFTER the dim quad in the UI stage (SSAtmoDimView::draw), on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);

    // The pole.
    gGL.color4f(0.8f, 0.8f, 0.8f, 0.35f);
    line(LLVector3(mx, my, ground), LLVector3(mx, my, ground + top_agl));

    // The rungs.
    const S32 n = mastRungCount(top_agl);
    for (S32 i = 0; i < n; ++i)
    {
        const F32 agl = mastRungAgl(i, top_agl);
        const F32 below = (i > 0) ? mastRungAgl(i - 1, top_agl) : 0.f;
        const F32 above = (i + 1 < n) ? mastRungAgl(i + 1, top_agl) : top_agl + (agl - below);
        const F32 gap = llmax(llmin(agl - below, above - agl), 10.f);

        const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
        const F32 speed = speedOf(w);
        LLVector3 dir(0.f, 1.f, 0.f);
        if (speed > 1.0e-4f)
        {
            dir.setVec(w.x / speed, w.y / speed, 0.f);
        }
        const F32 len = llmax(mastArrowLen(speed, d.mMaxSpeed, gap), 4.f);
        const LLVector3 centre(mx, my, ground + agl);
        const LLVector3 tip = centre + dir * len;
        const LLVector3 perp(-dir.mV[VY], dir.mV[VX], 0.f);
        const LLVector3 barb_a = tip - dir * (len * 0.22f) + perp * (len * 0.12f);
        const LLVector3 barb_b = tip - dir * (len * 0.22f) - perp * (len * 0.12f);

        const LLColor4 c = toColor(speedRamp(speed, d.mMaxSpeed), 0.95f);
        gGL.color4f(c.mV[0] * 0.25f, c.mV[1] * 0.25f, c.mV[2] * 0.25f, 0.6f);
        gGL.vertex3fv(drawn(centre).mV);
        gGL.color4fv(c.mV);
        gGL.vertex3fv(drawn(tip).mV);
        line(tip, barb_a);
        line(tip, barb_b);
    }

    // The rails: a cross on the pole at each load-bearing altitude, in the rail's own colour.
    std::vector<Rail> rails;
    collectRails(d, rails);
    for (const Rail& rail : rails)
    {
        if (!rail.mPresent || rail.mAgl < 0.f || rail.mAgl > top_agl) continue;
        const F32 tick = llmax(20.f, rail.mAgl * 0.04f);
        const F32 z = ground + rail.mAgl;
        gGL.color4fv(rail.mColor->mV);
        line(LLVector3(mx - tick, my, z), LLVector3(mx + tick, my, z));
        line(LLVector3(mx, my - tick, z), LLVector3(mx, my + tick, z));
    }

    gGL.end();

    // Labels: every rung's altitude, speed and heading; every rail its name.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        for (S32 i = 0; i < n; ++i)
        {
            const F32 agl = mastRungAgl(i, top_agl);
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
            const F32 speed = speedOf(w);
            const std::string label = llformat("%.0f m  %.1f m/s  %03.0f", agl, speed, headingOfVec(w.x, w.y));
            const LLColor4 c = toColor(speedRamp(speed, d.mMaxSpeed), 1.f);
            hud_render_utf8text(label, drawn(LLVector3(mx, my, ground + agl)), *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, 6.f, -4.f, c, false);
        }
        for (const Rail& rail : rails)
        {
            if (!rail.mPresent || rail.mAgl < 0.f || rail.mAgl > top_agl) continue;
            hud_render_utf8text(rail.mLabel, drawn(LLVector3(mx, my, ground + rail.mAgl)), *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, -6.f - (F32)font->getWidth(rail.mLabel), 4.f, *rail.mColor, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V2's storm cells
// ---------------------------------------------------------------------------

namespace
{
    // <SS:Nexii> S12 (phase-2b audit): stormCellsData() re-walks the lattice and both cell lists every call; V2's
    // in-world layer, its legend and its chart panel each called it once per draw, so a single frame with V2 active
    // paid the walk three times over. Memoised per APP FRAME (gFrameCount) rather than per wall-clock tick: this is
    // a display-only cache of an already-computed, frame-stable read (SSStormCells::update() ticks once per app
    // frame, and gAgent's position - which toAgentXY reads - does not move mid-frame), never a determinism input,
    // so a frame counter here decides nothing about the world, only how often this VIEW recomputes its own copy of it.
    U32 sStormCellsDataFrame = ~0u;
    SSAtmoInfoView::StormCellsData sStormCellsDataCache;
}

// Every read is a const getter on the scheduler's frame (cells/hero/whyNot as update() left them) or a pure core call on published inputs; nothing here can spawn, steer or reorder a cell. The lattice is re-read through SSStormCell::enumerateLattice / candidate at the CURRENT epoch for the tile tints - the same pure functions the scheduler itself walks.
SSAtmoInfoView::StormCellsData SSAtmoInfoView::stormCellsData()
{
    if (gFrameCount == sStormCellsDataFrame)
    {
        return sStormCellsDataCache;
    }

    using namespace SSStormCell;
    StormCellsData d;
    if (!SSStormCells::instanceExists())
    {
        sStormCellsDataFrame = gFrameCount;
        sStormCellsDataCache = d;
        return d;
    }
    const SSStormCells* sc = SSStormCells::getInstance();
    if (!sc->valid())
    {
        sStormCellsDataFrame = gFrameCount;
        sStormCellsDataCache = d;
        return d;
    }

    d.mValid = true;
    d.mNow = sc->now();
    d.mAnchorAgent = sc->toAgentXY(sc->anchor());

    const SSStormCells::WhyNot& why = sc->whyNot();
    d.mCandidates = why.mCandidates;
    d.mAlive = why.mAlive;
    d.mSpawned = why.mSpawned;
    d.mSupercells = why.mSupercells;
    d.mTornadoEligible = why.mTornadoEligible;
    d.mWhyNot = why.mFailing;

    // The applied track's live checkboxes (the gate reads them at each birth; the legend wants the current state).
    if (SSAtmoEnvManager::instanceExists())
    {
        const SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        const S32 track = sc->trackIndex();
        if (mgr->hasAsset() && track >= 0 && track < (S32)mgr->asset().mTracks.size())
        {
            const SSAtmoEnvWeatherInfluence& infl = mgr->asset().mTracks[(size_t)track].mWeatherInfluence;
            d.mAllowSupercells = infl.mEnabled && infl.mAllowSupercells;
            d.mAllowTornadoes = infl.mEnabled && infl.mAllowTornadoes;
        }
    }

    // Altitude: the deck base the cells will one day modulate, else the drift base the applier integrates at.
    if (SSVolCloud::instanceExists() && !SSVolCloud::getInstance()->empty())
    {
        d.mDeckBuilt = true;
        d.mLayerZ = SSVolCloud::getInstance()->cloudBaseZ();
    }
    else if (SSAtmoEnvApplier::instanceExists())
    {
        d.mLayerZ = SSAtmoEnvApplier::instance().windProfileBaseZ();
    }

    // Active cells, in the scheduler's id order.
    const std::vector<SSStormCells::ActiveCell>& cells = sc->cells();
    d.mCells.reserve(cells.size());
    for (const SSStormCells::ActiveCell& c : cells)
    {
        StormCellsData::Cell out;
        out.mId = c.mCandidate.mId; // S6: ActiveCell is now directly the core POD - mCandidate.mId is the ranking key, not a top-level field
        out.mCentreAgent = sc->toAgentXY(c.mCentre);
        out.mRadiusM = c.mRadiusM;
        out.mStage = (S32)c.mLifecycle.mStage;
        out.mAge01 = c.mAge01;
        out.mBirthTime = c.mCandidate.mBirthTime;
        out.mLifetimeS = c.mCandidate.mLifetimeS;
        out.mRotation = c.mCandidate.mRotation;
        out.mMeso = c.mLifecycle.mMeso;
        out.mIntensity = c.mGate.mIntensity;
        out.mSupercell = c.mGate.mSupercell;
        out.mIsHero = c.mIsHero;
        out.mLineId = c.mLineId;
        out.mIsForced = c.mIsForced;
        d.mCells.push_back(out);
    }

    // The hero's full trajectory.
    if (const SSStormCells::Hero* hero = sc->hero())
    {
        d.mHaveHero = true;
        d.mHero.mId = hero->mId;
        d.mHero.mOriginAgent = sc->toAgentXY(hero->mPath.mOrigin);
        d.mHero.mNowAgent = sc->toAgentXY(centreAt(hero->mPath.mOrigin, hero->mPath.mMotion, hero->mBirthTime, d.mNow));
        d.mHero.mDeathAgent = sc->toAgentXY(hero->mDeath);
        d.mHero.mClosestAgent = sc->toAgentXY(hero->mPath.mClosest);
        d.mHero.mMotion.setVec(hero->mPath.mMotion.x, hero->mPath.mMotion.y);
        d.mHero.mClosestDistM = hero->mPath.mClosestDistM;
        d.mHero.mBirthTime = hero->mBirthTime;
        d.mHero.mClosestTime = hero->mPath.mClosestTime;
        d.mHero.mLifetimeS = hero->mLifetimeS;
        for (const SSStormCells::ActiveCell& c : cells)
        {
            if (c.mIsHero) { d.mHero.mRotation = c.mCandidate.mRotation; break; }
        }
    }

    // <SS:Nexii> SQUALL (doc/atmo_magic_storm_dynamics.md section 5, V2's own "bar through the members" ask):
    // reconstructs each active line's geometry read-only, through the SAME pure functions the scheduler used to
    // spawn it (SSSquall::lineEvent/lineMember/qlcsJunction), never a new field carried on ActiveCell beyond the
    // mLineId it already has. Every member of a line shares one epoch (sssquallcore.h's lineMember sets
    // c.mEpoch = epochOf(the line's own birth) for every i), so grouping active cells by mLineId and calling
    // lineEvent ONCE per distinct (epoch, lineId) - phase read through sc->schedulerPhaseAt (7c NEW-2: the SAME
    // map ssstormcells.cpp's own lineAtEpoch used, real or preview-overridden, never mTrack->dayCyclePhaseAt
    // unconditionally), windAnvil then resolved the same way (-> SSAtmoEnvApplier::windProfileAt -> windAt
    // (anvilAgl)), severe recomputed as SSWindProfile::consolidation(moisture, convection) >=
    // SSSquall::SEVERE_CONSOLIDATION_MIN at that SAME phase (7c NEW-5: the identical expression
    // ssstormcells.cpp's lineAtEpoch gates on, not a hard-coded true - a line member existing already proves it
    // was true at emission, but recomputing it here catches a future divergence between the two instead of
    // masking one) - reproduces the identical origin/direction/motion/member template. e.mLineId is checked
    // against the cell's own mLineId before trusting anything from the reconstruction; a mismatch (should not
    // happen - the epoch is read straight off the member itself) skips the line rather than drawing a wrong one.
    // Members are matched back to active cells by mCandidate.mId (the SAME pure function applied to the SAME
    // (lineId, i) gives the SAME id, sssquallcore.h's lineMember), so mMembersAgent only ever contains members
    // that actually gated and are alive THIS frame, in the template's own along-the-line order (the offset
    // formula is monotone in member index). With schedulerPhaseAt reading the SAME captured map (real or
    // preview) update() resolved this frame's cells with, the phase used here and the phase lineAtEpoch used are
    // identical, not merely close - the old "can disagree by up to one 2s memo bucket" limitation no longer
    // applies; the only way this reconstruction can be stale is if update() has not run yet this frame (there is
    // then nothing to reconstruct, since cells is empty). [interaction: SSAtmoEnvApplier::windProfileAt]
    // [interaction: SSStormCells::schedulerPhaseAt]
    if (SSAtmoEnvManager::instanceExists())
    {
        const SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        const S32 track_idx = sc->trackIndex();
        if (mgr->hasAsset() && track_idx >= 0 && track_idx < (S32)mgr->asset().mTracks.size())
        {
            const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)track_idx];
            const SSSquall::Vec2 anchorSq{ sc->anchor().x, sc->anchor().y };

            // Distinct line ids among this frame's active cells, each remembering the (shared) epoch it was born on.
            std::vector<std::pair<U64, S64> > line_epochs;
            for (const SSStormCells::ActiveCell& c : cells)
            {
                if (c.mLineId == 0) continue;
                bool have = false;
                for (const auto& p : line_epochs) { if (p.first == c.mLineId) { have = true; break; } }
                if (!have) line_epochs.emplace_back(c.mLineId, c.mCandidate.mEpoch);
            }

            for (const auto& le : line_epochs)
            {
                const U64 lineId = le.first;
                const S64 epoch = le.second;
                const F64 phase = sc->schedulerPhaseAt((F64)epoch * SSStormCell::EPOCH_S);
                const F32 moisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
                const F32 convection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
                const bool severe = SSWindProfile::consolidation(moisture, convection) >= SSSquall::SEVERE_CONSOLIDATION_MIN;
                const SSWindProfile::Params profile = SSAtmoEnvApplier::windProfileAt(track, phase);
                const SSWindProfile::Vec2 wind = SSWindProfile::windAt(profile.mAnvilAglM, profile);
                const SSSquall::Vec2 windAnvil{ wind.x, wind.y };
                const SSSquall::LineEvent e = SSSquall::lineEvent(sc->seed(), epoch, anchorSq, windAnvil, severe);
                if (!e.mIsLine || e.mLineId != lineId) continue;

                StormCellsData::SquallLine sl;
                sl.mLineId = lineId;
                const S32 n = llmin((S32)SSSquall::LINE_MEMBERS_MAX, SSStormCell::LINE_MEMBERS_CAP);
                for (S32 i = 0; i < n; ++i)
                {
                    const SSStormCell::Candidate m = SSSquall::lineMember(e, i);
                    for (const SSStormCells::ActiveCell& c : cells)
                    {
                        if (c.mLineId == lineId && c.mCandidate.mId == m.mId)
                        {
                            sl.mMembersAgent.push_back(sc->toAgentXY(c.mCentre));
                            break;
                        }
                    }
                }
                for (S32 i = 0; i + 1 < n; ++i)
                {
                    // 7b F9: qlcsJunctionAt advects the birth-frame junction to the wall time the cells were
                    // resolved at (sc->now()) - qlcsJunction alone is the birth-frame position, and drawing it
                    // beside members already advected by mMotion * age would separate the marker from the line by
                    // |motion| * age (lesson 12).
                    const SSSquall::Vec2 j = SSSquall::qlcsJunctionAt(e, i, sc->now());
                    sl.mJunctionsAgent.push_back(sc->toAgentXY(SSStormCell::Vec2{ j.x, j.y }));
                }
                if (!sl.mMembersAgent.empty())
                {
                    d.mSquallLines.push_back(sl);
                }
            }
        }
    }

    // Lattice tiles within the field, tinted by this epoch's potential.
    constexpr S32 CAP = 256;
    S32 lx[CAP];
    S32 ly[CAP];
    const S32 count = llmin(enumerateLattice(sc->anchor(), SSStormCells::FIELD_M, lx, ly, CAP), CAP);
    d.mTiles.reserve((size_t)count);
    for (S32 i = 0; i < count; ++i)
    {
        const Candidate c = candidate(sc->seed(), lx[i], ly[i], sc->currentEpoch());
        StormCellsData::Tile t;
        Vec2 centre;
        centre.x = ((F32)lx[i] + 0.5f) * LATTICE_M;
        centre.y = ((F32)ly[i] + 0.5f) * LATTICE_M;
        t.mCentreAgent = sc->toAgentXY(centre);
        t.mPotential = c.mPotential;
        t.mShearNoise = c.mShearNoise;
        // <SS:Nexii> S10 (phase-2b audit): match by the alive cell's ACTUAL origin tile, not its natal
        // candidate.mLX/mLY - for the hero those differ (composeHero overrides mOrigin up to HERO_SPAWN_MAX_M from
        // the anchor, which can land in a different lattice cell than the one it was born on), so this is the tile
        // the hero's marker actually follows. Every non-hero cell's mOrigin floors to the same cell as its
        // candidate's natal mLX/mLY (jitter never crosses a cell boundary - JITTER_FRAC <= 0.35 keeps it inside
        // [0.15, 0.85] of the cell), so this is a no-op for them. When alive, the tile is tinted by THAT cell's own
        // mCandidate.mPotential (its value at ITS OWN birth epoch), not this loop's freshly recomputed candidate()
        // at the CURRENT epoch, which can legitimately differ - POTENTIAL_DRIFT scrolls the field per epoch, so an
        // older survivor's birth-epoch potential is not what "now"'s epoch would draw for that same lattice cell.
        for (const SSStormCells::ActiveCell& a : cells)
        {
            const S32 ax = (S32)std::floor(a.mOrigin.x / LATTICE_M);
            const S32 ay = (S32)std::floor(a.mOrigin.y / LATTICE_M);
            if (ax == lx[i] && ay == ly[i])
            {
                t.mAlive = true;
                t.mPotential = a.mCandidate.mPotential;
                break;
            }
        }
        d.mTiles.push_back(t);
    }

    // <SS:Nexii> DEBUG: vortex icons - SSVortices::update() is ticked unconditionally from SSAtmoMagic::idle()
    // right after SSStormCells::update(), so this is the SAME frame's resolved state as everything gathered above
    // it. SCHEDULER fix 9, revised by review NEW-3: SSVortices claims its OWN SSStormCells::Interest only while the
    // applied track's mAllowSupercells/mAllowTornadoes could actually produce a live storm-cell child this frame
    // (see SSVortices::update()'s own comment in the .cpp for the cost-gate reasoning) - this view claims nothing
    // extra either way, and dustDevils()/active() below may both be empty on a frame where that gate is off and
    // nothing else holds SSStormCells' Interest. Read-only: every field below is copied straight off
    // active()/dustDevils()/whyNot() as the scheduler left them. [interaction: SSVortices]
    if (SSVortices::instanceExists() && SSVortices::getInstance()->valid())
    {
        const SSVortices* vortices = SSVortices::getInstance();

        d.mVortices.reserve(vortices->active().size());
        for (const SSVortices::LiveVortex& v : vortices->active())
        {
            StormCellsData::VortexIcon icon;
            icon.mId = v.mCandidate.mId;
            icon.mKind = (S32)v.mKind;
            icon.mRotationSign = v.mCandidate.mRotSign;
            icon.mParentIsHero = v.mParentIsHero;
            icon.mWasRelabelledWaterspout = v.mWasRelabelledWaterspout;
            icon.mContactAgent = sc->toAgentXY(toStormVec2(v.mContactGlobal));
            icon.mCondensation = v.mState.mCondensation;
            icon.mIntensity = v.mState.mIntensity;
            icon.mMultiN = v.mCandidate.mMultiN;
            icon.mHasFunnel = v.mHasFunnel;
            if (v.mHasFunnel)
            {
                icon.mCollars.reserve(v.mCollars.size());
                for (const SSVortices::Collar& c : v.mCollars)
                {
                    StormCellsData::VortexIcon::Collar out;
                    out.mRadiusM = c.mRadiusM;
                    out.mAltitudeZ = c.mAltitudeM;
                    icon.mCollars.push_back(out);
                }
            }
            d.mVortices.push_back(icon);
        }

        d.mDustDevils.reserve(vortices->dustDevils().size());
        for (const SSVortices::DustVortex& dv : vortices->dustDevils())
        {
            StormCellsData::DustIcon di;
            di.mId = dv.mCandidate.mId;
            di.mOriginAgent = sc->toAgentXY(toStormVec2(dv.mCandidate.mOriginXY));
            di.mIntensity = dv.mIntensity;
            d.mDustDevils.push_back(di);
        }

        const SSVortices::WhyNotTornado& vwhy = vortices->whyNot();
        d.mVortexWhyNot.mHaveHero = vwhy.mHaveHero;
        d.mVortexWhyNot.mKind = (S32)vwhy.mKind;
        d.mVortexWhyNot.mAlive = vwhy.mAlive;
        d.mVortexWhyNot.mMeso = vwhy.mMeso;
        d.mVortexWhyNot.mPotential = vwhy.mPotential;
        d.mVortexWhyNot.mSupercell = vwhy.mSupercell;
        d.mVortexWhyNot.mTornadoEligible = vwhy.mTornadoEligible;
        d.mVortexWhyNot.mAllowTornadoes = vwhy.mAllowTornadoes;
        d.mVortexWhyNot.mFailing = vwhy.mFailing;
    }

    sStormCellsDataFrame = gFrameCount;
    sStormCellsDataCache = d;
    return d;
}

// The V2 layer (doc/atmo_magic_debug_views.md V2), drawn at the deck base: lattice tiles tinted by potential (energy ramp, alpha floored at the spawn threshold), alive cells as influence rings coloured by stage with the 40% plateau ring inside, the hero's trajectory ribbon origin -> now -> death coloured by lifecycle with age ticks and the closest-approach marker tied to the anchor, rotation as orbiting purple arrows (anticlockwise for cyclonic, clockwise for anticyclonic, advanced by the wall clock), and - DEBUG - a vortex icon (kind-coloured cross, labelled by taxonomy, with a condensation bar and the multi-vortex N) at every active vortex's contact point, its funnel's collar table as a stack of wireframe rings, and dust devils as small tan crosses. Everything is squash-corrected through the cloud field and distance-thinned (ring segments and labels fall off with camera distance) - the camera shapes the DISPLAY only; every position drawn came from the scheduler's (or SSVortices') world-frame state. [interaction: SSVolCloud squashScale] [interaction: SSVortices active/dustDevils]
void SSAtmoInfoView::renderStormCells()
{
    const StormCellsData d = stormCellsData();
    if (!d.mValid) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();
    const F32 z = d.mLayerZ;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };
    auto distXY = [&](const LLVector2& p) -> F32
    {
        const F32 dx = p.mV[0] - cam.mV[VX];
        const F32 dy = p.mV[1] - cam.mV[VY];
        return std::sqrt(dx * dx + dy * dy);
    };
    auto at = [&](const LLVector2& p, F32 dz = 0.f) -> LLVector3
    {
        return LLVector3(p.mV[0], p.mV[1], z + dz);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this now draws AFTER the dim quad in the UI stage (SSAtmoDimView::draw), on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Filled lattice tiles first (two triangles each), so every line lands on top of them.
    const F32 half = SSStormCell::LATTICE_M * 0.47f;
    gGL.begin(LLRender::TRIANGLES);
    for (const StormCellsData::Tile& t : d.mTiles)
    {
        const LLColor4 c = toColor(energyRamp(t.mPotential, 1.f), latticeTileAlpha(t.mPotential, SSStormCell::SPAWN_THRESHOLD));
        gGL.color4fv(c.mV);
        const LLVector3 a = drawn(LLVector3(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] - half, z));
        const LLVector3 b = drawn(LLVector3(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] - half, z));
        const LLVector3 cc = drawn(LLVector3(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] + half, z));
        const LLVector3 dd = drawn(LLVector3(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] + half, z));
        gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(cc.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(cc.mV); gGL.vertex3fv(dd.mV);
    }
    gGL.end();

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    auto ring = [&](const LLVector2& centre, F32 radius, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        const S32 n = ringSegments(radius, distXY(centre));
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(at(LLVector2(centre.mV[0] + radius, centre.mV[1])));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(at(LLVector2(centre.mV[0] + std::cos(a) * radius, centre.mV[1] + std::sin(a) * radius)));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    // Same as `ring` but at an explicit world Z rather than the layer's fixed z - the collar wireframe stacks one
    // of these per collar height (SSVortex::collarAltitudeM already returns world Z, so no further conversion).
    auto ringZ = [&](const LLVector2& centre, F32 radius, F32 zAt, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        const S32 n = ringSegments(radius, distXY(centre));
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(centre.mV[0] + radius, centre.mV[1], zAt));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(centre.mV[0] + std::cos(a) * radius, centre.mV[1] + std::sin(a) * radius, zAt));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    auto cross = [&](const LLVector2& centre, F32 arm, const LLColor4& color)
    {
        gGL.color4fv(color.mV);
        line(at(LLVector2(centre.mV[0] - arm, centre.mV[1])), at(LLVector2(centre.mV[0] + arm, centre.mV[1])));
        line(at(LLVector2(centre.mV[0], centre.mV[1] - arm)), at(LLVector2(centre.mV[0], centre.mV[1] + arm)));
    };
    auto arrow = [&](const LLVector2& tail, const LLVector2& dir, F32 len, const LLColor4& color)
    {
        gGL.color4fv(color.mV);
        const LLVector2 tip(tail.mV[0] + dir.mV[0] * len, tail.mV[1] + dir.mV[1] * len);
        const LLVector2 perp(-dir.mV[1], dir.mV[0]);
        const LLVector2 barb_a(tip.mV[0] - dir.mV[0] * len * 0.3f + perp.mV[0] * len * 0.18f, tip.mV[1] - dir.mV[1] * len * 0.3f + perp.mV[1] * len * 0.18f);
        const LLVector2 barb_b(tip.mV[0] - dir.mV[0] * len * 0.3f - perp.mV[0] * len * 0.18f, tip.mV[1] - dir.mV[1] * len * 0.3f - perp.mV[1] * len * 0.18f);
        line(at(tail), at(tip));
        line(at(tip), at(barb_a));
        line(at(tip), at(barb_b));
    };

    gGL.begin(LLRender::LINES);

    // Tile outlines: faint everywhere, bright where an active cell was born.
    for (const StormCellsData::Tile& t : d.mTiles)
    {
        const LLColor4& c = t.mAlive ? TILE_ALIVE : TILE_EDGE;
        gGL.color4fv(c.mV);
        const LLVector2 p0(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] - half);
        const LLVector2 p1(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] - half);
        const LLVector2 p2(t.mCentreAgent.mV[0] + half, t.mCentreAgent.mV[1] + half);
        const LLVector2 p3(t.mCentreAgent.mV[0] - half, t.mCentreAgent.mV[1] + half);
        line(at(p0), at(p1)); line(at(p1), at(p2)); line(at(p2), at(p3)); line(at(p3), at(p0));
    }

    // The anchor: the weather domain's region centre, with the hero pass band as two faint rings.
    cross(d.mAnchorAgent, 120.f, ANCHOR_WHITE);
    ring(d.mAnchorAgent, SSStormCell::HERO_PASS_MIN_M, LLColor4(1.f, 1.f, 1.f, 0.25f));
    ring(d.mAnchorAgent, SSStormCell::HERO_PASS_MAX_M, LLColor4(1.f, 1.f, 1.f, 0.25f));

    // Alive cells: influence ring by stage, the 40% plateau inside it, a centre cross, and rotation glyphs for supercells.
    for (const StormCellsData::Cell& c : d.mCells)
    {
        const LLColor4 sc = toColor(stageColor(c.mStage), c.mIsHero ? 1.f : 0.85f);
        ring(c.mCentreAgent, c.mRadiusM, sc);
        // <SS:Nexii> review S10: the 40% plateau is SSStormCouple::INFLUENCE_CORE, not a second hand-spelled 0.4f - the ring drawn here must be the same radius SSStormCouple::influence treats as full influence, or the readout lies about where the plateau actually is.
        ring(c.mCentreAgent, c.mRadiusM * SSStormCouple::INFLUENCE_CORE, LLColor4(sc.mV[0], sc.mV[1], sc.mV[2], 0.35f));
        cross(c.mCentreAgent, llmax(40.f, c.mRadiusM * 0.06f), sc);

        if (c.mSupercell)
        {
            const LLColor4 rc = toColor(rotationRamp(c.mRotation), 0.95f);
            const F32 orbit = llmax(c.mRadiusM * 0.55f, 120.f);
            const F32 len = llmax(c.mRadiusM * 0.14f, 40.f) * llclamp(0.4f + 0.6f * c.mMeso, 0.4f, 1.f);
            const F32 sign = (c.mRotation < 0.f) ? -1.f : 1.f;
            for (S32 i = 0; i < ORBIT_GLYPHS; ++i)
            {
                const F32 a = orbitAngle(d.mNow, c.mRotation, i, ORBIT_GLYPHS, ORBIT_PERIOD_S);
                const LLVector2 tail(c.mCentreAgent.mV[0] + std::cos(a) * orbit, c.mCentreAgent.mV[1] + std::sin(a) * orbit);
                // Tangent: anticlockwise for positive omega, clockwise for negative.
                const LLVector2 dir(-std::sin(a) * sign, std::cos(a) * sign);
                arrow(tail, dir, len, rc);
            }
        }

        if (c.mIsForced)
        {
            // <SS:Nexii> FORCED: a distinct outline (a bright ring just outside the stage ring, with a dimmer one
            // outside that) marking the authored/forced pin (SSSquall::forcedCandidate) apart from every hashed
            // cell around it - a forced cell is never also a squall-line member (ssstormcellcore.h's own comment on
            // ActiveCell), so this never competes with the squall bar below for the same swatch.
            ring(c.mCentreAgent, c.mRadiusM * 1.06f, FORCED_OUTLINE);
            ring(c.mCentreAgent, c.mRadiusM * 1.16f, LLColor4(FORCED_OUTLINE.mV[0], FORCED_OUTLINE.mV[1], FORCED_OUTLINE.mV[2], 0.45f));
        }
    }

    // <SS:Nexii> SQUALL: the bar through a line's own currently-alive members, in the template's own along-the-line
    // order (stormCellsData()'s own comment), plus a marker at every leading-edge QLCS spin-up junction the
    // template defines - drawn whether or not its neighbouring members happen to be alive right now, a property of
    // the line's geometry rather than of which members gated this instant.
    for (const StormCellsData::SquallLine& sl : d.mSquallLines)
    {
        gGL.color4fv(SQUALL_LINE.mV);
        for (size_t i = 0; i + 1 < sl.mMembersAgent.size(); ++i)
        {
            line(at(sl.mMembersAgent[i]), at(sl.mMembersAgent[i + 1]));
        }
        for (const LLVector2& j : sl.mJunctionsAgent)
        {
            cross(j, 90.f, SQUALL_JUNCTION);
        }
    }

    // The hero ribbon: origin -> death, coloured by the lifecycle stage each stretch falls in, with age ticks; a marker at now; the closest approach tied to the anchor.
    if (d.mHaveHero)
    {
        const StormCellsData::HeroPath& h = d.mHero;
        const F32 life = llmax(h.mLifetimeS, 1.f);
        const LLVector2 span(h.mDeathAgent.mV[0] - h.mOriginAgent.mV[0], h.mDeathAgent.mV[1] - h.mOriginAgent.mV[1]);
        const F32 span_len = span.length();
        const LLVector2 dir = (span_len > 1e-3f) ? LLVector2(span.mV[0] / span_len, span.mV[1] / span_len) : LLVector2(0.f, 1.f);
        const LLVector2 perp(-dir.mV[1], dir.mV[0]);

        const S32 strips = 40;
        const F32 age_now = (F32)((d.mNow - h.mBirthTime) / (F64)life);
        for (S32 i = 0; i < strips; ++i)
        {
            const F32 t0 = (F32)i / (F32)strips;
            const F32 t1 = (F32)(i + 1) / (F32)strips;
            const LLVector2 a(h.mOriginAgent.mV[0] + span.mV[0] * t0, h.mOriginAgent.mV[1] + span.mV[1] * t0);
            const LLVector2 b(h.mOriginAgent.mV[0] + span.mV[0] * t1, h.mOriginAgent.mV[1] + span.mV[1] * t1);
            const bool past = t1 <= age_now;
            gGL.color4fv(toColor(lifecycleRamp((t0 + t1) * 0.5f), past ? 1.f : 0.45f).mV);
            line(at(a), at(b));
        }

        // Age ticks, perpendicular, one every ribbonTickIntervalS of lifetime.
        const F32 tick_s = ribbonTickIntervalS(h.mLifetimeS);
        const F32 tick_len = 60.f;
        for (F32 s_age = tick_s; s_age < h.mLifetimeS; s_age += tick_s)
        {
            const F32 t = s_age / life;
            const LLVector2 p(h.mOriginAgent.mV[0] + span.mV[0] * t, h.mOriginAgent.mV[1] + span.mV[1] * t);
            gGL.color4fv(toColor(lifecycleRamp(t), 0.9f).mV);
            line(at(LLVector2(p.mV[0] - perp.mV[0] * tick_len, p.mV[1] - perp.mV[1] * tick_len)),
                 at(LLVector2(p.mV[0] + perp.mV[0] * tick_len, p.mV[1] + perp.mV[1] * tick_len)));
        }

        cross(h.mOriginAgent, 150.f, HERO_ORIGIN);
        cross(h.mDeathAgent, 150.f, HERO_DEATH);
        // Now: a diamond.
        {
            const F32 r = 110.f;
            gGL.color4fv(ANCHOR_WHITE.mV);
            const LLVector2 n = h.mNowAgent;
            line(at(LLVector2(n.mV[0] - r, n.mV[1])), at(LLVector2(n.mV[0], n.mV[1] + r)));
            line(at(LLVector2(n.mV[0], n.mV[1] + r)), at(LLVector2(n.mV[0] + r, n.mV[1])));
            line(at(LLVector2(n.mV[0] + r, n.mV[1])), at(LLVector2(n.mV[0], n.mV[1] - r)));
            line(at(LLVector2(n.mV[0], n.mV[1] - r)), at(LLVector2(n.mV[0] - r, n.mV[1])));
        }
        // Closest approach: a ring at the point, and the perpendicular to the anchor.
        ring(h.mClosestAgent, 90.f, ANCHOR_WHITE);
        gGL.color4fv(LLColor4(1.f, 1.f, 1.f, 0.6f).mV);
        line(at(h.mClosestAgent), at(d.mAnchorAgent));
        // Motion arrow from the origin.
        const F32 speed = h.mMotion.length();
        if (speed > 1e-3f)
        {
            arrow(h.mOriginAgent, LLVector2(h.mMotion.mV[0] / speed, h.mMotion.mV[1] / speed), 400.f, HERO_ORIGIN);
        }
    }

    // <SS:Nexii> DEBUG: vortex icons - a kind-coloured cross at each active vortex's contact point (colour =
    // vortexKindColor, sign-only rotation ramp), a condensation bar beside it (grey trough, filled to
    // mCondensation in the kind colour), and - funnel-having kinds only - the funnel's own collar table drawn as a
    // stack of wireframe rings, squash-corrected through `drawn` exactly like every other ring in this layer (the
    // camera shapes the DISPLAY only; every collar radius/altitude came from SSVortices' world-frame state).
    // [interaction: SSVortices active]
    for (const StormCellsData::VortexIcon& v : d.mVortices)
    {
        const LLColor4 kc = toColor(vortexKindColor(v.mRotationSign), v.mParentIsHero ? 1.f : 0.85f);
        const F32 arm = llclamp(90.f * llmax(v.mIntensity, 0.2f), 30.f, 140.f);
        cross(v.mContactAgent, arm, kc);

        // Condensation bar: a fixed-height trough beside the icon, filled bottom-up to mCondensation.
        const F32 bar_h = 260.f;
        const LLVector2 bar_pos(v.mContactAgent.mV[0] + arm * 1.6f, v.mContactAgent.mV[1]);
        gGL.color4fv(LLColor4(kc.mV[0], kc.mV[1], kc.mV[2], 0.25f).mV);
        line(at(bar_pos), at(bar_pos, bar_h));
        gGL.color4fv(kc.mV);
        line(at(bar_pos), at(bar_pos, bar_h * llclamp(v.mCondensation, 0.f, 1.f)));

        // Collar wireframe: one ring per collar, at its own world Z.
        if (v.mHasFunnel)
        {
            const LLColor4 collar_c(kc.mV[0], kc.mV[1], kc.mV[2], 0.35f);
            for (const StormCellsData::VortexIcon::Collar& col : v.mCollars)
            {
                ringZ(v.mContactAgent, col.mRadiusM, col.mAltitudeZ, collar_c);
            }
        }
    }

    // Dust devils: parentless, funnel-less - a small dusty-tan cross at the origin, no collar table.
    for (const StormCellsData::DustIcon& dv : d.mDustDevils)
    {
        cross(dv.mOriginAgent, llclamp(60.f * llmax(dv.mIntensity, 0.2f), 20.f, 90.f), DUST_COLOR);
    }

    gGL.end();

    // Labels, thinned by camera distance: tiles within 5 km show P, cells within 9 km their id/stage/age, the hero and anchor always.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        for (const StormCellsData::Tile& t : d.mTiles)
        {
            if (distXY(t.mCentreAgent) > 5000.f) continue;
            const LLColor4 c = toColor(energyRamp(t.mPotential, 1.f), 0.9f);
            hud_render_utf8text(llformat("P %.2f  SH %.2f", t.mPotential, t.mShearNoise), drawn(at(t.mCentreAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, -30.f, -4.f, c, false);
        }
        for (const StormCellsData::Cell& c : d.mCells)
        {
            if (!c.mIsHero && distXY(c.mCentreAgent) > 9000.f) continue;
            const F64 age = d.mNow - c.mBirthTime;
            const std::string rot = c.mSupercell ? llformat("  rot %+.2f", c.mRotation) : std::string();
            const std::string tag = c.mIsForced ? "  FORCED" : (c.mLineId != 0 ? llformat("  line %s", shortId(c.mLineId).c_str()) : std::string());
            const std::string label = llformat("%s%s %s  %.0f/%.0f min  r %.0f%s  I %.2f%s", c.mIsHero ? "HERO " : (c.mSupercell ? "SUPER " : ""),
                                               shortId(c.mId).c_str(), stageLabel(c.mStage), age / 60.0, c.mLifetimeS / 60.f, c.mRadiusM,
                                               rot.c_str(), c.mIntensity, tag.c_str());
            const LLColor4 label_c = c.mIsForced ? FORCED_OUTLINE : toColor(stageColor(c.mStage), 1.f);
            hud_render_utf8text(label, drawn(at(c.mCentreAgent, 30.f)), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f,
                                label_c, false);
        }
        hud_render_utf8text("anchor (region centre)", drawn(at(d.mAnchorAgent)), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, -14.f, ANCHOR_WHITE, false);
        if (d.mHaveHero)
        {
            const StormCellsData::HeroPath& h = d.mHero;
            hud_render_utf8text(llformat("hero origin  %.0f m upwind", (h.mOriginAgent - h.mClosestAgent).length()), drawn(at(h.mOriginAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, HERO_ORIGIN, false);
            hud_render_utf8text(llformat("closest %.0f m  %+.0f s", h.mClosestDistM, h.mClosestTime - d.mNow), drawn(at(h.mClosestAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, ANCHOR_WHITE, false);
            hud_render_utf8text(llformat("death  +%.0f min", (h.mBirthTime + (F64)h.mLifetimeS - d.mNow) / 60.0), drawn(at(h.mDeathAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, HERO_DEATH, false);
        }

        // Vortex icons: kind, condensation, live intensity, and the multi-vortex N (0 = single vortex).
        for (const StormCellsData::VortexIcon& v : d.mVortices)
        {
            const LLColor4 c = toColor(vortexKindColor(v.mRotationSign), 1.f);
            std::string label = llformat("%s%s%s  cond %.2f  I %.2f", v.mParentIsHero ? "HERO " : "", vortexKindLabel(v.mKind),
                                         v.mWasRelabelledWaterspout ? " (relabelled)" : "", v.mCondensation, v.mIntensity);
            if (v.mMultiN > 0)
            {
                label += llformat("  N %d", v.mMultiN);
            }
            hud_render_utf8text(label, drawn(at(v.mContactAgent, 20.f)), *font, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, c, false);
        }
        for (const StormCellsData::DustIcon& dv : d.mDustDevils)
        {
            if (distXY(dv.mOriginAgent) > 5000.f) continue;
            hud_render_utf8text(llformat("dust devil  I %.2f", dv.mIntensity), drawn(at(dv.mOriginAgent)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 8.f, 6.f, DUST_COLOR, false);
        }
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V3's deck LOD
// ---------------------------------------------------------------------------

// The V3 layer (ssdecklodcore.h CONTRACT): five rings about the camera on the deck plane, one per LOD rail
// (SUBS_FULL_M, SUBS_TWO_M, THIN_START_M, FIELD_FADE_START_M, DECK_EDGE_M), and a coarse grid of tinted tiles
// (grey ramp, SSDeckLod::keepFrac at each tile's own camera distance). This is a DIAGRAM of the distance-only LOD
// ramp, not a replay of buildDeck's own cell gate/occupancy - the view has no access to that state and the
// contract forbids reimplementing it here, so the tile grid is a fixed pitch centred on the camera rather than
// the builder's own drifting, hero-shifted, gated lattice. Squash-corrected like every other in-world layer.
// [interaction: SSVolCloud squashScale/cloudBaseZ/puffCount]
void SSAtmoInfoView::renderDeckLod()
{
    const DeckLodData d = deckLodData();

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();
    const F32 z = d.mLayerZ;

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this now draws AFTER the dim quad in the UI stage (SSAtmoDimView::draw), on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Tinted tiles: a fixed pitch (coarser than the builder's own SSDeckLod::CELL_M, since this is a display
    // sampling of a pure distance function, not a cell-for-cell replay), covering the ramp's whole reach.
    const F32 pitch = SSDeckLod::CELL_M * 4.f;
    const F32 reach = SSDeckLod::DECK_EDGE_M;
    const S32 half_n = (S32)std::ceil(reach / pitch);
    const S32 cx0 = (S32)std::floor(cam.mV[VX] / pitch);
    const S32 cy0 = (S32)std::floor(cam.mV[VY] / pitch);
    const F32 half_tile = pitch * 0.47f;

    gGL.begin(LLRender::TRIANGLES);
    for (S32 ty = -half_n; ty <= half_n; ++ty)
    {
        for (S32 tx = -half_n; tx <= half_n; ++tx)
        {
            const F32 tcx = (F32)(cx0 + tx) * pitch + pitch * 0.5f;
            const F32 tcy = (F32)(cy0 + ty) * pitch + pitch * 0.5f;
            const F32 ddx = tcx - cam.mV[VX];
            const F32 ddy = tcy - cam.mV[VY];
            const F32 dist = std::sqrt(ddx * ddx + ddy * ddy);
            if (dist > reach) continue;

            const LLColor4 tint = toColor(presenceRamp(SSDeckLod::keepFrac(dist), 1.f), 0.30f);
            gGL.color4fv(tint.mV);
            const LLVector3 a = drawn(LLVector3(tcx - half_tile, tcy - half_tile, z));
            const LLVector3 b = drawn(LLVector3(tcx + half_tile, tcy - half_tile, z));
            const LLVector3 c = drawn(LLVector3(tcx + half_tile, tcy + half_tile, z));
            const LLVector3 e = drawn(LLVector3(tcx - half_tile, tcy + half_tile, z));
            gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
            gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(e.mV);
        }
    }
    gGL.end();

    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): the macro grid, a coarser MACRO_M-pitch tint drawn
    // ONLY beyond the same TIER_B_M - TIER_BLEND_M/2 rail buildDeck's own accumulator uses (macroEligible's own
    // cellInWalk-style membership on the tile centre is display-only here, same caveat as the fine tile grid
    // above - a diagram of the distance ramp, not a replay of buildDeck's own occupancy), alpha carrying wB
    // (SSDeckMacro::tierWeights) so the tint itself crossfades in across the blend band the way Tier B's real
    // bodies do.
    const F32 macro_pitch = SSDeckMacro::MACRO_M;
    const S32 macro_half_n = (S32)std::ceil(reach / macro_pitch);
    const S32 macro_cx0 = (S32)std::floor(cam.mV[VX] / macro_pitch);
    const S32 macro_cy0 = (S32)std::floor(cam.mV[VY] / macro_pitch);
    const F32 macro_half_tile = macro_pitch * 0.47f;
    const F32 macro_lo = SSDeckMacro::TIER_B_M - SSDeckMacro::TIER_BLEND_M * 0.5f;

    gGL.begin(LLRender::TRIANGLES);
    for (S32 ty = -macro_half_n; ty <= macro_half_n; ++ty)
    {
        for (S32 tx = -macro_half_n; tx <= macro_half_n; ++tx)
        {
            const F32 tcx = (F32)(macro_cx0 + tx) * macro_pitch + macro_pitch * 0.5f;
            const F32 tcy = (F32)(macro_cy0 + ty) * macro_pitch + macro_pitch * 0.5f;
            const F32 ddx = tcx - cam.mV[VX];
            const F32 ddy = tcy - cam.mV[VY];
            const F32 dist = std::sqrt(ddx * ddx + ddy * ddy);
            if (dist <= macro_lo || dist > reach) continue;

            F32 wA, wB;
            SSDeckMacro::tierWeights(dist, wA, wB);
            const LLColor4 tint(RING_TIER_B.mV[0], RING_TIER_B.mV[1], RING_TIER_B.mV[2], 0.30f * wB);
            gGL.color4fv(tint.mV);
            const LLVector3 a = drawn(LLVector3(tcx - macro_half_tile, tcy - macro_half_tile, z));
            const LLVector3 b = drawn(LLVector3(tcx + macro_half_tile, tcy - macro_half_tile, z));
            const LLVector3 c = drawn(LLVector3(tcx + macro_half_tile, tcy + macro_half_tile, z));
            const LLVector3 e = drawn(LLVector3(tcx - macro_half_tile, tcy + macro_half_tile, z));
            gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
            gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(e.mV);
        }
    }
    gGL.end();

    // The five rings, one per rail, centred on the camera - a fixed segment count rather than ringSegments'
    // apparent-size heuristic (that helper thins by distance FROM the camera TO a ring's centre, which here is
    // always zero; a flat 64 keeps every rail smooth without it).
    auto ring = [&](F32 radius, const LLColor4& color)
    {
        constexpr S32 n = 64;
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(cam.mV[VX] + radius, cam.mV[VY], z));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(cam.mV[VX] + std::cos(a) * radius, cam.mV[VY] + std::sin(a) * radius, z));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);
    ring(SSDeckLod::SUBS_FULL_M, RING_SUBS_FULL);
    ring(SSDeckLod::SUBS_TWO_M, RING_SUBS_TWO);
    ring(SSDeckLod::THIN_START_M, RING_THIN_START);
    ring(SSDeckLod::FIELD_FADE_START_M, RING_FADE_START);
    ring(SSDeckLod::DECK_EDGE_M, RING_DECK_EDGE);
    gGL.end();

    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        auto label = [&](F32 radius, const char* text, const LLColor4& color)
        {
            hud_render_utf8text(text, drawn(LLVector3(cam.mV[VX] + radius, cam.mV[VY], z)), *font, LLFontGL::NORMAL,
                                LLFontGL::DROP_SHADOW, 6.f, 4.f, color, false);
        };
        label(SSDeckLod::SUBS_FULL_M, "subs full", RING_SUBS_FULL);
        label(SSDeckLod::SUBS_TWO_M, "subs 2->1", RING_SUBS_TWO);
        label(SSDeckLod::THIN_START_M, "thin start", RING_THIN_START);
        label(SSDeckLod::FIELD_FADE_START_M, "edge fade start", RING_FADE_START);
        label(SSDeckLod::DECK_EDGE_M, "deck edge", RING_DECK_EDGE);
    }
}

// ---------------------------------------------------------------------------
// The in-world layer: V4's precip & virga
// ---------------------------------------------------------------------------

// The V4 layer (doc/atmo_magic_debug_views.md V4, ssvirgacore.h CONTRACT): every qualifying cell from the LAST
// build's snapshot (SSAtmoInfoView::virgaData(), itself SSVolCloud::virgaDebug() read straight across) outlined
// at deck-base height and tinted by drive (presence ramp - grey to white, per the design's colour language),
// bright for the cells the hashed trim actually KEPT and dim for the ones MAX_SHAFTS trimmed away; a fall-tilt
// comparison line per kept cell - deck base entry point to landing - using precip's OWN wind-tilt formula
// (SSAtmoInfoViewCore::fallTiltOffsetM), a separate line from the shaft's own vertical card stack (ssvirgacore.h:
// a curtain has no per-altitude lean) so the two can be read against each other; and the particle rain's own
// handoff boundary (r2 * SSVirga::HANDOFF_SKIP) as a ring on the ground plane about the CAMERA, with its ramp
// band (HANDOFF_BAND_M) drawn as a short run of fading rings out to full shaft alpha. Squash-corrected like every
// other in-world layer. [interaction: SSVolCloud squashScale/virgaDebug]
void SSAtmoInfoView::renderVirga()
{
    const VirgaData d = virgaData();
    if (!d.mValid || !d.mActive) return;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;
    const LLVector3 cam = camera->getOrigin();

    const SSVolCloud* clouds = SSVolCloud::instanceExists() ? SSVolCloud::getInstance() : nullptr;
    auto drawn = [&](const LLVector3& p) -> LLVector3
    {
        if (!clouds) return p;
        const LLVector3 rel = p - cam;
        const F32 dist = rel.magVec();
        if (dist <= 1.0e-4f) return p;
        return cam + rel * clouds->squashScale(dist);
    };

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE); // <SS:Nexii> off, not just no-write: this now draws AFTER the dim quad in the UI stage (SSAtmoDimView::draw), on top like a Skylines layer, so it must never be occluded by world geometry
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto line = [&](const LLVector3& a, const LLVector3& b)
    {
        const LLVector3 span = b - a;
        const S32 segs = llclamp((S32)(span.magVec() / 200.f), 1, 24);
        LLVector3 prev = drawn(a);
        for (S32 i = 1; i <= segs; ++i)
        {
            const LLVector3 next = drawn(a + span * ((F32)i / (F32)segs));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };
    // A ring centred on the camera at the layer's own z - the same fixed-64-segment idiom renderDeckLod uses for
    // its camera-centred rails (ringSegments' apparent-size heuristic thins by distance TO a ring's centre, which
    // here is always zero).
    auto camRing = [&](F32 radius, F32 zAt, const LLColor4& color)
    {
        if (radius <= 0.f) return;
        constexpr S32 n = 64;
        gGL.color4fv(color.mV);
        LLVector3 prev = drawn(LLVector3(cam.mV[VX] + radius, cam.mV[VY], zAt));
        for (S32 i = 1; i <= n; ++i)
        {
            const F32 a = (F32)i / (F32)n * TWO_PI_F;
            const LLVector3 next = drawn(LLVector3(cam.mV[VX] + std::cos(a) * radius, cam.mV[VY] + std::sin(a) * radius, zAt));
            gGL.vertex3fv(prev.mV);
            gGL.vertex3fv(next.mV);
            prev = next;
        }
    };

    gGL.begin(LLRender::LINES);

    // The qualifying cells, outlined at the deck base (the shaft column's own top): bright/kept, dim/trimmed.
    const F32 half = SSVirga::CELL_M * 0.45f;
    for (const VirgaData::Cell& c : d.mCells)
    {
        const LLColor4 col = toColor(presenceRamp(c.mDrive, 1.f), c.mKept ? 0.85f : 0.25f);
        gGL.color4fv(col.mV);
        const LLVector3 p0(c.mX - half, c.mY - half, d.mBaseZ);
        const LLVector3 p1(c.mX + half, c.mY - half, d.mBaseZ);
        const LLVector3 p2(c.mX + half, c.mY + half, d.mBaseZ);
        const LLVector3 p3(c.mX - half, c.mY + half, d.mBaseZ);
        line(p0, p1); line(p1, p2); line(p2, p3); line(p3, p0);
    }

    // Fall-tilt comparison lines, kept cells only (the ones actually drawn as shaft cards): landing at the
    // cell's own ground point (the SAME x/y the vertical curtain hangs at - ssvirgacore.h's own "no per-altitude
    // lean"), entry upwind at the deck base by precip's own wind-tilt offset.
    const F32 drop_h = llmax(d.mBaseZ - d.mGroundZ, 0.f);
    const Vec2M tilt = fallTiltOffsetM(d.mWindGround.x, d.mWindGround.y, d.mFallSpeedMS, drop_h);
    for (const VirgaData::Cell& c : d.mCells)
    {
        if (!c.mKept) continue;
        gGL.color4fv(RAIL_BASE.mV);
        const LLVector3 landing(c.mX, c.mY, d.mGroundZ);
        const LLVector3 entry(c.mX - tilt.x, c.mY - tilt.y, d.mBaseZ);
        line(entry, landing);
    }

    // The handoff ring and its ramp band, on the ground plane, centred on the camera.
    if (d.mHandoffRadius > 0.f)
    {
        camRing(d.mHandoffRadius, d.mGroundZ, VIRGA_HANDOFF);
        constexpr S32 BAND_STEPS = 5;
        for (S32 i = 1; i <= BAND_STEPS; ++i)
        {
            const F32 radius = d.mHandoffRadius + SSVirga::HANDOFF_BAND_M * (F32)i / (F32)BAND_STEPS;
            const F32 alpha = VIRGA_HANDOFF.mV[3] * SSVirga::handoff(radius, d.mR2);
            camRing(radius, d.mGroundZ, LLColor4(VIRGA_HANDOFF.mV[0], VIRGA_HANDOFF.mV[1], VIRGA_HANDOFF.mV[2], alpha));
        }
    }

    gGL.end();

    // Labels: drive at every kept cell (within 6 km), the handoff radius once.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    if (font)
    {
        for (const VirgaData::Cell& c : d.mCells)
        {
            if (!c.mKept) continue;
            const F32 dx = c.mX - cam.mV[VX];
            const F32 dy = c.mY - cam.mV[VY];
            if (dx * dx + dy * dy > 6000.f * 6000.f) continue;
            const LLColor4 col = toColor(presenceRamp(c.mDrive, 1.f), 1.f);
            hud_render_utf8text(llformat("drive %.2f", c.mDrive), drawn(LLVector3(c.mX, c.mY, d.mBaseZ)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f, col, false);
        }
        if (d.mHandoffRadius > 0.f)
        {
            hud_render_utf8text(llformat("handoff  r2 x %.1f = %.0f m", SSVirga::HANDOFF_SKIP, d.mHandoffRadius),
                                drawn(LLVector3(cam.mV[VX] + d.mHandoffRadius, cam.mV[VY], d.mGroundZ)), *font,
                                LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 6.f, 4.f, VIRGA_HANDOFF, false);
        }
    }
}

// ---------------------------------------------------------------------------
// SSAtmoDimView
// ---------------------------------------------------------------------------

SSAtmoDimView::SSAtmoDimView(const Params& p)
:   LLView(p)
{
}

// <SS:Nexii> Used to draw only the dim quad, with the in-world layer left to LLPipeline::renderDebug's 3-D pass BENEATH it - so the quad darkened the wind mast/cell rings/etc right along with the world it was dimming, which is backwards for a Skylines-style info overlay (it should always read on top). Now the quad draws first (still nothing while off, still one integer compare at mode 0) and SSAtmoInfoView::renderWorld() draws SECOND, re-entering 3-D from here: push the camera's OWN live projection+modelview onto gGL's matrix stack (the llhudrender.cpp idiom for world-space drawing from the UI pass) rather than recomputing via LLViewerCamera::setPerspective, which would also stomp glViewport and cache a different far clip than the main pass's MAX_FAR_CLIP - loadMatrix only touches gGL's stack, nothing global. Depth test is OFF for the whole overlay (each render* helper's own LLGLDepthTest, not this function's problem to guard) so it is never occluded by geometry - the one behavioural change from the old renderDebug call site. Runs even when the dim alpha is 0 (SSAtmoInfoViewDim at 0 must still show the layer, just without the tint), and pipeline.cpp's renderDebug no longer calls renderWorld() at all, so this is the layer's only draw site now.
void SSAtmoDimView::draw()
{
    if (SSAtmoInfoView::mode() == MODE_OFF) return;

    static LLCachedControl<F32> dim_setting(gSavedSettings, "SSAtmoInfoViewDim", 0.55f);
    const F32 a = llclamp((F32)dim_setting, 0.f, 0.95f);
    if (a > 0.f)
    {
        const LLRect& r = getRect();
        gl_rect_2d(0, r.getHeight(), r.getWidth(), 0, LLColor4(0.02f, 0.03f, 0.05f, a));
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (!camera) return;

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix((GLfloat*)camera->getProjection().mMatrix);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix((GLfloat*)camera->getModelview().mMatrix);

    LLGLDisable cull(GL_CULL_FACE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    SSAtmoInfoView::renderWorld();

    gGL.popMatrix(); // MODELVIEW
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
}

// ---------------------------------------------------------------------------
// SSAtmoLegendView
// ---------------------------------------------------------------------------

SSAtmoLegendView::SSAtmoLegendView(const Params& p)
:   LLView(p)
{
}

void SSAtmoLegendView::buildWindProfileSpec(Spec& spec)
{
    const SSAtmoInfoView::WindProfileData d = SSAtmoInfoView::windProfileData();
    spec.mTitle = "V1  WIND PROFILE";
    spec.mSubtitle = d.mValid
        ? llformat("10m %.1f m/s @ %03.0f  S %.2f  veer %.0f  exp %.2f", d.mParams.mSpeed10MS, d.mParams.mHeading10Deg,
                   d.mParams.mShearStrength, d.mParams.mVeerDeg, d.mParams.mExponent)
        : "no weather cube applied";
    spec.mRampLabel = "wind speed (arrow colour, curve)";
    spec.mRampMin = "0 m/s";
    spec.mRampMax = llformat("%.0f m/s", d.mMaxSpeed);
    spec.mRamp = &rampSpeed;
    spec.mKeys.push_back({ RAIL_REF,    "10 m reference wind", true });
    spec.mKeys.push_back({ RAIL_BL,     "1500 m boundary-layer top", true });
    spec.mKeys.push_back({ RAIL_BASE,   d.mDeckBuilt ? llformat("deck base  z %.0f", d.mBaseZ) : "deck base  (not built)", true });
    spec.mKeys.push_back({ RAIL_LID,    d.mDeckBuilt ? llformat("deck lid   z %.0f", d.mLidZ) : "deck lid   (not built)", true });
    spec.mKeys.push_back({ RAIL_CIRRUS, llformat("cirrus band now  z %.0f", d.mCirrusZ), true });
    spec.mKeys.push_back({ CURVE_LIVE,  "live: weather-cube exponent", true });
    spec.mKeys.push_back({ CURVE_FLOW,  llformat("flowmap region exp %.2f%s", d.mFlowAlpha, d.mFlowSolved ? "" : " (fallback)"), true });
    spec.mKeys.push_back({ CURVE_GRAD,  llformat("gradient setting %.2f", d.mGradientSetting), true });
    spec.mKeys.push_back({ TEXT_DIM,    "mast arrows point the way the air moves", false });
}

// V2's legend: the potential ramp the tiles are tinted with (threshold named as the floor), a row per lifecycle stage in ring colour, the hero ribbon and anchor marks, the two rotation hues - and, with no hero while an Allow checkbox is on, the scheduler's "why not" terms with their live values (SSStormCells::WhyNot), so formation can be debugged without reading code.
void SSAtmoLegendView::buildStormCellsSpec(Spec& spec)
{
    const SSAtmoInfoView::StormCellsData d = SSAtmoInfoView::stormCellsData();
    spec.mTitle = "V2  STORM CELLS";
    if (!d.mValid)
    {
        spec.mSubtitle = "scheduler not running (no track, region or asset)";
        return;
    }
    spec.mSubtitle = llformat("%d alive  %d spawned  %d super  %d eligible  %s  z %.0f%s", d.mAlive, d.mSpawned, d.mSupercells, d.mTornadoEligible,
                              d.mHaveHero ? "HERO LIVE" : "no hero", d.mLayerZ, d.mDeckBuilt ? "" : " (deck not built)");
    spec.mRampLabel = llformat("lattice potential P (tile tint; spawn floor %.2f)", SSStormCell::SPAWN_THRESHOLD);
    spec.mRampMin = "0";
    spec.mRampMax = "1";
    spec.mRamp = &rampEnergy;

    spec.mKeys.push_back({ TILE_ALIVE, "tile outline bright: an active cell was born there", true });
    static const char* kBands[STAGE_COUNT] = { "0.00-0.15", "0.15-0.30", "0.30-0.55", "0.55-0.80", "0.80-1.00" };
    for (S32 s = 0; s < STAGE_COUNT; ++s)
    {
        spec.mKeys.push_back({ toColor(stageColor(s), 1.f), llformat("ring: %-8s age %s", stageLabel(s), kBands[s]), true });
    }
    spec.mKeys.push_back({ HERO_ORIGIN, "hero origin + motion arrow", true });
    spec.mKeys.push_back({ ANCHOR_WHITE, d.mHaveHero ? llformat("now diamond; closest approach %.0f m at anchor", d.mHero.mClosestDistM) : "anchor cross, 500 / 1000 m pass rings", true });
    spec.mKeys.push_back({ HERO_DEATH, d.mHaveHero ? llformat("hero death; ribbon ticks every %.0f s", ribbonTickIntervalS(d.mHero.mLifetimeS)) : "hero death", true });
    spec.mKeys.push_back({ toColor(rotationRamp(1.f), 1.f), "rotation + cyclonic: arrows orbit anticlockwise", true });
    spec.mKeys.push_back({ toColor(rotationRamp(-1.f), 1.f), "rotation - anticyclonic: arrows orbit clockwise", true });

    // <SS:Nexii> SQUALL/FORCED (doc/atmo_magic_storm_dynamics.md sections 5-6): the bar through a line's own
    // members, its leading-edge QLCS junction markers, and the forced/authored pin's distinct double outline.
    spec.mKeys.push_back({ SQUALL_LINE, "squall line: bar through a line's own active members", true });
    spec.mKeys.push_back({ SQUALL_JUNCTION, "squall line: leading-edge QLCS junction marker", true });
    spec.mKeys.push_back({ FORCED_OUTLINE, "forced/authored cell: double outline (mStormOverride cue)", true });

    // <SS:Nexii> DEBUG: vortex icons - one swatch pair (the icon carries the SAME sign-only rotation colour as the
    // cell arrows above it, per vortexKindColor's own comment), then the taxonomy label list an icon can read as
    // (kind is text, not colour - see ssatmoinfoviewcore.h's vortexKindLabel), plus the condensation bar and
    // collar-wireframe/multi-vortex keys.
    spec.mKeys.push_back({ toColor(vortexKindColor(1.f), 1.f), "vortex icon (+): mesocyclonic / landspout / waterspout / satellite", true });
    spec.mKeys.push_back({ toColor(vortexKindColor(-1.f), 1.f), "vortex icon (-): anticyclonic", true });
    spec.mKeys.push_back({ TEXT_DIM, "vortex icon (funnel-less): gustnado", false });
    spec.mKeys.push_back({ DUST_COLOR, "dust devil (tan cross, no funnel)", true });
    spec.mKeys.push_back({ TEXT_DIM, "condensation bar: filled 0 (aloft) -> 1 (touchdown) -> 0 (roped out)", false });
    spec.mKeys.push_back({ TEXT_DIM, "collar rings: the funnel's own radius/altitude table, wireframe", false });
    spec.mKeys.push_back({ TEXT_DIM, "label N: multi-vortex suction-vortex count (absent = single vortex)", false });

    if (!d.mHaveHero)
    {
        if (!d.mAllowSupercells && !d.mAllowTornadoes)
        {
            spec.mKeys.push_back({ TEXT_DIM, "no hero: Allow Supercells / Allow Tornadoes off (Weather Influence)", false });
        }
        else
        {
            spec.mKeys.push_back({ TEXT_DIM, llformat("WHY NOT (best candidate)  Allow super %s  tornado %s", d.mAllowSupercells ? "on" : "OFF",
                                                      d.mAllowTornadoes ? "on" : "OFF"), false });
            for (const std::string& f : d.mWhyNot)
            {
                spec.mKeys.push_back({ TEXT_DIM, "  " + f, false });
            }
        }
    }
    else if (!d.mVortexWhyNot.mAlive)
    {
        // <SS:Nexii> DEBUG: the tornado "why not" readout - the hero DOES exist (the block above's WHY NOT never
        // fires here), but it carries no live tornado-family funnel right now (SSVortices::WhyNotTornado). Distinct
        // question from "why no hero" above it: this is "why not a tornado on the hero we have".
        spec.mKeys.push_back({ TEXT_DIM, llformat("TORNADO WHY NOT (hero)  meso %.2f  potential %.2f  super %s  eligible %s  allowTornadoes %s",
                                                  d.mVortexWhyNot.mMeso, d.mVortexWhyNot.mPotential, d.mVortexWhyNot.mSupercell ? "yes" : "no",
                                                  d.mVortexWhyNot.mTornadoEligible ? "yes" : "no", d.mVortexWhyNot.mAllowTornadoes ? "on" : "OFF"), false });
        for (const std::string& f : d.mVortexWhyNot.mFailing)
        {
            spec.mKeys.push_back({ TEXT_DIM, "  " + f, false });
        }
    }
}

// V3's legend: the five LOD rails in ring colour, the tile ramp (SSDeckLod::keepFrac, floored at THIN_KEEP_MIN),
// and the live puff count against the budget dial plus the LOD-predicted (pre-thin, pre-budget) figure so the
// two can be read against each other - a predicted figure well over the budget means the far thinning is doing
// most of the work; one close to it means the budget clamp (buildDeck's own LL_DEBUGS line) is the backstop
// actually firing.
void SSAtmoLegendView::buildDeckLodSpec(Spec& spec)
{
    const SSAtmoInfoView::DeckLodData d = SSAtmoInfoView::deckLodData();
    spec.mTitle = "V3  DECK LOD";
    if (!d.mDeckBuilt)
    {
        spec.mSubtitle = "no cloud deck built";
        return;
    }
    spec.mSubtitle = llformat("placed %d / budget %d  dial %d  LOD-predicted %lld over %d cells",
                              d.mPuffsPlaced, d.mBudget, d.mPuffsPerCell, (long long)d.mLodPredicted, d.mCellsWalked);
    spec.mRampLabel = llformat("keep fraction (tile tint; floor %.2f)", SSDeckLod::THIN_KEEP_MIN);
    spec.mRampMin = "0";
    spec.mRampMax = "1";
    spec.mRamp = &rampGrey;

    spec.mKeys.push_back({ RING_SUBS_FULL,  llformat("subs full below  %.0f m", SSDeckLod::SUBS_FULL_M), true });
    spec.mKeys.push_back({ RING_SUBS_TWO,   llformat("2 subs until  %.0f m", SSDeckLod::SUBS_TWO_M), true });
    spec.mKeys.push_back({ RING_THIN_START, llformat("far thinning from  %.0f m", SSDeckLod::THIN_START_M), true });
    spec.mKeys.push_back({ RING_FADE_START, llformat("edge fade from  %.0f m", SSDeckLod::FIELD_FADE_START_M), true });
    spec.mKeys.push_back({ RING_DECK_EDGE,  llformat("deck edge  %.0f m", SSDeckLod::DECK_EDGE_M), true });
    // <SS:Nexii> LOD phase 6d (ssdeckmacrocore.h CONTRACT): Tier B's own line - the merged macro-puff bodies
    // placed this build (already counted inside mPuffsPlaced above; broken out here so the crossfade band and the
    // body count can be read against each other), and the tinted macro grid renderDeckLod draws beyond TIER_B_M.
    spec.mKeys.push_back({ RING_TIER_B, llformat("Tier B macro bodies  %d  (merge from  %.0f m, blend  %.0f m)",
                                                  d.mTierBCount, SSDeckMacro::TIER_B_M, SSDeckMacro::TIER_BLEND_M), true });
    spec.mKeys.push_back({ TEXT_DIM, "tile tint: a distance-only diagram, not the builder's own cell gate", false });
}

// V4's legend: the drive scale (SSVirga::drive, presence ramp) the cell outlines are tinted by, the qualifying
// count against SSVirga::MAX_SHAFTS and how many the hashed trim left out, the fall-tilt line's own colour (the
// deck-base rail's blue, shared with V1), and the handoff ring/band. mActive false reads as "off" rather than
// showing a stale snapshot from a build where Distant Rain was on.
void SSAtmoLegendView::buildVirgaSpec(Spec& spec)
{
    const SSAtmoInfoView::VirgaData d = SSAtmoInfoView::virgaData();
    spec.mTitle = "V4  PRECIP & VIRGA";
    if (!d.mValid)
    {
        spec.mSubtitle = "no weather cube applied";
        return;
    }
    if (!d.mActive)
    {
        spec.mSubtitle = "Distant Rain off (Weather Influence), or this deck is not the storm-coupled one";
        return;
    }
    spec.mSubtitle = llformat("%d qualifying  kept %d / %d  trimmed %d  r2 %.0f m%s", d.mCandidates, d.mKept,
                              SSVirga::MAX_SHAFTS, d.mTrimmed, d.mR2, d.mDeckBuilt ? "" : "  (deck not built)");
    spec.mRampLabel = "shaft drive (precip x presence x tower)";
    spec.mRampMin = "0";
    spec.mRampMax = "1";
    spec.mRamp = &rampGrey;

    spec.mKeys.push_back({ TEXT_NORMAL, "cell outline: bright = kept, dim = trimmed by MAX_SHAFTS", true });
    spec.mKeys.push_back({ RAIL_BASE, "fall-tilt: deck base entry -> landing (precip's own wind tilt, kept cells)", true });
    spec.mKeys.push_back({ VIRGA_HANDOFF, llformat("handoff ring  r2 x %.1f = %.0f m (particle rain sheets)", SSVirga::HANDOFF_SKIP, d.mHandoffRadius), true });
    spec.mKeys.push_back({ LLColor4(VIRGA_HANDOFF.mV[0], VIRGA_HANDOFF.mV[1], VIRGA_HANDOFF.mV[2], 0.25f),
                           llformat("handoff band  +%.0f m to full shaft alpha", SSVirga::HANDOFF_BAND_M), true });
    spec.mKeys.push_back({ TEXT_DIM, llformat("qualify threshold %.2f / Distant Rain strength", SSVirga::THRESHOLD), false });
}

// V5's legend: one swatch per curve, split across the chart's two lanes (top: the authored day-cycle curves;
// bottom: the derived gates), the "now" cursor, and the two cue-marker colours (storm cue vs precipitation cue).
void SSAtmoLegendView::buildWeatherCubeSpec(Spec& spec)
{
    const SSAtmoInfoView::WeatherCubeData d = SSAtmoInfoView::weatherCubeData();
    spec.mTitle = "V5  WEATHER CUBE";
    if (!d.mValid)
    {
        spec.mSubtitle = "no weather cube applied";
        return;
    }
    spec.mSubtitle = llformat("track '%s'  day length %.0f min  %d cues", d.mTrackName.c_str(), d.mDayLengthS / 60.0, (S32)d.mCues.size());

    spec.mKeys.push_back({ CUBE_MOISTURE, "top lane: moisture", true });
    spec.mKeys.push_back({ CUBE_CONVECTION, "top lane: convection", true });
    spec.mKeys.push_back({ CUBE_TEMP, "top lane: temperature C (own scale)", true });
    spec.mKeys.push_back({ CUBE_WIND, "top lane: wind speed (heading in the tooltip readout)", true });
    spec.mKeys.push_back({ CUBE_SHEAR, "top lane: shear strength (veer deg on its own scale)", true });
    spec.mKeys.push_back({ CUBE_CONSOLIDATE, "bottom lane: storm consolidation (SSWindProfile::consolidation)", true });
    spec.mKeys.push_back({ CUBE_GLOOM, "bottom lane: puff gloom (1 = fair-weather albedo, 0 = darkest)", true });
    spec.mKeys.push_back({ CUBE_ANVIL, "bottom lane: anvil ramp (deck-wide)", true });
    spec.mKeys.push_back({ CUBE_LIGHTNING, "bottom lane: lightning gate (intensity, 0 = no strikes)", true });
    spec.mKeys.push_back({ CUBE_STORM_SCORE, "bottom lane: storm spawn score at the anchor's own lattice cell", true });
    spec.mKeys.push_back({ CUBE_NOW, "now cursor (SSAtmoEnvApplier::appliedPhase)", true });
    spec.mKeys.push_back({ CUBE_CUE_STORM, "cue marker: authored mStormOverride keyframe", true });
    spec.mKeys.push_back({ CUBE_CUE_PRECIP, "cue marker: authored mPrecipitationOverride keyframe", true });
}

void SSAtmoLegendView::draw()
{
    const U32 mode = SSAtmoInfoView::mode();
    if (mode == MODE_OFF) return;

    Spec spec;
    switch (mode)
    {
        case MODE_WIND_PROFILE: buildWindProfileSpec(spec); break;
        case MODE_STORM_CELLS:  buildStormCellsSpec(spec); break;
        case MODE_DECK_LOD:     buildDeckLodSpec(spec); break;
        case MODE_PRECIP_VIRGA: buildVirgaSpec(spec); break;
        case MODE_WEATHER_CUBE: buildWeatherCubeSpec(spec); break;
        default:
            spec.mTitle = llformat("INFO VIEW %u", mode);
            spec.mSubtitle = "not implemented yet";
            break;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;

    const S32 lh = font->getLineHeight();
    const S32 bar_w = 180;
    const S32 bar_h = 10;
    const S32 swatch = 10;

    // Measure.
    S32 widest = llmax(font->getWidth(spec.mTitle), font->getWidth(spec.mSubtitle));
    S32 needed_h = PAD * 2 + lh * 2;
    if (spec.mRamp)
    {
        widest = llmax(widest, bar_w);
        widest = llmax(widest, font->getWidth(spec.mRampLabel));
        needed_h += lh + bar_h + 2 + lh + 4;
    }
    for (const KeyRow& k : spec.mKeys)
    {
        widest = llmax(widest, swatch + 6 + font->getWidth(k.mLabel));
        needed_h += lh;
    }
    const S32 needed_w = widest + PAD * 2;

    const LLRect drawn = getRect();
    const S32 box_top = needed_h; // anchored at the bottom, grows upward

    gl_rect_2d(0, box_top, needed_w, 0, LLColor4(0.f, 0.f, 0.f, 0.55f));

    S32 y = box_top - PAD;
    font->renderUTF8(spec.mTitle, 0, PAD, y, TEXT_NORMAL, LLFontGL::LEFT, LLFontGL::TOP);
    y -= lh;
    font->renderUTF8(spec.mSubtitle, 0, PAD, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP);
    y -= lh;

    if (spec.mRamp)
    {
        y -= 2;
        font->renderUTF8(spec.mRampLabel, 0, PAD, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP);
        y -= lh;
        const S32 strips = 32;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (S32 i = 0; i < strips; ++i)
        {
            const S32 x0 = PAD + (bar_w * i) / strips;
            const S32 x1 = PAD + (bar_w * (i + 1)) / strips;
            gl_rect_2d(x0, y, x1, y - bar_h, spec.mRamp((F32)i / (F32)(strips - 1)));
        }
        y -= bar_h + 2;
        font->renderUTF8(spec.mRampMin, 0, PAD, y, TEXT_DIM, LLFontGL::LEFT, LLFontGL::TOP);
        font->renderUTF8(spec.mRampMax, 0, PAD + bar_w, y, TEXT_DIM, LLFontGL::RIGHT, LLFontGL::TOP);
        y -= lh + 2;
    }

    for (const KeyRow& k : spec.mKeys)
    {
        const S32 sy = y - (lh - swatch) / 2;
        if (k.mSwatchIsLine)
        {
            gl_line_2d(PAD, sy - swatch / 2, PAD + swatch, sy - swatch / 2, k.mColor);
        }
        else
        {
            gl_rect_2d(PAD, sy, PAD + swatch, sy - swatch, k.mColor);
        }
        font->renderUTF8(k.mLabel, 0, PAD + swatch + 6, y, TEXT_NORMAL, LLFontGL::LEFT, LLFontGL::TOP);
        y -= lh;
    }

    // Sized to content, growing up and right from the bottom-left corner the debug view docked it at.
    if (drawn.getWidth() != needed_w || drawn.getHeight() != needed_h)
    {
        LLRect r = drawn;
        r.mRight = r.mLeft + needed_w;
        r.mTop = r.mBottom + needed_h;
        setRect(r);
    }
}

// ---------------------------------------------------------------------------
// SSAtmoGraphView
// ---------------------------------------------------------------------------

SSAtmoGraphView::SSAtmoGraphView(const Params& p)
:   LLView(p)
{
}

void SSAtmoGraphView::polyline(const std::vector<std::pair<S32, S32> >& pts, const LLColor4& color, bool dashed)
{
    if (pts.size() < 2) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(color.mV);
    if (dashed)
    {
        gGL.begin(LLRender::LINES);
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
        {
            gGL.vertex2i(pts[i].first, pts[i].second);
            gGL.vertex2i(pts[i + 1].first, pts[i + 1].second);
        }
        gGL.end();
        return;
    }
    gGL.begin(LLRender::LINE_STRIP);
    for (const auto& p : pts)
    {
        gGL.vertex2i(p.first, p.second);
    }
    gGL.end();
}

void SSAtmoGraphView::text(const std::string& s, S32 x, S32 y, const LLColor4& color, bool right_align)
{
    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    font->renderUTF8(s, 0, x, y, color, right_align ? LLFontGL::RIGHT : LLFontGL::LEFT, LLFontGL::TOP);
}

void SSAtmoGraphView::drawMessage(const std::string& msg)
{
    const LLRect r = getLocalRect();
    text(msg, PAD, r.getHeight() - PAD, TEXT_DIM);
}

// <SS:Nexii> CHART (design section 6.2): floating over the world on the debug HUD now, rather than sitting inside
// the Views panel's own tab, so there is no "pick a view above" placeholder to show any more when off, and no
// "no chart yet" placeholder box to float uselessly over the world for a mode the dispatch below does not handle
// (currently V4 Precip & Virga) - both cases now draw nothing at all, checked against this SAME switch (not a
// separately maintained mode list) so a future mode gaining a drawX() case picks up a chart automatically.
void SSAtmoGraphView::draw()
{
    const U32 mode = SSAtmoInfoView::mode();
    if (mode == MODE_OFF)
    {
        return;
    }

    // Docked to the legend's right edge, bottom-aligned; re-read every frame since the legend's own width tracks
    // its content (SSAtmoLegendView::draw) and can change from one frame to the next as the active spec's row
    // count changes. FOLLOWS_BOTTOM|FOLLOWS_LEFT (set once in SSAtmoInfoView::attach) keeps this pair anchored
    // together through a window resize the same way the legend anchors itself; this only chases the legend's own
    // content-driven width changes, which a follows flag cannot express. [interaction: SSAtmoLegendView]
    if (LLView* legend = sLegendHandle.get())
    {
        const LLRect legend_rect = legend->getRect();
        const LLRect cur = getRect();
        const S32 want_left = legend_rect.mRight + CHART_GAP;
        const S32 want_bottom = legend_rect.mBottom;
        if (cur.mLeft != want_left || cur.mBottom != want_bottom)
        {
            LLRect r = cur;
            r.mLeft = want_left;
            r.mRight = want_left + CHART_W;
            r.mBottom = want_bottom;
            r.mTop = want_bottom + CHART_H;
            setRect(r);
        }
    }

    switch (mode)
    {
        case MODE_WIND_PROFILE:
        case MODE_STORM_CELLS:
        case MODE_DECK_LOD:
        case MODE_WEATHER_CUBE:
            break;
        default:
            return; // this mode has no chart (currently V4 Precip & Virga) - draw nothing, not even the background quad
    }

    const LLRect r = getLocalRect();
    gl_rect_2d(0, r.getHeight(), r.getWidth(), 0, LLColor4(0.f, 0.f, 0.f, 0.35f));

    switch (mode)
    {
        case MODE_WIND_PROFILE: drawWindProfile(); break;
        case MODE_STORM_CELLS:  drawStormCells(); break;
        case MODE_DECK_LOD:     drawDeckLod(); break;
        case MODE_WEATHER_CUBE: drawWeatherCube(); break;
        default: break;
    }

    LLView::draw();
}

// The boundary-layer curve: altitude AGL on Y (0 -> cirrus), speed on X, sampled from windAt(z) on the ground-biased ladder; rails at the load-bearing altitudes with live speed and heading; the flowmap-exponent and gradient-setting curves alongside for comparison; and the hodograph inset - wind vector tips per altitude joined into the veer curve.
void SSAtmoGraphView::drawWindProfile()
{
    const SSAtmoInfoView::WindProfileData d = SSAtmoInfoView::windProfileData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Wind Profile: no weather cube applied - nothing to profile.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    ChartBox box;
    box.left   = 58;
    box.bottom = 2 * lh + 8;
    box.w      = W - box.left - 8;
    box.h      = H - box.bottom - (lh + 6);
    if (box.w < 60 || box.h < 60)
    {
        drawMessage("Wind Profile: widen the floater to see the chart.");
        return;
    }

    const F32 top = d.mTopAgl;
    const F32 vmax = d.mMaxSpeed;

    // Title.
    text(llformat("WIND PROFILE  altitude AGL vs speed  ground z %.0f  cirrus %.0f m AGL", d.mGroundZ, top),
         PAD, H - 2, TEXT_NORMAL);

    // Axes.
    gl_line_2d(box.left, box.bottom, box.left, box.bottom + box.h, AXIS);
    gl_line_2d(box.left, box.bottom, box.left + box.w, box.bottom, AXIS);

    // Speed grid: five divisions of the nice ceiling.
    for (S32 i = 0; i <= 5; ++i)
    {
        const F32 v = vmax * (F32)i / 5.f;
        const S32 x = chartX(v, vmax, box);
        if (i > 0) gl_line_2d(x, box.bottom, x, box.bottom + box.h, GRID);
        text(llformat("%.0f", v), x, box.bottom - 2, TEXT_DIM, i == 5);
    }
    text("m/s", box.left + box.w, box.bottom - 2 - lh, TEXT_DIM, true);

    // Altitude grid: 1000 m (500 m on a low ceiling).
    const F32 zstep = (top <= 3000.f) ? 500.f : 1000.f;
    for (F32 z = zstep; z < top; z += zstep)
    {
        const S32 y = chartY(z, top, box);
        gl_line_2d(box.left, y, box.left + box.w, y, GRID);
        text(llformat("%.0f", z), box.left - 4, y + lh / 2, TEXT_DIM, true);
    }
    text("0 m", box.left - 4, box.bottom + lh / 2, TEXT_DIM, true);

    // Curves.
    static LLCachedControl<bool> compare(gSavedSettings, "SSAtmoInfoViewCompare", true);
    auto curve = [&](const SSWindProfile::Params& p, const LLColor4& color, bool dashed)
    {
        std::vector<std::pair<S32, S32> > pts;
        pts.reserve(CURVE_SAMPLES);
        for (S32 i = 0; i < CURVE_SAMPLES; ++i)
        {
            const F32 agl = altitudeLadder(top, CURVE_SAMPLES, i);
            const F32 v = speedOf(SSWindProfile::windAt(agl, p));
            pts.emplace_back(chartX(v, vmax, box), chartY(agl, top, box));
        }
        polyline(pts, color, dashed);
    };
    if (compare)
    {
        SSWindProfile::Params flow = d.mParams;
        flow.mExponent = d.mFlowAlpha;
        curve(flow, CURVE_FLOW, true);
        SSWindProfile::Params grad = d.mParams;
        grad.mExponent = d.mGradientSetting;
        curve(grad, CURVE_GRAD, true);
    }
    curve(d.mParams, CURVE_LIVE, false);

    // Rails with live values. Labels alternate above/below when two rails crowd.
    std::vector<Rail> rails;
    collectRails(d, rails);
    S32 last_label_y = -10000;
    for (const Rail& rail : rails)
    {
        if (!rail.mPresent)
        {
            continue;
        }
        if (rail.mAgl < 0.f || rail.mAgl > top) continue;
        const S32 y = chartY(rail.mAgl, top, box);
        gl_line_2d(box.left, y, box.left + box.w, y, *rail.mColor);
        const SSWindProfile::Vec2 w = SSWindProfile::windAt(rail.mAgl, d.mParams);
        const std::string label = llformat("%s  %.0f m  %.1f m/s  %03.0f", rail.mLabel, rail.mAgl, speedOf(w), headingOfVec(w.x, w.y));
        S32 ly = y + lh + 1; // TOP-aligned text sitting just above the rail
        if (llabs(ly - last_label_y) < lh)
        {
            ly = y - 1;      // crowd: hang it below instead
        }
        text(label, box.left + box.w - 4, ly, *rail.mColor, true);
        last_label_y = ly;
    }
    if (!d.mDeckBuilt)
    {
        text("deck: not built", box.left + 6, box.bottom + box.h - 2, RAIL_BASE);
    }

    // Comparison key, bottom-right inside the chart.
    if (compare)
    {
        S32 ky = box.bottom + 3 * lh + 4;
        text(llformat("live  exp %.2f (cube)", d.mParams.mExponent), box.left + box.w - 4, ky, CURVE_LIVE, true); ky -= lh;
        text(llformat("flowmap region exp %.2f%s", d.mFlowAlpha, d.mFlowSolved ? "" : " (fallback)"), box.left + box.w - 4, ky, CURVE_FLOW, true); ky -= lh;
        text(llformat("gradient setting %.2f", d.mGradientSetting), box.left + box.w - 4, ky, CURVE_GRAD, true);
    }

    // Hodograph inset, top-right of the chart.
    const S32 side = llmin(box.w / 3, box.h / 2);
    if (side >= 70)
    {
        const S32 il = box.left + box.w - side - 6;
        const S32 ib = box.bottom + box.h - side - 6;
        const S32 cx = il + side / 2;
        const S32 cy = ib + side / 2;
        const S32 radius = side / 2 - lh;

        gl_rect_2d(il, ib + side, il + side, ib, LLColor4(0.f, 0.f, 0.f, 0.55f));
        gl_rect_2d(il, ib + side, il + side, ib, GRID, false);
        gl_line_2d(cx - radius, cy, cx + radius, cy, GRID);
        gl_line_2d(cx, cy - radius, cx, cy + radius, GRID);

        std::vector<std::pair<S32, S32> > ring;
        for (S32 i = 0; i <= 32; ++i)
        {
            const F32 a = (F32)i / 32.f * 6.2831853f;
            ring.emplace_back(cx + (S32)floor(std::sin(a) * (F32)radius + 0.5f), cy + (S32)floor(std::cos(a) * (F32)radius + 0.5f));
        }
        polyline(ring, AXIS, false);
        text("N", cx, cy + radius + lh, TEXT_DIM);
        text("E", cx + radius + 2, cy + lh / 2, TEXT_DIM);

        std::vector<std::pair<S32, S32> > hodo;
        hodo.reserve(CURVE_SAMPLES);
        for (S32 i = 0; i < CURVE_SAMPLES; ++i)
        {
            const F32 agl = altitudeLadder(top, CURVE_SAMPLES, i);
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
            const PixelPoint p = hodoPoint(w.x, w.y, vmax, cx, cy, radius);
            hodo.emplace_back(p.x, p.y);
        }
        polyline(hodo, CURVE_LIVE, false);

        auto dot = [&](F32 agl, const LLColor4& c)
        {
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(agl, d.mParams);
            const PixelPoint p = hodoPoint(w.x, w.y, vmax, cx, cy, radius);
            gl_rect_2d(p.x - 2, p.y + 2, p.x + 2, p.y - 2, c);
        };
        dot(SSWindProfile::REF_M, RAIL_REF);
        dot(SSWindProfile::BL_TOP_M, RAIL_BL);
        if (d.mDeckBuilt)
        {
            dot(d.mBaseZ - d.mGroundZ, RAIL_BASE);
            dot(d.mLidZ - d.mGroundZ, RAIL_LID);
        }
        dot(top, RAIL_CIRRUS);

        text(llformat("hodograph  ring %.0f m/s", vmax), il + 3, ib + side - 2, TEXT_DIM);
    }
}

// V2's chart: the active-cell timeline. One row per cell (hero pinned on top, the rest in id order), a bar across the five lifecycle stages in stage colour with the "now" cursor at age01, the id, supercell flag, age and lifetime; the window's candidate counts in the header. Read-only: it draws the scheduler's frame.
void SSAtmoGraphView::drawStormCells()
{
    const SSAtmoInfoView::StormCellsData d = SSAtmoInfoView::stormCellsData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Storm Cells: scheduler not running (no track, region or asset).");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    text(llformat("STORM CELL TIMELINE  %d candidates in window  %d alive  %d spawned  %d super  %d eligible", d.mCandidates, d.mAlive, d.mSpawned,
                  d.mSupercells, d.mTornadoEligible), PAD, H - 2, TEXT_NORMAL);

    const S32 label_w = 150;
    const S32 right_w = 130;
    const S32 bar_left = PAD + label_w;
    const S32 bar_w = W - bar_left - right_w - PAD;
    if (bar_w < 80 || H < 4 * lh)
    {
        drawMessage("Storm Cells: widen the floater to see the timeline.");
        return;
    }

    // Stage scale under the header.
    S32 y = H - 2 - lh - 2;
    static const F32 kBounds[STAGE_COUNT + 1] = { 0.f, 0.15f, 0.30f, 0.55f, 0.80f, 1.f };
    for (S32 s = 0; s < STAGE_COUNT; ++s)
    {
        const S32 x0 = bar_left + (S32)floor(kBounds[s] * (F32)bar_w + 0.5f);
        text(stageLabel(s), x0 + 2, y, toColor(stageColor(s), 1.f));
    }
    y -= lh + 2;

    if (d.mCells.empty())
    {
        text("no active cells", PAD, y, TEXT_DIM);
        if (!d.mHaveHero)
        {
            y -= lh;
            for (const std::string& f : d.mWhyNot)
            {
                text("why not: " + f, PAD, y, TEXT_DIM);
                y -= lh;
                if (y < lh) break;
            }
        }
        return;
    }

    // Row order: hero first, then id order as delivered.
    std::vector<const SSAtmoInfoView::StormCellsData::Cell*> rows;
    rows.reserve(d.mCells.size());
    for (const auto& c : d.mCells) if (c.mIsHero) rows.push_back(&c);
    for (const auto& c : d.mCells) if (!c.mIsHero) rows.push_back(&c);

    const S32 row_h = lh + 4;
    const S32 bar_h = lh - 2;
    for (const auto* c : rows)
    {
        if (y - row_h < 0)
        {
            text(llformat("... %d more", (S32)rows.size()), PAD, y, TEXT_DIM);
            break;
        }
        const S32 bar_top = y - 1;
        const S32 bar_bottom = bar_top - bar_h;

        // Stage bands, bright up to now, dim beyond it.
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (S32 s = 0; s < STAGE_COUNT; ++s)
        {
            const S32 x0 = bar_left + (S32)floor(kBounds[s] * (F32)bar_w + 0.5f);
            const S32 x1 = bar_left + (S32)floor(kBounds[s + 1] * (F32)bar_w + 0.5f);
            const S32 xn = bar_left + (S32)floor(llclamp(c->mAge01, kBounds[s], kBounds[s + 1]) * (F32)bar_w + 0.5f);
            const LLColor4 bright = toColor(stageColor(s), 0.9f);
            const LLColor4 dim = toColor(stageColor(s), 0.25f);
            if (xn > x0) gl_rect_2d(x0, bar_top, xn, bar_bottom, bright);
            if (x1 > xn) gl_rect_2d(xn, bar_top, x1, bar_bottom, dim);
        }
        // Now cursor.
        const S32 xnow = bar_left + (S32)floor(llclamp(c->mAge01, 0.f, 1.f) * (F32)bar_w + 0.5f);
        gl_line_2d(xnow, bar_top + 2, xnow, bar_bottom - 2, TEXT_NORMAL);

        const LLColor4 label_c = c->mIsForced ? FORCED_OUTLINE : (c->mIsHero ? HERO_ORIGIN : (c->mSupercell ? toColor(rotationRamp(c->mRotation), 1.f) : TEXT_NORMAL));
        text(llformat("%s%s", c->mIsHero ? "H " : (c->mSupercell ? "S " : "  "), shortId(c->mId).c_str()), PAD, y, label_c);
        const F64 age = d.mNow - c->mBirthTime;
        // <SS:Nexii> SQUALL/FORCED: the same tag renderStormCells' own labels carry, so the timeline and the
        // in-world layer read the same cell the same way.
        const std::string tag = c->mIsForced ? "  FORCED" : (c->mLineId != 0 ? llformat("  line %s", shortId(c->mLineId).c_str()) : std::string());
        text(llformat("%.0f/%.0f min  I %.2f%s", age / 60.0, c->mLifetimeS / 60.f, c->mIntensity, tag.c_str()), W - PAD, y, TEXT_DIM, true);
        y -= row_h;
    }
}

// V3's chart: SSDeckLod::keepFrac plotted against camera distance out to DECK_EDGE_M, with the five LOD rails as
// vertical lines in ring colour and the header carrying the same placed/budget/predicted figures the legend
// shows - a pure function of the core, so this curve is the SAME one the tile tint and the in-world rings draw
// from, just read off the axis directly rather than eyeballed off a grey swatch.
void SSAtmoGraphView::drawDeckLod()
{
    const SSAtmoInfoView::DeckLodData d = SSAtmoInfoView::deckLodData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mDeckBuilt)
    {
        drawMessage("Deck LOD: no cloud deck built.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    ChartBox box;
    box.left   = 58;
    box.bottom = 2 * lh + 8;
    box.w      = W - box.left - 8;
    box.h      = H - box.bottom - (lh + 6);
    if (box.w < 60 || box.h < 60)
    {
        drawMessage("Deck LOD: widen the floater to see the chart.");
        return;
    }

    const F32 dmax = SSDeckLod::DECK_EDGE_M;

    text(llformat("DECK LOD  keep fraction vs camera distance  placed %d / budget %d  dial %d  predicted %lld/%d cells",
                  d.mPuffsPlaced, d.mBudget, d.mPuffsPerCell, (long long)d.mLodPredicted, d.mCellsWalked),
         PAD, H - 2, TEXT_NORMAL);

    gl_line_2d(box.left, box.bottom, box.left, box.bottom + box.h, AXIS);
    gl_line_2d(box.left, box.bottom, box.left + box.w, box.bottom, AXIS);

    for (S32 i = 0; i <= 5; ++i)
    {
        const F32 dd = dmax * (F32)i / 5.f;
        const S32 x = box.left + (S32)floor((dd / dmax) * (F32)box.w + 0.5f);
        if (i > 0) gl_line_2d(x, box.bottom, x, box.bottom + box.h, GRID);
        text(llformat("%.0f", dd), x, box.bottom - 2, TEXT_DIM, i == 5);
    }
    text("m", box.left + box.w, box.bottom - 2 - lh, TEXT_DIM, true);

    for (S32 i = 0; i <= 4; ++i)
    {
        const F32 kf = (F32)i / 4.f;
        const S32 y = box.bottom + (S32)floor(kf * (F32)box.h + 0.5f);
        gl_line_2d(box.left, y, box.left + box.w, y, GRID);
        text(llformat("%.2f", kf), box.left - 4, y + lh / 2, TEXT_DIM, true);
    }

    std::vector<std::pair<S32, S32> > pts;
    pts.reserve(CURVE_SAMPLES);
    for (S32 i = 0; i < CURVE_SAMPLES; ++i)
    {
        const F32 dd = dmax * (F32)i / (F32)(CURVE_SAMPLES - 1);
        const F32 kf = SSDeckLod::keepFrac(dd);
        const S32 x = box.left + (S32)floor((dd / dmax) * (F32)box.w + 0.5f);
        const S32 y = box.bottom + (S32)floor(kf * (F32)box.h + 0.5f);
        pts.emplace_back(x, y);
    }
    polyline(pts, CURVE_LIVE, false);

    auto rail = [&](F32 dd, const char* label, const LLColor4& c)
    {
        const S32 x = box.left + (S32)floor((dd / dmax) * (F32)box.w + 0.5f);
        gl_line_2d(x, box.bottom, x, box.bottom + box.h, c);
        text(label, x + 2, box.bottom + box.h - 2, c);
    };
    rail(SSDeckLod::SUBS_FULL_M, "full", RING_SUBS_FULL);
    rail(SSDeckLod::SUBS_TWO_M, "2->1", RING_SUBS_TWO);
    rail(SSDeckLod::THIN_START_M, "thin", RING_THIN_START);
    rail(SSDeckLod::FIELD_FADE_START_M, "fade", RING_FADE_START);
}

// V5's chart: the day-cycle cube in two lanes (SSAtmoInfoViewCore::cubeLaneBox) - the top lane the AUTHORED curves
// (moisture, convection, temperature, wind speed/heading, shear strength/veer, every one of them read through the
// SAME resolvers the sky itself samples - SSAtmoEnvWeatherResolver::resolve, weatherCubeData()'s own comment), the
// bottom lane the DERIVED gates (storm consolidation, puff gloom, anvil ramp, the lightning gate, and the storm
// spawn score at the anchor's own lattice cell) - with the "now" cursor spanning both lanes and a marker at every
// authored override cue (mStormOverride / mPrecipitationOverride). Magnitude curves in the top lane are each
// normalised to their own domain (moisture/convection/shear are already unit; temperature and wind speed get a
// nice-ceiling domain over this cube's own samples, SSAtmoInfoViewCore::niceMax/unitOf, the SAME helpers V1's
// chart uses); a magnitude's paired ANGLE (heading for wind, veer for shear) draws DASHED in the same colour -
// solid means magnitude, dashed means the angle that rides with it. Every bottom-lane curve is already unit
// (0..1), so it shares the lane's axis directly with no per-curve domain.
void SSAtmoGraphView::drawWeatherCube()
{
    const SSAtmoInfoView::WeatherCubeData d = SSAtmoInfoView::weatherCubeData();
    const LLRect r = getLocalRect();
    const S32 W = r.getWidth();
    const S32 H = r.getHeight();

    if (!d.mValid)
    {
        drawMessage("Weather Cube: no weather cube applied - nothing to chart.");
        return;
    }

    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;
    const S32 lh = font->getLineHeight();

    text(llformat("WEATHER CUBE  track '%s'  day length %.0f min  now phase %.3f  %d cue%s", d.mTrackName.c_str(),
                  d.mDayLengthS / 60.0, d.mNowPhase, (S32)d.mCues.size(), d.mCues.size() == 1 ? "" : "s"),
         PAD, H - 2, TEXT_NORMAL);

    ChartBox outer;
    outer.left   = 58;
    outer.bottom = 2 * lh + 8;
    outer.w      = W - outer.left - 8;
    outer.h      = H - outer.bottom - (lh + 6);
    if (outer.w < 80 || outer.h < 100 || d.mSamples.empty())
    {
        drawMessage("Weather Cube: widen the floater to see the chart.");
        return;
    }

    const ChartBox top_box = cubeLaneBox(outer, 0, 2, 16);
    const ChartBox bot_box = cubeLaneBox(outer, 1, 2, 16);

    // Per-cube domains for the top lane's two non-unit magnitudes, over THIS cube's own samples - the same
    // "nice ceiling" niceMax gives V1's wind-speed axis, and a rounded-out temperature span so a near-flat day
    // still gets a readable axis rather than one that hugs a single degree.
    F32 temp_min = d.mSamples[0].mTemperatureC, temp_max = d.mSamples[0].mTemperatureC, wind_peak = 0.f;
    for (const auto& s : d.mSamples)
    {
        temp_min = llmin(temp_min, s.mTemperatureC);
        temp_max = llmax(temp_max, s.mTemperatureC);
        wind_peak = llmax(wind_peak, s.mWindSpeed);
    }
    temp_min = llmin(temp_min, 0.f) - 5.f;
    temp_max = llmax(temp_max, temp_min + 10.f) + 5.f;
    const F32 wind_ceiling = niceMax(llmax(wind_peak, 1.f));

    // A generic lane frame: axes, four quarter-phase gridlines, and the 0/1 (or domain-end) value labels.
    auto laneFrame = [&](const ChartBox& box, const char* title, const char* lo_label, const char* hi_label)
    {
        text(title, box.left, box.bottom + box.h - 2, TEXT_NORMAL);
        gl_line_2d(box.left, box.bottom, box.left, box.bottom + box.h, AXIS);
        gl_line_2d(box.left, box.bottom, box.left + box.w, box.bottom, AXIS);
        for (S32 q = 1; q < 4; ++q)
        {
            const S32 x = cubePhaseX((F64)q / 4.0, box);
            gl_line_2d(x, box.bottom, x, box.bottom + box.h, GRID);
        }
        text(lo_label, box.left - 4, box.bottom + lh / 2, TEXT_DIM, true);
        text(hi_label, box.left - 4, box.bottom + box.h + lh / 2, TEXT_DIM, true);
    };
    laneFrame(top_box, "day-cycle curves (solid = magnitude, dashed = paired angle)", "0", "1 / max");
    laneFrame(bot_box, "derived gates", "0", "1");

    // Plots a per-sample unit-space series (already mapped to [0,1] by the caller) as a polyline in `box`.
    auto plot = [&](const ChartBox& box, const std::vector<F32>& unit_values, const LLColor4& color, bool dashed)
    {
        std::vector<std::pair<S32, S32> > pts;
        pts.reserve(d.mSamples.size());
        for (size_t i = 0; i < d.mSamples.size(); ++i)
        {
            pts.emplace_back(cubePhaseX((F64)d.mSamples[i].mPhase, box), chartY(unit_values[i], 1.f, box));
        }
        polyline(pts, color, dashed);
    };

    const size_t n = d.mSamples.size();
    std::vector<F32> moisture(n), convection(n), temp(n), wind_speed(n), wind_heading(n), shear(n), veer(n);
    std::vector<F32> consolidation(n), gloom(n), anvil(n), lightning(n), storm_score(n);
    for (size_t i = 0; i < n; ++i)
    {
        const auto& s = d.mSamples[i];
        moisture[i]      = s.mMoisture;
        convection[i]    = s.mConvection;
        temp[i]           = unitOf(s.mTemperatureC - temp_min, temp_max - temp_min);
        wind_speed[i]     = unitOf(s.mWindSpeed, wind_ceiling);
        wind_heading[i]   = unitOf(s.mWindHeading, 360.f);
        shear[i]          = llclamp(s.mShearStrength, 0.f, 1.f);
        veer[i]           = unitOf(s.mVeerDeg + 180.f, 360.f);
        consolidation[i]  = s.mConsolidation;
        gloom[i]          = s.mGloom;
        anvil[i]          = s.mAnvilRamp;
        lightning[i]      = s.mLightningIntensity;
        storm_score[i]    = s.mStormScore;
    }

    plot(top_box, moisture, CUBE_MOISTURE, false);
    plot(top_box, convection, CUBE_CONVECTION, false);
    plot(top_box, temp, CUBE_TEMP, false);
    plot(top_box, wind_speed, CUBE_WIND, false);
    plot(top_box, wind_heading, CUBE_WIND, true);
    plot(top_box, shear, CUBE_SHEAR, false);
    plot(top_box, veer, CUBE_SHEAR, true);

    plot(bot_box, consolidation, CUBE_CONSOLIDATE, false);
    plot(bot_box, gloom, CUBE_GLOOM, false);
    plot(bot_box, anvil, CUBE_ANVIL, false);
    plot(bot_box, lightning, CUBE_LIGHTNING, false);
    if (d.mHaveStormScore)
    {
        plot(bot_box, storm_score, CUBE_STORM_SCORE, false);
    }

    // Phase-of-cycle labels under the bottom lane only (the two lanes share one phase axis).
    for (S32 q = 0; q <= 4; ++q)
    {
        const F32 phase = (F32)q / 4.f;
        const S32 x = cubePhaseX((F64)phase, bot_box);
        text(llformat("%.2f", phase), x, bot_box.bottom - 2, TEXT_DIM, q == 4);
    }
    text("phase of cycle", outer.left + outer.w / 2, bot_box.bottom - 2 - lh, TEXT_DIM);

    // The "now" cursor, spanning both lanes.
    {
        const S32 x = cubePhaseX(d.mNowPhase, outer);
        gl_line_2d(x, outer.bottom, x, outer.bottom + outer.h, CUBE_NOW);
        text(llformat("now %.3f", d.mNowPhase), x + 3, outer.bottom + outer.h - 2, CUBE_NOW);
    }

    // Authored override cue markers, spanning both lanes, alternating label height so adjacent cues stay legible.
    S32 cue_row = 0;
    for (const auto& cue : d.mCues)
    {
        const LLColor4& c = cue.mIsStorm ? CUBE_CUE_STORM : CUBE_CUE_PRECIP;
        const S32 x = cubePhaseX(cue.mPhase, outer);
        gl_line_2d(x, outer.bottom, x, outer.bottom + outer.h, c);
        const S32 ly = outer.bottom + outer.h - lh - (cue_row % 3) * lh;
        text(cue.mLabel, x + 3, ly, c);
        ++cue_row;
    }
}
