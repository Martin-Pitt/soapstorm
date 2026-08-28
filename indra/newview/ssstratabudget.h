/**
 * @file ssstratabudget.h
 * @brief The cache budget arbiter - CacheSize as a genuinely total number, split N ways, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_STRATA_BUDGET_H
#define SS_STRATA_BUDGET_H

#include "stdtypes.h"

#include <string>

// <SS:Nexii> CacheSize has never been a total. FSDiskCacheSize (16384 MB) and SSROCDiskBudgetMB (2048 MB) both sit entirely OUTSIDE it, so a user who sets the slider to 16384 has in fact authorised 34816 MB, and the measured spend was 11171 MB and climbing. This file is the one place that decides how the total is divided, so that a tier can no longer spend beyond the number the user set without anything being wrong by its own reckoning.
//
// The computation is split in two on purpose. THIS half - ssstratabudget.cpp - reaches for no setting, no singleton and no filesystem: numbers in, plan out, so the N-way normalisation can be built and driven in an offline harness where getting the arithmetic wrong costs a failed assertion rather than somebody's cache. The half that reads gSavedSettings and pushes the result into the live tiers is ssstratabudgetwire.cpp.
//
// NOTHING IS ENFORCED until SSStrataBudgetEnforce is turned on, and it ships off. Every tier keeps the exact byte count it gets today and the arbiter only reports what it WOULD hand out, because turning the total on will delete data that currently survives indefinitely and that has to be the owner's stated decision rather than a side effect of an upgrade.

enum ESSBudgetTier
{
    SSBUDGET_TIER_J2C = 0,      // LLTextureCache bodies, the re-encode source for everything below it, so it is the remainder rather than a share and it has a floor
    SSBUDGET_TIER_BC7,          // the compressed sidecar the renderer actually reads from
    SSBUDGET_TIER_ASSETS,       // LLDiskCache, which is FSDiskCacheSize today and is the single largest unaccounted number in the tree
    SSBUDGET_TIER_ROC,          // the region object cache - REPORTED, NOT GOVERNED, see doc/strata.md owner decision 5
    SSBUDGET_TIER_COUNT
};

// Named because a share that silently became zero is indistinguishable from a tier that was never asked for, and this project has been bitten four times by a bare refusal with no reason attached.
enum ESSBudgetVerdict
{
    SSBUDGET_AS_REQUESTED = 0,  // the tier got exactly what its own settings asked for
    SSBUDGET_FLOORED,           // the J2C remainder would have gone below its floor and was held at it
    SSBUDGET_PERCENT_SCALED,    // the percent shares oversubscribed the total and were scaled down proportionally
    SSBUDGET_OVERRIDE_SCALED,   // the absolute overrides ALONE oversubscribed the total, so even a pinned tier had to move
    SSBUDGET_SQUEEZED_OUT,      // absolute overrides consumed everything, so a percent share got nothing at all
    SSBUDGET_UNGOVERNED,        // outside the total by design - ROC, until the owner says otherwise
    SSBUDGET_TIER_DISABLED,     // the tier is switched off, so it asks for nothing and the others get its share
    SSBUDGET_VERDICT_COUNT
};

// What one tier's own settings say about it. Filled from gSavedSettings by the wire half, or by hand in the harness.
struct SSBudgetTierInput
{
    SSBudgetTierInput() : mPercent(0), mAbsoluteMB(0), mEnabled(true) {}

    U32  mPercent;      // share of the total, in percent, clamped to 0..90 because the other tiers always need somewhere to live
    U32  mAbsoluteMB;   // the escape hatch SSBC7CacheSize already models: a percent of ZERO selects this absolute number instead
    bool mEnabled;      // a tier that is switched off asks for nothing, and its share goes to whoever is left rather than being burned
};

struct SSBudgetInputs
{
    SSBudgetInputs() : mTotalMB(0), mEnforce(false) {}

    U32  mTotalMB;                                  // CacheSize, raw and unclamped, because two of the legacy formulas below clamp it and one deliberately does not
    bool mEnforce;                                  // SSStrataBudgetEnforce
    SSBudgetTierInput mTiers[SSBUDGET_TIER_COUNT];  // J2C's own percent and absolute are ignored - it is the remainder by construction
};

struct SSBudgetShare
{
    SSBudgetShare();

    ESSBudgetTier    mTier;
    U64              mRequestedBytes;   // what the tier's settings asked for, before anyone else was considered
    U64              mGrantedBytes;     // what the arbiter would hand out - the number that becomes real when enforcement is turned on
    U64              mLegacyBytes;      // what the tier gets TODAY, reproduced formula for formula including the two places the existing code disagrees with itself by tens of bytes
    bool             mAbsolute;         // the request came from the absolute override rather than from a percent
    bool             mGoverned;         // false for ROC, which is reported and left alone
    ESSBudgetVerdict mVerdict;
};

struct SSBudgetPlan
{
    SSBudgetPlan();

    U64  mTotalBytes;           // CacheSize clamped, which is the pot the governed tiers divide
    U64  mJ2CFloorBytes;        // a quarter of the total, which the J2C tier keeps whatever the other shares add up to
    U64  mGrantedTotal;         // the sum over the governed tiers - never above mTotalBytes, and that is the whole point of the file
    U64  mLegacyTotal;          // the sum of what those same tiers are authorised to spend today, which is the number that is bigger than the slider
    U64  mUngovernedBytes;      // ROC, still outside the total
    bool mEnforce;
    SSBudgetShare mShares[SSBUDGET_TIER_COUNT];
};

// The arbiter itself: pure, total, and the only function in the codebase allowed to decide how CacheSize is divided.
SSBudgetPlan ssBudgetComputePlan(const SSBudgetInputs& in);

// The number a tier should enforce right now. Granted when enforcement is on, LEGACY when it is off - which is what guarantees the first build cannot move a single byte.
U64 ssBudgetEffectiveBytes(const SSBudgetPlan& plan, ESSBudgetTier tier);

const char* ssBudgetTierName(ESSBudgetTier tier);
const char* ssBudgetVerdictName(ESSBudgetVerdict verdict);

// One multi-line block naming every tier, what it asked for, what it would get, what it gets today and what would have to be deleted to reach the granted number. Built here rather than in the wire half so the harness can read exactly what the owner will read in the log.
std::string ssBudgetPlanReport(const SSBudgetPlan& plan, const U64 allocated_bytes[SSBUDGET_TIER_COUNT]);

// <SS:Nexii> The tier interface doc/strata.md stage 1 specifies, and deliberately nothing more than it specifies: five members, no lifecycle, no eviction, no registration. Anything that needs a sixth member wants a different object.
class ISSCacheTier
{
public:
    virtual ~ISSCacheTier() {}

    virtual U64         allocatedBytes() const = 0;   // what the tier costs on disk right now
    virtual U64         budgetBytes() const = 0;      // what it is enforcing right now, which is the legacy number until enforcement is flipped
    virtual const char* tierName() const = 0;
    virtual bool        isReadOnly() const = 0;       // a second viewer instance opens every cache read-only, and a read-only tier can never be the reason bytes went missing
    virtual std::string metricsLine() const = 0;
};
// </SS:Nexii>

// ---- the wire half, ssstratabudgetwire.cpp -------------------------------------------------------------

bool          ssBudgetEnforced();                      // SSStrataBudgetEnforce, read live
SSBudgetPlan  ssBudgetCurrentPlan();                   // reads the settings and computes; cheap enough to call per tick, never per texture
U64           ssBudgetTierBytes(ESSBudgetTier tier);   // the one call every tier makes to find out what it may spend
void          ssBudgetApply(const char* why);          // pushes the current numbers into the two tiers that hold theirs in a variable rather than re-reading a setting
void          ssBudgetLogPlan(const char* why);        // the full block, at startup and whenever a share moves
void          ssBudgetTick();                          // a clock comparison until its own five minutes elapse, then one report block, so a session leaves behind a record of what enforcement would have done
std::string   ssBudgetOverlayLine();                   // one line for ssstatsview.cpp, which this pass does not edit
ISSCacheTier* ssBudgetTierAdapter(ESSBudgetTier tier); // null when the tier is not up yet, which is the normal state for ROC before login

#endif // SS_STRATA_BUDGET_H
