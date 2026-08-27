/**
 * @file ssstatsview.cpp
 * @brief Translucent overlay showing live Region Object Cache and Squeeze statistics
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssstatsview.h"

#include "llfontgl.h"
#include "llgl.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llui.h"
#include "llviewercontrol.h"

#include "ssrocaux.h"
#include "ssroccache.h"
#include "ssrocledger.h"
#include "ssbc7serve.h"
#include "ssbc7store.h"
#include "ssrocghost.h"

SSStatsView* gSSStatsView = NULL;

static LLDefaultChildRegistry::Register<SSStatsView> r("ss_stats_view");

namespace
{
    const S32 SSSV_PAD = 6;

    std::string ssMB(U64 bytes)
    {
        return llformat("%.1f MB", (F64)bytes / (1024.0 * 1024.0));
    }
}

SSStatsView::SSStatsView(const Params& p)
:   LLView(p)
{
}

SSStatsView::~SSStatsView()
{
}

void SSStatsView::line(const std::string& text, bool dim)
{
    mLines.push_back(std::make_pair(text, dim));
}

void SSStatsView::blank()
{
    mLines.push_back(std::make_pair(std::string(), false));
}

void SSStatsView::draw()
{
    mLines.clear();

    // ---- Region Object Cache -------------------------------------------------
    const bool roc_on = SSROCStore::enabled();

    line("REGION OBJECT CACHE", !roc_on);
    if (!roc_on)
    {
        line("  off  (Preferences > Soapstorm > Region Cache)", true);
    }
    else
    {
        // <SS:Nexii> The painted count leads because it is the only line that answers the question a user actually has, which is "did this do anything for me". Everything below it is plumbing: how the store, the terrain and the ledger are getting on is diagnostic detail that matters when the answer is no.
        //
        // "Painted" is deliberately the sum of validated and rebuilt rather than two numbers. The distinction between making an entry the simulator already sent visible early, and inventing one the .slc no longer held, matters enormously to the implementation and not at all to someone looking at an overlay to see whether their region came back faster.
        if (SSROCGhostMgr::instanceExists())
        {
            const SSROCGhostMgr::Metrics& g = SSROCGhostMgr::instance().metrics();
            const U32 painted = g.mValidated + g.mCreated;

            // <SS:Nexii> The reason is read from the outcome counters, never inferred from mRegionsArmed. That counter increments at the END of armRegion, after every early return, so a region entered with no cache file never reaches it - which is why an earlier version of this overlay reported "no regions entered yet" to someone who had just walked through four of them. A count of zero attempts and a count of zero successes look identical from the outside and mean completely different things.
            U32 attempts = 0;
            for (U32 i = 0; i < SSROC_ARM_COUNT; ++i)
            {
                if (i != SSROC_ARM_PENDING) attempts += g.mOutcomes[i];
            }

            if (attempts == 0)
            {
                line("  painted  no region has been checked yet this session", true);
            }
            else if (painted == 0)
            {
                // Every one of these is a different problem with a different answer, and collapsing them into "nothing happened" is what let this feature look inert for days while working correctly.
                line(llformat("  painted  nothing yet, across %u region%s checked", attempts, attempts == 1 ? "" : "s"), true);

                const U32 no_file  = g.mOutcomes[SSROC_ARM_NO_FILE] + g.mOutcomes[SSROC_ARM_FILE_EMPTY];
                const U32 restart  = g.mOutcomes[SSROC_ARM_CACHEID_CHANGED];
                const U32 nothing  = g.mOutcomes[SSROC_ARM_PLAN_EMPTY];

                if (no_file)  line(llformat("           %u not visited before, so there was nothing remembered to paint", no_file), true);
                if (restart)  line(llformat("           %u restarted since your last visit, so what was remembered is no longer trustworthy", restart), true);
                if (nothing)  line(llformat("           %u remembered, but nothing in them was settled enough to paint yet", nothing), true);
            }
            else
            {
                // Confirmation is what turns "we drew something" into "we drew the right thing": the simulator later sent the real object and it matched. Anything painted and never confirmed expired quietly, which is a miss rather than a fault.
                const U32 pct = (painted > 0) ? (U32)((U64)g.mConfirmed * 100ull / painted) : 0;
                line(llformat("  painted  %u objects from memory across %u region%s",
                              painted, g.mRegionsArmed, g.mRegionsArmed == 1 ? "" : "s"));
                line(llformat("           %u confirmed correct (%u%%)   %u expired unconfirmed",
                              g.mConfirmed, pct, g.mExpired));

                // Only ever shown when non-zero. These are the failures that would matter - an object drawn where a different one now lives, or a whole region so contradicted that its memory was thrown away - and a row of zeroes every session would train the eye to skip exactly the line worth noticing.
                if (g.mIdReuse || g.mTrashed)
                {
                    line(llformat("           %u drawn wrong and removed   %u region%s discarded as unreliable",
                                  g.mIdReuse, g.mTrashed, g.mTrashed == 1 ? "" : "s"));
                }
                if (g.mRegionsModeB)
                {
                    line(llformat("           %u region%s skipped: restarted since the last visit",
                                  g.mRegionsModeB, g.mRegionsModeB == 1 ? "" : "s"), true);
                }
            }
        }

        if (SSROCStore::instanceExists())
        {
            const SSROCStore::Metrics& m = SSROCStore::instance().metrics();
            line(llformat("  store    loaded %u  missing %u  corrupt %u",
                          m.mLoadsOk.load(), m.mLoadsMissing.load(), m.mLoadsCorrupt.load()));
            line(llformat("           saved %u  failed %u  evicted %u  disk %s",
                          m.mSavesOk.load(), m.mSavesFailed.load(), m.mFilesEvicted.load(),
                          ssMB(SSROCStore::instance().diskBytesUsed()).c_str()));
            line(llformat("           read %s  written %s",
                          ssMB(m.mBytesRead.load()).c_str(), ssMB(m.mBytesWritten.load()).c_str()));
        }
        else
        {
            line("  store    not initialised yet", true);
        }

        if (SSROCAuxMgr::instanceExists())
        {
            const SSROCAuxMgr::Metrics& a = SSROCAuxMgr::instance().metrics();
            line(llformat("  terrain  applied %u  skipped %u   water %u   ground %u",
                          a.mTerrainApplied, a.mTerrainSkipped, a.mWaterApplied, a.mCompApplied));
            line(llformat("           regions loaded %u  new %u  captured %u",
                          a.mRegionsLoaded, a.mRegionsMissing, a.mCaptured));
        }

        if (SSROCLedger::instanceExists())
        {
            const SSROCLedger::Metrics& l = SSROCLedger::instance().metrics();
            line(llformat("  ledger   regions %u  sightings %u  records %u  blobless %u",
                          l.mRegionsTracked, l.mSightings, l.mRecordsCreated, l.mNoBlob));
            line(llformat("           promoted %u  sandbox regions %u  owner lookups %u",
                          l.mPromoted, l.mRegionsSandbox, l.mOwnerLookups));
        }
    }

    blank();

    // ---- Squeeze -------------------------------------------------------------
    const bool squeeze_on = LLImageGL::sSqueezeEnabled;
    const bool squeeze_usable = LLImageGL::canUseSqueeze();

    line("SQUEEZE  (GPU-compressed textures)", !squeeze_on);
    if (!squeeze_on)
    {
        line("  off  (Preferences > Soapstorm > Squeeze)", true);
    }
    else if (!squeeze_usable)
    {
        // Distinguishing these two is the whole point: "on but the card cannot" looks identical to "on and working" without it.
        line("  enabled, but this graphics card has no BC7 support - using the ordinary texture path", true);
    }
    else
    {
        // <SS:Nexii> The saving IS the feature, so it is the first line and it is phrased as a saving rather than as two sizes to subtract. Everything else about Squeeze - what is stored, what is permitted, how much texture memory is in use - is context for this number, and an overlay that made the reader compute it themselves was the reason this console kept being called unhelpful.
        const SSBC7ServeResidency r = ssBC7ServeResidencyNow();
        if (r.mTextures > 0 && r.mSavedBytes > 0)
        {
            const S64 without = r.mBC7Bytes + r.mSavedBytes;
            const F32 ratio   = (r.mBC7Bytes > 0) ? ((F32)without / (F32)r.mBC7Bytes) : 0.f;
            line(llformat("  SAVING   %s of video memory right now  (%.1fx smaller)",
                          ssMB((U64)r.mSavedBytes).c_str(), ratio));
            line(llformat("           %u textures using %s instead of %s",
                          r.mTextures, ssMB((U64)r.mBC7Bytes).c_str(), ssMB((U64)without).c_str()));
        }
        else
        {
            // Serving nothing is the expected state on a cold store rather than a fault, and saying so stops it reading as breakage while the store fills.
            line("  active   nothing served yet - textures are compressed as they are seen, and reused from the next sighting on", true);
        }
    }

    // <SS:Nexii> The store block is here because until now the overlay reported on every ROC subsystem and NOTHING about the tier that has written several gigabytes - the one part of the cache a user might reasonably want to watch was the one part with no readout at all.
    //
    // Held against its budget rather than shown as a bare size, because a number of megabytes says nothing without the ceiling it is heading for, and this tier's whole failure mode was growing unbounded while looking perfectly healthy by its own reckoning.
    if (squeeze_on && SSBC7Store::instanceExists() && SSBC7Store::instance().isInitialized())
    {
        SSBC7Store& store = SSBC7Store::instance();
        const U64 used   = store.allocatedBytes();
        const U64 budget = store.budgetBytes();
        const U32 pct    = budget ? (U32)llmin((U64)999, (U64)(used * 100ull / budget)) : 0;

        line(llformat("  stored   %u textures   %s of %s (%u%%)",
                      store.recordCount(), ssMB(used).c_str(), ssMB(budget).c_str(), pct));
    }

    line(llformat("  texture memory in use  %s", ssMB(LLImageGL::getTextureBytesAllocated()).c_str()));

    // ---- Layout and paint ----------------------------------------------------
    LLFontGL* font = LLFontGL::getFontMonospace();
    if (!font) return;

    const S32 line_height = font->getLineHeight();
    const S32 needed_h = (S32)mLines.size() * line_height + SSSV_PAD * 2;

    S32 widest = 0;
    for (const auto& l : mLines)
    {
        widest = llmax(widest, (S32)font->getWidth(l.first));
    }
    const S32 needed_w = widest + SSSV_PAD * 2;

    // Painted against the rect the parent has already translated to for this frame; the resize below only takes effect next frame, because moving our own rect first would leave this frame's text sitting off the box behind it.
    const LLRect drawn = getRect();
    const S32 box_top = drawn.getHeight();

    gl_rect_2d(0, box_top, needed_w, box_top - needed_h, LLColor4(0.f, 0.f, 0.f, 0.4f));

    static const LLColor4 normal(1.f, 1.f, 1.f, 1.f);
    static const LLColor4 dimmed(0.6f, 0.6f, 0.6f, 1.f);

    S32 y = box_top - SSSV_PAD;
    for (const auto& l : mLines)
    {
        if (!l.first.empty())
        {
            font->renderUTF8(l.first, 0, SSSV_PAD, y, l.second ? dimmed : normal,
                             LLFontGL::LEFT, LLFontGL::TOP);
        }
        y -= line_height;
    }

    // The overlay sizes itself to its content rather than to a fixed rect, so adding a counter later cannot silently clip the bottom line off. It only ever grows right and down from the top left corner the debug view anchored it at, so it cannot back into the texture console sitting to its left.
    if (drawn.getWidth() != needed_w || drawn.getHeight() != needed_h)
    {
        LLRect r = drawn;
        r.mRight = r.mLeft + needed_w;
        r.mBottom = r.mTop - needed_h;
        setRect(r);
    }

    LLView::draw();
}
