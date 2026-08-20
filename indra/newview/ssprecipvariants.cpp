/**
 * @file ssprecipvariants.cpp
 * @brief Atmo Magic procedural particle texture generation.
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

#include "ssprecipvariants.h"

#include "llglslshader.h"
#include "llviewercontrol.h"
#include "llimage.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"

// <SS:Nexii> Atmo Magic procedural particle textures

// Saturating add into an alpha texel
static inline void splatAlpha(U8* data, S32 res, S32 x, S32 y, F32 a)
{
    if (x < 0 || y < 0 || x >= res || y >= res) return;
    U8* px = data + ((size_t)y * res + x) * 4 + 3;
    *px = (U8)llmin(255, (S32)*px + (S32)(a * 255.f));
}

// Soft round dot
static void drawDot(U8* data, S32 res, F32 cx, F32 cy, F32 radius, F32 brightness, F32 hardness)
{
    const S32 r = (S32)ceilf(radius);
    for (S32 y = -r; y <= r; ++y)
    {
        for (S32 x = -r; x <= r; ++x)
        {
            const F32 d = sqrtf((F32)(x * x + y * y)) / llmax(0.5f, radius);
            if (d > 1.f) continue;
            F32 a = 1.f - d;
            a = powf(a, 2.f - hardness);
            splatAlpha(data, res, (S32)cx + x, (S32)cy + y, a * brightness);
        }
    }
}

// Width profile of a motion-blurred drop along its length: t = 0 at the
// rounded leading head, t = 1 at the trailing tip.
static inline F32 teardropWidth(F32 t)
{
    const F32 bulb = 0.35f; // fraction of the length taken by the round head
    if (t <= bulb)
    {
        const F32 u = (bulb - t) / bulb;
        return sqrtf(llmax(0.f, 1.f - u * u));
    }
    const F32 u = (t - bulb) / (1.f - bulb);
    return powf(llmax(0.f, 1.f - u), 1.4f);
}

// Falling raindrop: round head leading the fall, tapering to a thin tail.
//
// The head goes at the low v end. The renderer builds a streak with its
// texture-up axis against the motion, so the +v end of the quad is the
// trailing end and v = 0 is the end that leads the fall. Baking the head at
// high v instead flies the drop tail first, which reads as upside down on any
// preset whose silhouette is legible at all.
static void drawTeardrop(U8* data, S32 res, F32 cx, F32 cy, F32 hw, F32 hh,
                         F32 brightness)
{
    const S32 y0 = (S32)floorf(cy - hh), y1 = (S32)ceilf(cy + hh);
    for (S32 y = y0; y <= y1; ++y)
    {
        const F32 t = llclamp(((F32)y - (cy - hh)) / llmax(1.f, 2.f * hh), 0.f, 1.f);

        const F32 w = teardropWidth(t) * hw;
        if (w <= 0.f) continue;

        // Most of the mass sits in the head; the tail thins out as it fades
        const F32 along = brightness * (0.45f + 0.55f * (1.f - t));

        const S32 x0 = (S32)floorf(cx - w), x1 = (S32)ceilf(cx + w);
        for (S32 x = x0; x <= x1; ++x)
        {
            const F32 fx = ((F32)x - cx) / llmax(0.5f, w);
            const F32 g = llmax(0.f, 1.f - fx * fx);
            splatAlpha(data, res, x, y, g * g * along);
        }
    }
}

// Hard shard: tapered at both ends rather than head-heavy like a droplet
static void drawSliver(U8* data, S32 res, F32 cx, F32 cy, F32 hw, F32 hh, F32 brightness)
{
    const S32 y0 = (S32)floorf(cy - hh), y1 = (S32)ceilf(cy + hh);
    for (S32 y = y0; y <= y1; ++y)
    {
        const F32 t = ((F32)y - cy) / llmax(0.5f, hh);   // -1..1 along the sliver
        if (fabsf(t) > 1.f) continue;
        const F32 w = hw * (1.f - t * t);
        if (w <= 0.f) continue;

        const S32 x0 = (S32)floorf(cx - w), x1 = (S32)ceilf(cx + w);
        for (S32 x = x0; x <= x1; ++x)
        {
            const F32 fx = ((F32)x - cx) / llmax(0.5f, w);
            const F32 g = llmax(0.f, 1.f - fx * fx);
            splatAlpha(data, res, x, y, g * g * brightness);
        }
    }
}

// Stable numeric key for a preset name, so bakes are cached per preset and
// renaming or editing one cannot collide with another
static U32 presetKey(const SSPrecipPreset& preset)
{
    U32 h = 0x811c9dc5u;
    for (char c : preset.mName)
    {
        h = (h ^ (U8)c) * 16777619u;
    }
    // Shape-affecting fields, so an edited preset rebakes rather than
    // serving the previous silhouette
    h ^= (U32)(preset.mTiers[TIER_DROPS].mSizeX * 1000.f);
    h ^= (U32)(preset.mTiers[TIER_DROPS].mSizeY * 1000.f) << 8;
    h ^= (U32)(preset.mDropScale * 1000.f) << 16;
    h ^= (U32)preset.mArchetype << 24;
    h ^= (U32)preset.mDropShape << 28;
    return h & 0x000fffffu;
}

// Splat layout for a tier texture. Rendering the represented drops at true
// world scale puts them far below a texel on the big cluster and sheet quads,
// which is why those tiers baked out as near-empty textures. Guarantee enough
// splats to read as a group and a minimum visible size, then scale the splat
// size so the total covered area still matches the true coverage.
static void splatLayout(F32 quad_x, F32 quad_y, F32 drop_x, F32 drop_y, S32 res,
                        S32 drops_represented, S32 min_group,
                        F32& hw, F32& hh, S32& count)
{
    const F32 px_x = (F32)res / (2.f * llmax(0.001f, quad_x));
    const F32 px_y = (F32)res / (2.f * llmax(0.001f, quad_y));
    const F32 hw_true = llmax(0.01f, drop_x * px_x);
    const F32 hh_true = llmax(0.01f, drop_y * px_y);

    count = llclamp(llmax(drops_represented, min_group), 1, 400);

    // Splitting the same coverage over more splats shrinks each one
    const F32 scale = sqrtf((F32)drops_represented / (F32)count);

    // The minimum visible splat is a fraction of the quad, not a fixed texel
    // count: pinning it to texels would make the same sheet render fainter
    // at higher resolution, since the floored splats would cover less of it
    const F32 min_splat = 0.9f * (F32)res / 128.f;
    hw = llclamp(hw_true * scale, min_splat, res * 0.45f);
    hh = llclamp(hh_true * scale, min_splat, res * 0.45f);
}

void SSPrecipVariants::splatInflation(const SSPrecipPreset& preset, SSPrecipTier tier,
                                      F32& scale_x, F32& scale_y)
{
    scale_x = 1.f;
    scale_y = 1.f;

    F32 quad_x, quad_y, drop_x, drop_y;
    S32 splats;
    if (!SSPrecipSim::tierSprite(preset, tier, quad_x, quad_y, drop_x, drop_y, splats)) return;

    // Same arithmetic the bake runs, at the same resolution, so the answer is
    // the ratio the bake actually applied rather than an estimate of it
    const S32 res = 256;
    F32 hw, hh;
    S32 count;
    splatLayout(quad_x, quad_y, drop_x, drop_y, res, splats,
                (tier == TIER_DROPS) ? 1 : (tier == TIER_CLUSTERS) ? 14 : 90,
                hw, hh, count);

    const F32 px_x = (F32)res / (2.f * llmax(0.001f, quad_x));
    const F32 px_y = (F32)res / (2.f * llmax(0.001f, quad_y));
    const F32 shrink = sqrtf((F32)splats / (F32)llmax(1, count));
    const F32 hw_true = llmax(0.01f, drop_x * px_x) * shrink;
    const F32 hh_true = llmax(0.01f, drop_y * px_y) * shrink;

    if (hw_true > 0.f) scale_x = llmax(1.f, hw / hw_true);
    if (hh_true > 0.f) scale_y = llmax(1.f, hh / hh_true);
}

LLViewerTexture* SSPrecipVariants::get(const SSPrecipPreset& preset, SSPrecipTier tier, U32 variant,
                                       LLViewerTexture* custom_drop)
{
    variant %= VARIANT_COUNT;
    // Keyed on the preset name: editing sizes or shapes has to rebake, and
    // two presets must never share a bake
    U64 key = ((U64)presetKey(preset) << 20) | ((U64)tier << 4) | variant;
    if (custom_drop)
    {
        // Key on the source texture so swapping the configured texture
        // bakes a fresh set of variants
        const LLUUID& id = custom_drop->getID();
        key ^= ((U64)id.mData[0] << 56) ^ ((U64)id.mData[5] << 44) ^
               ((U64)id.mData[10] << 32) ^ ((U64)id.mData[15] << 20) ^ 0x10000000ull;
    }

    auto it = mCache.find(key);
    if (it == mCache.end())
    {
        if (custom_drop)
        {
            if (!custom_drop->hasGLTexture())
            {
                // Source art still streaming in; use it directly for now and
                // bake once it exists (no cache entry yet)
                return custom_drop;
            }
            LLPointer<LLViewerTexture> baked = bakeFromCustom(preset, tier, variant, custom_drop);
            if (baked.isNull())
            {
                return custom_drop;
            }
            it = mCache.emplace(key, baked).first;
        }
        else
        {
            it = mCache.emplace(key, build(preset, tier, variant)).first;
        }
    }
    return it->second;
}

LLViewerTexture* SSPrecipVariants::utility(EUtility kind)
{
    const U64 key = 0xFF00ull | (U64)kind;
    auto it = mCache.find(key);
    if (it == mCache.end())
    {
        it = mCache.emplace(key, buildUtility(kind)).first;
    }
    return it->second;
}

LLPointer<LLViewerTexture> SSPrecipVariants::bakeFromCustom(const SSPrecipPreset& preset, SSPrecipTier tier,
                                                            U32 variant, LLViewerTexture* custom_drop)
{
    F32 quad_x, quad_y, drop_x, drop_y;
    S32 splats;
    if (!SSPrecipSim::tierSprite(preset, tier, quad_x, quad_y, drop_x, drop_y, splats))
    {
        return nullptr;
    }

    const S32 res = 256;
    LLRenderTarget target;
    if (!target.allocate(res, res, GL_RGBA))
    {
        return nullptr;
    }

    // Splat the configured drop art over a transparent ground, sized by the
    // drop-to-quad world ratio, same layout logic as the procedural path
    F32 hw_px, hh_px;
    S32 count;
    splatLayout(quad_x, quad_y, drop_x, drop_y, res, splats,
                (tier == TIER_CLUSTERS) ? 14 : 90, hw_px, hh_px, count);
    const F32 hw = hw_px / (F32)res;
    const F32 hh = hh_px / (F32)res;

    SSRandStream rng(SSAtmoNoise::combine(0x5EEDF00Du,
        (presetKey(preset) << 12) ^ ((U32)tier << 6) ^ variant));
    rng.next();

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();

    target.bindTarget();
    glClearColor(1.f, 1.f, 1.f, 0.f);
    target.clear(GL_COLOR_BUFFER_BIT);

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.ortho(0.f, 1.f, 0.f, 1.f, -1.f, 1.f);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();

    gUIProgram.bind();
    gGL.getTexUnit(0)->bind(custom_drop);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    // Accumulate coverage in the alpha channel too
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                  LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < count; ++i)
    {
        const F32 cx = (count == 1) ? 0.5f : rng.frand(hw, 1.f - hw);
        const F32 cy = (count == 1) ? 0.5f : rng.frand(hh, 1.f - hh);
        const F32 brightness = rng.frand(0.55f, 1.f);
        const F32 size = rng.frand(0.8f, 1.2f);
        const F32 x0 = cx - hw * size, x1 = cx + hw * size;
        const F32 y0 = cy - hh * size, y1 = cy + hh * size;

        gGL.color4f(1.f, 1.f, 1.f, brightness);
        gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(x0, y0);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(x1, y0);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(x1, y1);
        gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(x0, y0);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(x1, y1);
        gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(x0, y1);
    }
    gGL.end();
    gGL.flush();

    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)res, (U16)res, 4);
    glReadPixels(0, 0, res, res, GL_RGBA, GL_UNSIGNED_BYTE, raw->getData());

    gUIProgram.unbind();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();

    target.flush();
    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

    return LLViewerTextureManager::getLocalTexture(raw.get(), true);
}

LLPointer<LLViewerTexture> SSPrecipVariants::build(const SSPrecipPreset& preset, SSPrecipTier tier, U32 variant)
{
    F32 quad_x, quad_y, drop_x, drop_y;
    S32 splats;
    if (!SSPrecipSim::tierSprite(preset, tier, quad_x, quad_y, drop_x, drop_y, splats))
    {
        quad_x = quad_y = drop_x = drop_y = 0.05f;
        splats = 1;
    }

    const S32 res = (tier == TIER_DROPS) ? 64 : 256;
    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)res, (U16)res, 4);
    U8* data = raw->getData();
    for (size_t i = 0; i < (size_t)res * res; ++i)
    {
        data[i * 4 + 0] = 255;
        data[i * 4 + 1] = 255;
        data[i * 4 + 2] = 255;
        data[i * 4 + 3] = 0;
    }

    F32 hw, hh;
    S32 count;
    splatLayout(quad_x, quad_y, drop_x, drop_y, res, splats,
                (tier == TIER_DROPS) ? 1 : (tier == TIER_CLUSTERS) ? 14 : 90,
                hw, hh, count);


    // Fixed seed, not the user's weather seed: same textures on every
    // client and no cache churn when the seed slider moves
    SSRandStream rng(SSAtmoNoise::combine(0x5EEDF00Du,
        (presetKey(preset) << 12) ^ ((U32)tier << 6) ^ variant));
    rng.next();

    for (S32 i = 0; i < count; ++i)
    {
        // A lone drop sits centered; scattered ones fill the quad
        const F32 cx = (count == 1) ? res * 0.5f : rng.frand(hw, res - hw);
        const F32 cy = (count == 1) ? res * 0.5f : rng.frand(hh, res - hh);
        const F32 brightness = rng.frand(0.55f, 1.f);
        const F32 size = rng.frand(0.8f, 1.2f);
        switch (preset.mDropShape)
        {
            case DROP_TEARDROP:
                drawTeardrop(data, res, cx, cy, hw * size, hh * size, brightness);
                break;
            case DROP_SLIVER:
                drawSliver(data, res, cx, cy, hw * size, hh * size, brightness);
                break;
            default:
            {
                const F32 hard = (preset.mArchetype == SSPrecipArchetype::SOLID) ? 0.9f : 0.3f;
                drawDot(data, res, cx, cy, llmax(hw, hh) * size, brightness, hard);
                break;
            }
        }
    }

    return LLViewerTextureManager::getLocalTexture(raw.get(), true);
}

LLPointer<LLViewerTexture> SSPrecipVariants::buildUtility(EUtility kind)
{
    // The ring is baked at twice the rest: it carries a normal now, and the
    // wave's band is only a tenth of the texture wide, so at 64 the whole of
    // the relief was six texels of it
    const S32 res = (kind == UTIL_RING) ? 128 : 64;
    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)res, (U16)res, 4);
    U8* data = raw->getData();
    for (size_t i = 0; i < (size_t)res * res; ++i)
    {
        data[i * 4 + 0] = 255;
        data[i * 4 + 1] = 255;
        data[i * 4 + 2] = 255;
        data[i * 4 + 3] = 0;
    }

    const F32 c = res * 0.5f;
    if (kind == UTIL_PUFF)
    {
        // Vague soft cloud: several overlapping blobs so the silhouette is
        // irregular rather than a clean disc, with a very soft falloff
        SSRandStream rng(0x9F00DBEEu);
        for (S32 i = 0; i < 6; ++i)
        {
            const F32 ox = rng.frand(-0.13f, 0.13f) * (F32)res;
            const F32 oy = rng.frand(-0.13f, 0.13f) * (F32)res;
            drawDot(data, res, c + ox, c + oy, (F32)res * rng.frand(0.20f, 0.32f),
                    rng.frand(0.22f, 0.38f), -0.7f);
        }
    }
    else if (kind == UTIL_SHARD)
    {
        // Sharp sliver: bright core tapering to points at both ends
        const F32 half_len = res * 0.42f;
        const F32 half_w = res * 0.09f;
        for (S32 y = 0; y < res; ++y)
        {
            const F32 t = ((F32)y - c) / half_len;   // -1..1 along the sliver
            if (fabsf(t) > 1.f) continue;
            const F32 w = half_w * (1.f - t * t);    // widest in the middle
            if (w <= 0.f) continue;
            for (S32 x = 0; x < res; ++x)
            {
                const F32 fx = ((F32)x - c) / llmax(0.5f, w);
                if (fabsf(fx) > 1.f) continue;
                const F32 g = 1.f - fx * fx;
                splatAlpha(data, res, x, y, g * g);
            }
        }
    }
    else if (kind == UTIL_RING)
    {
        // Soft annulus for the expanding surface ripple, with the shape of the
        // wave itself baked into the colour channels as a tangent-space normal.
        //
        // A ripple is a raised crest of water travelling outward, and until now
        // the only thing said about it was where it was opaque: the lit pass
        // shaded the whole ring with the flat normal of the ground under it, so
        // it took the sun as evenly as the concrete did and read as a painted
        // circle rather than as water standing up off it. The crest has real
        // relief and it is the same relief everywhere around the ring, only
        // rotated - purely a function of how far across the band a texel sits -
        // so it bakes exactly rather than being approximated at runtime the way
        // the drops' normals are.
        //
        // The RGB the other utility shapes leave white is a tint the lit shader
        // multiplies through, so this one is read as a normal only where the
        // renderer says the generated ring is what is actually bound; a
        // developer-set ripple texture keeps the old meaning.
        const F32 radius = res * 0.36f;
        const F32 width = res * 0.1f;

        // How tall the crest stands, in units of the band's own half width.
        // Slope, not height, is what survives into the normal, so this is the
        // dial that decides whether the ring catches a light along its flanks
        // or lies down flat like the ground it is on.
        const F32 relief = 0.8f;

        for (S32 y = 0; y < res; ++y)
        {
            for (S32 x = 0; x < res; ++x)
            {
                U8* px = data + ((size_t)y * res + x) * 4;

                const F32 rx = (F32)x - c;
                const F32 ry = (F32)y - c;
                const F32 d = sqrtf(rx * rx + ry * ry);

                // Signed position across the band: -1 at the inner edge, 0 on
                // the crest, +1 at the outer
                const F32 sp = (d - radius) / width;
                const F32 band = 1.f - fabsf(sp);
                if (band <= 0.f)
                {
                    // Flat, so the filtering either side of the band has
                    // something meaning "level ground" to blend toward rather
                    // than the white that would decode as a normal pointing
                    // off in all three axes at once
                    px[0] = 128;
                    px[1] = 128;
                    px[2] = 255;
                    continue;
                }

                splatAlpha(data, res, x, y, band * band * 0.9f);

                // Height across the band is the same profile as the coverage,
                // so the relief and the visible ring are the one wave. Its
                // slope runs radially: uphill toward the crest from both
                // sides, which tilts the surface inward on the outer flank and
                // outward on the inner.
                const F32 dh_ds = -2.f * band * ((sp < 0.f) ? -1.f : 1.f);
                const F32 slope = dh_ds * relief;

                const F32 inv_d = (d > 0.5f) ? 1.f / d : 0.f;
                F32 nx = -slope * rx * inv_d;
                F32 ny = -slope * ry * inv_d;
                F32 nz = 1.f;
                const F32 inv_len = 1.f / sqrtf(nx * nx + ny * ny + nz * nz);
                nx *= inv_len;
                ny *= inv_len;
                nz *= inv_len;

                px[0] = (U8)llclamp((S32)((nx * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
                px[1] = (U8)llclamp((S32)((ny * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
                px[2] = (U8)llclamp((S32)((nz * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
            }
        }
    }
    else
    {
        drawDot(data, res, c, c, res * 0.4f, 0.9f, 0.2f);
    }

    return LLViewerTextureManager::getLocalTexture(raw.get(), true);
}

// </SS:Nexii>
