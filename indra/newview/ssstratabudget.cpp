/**
 * @file ssstratabudget.cpp
 * @brief The cache budget arbiter, decisions half - numbers in, plan out, with no viewer attached, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssstratabudget.h"

// Split from ssstratabudgetwire.cpp deliberately, exactly as ssbc7store.cpp is split from ssbc7storeio.cpp: this half reaches for no setting, no singleton and no filesystem, so the whole N-way normalisation can be driven in an offline harness. An arithmetic mistake in here deletes gigabytes of somebody's cache, which is the strongest argument this codebase has for a file that can be tested without a viewer.

#include <cstdio>

namespace
{
    const U64 SSBUDGET_MB = 1024ull * 1024ull;

    // Mirrors the clamp llappviewer.cpp initCache has always applied to CacheSize. A slider below the floor is a user who does not know what they are asking for, and one above the ceiling is a typo.
    const U64 SSBUDGET_MIN_TOTAL = 256ull * SSBUDGET_MB;
    const U64 SSBUDGET_MAX_TOTAL = 100ull * 1024ull * SSBUDGET_MB;

    // 90 rather than 100 because every governed tier below the J2C one is a CONSUMER of it: a BC7 blob is re-encoded from the J2C body when it is evicted, so a share that took the whole cache would leave the tier refilling itself from the network.
    const U32 SSBUDGET_MAX_PERCENT = 90;

    U64 ssBudgetScaleInto(U64 request, U64 room, U64 sum, U64& remaining)
    {
        // F64 rather than a U64 multiply because request and room are each up to 100 GB and their product overflows a U64 well before that. The mantissa carries 53 bits, so the relative error here is around 1e-16 on a number below 1e11 - a fraction of a byte - and the running remainder below is what makes the truncation safe rather than merely small: the granted shares can never sum past the room, whatever the rounding did.
        if (!sum || !request) return 0;
        U64 granted = (U64)((F64)request * ((F64)room / (F64)sum));
        if (granted > remaining) granted = remaining;
        remaining -= granted;
        return granted;
    }
}

SSBudgetShare::SSBudgetShare()
:   mTier(SSBUDGET_TIER_J2C),
    mRequestedBytes(0),
    mGrantedBytes(0),
    mLegacyBytes(0),
    mAbsolute(false),
    mGoverned(true),
    mVerdict(SSBUDGET_AS_REQUESTED)
{
}

SSBudgetPlan::SSBudgetPlan()
:   mTotalBytes(0),
    mJ2CFloorBytes(0),
    mGrantedTotal(0),
    mLegacyTotal(0),
    mUngovernedBytes(0),
    mEnforce(false)
{
    for (U32 i = 0; i < SSBUDGET_TIER_COUNT; ++i)
    {
        mShares[i].mTier = (ESSBudgetTier)i;
    }
}

const char* ssBudgetTierName(ESSBudgetTier tier)
{
    switch (tier)
    {
        case SSBUDGET_TIER_J2C:    return "J2C textures";
        case SSBUDGET_TIER_BC7:    return "BC7 sidecar";
        case SSBUDGET_TIER_ASSETS: return "asset cache";
        case SSBUDGET_TIER_ROC:    return "region cache";
        default:                   return "unknown tier";
    }
}

const char* ssBudgetVerdictName(ESSBudgetVerdict verdict)
{
    switch (verdict)
    {
        case SSBUDGET_AS_REQUESTED:    return "AS_REQUESTED";
        case SSBUDGET_FLOORED:         return "FLOORED";
        case SSBUDGET_PERCENT_SCALED:  return "PERCENT_SCALED";
        case SSBUDGET_OVERRIDE_SCALED: return "OVERRIDE_SCALED";
        case SSBUDGET_SQUEEZED_OUT:    return "SQUEEZED_OUT";
        case SSBUDGET_UNGOVERNED:      return "UNGOVERNED";
        case SSBUDGET_TIER_DISABLED:   return "TIER_DISABLED";
        default:                       return "UNKNOWN";
    }
}

// <SS:Nexii> The arbiter. Three rules, in this order, and the order is the whole design:
//
//   1. The J2C tier is reserved a quarter of the total FIRST and is never scaled, because it is the re-encode source for everything else - a BC7 blob whose J2C body has been evicted comes back from the network, not from disk, which is the opposite of what a cache tier is for.
//   2. Absolute overrides are honoured at face value out of what is left. A share of zero selecting an absolute number is the escape hatch SSBC7CacheSize already models, and an override that gets quietly scaled is not an override.
//   3. Percent shares divide whatever the overrides left. If they oversubscribe it they are scaled proportionally rather than served first-come, so a user who sets three tiers to 50% gets three tiers at a third each rather than two full tiers and one empty one.
//
// Only when the absolutes ALONE do not fit does rule 2 give way, and that case is named OVERRIDE_SCALED rather than being folded into the general scaling, because it is the one outcome where the arbiter overrode something the user asked for explicitly and the log has to say so.
SSBudgetPlan ssBudgetComputePlan(const SSBudgetInputs& in)
{
    SSBudgetPlan plan;
    plan.mEnforce = in.mEnforce;

    const U64 raw_total = (U64)in.mTotalMB * SSBUDGET_MB;
    plan.mTotalBytes    = llclamp(raw_total, SSBUDGET_MIN_TOTAL, SSBUDGET_MAX_TOTAL);
    plan.mJ2CFloorBytes = plan.mTotalBytes / 4;

    for (U32 i = 0; i < SSBUDGET_TIER_COUNT; ++i)
    {
        plan.mShares[i].mTier = (ESSBudgetTier)i;
    }

    const SSBudgetTierInput& bc7_in    = in.mTiers[SSBUDGET_TIER_BC7];
    const SSBudgetTierInput& assets_in = in.mTiers[SSBUDGET_TIER_ASSETS];
    const SSBudgetTierInput& roc_in    = in.mTiers[SSBUDGET_TIER_ROC];
    const U32 bc7_pct                  = llmin(bc7_in.mPercent, SSBUDGET_MAX_PERCENT);

    // ---- what every tier is authorised to spend TODAY -------------------------------------------------
    // Reproduced formula for formula rather than recomputed cleanly, because "no tier's effective budget differs from today's by a single byte" is the requirement this whole first build exists to satisfy, and the two existing formulas genuinely disagree with each other. SSBC7Store::budgetBytes multiplies the RAW setting and then divides; llappviewer.cpp divides the CLAMPED total and then multiplies. At CacheSize 16384 with a 50% share those differ by 42 bytes. Neither is wrong enough to be worth changing in the same pass that introduces the arbiter, so both are preserved exactly and the granted column below uses one consistent rule instead.
    const U64 bc7_legacy_raw = bc7_pct ? ((raw_total * (U64)bc7_pct) / 100ull)
                                       : ((U64)bc7_in.mAbsoluteMB * SSBUDGET_MB);
    // NOT gated on mEnabled: this is what SSBC7Store::budgetBytes returns today, byte for byte, and that function does not consult SSSqueezeEnabled either. The gate belongs on the TOTAL below - a tier that is switched off is not spending anything - and on the split, which llappviewer.cpp does gate.

    // llappviewer.cpp's split, verbatim: a percent of zero contributes NOTHING to the split there, so the J2C tier keeps the whole total while SSBC7Store::budgetBytes hands the sidecar its absolute number on top. That is a genuine overspend and it is exactly what the granted column fixes.
    const U64 bc7_split_legacy = (bc7_in.mEnabled && bc7_pct) ? ((plan.mTotalBytes / 100ull) * (U64)bc7_pct) : 0;
    const U64 j2c_legacy       = llmax(plan.mJ2CFloorBytes,
                                       plan.mTotalBytes > bc7_split_legacy ? plan.mTotalBytes - bc7_split_legacy : 0);

    // FSDiskCacheSize, which is an absolute number sitting entirely outside the total and is the single largest unaccounted figure in the tree.
    const U64 assets_legacy = assets_in.mEnabled ? ((U64)assets_in.mAbsoluteMB * SSBUDGET_MB) : 0;

    // SSROCDiskBudgetMB, also outside the total. Reported and left alone - governing it is a separate decision the owner has not made yet.
    const U64 roc_legacy = roc_in.mEnabled ? ((U64)roc_in.mAbsoluteMB * SSBUDGET_MB) : 0;

    plan.mShares[SSBUDGET_TIER_J2C].mLegacyBytes    = j2c_legacy;
    plan.mShares[SSBUDGET_TIER_BC7].mLegacyBytes    = bc7_legacy_raw;
    plan.mShares[SSBUDGET_TIER_ASSETS].mLegacyBytes = assets_legacy;
    plan.mShares[SSBUDGET_TIER_ROC].mLegacyBytes    = roc_legacy;
    plan.mLegacyTotal = j2c_legacy + (bc7_in.mEnabled ? bc7_legacy_raw : 0) + assets_legacy;

    // ---- what the arbiter would hand out --------------------------------------------------------------
    // Every request below is a share of the CLAMPED total, one rule for all of them, which is the difference between this column and the one above.
    U64 requests[SSBUDGET_TIER_COUNT] = { 0, 0, 0, 0 };
    bool absolute[SSBUDGET_TIER_COUNT] = { false, false, false, false };

    for (U32 i = (U32)SSBUDGET_TIER_BC7; i <= (U32)SSBUDGET_TIER_ASSETS; ++i)
    {
        const SSBudgetTierInput& t = in.mTiers[i];
        SSBudgetShare& share = plan.mShares[i];
        share.mGoverned = true;

        if (!t.mEnabled)
        {
            share.mVerdict = SSBUDGET_TIER_DISABLED;
            continue;
        }

        const U32 pct = llmin(t.mPercent, SSBUDGET_MAX_PERCENT);
        if (pct == 0)
        {
            // A zero share selects the absolute number. A zero absolute on top of that is a setting nobody can honour, not an instruction to delete everything, so it is left asking for nothing and the tier keeps whatever its own eviction pass decides.
            requests[i] = (U64)t.mAbsoluteMB * SSBUDGET_MB;
            absolute[i] = true;
        }
        else
        {
            requests[i] = (plan.mTotalBytes * (U64)pct) / 100ull;
        }
        share.mRequestedBytes = requests[i];
        share.mAbsolute       = absolute[i];
    }

    const U64 room = plan.mTotalBytes - plan.mJ2CFloorBytes;

    U64 abs_sum = 0;
    U64 pct_sum = 0;
    for (U32 i = (U32)SSBUDGET_TIER_BC7; i <= (U32)SSBUDGET_TIER_ASSETS; ++i)
    {
        if (plan.mShares[i].mVerdict == SSBUDGET_TIER_DISABLED) continue;
        if (absolute[i]) abs_sum += requests[i];
        else             pct_sum += requests[i];
    }

    U64 remaining = room;

    if (abs_sum > room)
    {
        // The pinned tiers alone do not fit. Rule 2 gives way here and nowhere else, and it is named so the log can say which override moved.
        for (U32 i = (U32)SSBUDGET_TIER_BC7; i <= (U32)SSBUDGET_TIER_ASSETS; ++i)
        {
            SSBudgetShare& share = plan.mShares[i];
            if (share.mVerdict == SSBUDGET_TIER_DISABLED) continue;

            if (absolute[i])
            {
                share.mGrantedBytes = ssBudgetScaleInto(requests[i], room, abs_sum, remaining);
                share.mVerdict      = SSBUDGET_OVERRIDE_SCALED;
            }
            else
            {
                share.mGrantedBytes = 0;
                share.mVerdict      = SSBUDGET_SQUEEZED_OUT;
            }
        }
    }
    else
    {
        for (U32 i = (U32)SSBUDGET_TIER_BC7; i <= (U32)SSBUDGET_TIER_ASSETS; ++i)
        {
            SSBudgetShare& share = plan.mShares[i];
            if (share.mVerdict == SSBUDGET_TIER_DISABLED || !absolute[i]) continue;
            share.mGrantedBytes = llmin(requests[i], remaining);
            remaining          -= share.mGrantedBytes;
            share.mVerdict      = SSBUDGET_AS_REQUESTED;
        }

        const U64 pct_room = remaining;
        for (U32 i = (U32)SSBUDGET_TIER_BC7; i <= (U32)SSBUDGET_TIER_ASSETS; ++i)
        {
            SSBudgetShare& share = plan.mShares[i];
            if (share.mVerdict == SSBUDGET_TIER_DISABLED || absolute[i]) continue;

            if (pct_sum <= pct_room)
            {
                share.mGrantedBytes = llmin(requests[i], remaining);
                remaining          -= share.mGrantedBytes;
                share.mVerdict      = SSBUDGET_AS_REQUESTED;
            }
            else if (pct_room == 0)
            {
                share.mGrantedBytes = 0;
                share.mVerdict      = SSBUDGET_SQUEEZED_OUT;
            }
            else
            {
                share.mGrantedBytes = ssBudgetScaleInto(requests[i], pct_room, pct_sum, remaining);
                share.mVerdict      = SSBUDGET_PERCENT_SCALED;
            }
        }
    }

    // The J2C tier is the remainder and cannot be anything else: it is what every other tier is derived FROM, so slack belongs to it rather than being left unspent. By construction the tiers above could take at most `room`, so this can never fall below the floor.
    {
        U64 taken = 0;
        for (U32 i = (U32)SSBUDGET_TIER_BC7; i <= (U32)SSBUDGET_TIER_ASSETS; ++i)
        {
            taken += plan.mShares[i].mGrantedBytes;
        }

        SSBudgetShare& j2c = plan.mShares[SSBUDGET_TIER_J2C];
        j2c.mGoverned       = true;
        j2c.mAbsolute       = false;
        j2c.mRequestedBytes = plan.mTotalBytes > taken ? plan.mTotalBytes - taken : 0;

        if (!in.mTiers[SSBUDGET_TIER_J2C].mEnabled)
        {
            j2c.mGrantedBytes = 0;
            j2c.mVerdict      = SSBUDGET_TIER_DISABLED;
        }
        else
        {
            j2c.mGrantedBytes = llmax(plan.mJ2CFloorBytes, j2c.mRequestedBytes);
            j2c.mVerdict      = (j2c.mGrantedBytes == plan.mJ2CFloorBytes && taken >= room) ? SSBUDGET_FLOORED : SSBUDGET_AS_REQUESTED;
        }
    }

    // ROC is reported so the owner can see the whole number, and left entirely alone so that this pass cannot be the reason a region cache went missing. doc/strata.md owner decision 5 anticipated exactly this split.
    {
        SSBudgetShare& roc = plan.mShares[SSBUDGET_TIER_ROC];
        roc.mGoverned       = false;
        roc.mAbsolute       = true;
        roc.mRequestedBytes = roc_legacy;
        roc.mGrantedBytes   = roc_legacy;
        roc.mVerdict        = roc_in.mEnabled ? SSBUDGET_UNGOVERNED : SSBUDGET_TIER_DISABLED;
        plan.mUngovernedBytes = roc_legacy;
    }

    plan.mGrantedTotal = 0;
    for (U32 i = 0; i < SSBUDGET_TIER_COUNT; ++i)
    {
        if (plan.mShares[i].mGoverned) plan.mGrantedTotal += plan.mShares[i].mGrantedBytes;
    }

    return plan;
}

// The one function every tier calls, and the only place the enforcement switch is read into a number. Off means LEGACY, which is today's byte count reproduced exactly rather than a recomputation that happens to land near it.
U64 ssBudgetEffectiveBytes(const SSBudgetPlan& plan, ESSBudgetTier tier)
{
    if (tier >= SSBUDGET_TIER_COUNT) return 0;
    const SSBudgetShare& share = plan.mShares[tier];
    if (!share.mGoverned) return share.mLegacyBytes;   // ROC is unchanged whichever way the switch is thrown
    return plan.mEnforce ? share.mGrantedBytes : share.mLegacyBytes;
}

std::string ssBudgetPlanReport(const SSBudgetPlan& plan, const U64 allocated_bytes[SSBUDGET_TIER_COUNT])
{
    char buf[512];
    std::string out;

    const U64 MB = SSBUDGET_MB;

    snprintf(buf, sizeof(buf), "CacheSize is %llu MB and enforcement is %s\n",
             (unsigned long long)(plan.mTotalBytes / MB), plan.mEnforce ? "ON" : "OFF (nothing below is being applied)");
    out += buf;

    snprintf(buf, sizeof(buf), "  %-14s %10s %10s %10s %10s  %s\n",
             "tier", "on disk", "today", "would get", "would drop", "verdict");
    out += buf;

    U64 total_now = 0;
    U64 total_drop = 0;

    for (U32 i = 0; i < SSBUDGET_TIER_COUNT; ++i)
    {
        const SSBudgetShare& share = plan.mShares[i];
        const U64 used  = allocated_bytes ? allocated_bytes[i] : 0;
        // What enforcement would cost, stated as a number rather than as a warning, because the owner is being asked to decide a ceiling and "some data" is not a number anyone can decide against. A tier that is already under its share drops nothing.
        const U64 drop  = (share.mGoverned && share.mGrantedBytes < used) ? (used - share.mGrantedBytes) : 0;

        total_now  += used;
        total_drop += drop;

        snprintf(buf, sizeof(buf), "  %-14s %7llu MB %7llu MB %7llu MB %7llu MB  %s%s\n",
                 ssBudgetTierName(share.mTier),
                 (unsigned long long)(used / MB),
                 (unsigned long long)(share.mLegacyBytes / MB),
                 (unsigned long long)(share.mGrantedBytes / MB),
                 (unsigned long long)(drop / MB),
                 ssBudgetVerdictName(share.mVerdict),
                 share.mAbsolute && share.mGoverned ? " (absolute override)" : "");
        out += buf;
    }

    snprintf(buf, sizeof(buf), "  governed tiers authorise %llu MB today against a %llu MB slider; the plan authorises %llu MB\n",
             (unsigned long long)(plan.mLegacyTotal / MB),
             (unsigned long long)(plan.mTotalBytes / MB),
             (unsigned long long)(plan.mGrantedTotal / MB));
    out += buf;

    snprintf(buf, sizeof(buf), "  ungoverned on top of that: %llu MB (region cache, reported only)\n",
             (unsigned long long)(plan.mUngovernedBytes / MB));
    out += buf;

    snprintf(buf, sizeof(buf), "  %llu MB in use across all tiers; turning enforcement on would drop %llu MB\n",
             (unsigned long long)(total_now / MB), (unsigned long long)(total_drop / MB));
    out += buf;

    return out;
}
// </SS:Nexii>
