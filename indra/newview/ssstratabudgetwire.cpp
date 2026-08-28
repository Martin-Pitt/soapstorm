/**
 * @file ssstratabudgetwire.cpp
 * @brief The cache budget arbiter, wiring half - settings in, tiers out, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssstratabudget.h"

#include "llappviewer.h"
#include "lldiskcache.h"
#include "lltexturecache.h"
#include "llviewercontrol.h"
#include "ssbc7store.h"
#include "ssroccache.h"
#include "ssstrata.h"

#include "lltimer.h"

// <SS:Nexii> Everything in this file is the half of the arbiter that has to know about the viewer: which setting holds which share, which singleton is up yet, and which of the two mutable budget holders has to be written to when a share moves. The arithmetic is next door in ssstratabudget.cpp and is deliberately reachable without any of this.
//
// The lever, and why there is only one: the BC7 store and the asset tier both re-read their budget on every maintenance tick, so moving a setting is enough for them. The J2C tier holds its number in LLTextureCache::sCacheMaxTexturesSize, which both purge paths re-read live, so writing that one variable moves the tier mid-session with no worker protocol, no header trio and nothing added to the documented mutex ordering. LLDiskCache holds its number in mMaxSizeBytes, which Firestorm already moves mid-session through setMaxSizeBytes, so that seam existed before this file did.

namespace
{
    // Five minutes rather than the one minute the other maintenance ticks use. The point of this report is that a whole session leaves behind a readable record of what enforcement WOULD have deleted, and a block every minute is a block nobody reads.
    const F32 SSBUDGET_REPORT_INTERVAL = 300.f;

    LLTimer  gBudgetReportTimer;
    bool     gBudgetTimerStarted = false;

    // ---- the tier adapters ---------------------------------------------------------------------------
    // Five members each and nothing else, which is the whole interface doc/strata.md stage 1 specifies. They exist so that ssstatsview.cpp - which this pass deliberately does not edit - has ONE surface to walk rather than four bespoke blocks, and so that "what is this tier costing" is answerable without every caller knowing which singleton owns it.

    class SSBudgetJ2CTier : public ISSCacheTier
    {
    public:
        U64 allocatedBytes() const override
        {
            LLTextureCache* cache = LLAppViewer::getTextureCache();
            // The sum of LIVE body sizes, which is what LLTextureCache's own LRU purge is held against. Packed bodies still carry their mBodySize in the entry table, so this number does not change when Strata folds one into a volume.
            return cache ? (U64)llmax((S64)0, cache->getUsage().value()) : 0;
        }
        U64 budgetBytes() const override
        {
            LLTextureCache* cache = LLAppViewer::getTextureCache();
            return cache ? (U64)llmax((S64)0, cache->getMaxUsage().value()) : 0;
        }
        const char* tierName() const override { return ssBudgetTierName(SSBUDGET_TIER_J2C); }
        bool isReadOnly() const override { return LLAppViewer::instance() && LLAppViewer::instance()->isSecondInstance(); }
        std::string metricsLine() const override
        {
            const U64 used = allocatedBytes();
            const U64 cap  = budgetBytes();
            return llformat("%s: %llu MB of %llu MB%s", tierName(), (unsigned long long)(used / (1024 * 1024)),
                            (unsigned long long)(cap / (1024 * 1024)), isReadOnly() ? " (read only)" : "");
        }
    };

    class SSBudgetBC7Tier : public ISSCacheTier
    {
    public:
        U64 allocatedBytes() const override
        {
            if (!SSBC7Store::instanceExists() || !SSBC7Store::instance().isInitialized()) return 0;
            return SSBC7Store::instance().allocatedBytes();
        }
        U64 budgetBytes() const override
        {
            if (!SSBC7Store::instanceExists() || !SSBC7Store::instance().isInitialized()) return 0;
            return SSBC7Store::instance().budgetBytes();
        }
        const char* tierName() const override { return ssBudgetTierName(SSBUDGET_TIER_BC7); }
        bool isReadOnly() const override { return SSBC7Store::instanceExists() && SSBC7Store::instance().isReadOnly(); }
        std::string metricsLine() const override
        {
            const U64 used = allocatedBytes();
            const U64 cap  = budgetBytes();
            return llformat("%s: %llu MB of %llu MB%s", tierName(), (unsigned long long)(used / (1024 * 1024)),
                            (unsigned long long)(cap / (1024 * 1024)), isReadOnly() ? " (read only)" : "");
        }
    };

    class SSBudgetAssetTier : public ISSCacheTier
    {
    public:
        U64 allocatedBytes() const override
        {
            // Published by the once-a-minute purge from the directory walk it had to do anyway, so asking here costs nothing. It is read on the main thread and written on the purge thread; it is a reported number only, never a decision, so a stale read costs a slightly old log line.
            if (!LLDiskCache::instanceExists()) return 0;
            return (U64)LLDiskCache::getInstance()->getStoredCacheSize();
        }
        U64 budgetBytes() const override
        {
            if (!LLDiskCache::instanceExists()) return 0;
            return (U64)LLDiskCache::getInstance()->getMaxSizeBytes();
        }
        const char* tierName() const override { return ssBudgetTierName(SSBUDGET_TIER_ASSETS); }
        bool isReadOnly() const override
        {
            SSStrataStore* strata = SSStrataStore::live(SSSTRATA_TENANT_ASSETS);
            return strata ? strata->isReadOnly() : (LLAppViewer::instance() && LLAppViewer::instance()->isSecondInstance());
        }
        std::string metricsLine() const override
        {
            const U64 used = allocatedBytes();
            const U64 cap  = budgetBytes();
            return llformat("%s: %llu MB of %llu MB%s", tierName(), (unsigned long long)(used / (1024 * 1024)),
                            (unsigned long long)(cap / (1024 * 1024)), isReadOnly() ? " (read only)" : "");
        }
    };

    // Reported, never governed. This adapter reads two numbers and writes nothing, which is the entire extent of this pass's relationship with the region cache - doc/strata.md owner decision 5 splits the two workstreams here on purpose.
    class SSBudgetROCTier : public ISSCacheTier
    {
    public:
        U64 allocatedBytes() const override
        {
            if (!SSROCStore::instanceExists()) return 0;   // the region cache does not come up until the first region, which is always post-login, so zero here is the normal state at startup rather than a fault
            return SSROCStore::instance().diskBytesUsed();
        }
        U64 budgetBytes() const override
        {
            static LLCachedControl<U32> roc_mb(gSavedSettings, "SSROCDiskBudgetMB", 2048);
            return (U64)(U32)roc_mb * 1024ull * 1024ull;
        }
        const char* tierName() const override { return ssBudgetTierName(SSBUDGET_TIER_ROC); }
        bool isReadOnly() const override { return LLAppViewer::instance() && LLAppViewer::instance()->isSecondInstance(); }
        std::string metricsLine() const override
        {
            const U64 used = allocatedBytes();
            const U64 cap  = budgetBytes();
            return llformat("%s: %llu MB of %llu MB (not governed by CacheSize)", tierName(),
                            (unsigned long long)(used / (1024 * 1024)), (unsigned long long)(cap / (1024 * 1024)));
        }
    };

    SSBudgetJ2CTier   gJ2CTier;
    SSBudgetBC7Tier   gBC7Tier;
    SSBudgetAssetTier gAssetTier;
    SSBudgetROCTier   gROCTier;

    ISSCacheTier* gTiers[SSBUDGET_TIER_COUNT] = { &gJ2CTier, &gBC7Tier, &gAssetTier, &gROCTier };

    void ssBudgetCollectAllocated(U64 out[SSBUDGET_TIER_COUNT])
    {
        for (U32 i = 0; i < SSBUDGET_TIER_COUNT; ++i)
        {
            out[i] = gTiers[i] ? gTiers[i]->allocatedBytes() : 0;
        }
    }
}

bool ssBudgetEnforced()
{
    // Read live rather than latched, so the owner can turn the total on and off within one session while watching what it does. It ships OFF: turning it on will delete data that currently survives indefinitely, and that has to be a stated decision rather than something an upgrade did on its own.
    static LLCachedControl<bool> enforce(gSavedSettings, "SSStrataBudgetEnforce", false);
    return enforce;
}

SSBudgetPlan ssBudgetCurrentPlan()
{
    // Every share the arbiter divides, gathered in one place. LLCachedControl rather than getU32 because this is reached from SSBC7Store::budgetBytes, which an eviction pass calls per tick - a settings map lookup per call would be the one thing that made a budget check expensive.
    static LLCachedControl<U32>  total_mb(gSavedSettings, "CacheSize", 16384);
    static LLCachedControl<U32>  bc7_pct(gSavedSettings, "SSSqueezeCachePercent", 50);
    static LLCachedControl<U32>  bc7_abs_mb(gSavedSettings, "SSBC7CacheSize", 2048);
    static LLCachedControl<U32>  assets_pct(gSavedSettings, "SSStrataAssetCachePercent", 25);
    static LLCachedControl<U32>  assets_abs_mb(gSavedSettings, "FSDiskCacheSize", 16384);
    static LLCachedControl<U32>  roc_mb(gSavedSettings, "SSROCDiskBudgetMB", 2048);

    SSBudgetInputs in;
    in.mTotalMB = total_mb;
    in.mEnforce = ssBudgetEnforced();

    in.mTiers[SSBUDGET_TIER_J2C].mEnabled       = true;   // the J2C tier is the re-encode source for every other tier and cannot be switched off; it is the remainder by construction

    in.mTiers[SSBUDGET_TIER_BC7].mPercent       = bc7_pct;
    in.mTiers[SSBUDGET_TIER_BC7].mAbsoluteMB    = bc7_abs_mb;
    in.mTiers[SSBUDGET_TIER_BC7].mEnabled       = SSBC7Store::enabled();

    in.mTiers[SSBUDGET_TIER_ASSETS].mPercent    = assets_pct;
    in.mTiers[SSBUDGET_TIER_ASSETS].mAbsoluteMB = assets_abs_mb;
    in.mTiers[SSBUDGET_TIER_ASSETS].mEnabled    = true;   // LLDiskCache is always up; a share of zero selects FSDiskCacheSize, which is exactly what it uses today

    in.mTiers[SSBUDGET_TIER_ROC].mAbsoluteMB    = roc_mb;
    in.mTiers[SSBUDGET_TIER_ROC].mEnabled       = true;

    return ssBudgetComputePlan(in);
}

U64 ssBudgetTierBytes(ESSBudgetTier tier)
{
    return ssBudgetEffectiveBytes(ssBudgetCurrentPlan(), tier);
}

ISSCacheTier* ssBudgetTierAdapter(ESSBudgetTier tier)
{
    return tier < SSBUDGET_TIER_COUNT ? gTiers[tier] : NULL;
}

void ssBudgetLogPlan(const char* why)
{
    const SSBudgetPlan plan = ssBudgetCurrentPlan();
    U64 allocated[SSBUDGET_TIER_COUNT];
    ssBudgetCollectAllocated(allocated);

    LL_INFOS("Strata") << "Cache budget (" << (why ? why : "on request") << "):\n"
                       << ssBudgetPlanReport(plan, allocated) << LL_ENDL;
}

// The two tiers that hold their budget in a variable rather than re-reading a setting. Called at startup and whenever a share moves; the BC7 store is absent from this list on purpose, because it reads the arbiter directly on every tick and pushing a number at it would be a second copy to disagree with the first.
void ssBudgetApply(const char* why)
{
    const SSBudgetPlan plan = ssBudgetCurrentPlan();

    // Deliberately NOT gated on isSecondInstance() as a whole. Each tier declines for its own reason and says which - LLTextureCache::ssSetTexturesBudget refuses outright when it opened read-only - and a blanket return here would additionally stop LLDiskCache's stored ceiling following FSDiskCacheSize, which is what the About box reports and which a second instance updated before this file existed. A read-only instance runs no purge thread at all, so the number it holds is a display value and cannot evict anything.
    const U64 assets = ssBudgetEffectiveBytes(plan, SSBUDGET_TIER_ASSETS);
    if (LLDiskCache::instanceExists())
    {
        LLDiskCache::getInstance()->setMaxSizeBytes((uintmax_t)assets);
    }
    else
    {
        LL_WARNS("Strata") << "Cache budget: the asset tier's share of " << (assets / (1024 * 1024)) << " MB was not applied because LLDiskCache is not up yet; initCache passes it the same number a moment later" << LL_ENDL;
    }

    // <SS:Nexii> The J2C lever, and the single most important guard in this file. With enforcement OFF this variable is not written AT ALL - not written with the same value, not written with a recomputed one - because the only way to promise that no tier's effective budget differs from today's by a single byte is to leave the variable that holds it alone.
    if (plan.mEnforce)
    {
        LLTextureCache* cache = LLAppViewer::getTextureCache();
        if (cache)
        {
            cache->ssSetTexturesBudget((S64)ssBudgetEffectiveBytes(plan, SSBUDGET_TIER_J2C));
        }
        else
        {
            LL_WARNS("Strata") << "Cache budget: the J2C share was not applied because the texture cache is not up yet; initCache reads the same number directly a moment later" << LL_ENDL;
        }
    }
    // </SS:Nexii>

    ssBudgetLogPlan(why);
}

void ssBudgetTick()
{
    if (!gBudgetTimerStarted)
    {
        gBudgetTimerStarted = true;
        gBudgetReportTimer.reset();
        return;
    }

    if (gBudgetReportTimer.getElapsedTimeF32() < SSBUDGET_REPORT_INTERVAL) return;
    gBudgetReportTimer.reset();

    // Reporting only. Nothing here evicts, nothing here writes a budget: the tiers already re-read theirs, and the whole purpose of this tick is that a session spent with enforcement off still leaves a log the owner can decide a ceiling against.
    ssBudgetLogPlan("periodic");
}

std::string ssBudgetOverlayLine()
{
    const SSBudgetPlan plan = ssBudgetCurrentPlan();
    U64 allocated[SSBUDGET_TIER_COUNT];
    ssBudgetCollectAllocated(allocated);

    U64 governed_used = 0;
    for (U32 i = 0; i < SSBUDGET_TIER_COUNT; ++i)
    {
        if (plan.mShares[i].mGoverned) governed_used += allocated[i];
    }

    // "governed tiers" rather than "the cache", because doc/strata.md is explicit that the governed tiers are around 8,800 MB of an 11,171 MB cache directory and a line claiming to be the whole thing would be a lie the overlay tells every frame.
    return llformat("governed tiers %llu MB of %llu MB%s, region cache %llu MB outside it",
                    (unsigned long long)(governed_used / (1024 * 1024)),
                    (unsigned long long)((plan.mEnforce ? plan.mGrantedTotal : plan.mLegacyTotal) / (1024 * 1024)),
                    plan.mEnforce ? "" : " (not enforced)",
                    (unsigned long long)(allocated[SSBUDGET_TIER_ROC] / (1024 * 1024)));
}
// </SS:Nexii>
