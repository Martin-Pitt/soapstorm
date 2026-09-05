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
#include "ssatmomagic.h"
#include "ssvolcloud.h"
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

// Snapshot the flow-arrow mask on the way from off, then hold it where the active mode wants it; back to off restores the snapshot. V1 hands its near-ground layer to the flowmap arrows, so it wants them on.
void SSAtmoInfoView::onModeChanged(U32 previous, U32 now)
{
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

// ---------------------------------------------------------------------------
// The in-world layer: V1's wind mast
// ---------------------------------------------------------------------------

// A vertical stack of arrows from the track floor to the cirrus band, each rotated and scaled by windAt(z) and coloured by the speed ramp, so shear reads as the stack twisting with height. Stood a little ahead of the camera (SSAtmoInfoViewMastOffset, 0 for the camera column itself) and pulled through the cloud field's far squash so the upper rungs land beside the deck they describe. [interaction: SSVolCloud squashScale]
void SSAtmoInfoView::renderWorld()
{
    if (mode() != MODE_WIND_PROFILE) return;

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
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
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
// SSAtmoDimView
// ---------------------------------------------------------------------------

SSAtmoDimView::SSAtmoDimView(const Params& p)
:   LLView(p)
{
}

void SSAtmoDimView::draw()
{
    if (SSAtmoInfoView::mode() == MODE_OFF) return;

    static LLCachedControl<F32> dim_setting(gSavedSettings, "SSAtmoInfoViewDim", 0.55f);
    const F32 a = llclamp((F32)dim_setting, 0.f, 0.95f);
    if (a <= 0.f) return;

    const LLRect& r = getRect();
    gl_rect_2d(0, r.getHeight(), r.getWidth(), 0, LLColor4(0.02f, 0.03f, 0.05f, a));
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

void SSAtmoLegendView::draw()
{
    const U32 mode = SSAtmoInfoView::mode();
    if (mode == MODE_OFF) return;

    Spec spec;
    switch (mode)
    {
        case MODE_WIND_PROFILE: buildWindProfileSpec(spec); break;
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

void SSAtmoGraphView::draw()
{
    const LLRect r = getLocalRect();
    gl_rect_2d(0, r.getHeight(), r.getWidth(), 0, LLColor4(0.f, 0.f, 0.f, 0.35f));

    switch (SSAtmoInfoView::mode())
    {
        case MODE_OFF:          drawMessage("Info view off - pick a view above."); break;
        case MODE_WIND_PROFILE: drawWindProfile(); break;
        default:                drawMessage("This view has no chart yet."); break;
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
