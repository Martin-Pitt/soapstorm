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
#include "llframetimer.h"
#include "llgl.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llui.h"
#include "llviewercontrol.h"

#include "ssrocaux.h"
#include "ssroccache.h"
#include "ssrocledger.h"
#include "llassettype.h"
#include "ssstrata.h"
#include "ssbc7adaptive.h"
#include "ssbc7encodequeue.h"
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

        // <SS:Nexii> Three lines, each answering a question somebody actually has: did it help, does it know this place, and is it still learning. What was here before was four subsystems reporting their own internals - loads and saves and terrain patches and sighting counts - which is what you want when debugging the thing and never what you want when using it.
        //
        // The raw counters are all still in the log at region exit, which is the right home for them.
        if (SSROCAuxMgr::instanceExists())
        {
            const SSROCAuxMgr::Metrics& a = SSROCAuxMgr::instance().metrics();
            const U32 known = a.mRegionsLoaded;
            const U32 fresh = a.mRegionsMissing;
            const U64 disk  = SSROCStore::instanceExists() ? SSROCStore::instance().diskBytesUsed() : 0;

            if (known || fresh)
            {
                line(llformat("  knows     %u of the %u regions visited (%s on disk)",
                              known, known + fresh, ssMB(disk).c_str()), known == 0);
            }
        }

        if (SSROCLedger::instanceExists())
        {
            const SSROCLedger::Metrics& l = SSROCLedger::instance().metrics();

            // Promotion is what frees ghosts from the recent-visit window, so a zero here is the single most useful thing on this panel: it says the long-term tier is not carrying anything yet and every paint you saw came from having been here in the last few hours.
            if (l.mPromoted == 0 && l.mRecordsCreated > 0)
            {
                line(llformat("  learning  %u objects remembered, none settled into long term yet",
                              l.mRecordsCreated), true);
            }
            else if (l.mRecordsCreated > 0)
            {
                line(llformat("  learning  %u objects remembered, %u settled into long term",
                              l.mRecordsCreated, l.mPromoted));
            }
        }

        // <SS:Nexii> Says WHAT the number counts, WHETHER it matters, and WHAT happens next - which the previous line did none of. "problems 4 unreadable" names no unit, gives no consequence and offers no action, and it was reporting a routine format upgrade as damage besides.
        //
        // A region rebuilding itself is not a problem in any sense the user needs a line for, so an upgrade says so in ordinary words and is not filed under problems at all.
        if (SSROCStore::instanceExists())
        {
            const SSROCStore::Metrics& m = SSROCStore::instance().metrics();

            // <SS:Nexii> Salvaged and orphaned are shown apart because they are different news. A salvaged region kept its whole presence history - every distinct day it has ever been seen on - and only lost the object descriptions, which come back the first time the simulator describes them. An orphaned one starts from nothing. Since distinct days are the one currency that can only be earned by waiting, that difference is the difference between a format change costing an afternoon and costing three days.
            const U32 salvaged = m.mLoadsSalvaged.load();
            const U32 orphaned = m.mLoadsOldVersion.load();

            if (salvaged)
            {
                line(llformat("  upgraded  %u region%s carried their history across a format change",
                              salvaged, salvaged == 1 ? "" : "s"), true);
            }
            if (orphaned)
            {
                line(llformat("  reset     %u region%s were too old to carry over and start again",
                              orphaned, orphaned == 1 ? "" : "s"), true);
            }

            const U32 damaged = m.mLoadsCorrupt.load();
            const U32 unsaved = m.mSavesFailed.load();
            if (damaged)
            {
                line(llformat("  problems  %u region file%s damaged and discarded - %s will be remembered again on the next visit",
                              damaged, damaged == 1 ? "" : "s", damaged == 1 ? "it" : "they"));
            }
            if (unsaved)
            {
                // This one IS worth alarm: a region that cannot be written is a region that learns nothing, so the same visit repeats forever with no way for the user to tell.
                line(llformat("  problems  %u region%s could not be saved, so nothing was remembered from %s",
                              unsaved, unsaved == 1 ? "" : "s", unsaved == 1 ? "it" : "them"));
            }
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
        // <SS:Nexii> The three figures are made to RECONCILE, because the previous pair could not be. It read "using 855 MB instead of 2644 MB" beside a separate total of all texture memory, and no arithmetic relates those three: 2644 is a COUNTERFACTUAL that never occupied a byte of the card, sat next to an ACTUAL with nothing marking which was which.
        //
        // Stated as now-versus-without instead. The total is what the card really holds; the same total plus the saving is what it would hold if none of this existed; and the compressed share is called out as a part OF the real total rather than as a number floating beside it. Every figure on these lines can be checked against the others by adding up.
        //
        // They are commensurable to begin with only because both sides count the base level and no mips: getTextureBytesAllocated documents "Does not include mipmaps", and the compressed accounting in llimagegl.cpp deliberately matches that contract. If either side ever starts counting the chain, these lines stop meaning anything and must be revisited.
        const SSBC7ServeResidency r = ssBC7ServeResidencyNow();
        const U64 total_now = LLImageGL::getTextureBytesAllocated();

        if (r.mTextures > 0 && r.mSavedBytes > 0)
        {
            const S64 without_bc7 = r.mBC7Bytes + r.mSavedBytes;
            const F32 ratio       = (r.mBC7Bytes > 0) ? ((F32)without_bc7 / (F32)r.mBC7Bytes) : 0.f;

            // <SS:Nexii> "using X instead of Y" is kept exactly as it was, because it is the clearest line here: two numbers, one comparison, no arithmetic asked of the reader. What was wrong was never the phrasing, it was that Y is a COUNTERFACTUAL - bytes that never existed - and it used to sit beside a separate total of real memory with nothing relating them, so anyone trying to add up got nonsense.
            //
            // The fix is one line underneath giving the same before-and-after for ALL textures. Now the compressed pair is visibly a part of the whole pair, and every figure can be checked against the others.
            line(llformat("  SAVING    %s of video memory right now", ssMB((U64)r.mSavedBytes).c_str()));
            line(llformat("            %u textures using %s instead of %s  (%.1fx smaller)",
                          r.mTextures, ssMB((U64)r.mBC7Bytes).c_str(), ssMB((U64)without_bc7).c_str(), ratio));

            // The exact pair matches "GL Tex" in the texture console byte for byte - both count base levels and no mips - and the card figure beside it is the viewer's own estimate of what that misses. llviewertexture.cpp divides by RenderTextureVRAMDivisor to drive the discard bias, with the stock comment that the metrics "miss about half the vram we use" but land "within 5% of the real number", so reading the setting rather than hardcoding two keeps this line honest if anyone retunes it. It is the figure to hold against a task manager, and it says the exact numbers understate the saving rather than flatter it.
            static LLCachedControl<U32> vram_divisor(gSavedSettings, "RenderTextureVRAMDivisor", 2);
            const U32 div = llmax((U32)1, (U32)vram_divisor);

            if (div > 1)
            {
                line(llformat("            all textures %s, would be %s  (on the card, about %s and %s)",
                              ssMB(total_now).c_str(), ssMB(total_now + (U64)r.mSavedBytes).c_str(),
                              ssMB(total_now * div).c_str(), ssMB((total_now + (U64)r.mSavedBytes) * div).c_str()), true);
            }
            else
            {
                line(llformat("            all textures %s, would be %s without it",
                              ssMB(total_now).c_str(), ssMB(total_now + (U64)r.mSavedBytes).c_str()), true);
            }
        }
        else
        {
            // Serving nothing is the expected state on a cold store rather than a fault, and giving the real total alongside it stops the panel reading as breakage while the store fills.
            line(llformat("  textures  %s of video memory, none of it compressed yet", ssMB(total_now).c_str()), true);
            line("            they are compressed as they are seen, and reused from the next sighting on", true);
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

        line(llformat("  stored    %u textures compressed   %s of %s (%u%%)",
                      store.recordCount(), ssMB(used).c_str(), ssMB(budget).c_str(), pct));

        // <SS:Nexii> Quality, effort and repair, in the words a person would use. The profile alone is not the answer - "best" and "dropped to fast" mean nothing without WHY, and a user seeing lower quality needs to know whether the viewer chose it or they pinned it themselves.
        //
        // Busy workers come from the moving average rather than the instant count, because on a supply limited pool an instant sample reads zero most of the time and would make a healthy machine look idle.
        const SSBC7AdaptiveStats a = ssBC7AdaptiveStatsNow();
        if (a.mWorkers > 0)
        {
            std::string why;
            switch ((ESSBC7AdaptReason)a.mReason)
            {
                case SSBC7_ADAPT_PINNED:       why = "you chose this";                       break;
                case SSBC7_ADAPT_HEADROOM:     why = "keeping up easily";                    break;
                case SSBC7_ADAPT_HOLDING:      why = "keeping up";                           break;
                case SSBC7_ADAPT_STEPPED_DOWN: why = "lowered to catch up on a backlog";     break;
                case SSBC7_ADAPT_STEPPED_UP:   why = "raised again now the backlog cleared"; break;
                case SSBC7_ADAPT_NO_BACKEND:   why = "only one encoder in this build";       break;
                default:                       why = "nothing measured yet";                 break;
            }

            // The rate is only shown once it means something. A figure resting on no samples is worse than no figure, because it looks like a measurement.
            if (a.mAggregateMpixPerSec > 0.f)
            {
                line(llformat("  quality   %s  -  %s, at %.0f Mpix/s",
                              ssBC7QualityName((SSBC7Quality)a.mQuality), why.c_str(), a.mAggregateMpixPerSec));
            }
            else
            {
                line(llformat("  quality   %s  -  %s",
                              ssBC7QualityName((SSBC7Quality)a.mQuality), why.c_str()));
            }

            line(llformat("            %.0f of %u cores busy compressing", a.mWorkersBusy, a.mWorkers), true);
        }

        // The repair pass, and the reason both numbers appear together: "1240 improved" says nothing without "3891 to go". Together they say the viewer is quietly making the cache better while it has nothing else to do.
        if (a.mUpgraded || a.mBelowBest)
        {
            line(llformat("  improving %u old textures redone at better quality%s",
                          a.mUpgraded, a.mUpgradeRunning ? "  (running now)" : ""));
            if (a.mBelowBest)
            {
                line(llformat("            %u still stored at a lower quality", a.mBelowBest), true);
            }
        }

        // <SS:Nexii> The want list is why the saving climbs slowly rather than arriving at once, and without it the panel looks like the feature has finished when it has barely started. Compression only happens at full resolution, so a texture seen at a distance waits here until something brings it close enough to be fetched whole.
        const size_t waiting = ssBC7EncodeWantListSize();
        if (waiting > 0)
        {
            line(llformat("  waiting   %u textures need full resolution first", (U32)waiting), true);
        }
    }



    blank();

    // ---- Strata (both cache tiers) -------------------------------------------
    // <SS:Nexii> Two tenants, both shown. The texture tier is roughly ten times the size of the asset tier on a real cache, so a panel that reported only the asset store was describing under a tenth of what Strata actually holds. The number worth watching in each is LOOSE, not packed: packed is the achievement, but loose is what a user sees in Explorer, and during a cold-cache burst it runs to thousands before the packer catches up - which looked like the feature not working when the only place that number appeared was a log line.
    {
        SSStrataStore* tiers[SSSTRATA_TENANT_COUNT];
        for (U32 t = 0; t < SSSTRATA_TENANT_COUNT; ++t)
        {
            tiers[t] = SSStrataStore::live((ESSStrataTenant)t);
        }

        U64 all_packed = 0;
        U32 all_volumes = 0, all_loose = 0;
        bool any = false;
        for (U32 t = 0; t < SSSTRATA_TENANT_COUNT; ++t)
        {
            if (!tiers[t]) continue;
            any = true;
            all_packed  += tiers[t]->allocatedBytes();
            all_volumes += tiers[t]->volumeCount();
            all_loose   += tiers[t]->metrics().mLooseFiles.load();
        }

        line("STRATA  (cache in big files)", !any);
        if (!any)
        {
            line("  off", true);
        }
        else
        {
            line(llformat("  holding   %s in %u big file%s instead of thousands of small ones",
                          ssMB(all_packed).c_str(), all_volumes, all_volumes == 1 ? "" : "s"));

            static const char* TENANT_LABEL[SSSTRATA_TENANT_COUNT] = { "assets  ", "textures" };
            for (U32 t = 0; t < SSSTRATA_TENANT_COUNT; ++t)
            {
                SSStrataStore* tier = tiers[t];
                if (!tier)
                {
                    line(llformat("  %s  off", TENANT_LABEL[t]), true);
                    continue;
                }

                const U64 allocated = tier->allocatedBytes();
                const U64 budget    = tier->budgetBytes();
                const U32 pct       = budget ? (U32)llmin((U64)999, allocated * 100ull / budget) : 0;
                const SSStrataStore::Metrics& sm = tier->metrics();

                line(llformat("  %s  %s in %u file%s   %u%% of its %s share   %u objects",
                              TENANT_LABEL[t], ssMB(allocated).c_str(), tier->volumeCount(),
                              tier->volumeCount() == 1 ? "" : "s", pct, ssMB(budget).c_str(),
                              tier->objectCount()));

                const U32 loose = sm.mLooseFiles.load();
                if (loose)
                {
                    // Not a backlog and not a leak: SSStrataPackAgeSeconds deliberately leaves a file alone until it has stopped being written to. Saying so on the line stops the number reading as failure.
                    line(llformat("            %u still loose (%s), waiting out the settle time",
                                  loose, ssMB(sm.mLooseBytes.load()).c_str()), true);
                }
            }

            // The asset tier is the only mixed one - the texture tier is entirely J2C bodies, which is why it carries a default type instead of recording one per object. Refreshed on its own timer because it walks the whole index under the lock the fetch threads resolve reads through.
            SSStrataStore* assets = tiers[SSSTRATA_TENANT_ASSETS];
            if (assets)
            {
                static std::vector<SSStrataTypeStat> s_breakdown;
                static LLFrameTimer s_breakdown_timer;
                static bool s_breakdown_primed = false;
                if (!s_breakdown_primed || s_breakdown_timer.getElapsedTimeF32() >= 5.f)
                {
                    s_breakdown_primed = true;
                    s_breakdown_timer.reset();
                    assets->typeBreakdown(s_breakdown);
                }

                if (!s_breakdown.empty())
                {
                    line("  what is in the asset file(s)", true);
                    const size_t shown = llmin((size_t)5, s_breakdown.size());
                    for (size_t i = 0; i < shown; ++i)
                    {
                        const SSStrataTypeStat& st = s_breakdown[i];
                        // 0xFF is not corruption: Firestorm's cache filenames do not carry the asset type, so anything packed out of a file this session never wrote has no class to report. It decays as the cache turns over.
                        const char* name = (st.mType == 0xFF)
                            ? "not recorded"
                            : LLAssetType::lookupHumanReadable((LLAssetType::EType)st.mType);
                        line(llformat("     %-14s %9s  %u objects", name ? name : "other",
                                      ssMB(st.mBytes).c_str(), st.mCount), true);
                    }
                    if (s_breakdown.size() > shown)
                    {
                        U64 rest_bytes = 0; U32 rest_count = 0;
                        for (size_t i = shown; i < s_breakdown.size(); ++i)
                        {
                            rest_bytes += s_breakdown[i].mBytes;
                            rest_count += s_breakdown[i].mCount;
                        }
                        line(llformat("     %-14s %9s  %u objects", "everything else",
                                      ssMB(rest_bytes).c_str(), rest_count), true);
                    }
                }
            }

            // Session activity, summed across both tiers so the line reads as one feature rather than two accountants.
            U32 folded = 0, read_back = 0, killed = 0, dropped = 0, too_young = 0, too_big = 0, in_use = 0;
            U32 bad_reads = 0, bad_records = 0;
            U64 folded_bytes = 0, reclaimed_bytes = 0;
            for (U32 t = 0; t < SSSTRATA_TENANT_COUNT; ++t)
            {
                if (!tiers[t]) continue;
                const SSStrataStore::Metrics& sm = tiers[t]->metrics();
                folded          += sm.mPacked.load();
                folded_bytes    += sm.mPackedBytes.load();
                read_back       += sm.mReadsServed.load();
                killed          += sm.mVolumesKilled.load();
                dropped         += sm.mObjectsDropped.load();
                reclaimed_bytes += sm.mBytesReclaimed.load();
                too_young       += sm.mSkipTooYoung.load();
                too_big         += sm.mSkipTooBig.load();
                in_use          += sm.mSkipUnstable.load() + sm.mSkipChanged.load() + sm.mSkipPinned.load();
                bad_reads       += sm.mReadsFailedOpen.load() + sm.mReadsFailedShort.load()
                                 + sm.mReadsFailedIdentity.load() + sm.mReadsFailedCRC.load();
                bad_records     += sm.mRecordsRejected.load() + sm.mTombstoneFailed.load();
            }

            line(llformat("  session   %u folded in (%s), %u read back out",
                          folded, ssMB(folded_bytes).c_str(), read_back), folded == 0 && read_back == 0);

            if (killed || dropped)
            {
                line(llformat("  freed     %u big file%s deleted (%s), %u objects went with them",
                              killed, killed == 1 ? "" : "s", ssMB(reclaimed_bytes).c_str(), dropped));
            }

            // Only when something is actually waiting, so a settled cache does not carry a line of zeroes.
            if (all_loose && (too_young || too_big || in_use))
            {
                line(llformat("  passed by %u too new, %u too big, %u in use", too_young, too_big, in_use), true);
            }

            if (bad_reads || bad_records)
            {
                line(llformat("  problems  %u failed reads, %u rejected or lost records", bad_reads, bad_records));
            }
        }
    }


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
