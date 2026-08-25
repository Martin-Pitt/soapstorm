/**
 * @file ssprecipvariants.h
 * @brief Atmo Magic procedural particle textures: several variants per
 *        (type, tier) with individual drops splattered over a transparent
 *        ground, sized by the ratio of drop size to tier quad size so the
 *        splats read at the same world scale as the near-tier drops.
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

#ifndef SS_PRECIPVARIANTS_H
#define SS_PRECIPVARIANTS_H

// <SS:Nexii> Atmo Magic procedural particle textures

#include "ssprecipitation.h"

#include "llpointer.h"
#include "llsingleton.h"

#include <map>

class LLViewerTexture;

class SSPrecipVariants : public LLSingleton<SSPrecipVariants>
{
    LLSINGLETON_EMPTY_CTOR(SSPrecipVariants);

public:
    static const U32 VARIANT_COUNT = 8;

    // Lazily built, cached local texture for a (type, tier, variant). Deterministic from fixed seeds, so every client draws the same splats. When custom_drop is set (a developer-configured drop
    // texture), the cluster/sheet variants are baked by splatting that texture instead of the procedural shapes, keyed by its UUID so a changed texture rebakes. Falls back to returning custom_drop
    // itself until its GL image exists.
    LLViewerTexture* get(const SSPrecipPreset& preset, SSPrecipTier tier, U32 variant,
                         LLViewerTexture* custom_drop = nullptr);

    // How much bigger than life the drops in a (type, tier) bake came out. The splat layout floors each splat at a minimum share of the quad, or the far tiers bake out as near-empty textures: a
    // raindrop on an 18 by 36 metre sheet is a third of a texel, and a curtain of those seen across a field would be nothing at all. That floor is right for the thing those quads are for and wrong
    // for anything drawn close up - on rain's sheets it makes every drop four and a half times too wide. Anything that draws this art at arm's length can divide it back out, per axis, because the
    // floor bites on one axis and not the other.
    void splatInflation(const SSPrecipPreset& preset, SSPrecipTier tier,
                        F32& scale_x, F32& scale_y);

    // Impact helper shapes
    enum EUtility
    {
        UTIL_RING = 0,  // expanding surface ripple
        UTIL_DOT,       // soft splash crown
        UTIL_SHARD,     // sharp sliver, for mana hail shatter and dark embers
        UTIL_PUFF       // vague soft cloud, for the puffy ember flavour
    };
    LLViewerTexture* utility(EUtility kind);

    void clearCache() { mCache.clear(); }

private:
    LLPointer<LLViewerTexture> build(const SSPrecipPreset& preset, SSPrecipTier tier, U32 variant);
    LLPointer<LLViewerTexture> bakeFromCustom(const SSPrecipPreset& preset, SSPrecipTier tier,
                                              U32 variant, LLViewerTexture* custom_drop);
    LLPointer<LLViewerTexture> buildUtility(EUtility kind);

    std::map<U64, LLPointer<LLViewerTexture>> mCache;
};

// </SS:Nexii>

#endif // SS_PRECIPVARIANTS_H
