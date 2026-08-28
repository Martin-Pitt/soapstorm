/**
 * @file ssstatsview.h
 * @brief Translucent overlay showing live Region Object Cache and Squeeze statistics
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_STATSVIEW_H
#define SS_STATSVIEW_H

#include "llview.h"

// A read-only overlay in the style of the texture console: translucent background, monospaced lines, no interaction. Toggled from Advanced > Consoles alongside the stock consoles rather than being a floater, because it wants to sit over the world while you move around rather than take focus.
//
// Everything it shows is read from counters the features already maintain, so having it open changes nothing about their behaviour.
class SSStatsView : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };

    SSStatsView(const Params& p);
    virtual ~SSStatsView();

    void draw() override;

private:
    // One accumulated line of output plus the colour it should be drawn in, so a section can be dimmed when its feature is switched off.
    void line(const std::string& text, bool dim = false);
    void blank();

    std::vector<std::pair<std::string, bool> > mLines;
};

extern SSStatsView* gSSStatsView;

#endif // SS_STATSVIEW_H
