/**
 * @file ssbc7block_bc7e.cpp
 * @brief BC7 block backend: Binomial's bc7e, compiled by ISPC - see doc/super_compressed_textures.md
 *
 * This file is Soapstorm's own code and carries Soapstorm's licence. It only calls bc7e's exported C
 * API through the header ISPC generates; no bc7e source is copied here. bc7e itself lives unmodified
 * in indra/llimage/bc7e/ under the Apache Licence 2.0, with its LICENSE and README kept verbatim
 * beside it, and is recorded in indra/newview/licenses-*.txt alongside the other third party
 * components the viewer links. See doc/super_compressed_textures.md for why it is kept at arm's
 * length rather than merged into this module.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssbc7encoder.h"

#include "bc7e_ispc.h"

#include <cstring>
#include <mutex>

// bc7e replaces the portable mode 6 encoder in ssbc7block_mode6.cpp, and the two are alternatives: exactly one is compiled in, selected by indra/llimage/CMakeLists.txt on whether ISPC was configured. Both implement the same seam, so nothing above this file knows which one it got.
//
// The reason to prefer bc7e is coverage rather than speed. The portable backend implements mode 6 only, which fits one line through RGBA space and so handles a 4x4 containing several unrelated colours badly - measured at 11 dB on such content against 52-63 dB on everything else. bc7e implements the partitioned modes that exist precisely for that case.

namespace
{
    // bc7e builds global lookup tables on first use and returns blocks of zeroes if that is skipped, so the call is forced through std::call_once rather than left to whoever encodes first. Encoding runs on worker threads, so "once" has to mean once across all of them.
    std::once_flag sInitFlag;

    // <SS:Nexii> Squeeze adaptive quality - EVERY profile's parameter block is built during that same call_once, and this array is READ ONLY from then on. That is the whole synchronisation story for profile selection: bc7e_compress_blocks takes a const params pointer, so an encode picks its profile with an array index and needs no mutex, no atomic and no re-init even while the controller is changing its mind between one texture and the next.
    //
    // The alternative - re-initialising one shared struct whenever the profile changes - would put a writer on the hot path of sixteen workers for a struct they are all reading, which is a data race for the sake of saving three hundred bytes.
    ispc::bc7e_compress_block_params sParams[SSBC7_QUALITY_COUNT];

    // Perceptual weighting is deliberately off. It optimises a luma weighted error that suits albedo, but the same code path also encodes normal and specular maps where the channels are not colour at all and weighting them by human luminance response is simply wrong. Uniform error is correct for every channel type, which is what a backend with no knowledge of its input needs to be.
    const bool SSBC7E_PERCEPTUAL = false;

    // Three of bc7e's seven profiles, chosen from the offline benchmark rather than from their names. Measured at 512x512, single threaded, against a full-mode reference decoder:
    //
    //   ultrafast  73 Mpix/s   matches or beats the portable mode 6 backend everywhere, but shares its one blind spot: a 4x4 of several unrelated hues still scores about 11 dB
    //   veryfast    9 Mpix/s   the partitioned modes arrive and that same block jumps to 54 dB, a 43 dB step, while smooth content gains about 1.5 dB
    //   basic     2.5 Mpix/s   a further 1.8 dB on low saturation content
    //   slow      3.4 Mpix/s   0.6 dB BEHIND basic on skin, 45 dB AHEAD of it on multi-hue blocks, which it encodes losslessly, and faster than it
    //
    // HIGH maps to slow rather than basic because slow measurably DOMINATES it: repeatably faster and bit exact on the case both of the cheaper profiles merely make acceptable, for six tenths of a decibel on smooth content. The ordering is not monotonic and the names invite the wrong guess, which is exactly why the choice is made from the benchmark rather than from them.
    //
    // HIGH is the default because the economics changed once the pool widened and the promotion engine landed. A texture is encoded ONCE and then read from disk for the life of the cache, so encode time is a one-off and quality is permanent - and the pool is supply limited rather than compute limited, so its workers spend most of their time waiting on fetches regardless. Sixteen workers at slow is roughly fifty Mpix/s of aggregate throughput, comfortably more than four workers at veryfast managed, so this is better quality AND more of it than the previous build produced.
    void ssInitBC7E()
    {
        ispc::bc7e_compress_block_init();

        ispc::bc7e_compress_block_params_init_ultrafast(&sParams[SSBC7_QUALITY_FAST],     SSBC7E_PERCEPTUAL);
        ispc::bc7e_compress_block_params_init_veryfast(&sParams[SSBC7_QUALITY_BALANCED],  SSBC7E_PERCEPTUAL);
        ispc::bc7e_compress_block_params_init_slow(&sParams[SSBC7_QUALITY_HIGH],          SSBC7E_PERCEPTUAL);
    }

    // bc7e reads pixels as uint32 and writes blocks as uint64. Callers hand us byte pointers, and a byte pointer that happens to be odd would make those casts undefined, so misaligned input is staged through an aligned buffer instead. In practice the encoder wrapper passes vector storage and this never triggers; it exists so that a future caller with a stack buffer fails slowly rather than mysteriously.
    inline bool ssAligned(const void* p, size_t to)
    {
        return (reinterpret_cast<uintptr_t>(p) % to) == 0;
    }
}

void ssBC7EncodeBlocksRGBA(U32 num_blocks, const U8* rgba_blocks, U8* out_blocks, SSBC7Quality quality)
{
    if (num_blocks == 0)
    {
        return;
    }

    std::call_once(sInitFlag, ssInitBC7E);

    // <SS:Nexii> Squeeze adaptive quality - a profile outside the table would index past the end of a static array, so it is clamped rather than trusted. BALANCED is the fallback because it is the cheapest profile that still has the partitioned modes, so a corrupted value degrades to something usable rather than to the one profile with a known blind spot.
    U32 profile = (U32)quality;
    if (profile >= (U32)SSBC7_QUALITY_COUNT) profile = (U32)SSBC7_QUALITY_BALANCED;

    const U8* in = rgba_blocks;
    U8* out = out_blocks;

    // Staging buffers are thread local so that two workers encoding at once cannot share them, and are kept across calls so the rare misaligned caller does not allocate per batch.
    static thread_local std::vector<U8> sInStage;
    static thread_local std::vector<U8> sOutStage;

    const bool stage_in = !ssAligned(rgba_blocks, sizeof(uint32_t));
    const bool stage_out = !ssAligned(out_blocks, sizeof(uint64_t));

    if (stage_in)
    {
        const size_t bytes = (size_t)num_blocks * SSBC7ENC_BLOCK_TEXELS * 4;
        if (sInStage.size() < bytes) sInStage.resize(bytes);
        memcpy(sInStage.data(), rgba_blocks, bytes);
        in = sInStage.data();
    }

    if (stage_out)
    {
        const size_t bytes = (size_t)num_blocks * SSBC7ENC_BLOCK_BYTES;
        if (sOutStage.size() < bytes) sOutStage.resize(bytes);
        out = sOutStage.data();
    }

    ispc::bc7e_compress_blocks(num_blocks,
                               reinterpret_cast<uint64_t*>(out),
                               reinterpret_cast<const uint32_t*>(in),
                               &sParams[profile]);

    if (stage_out)
    {
        memcpy(out_blocks, sOutStage.data(), (size_t)num_blocks * SSBC7ENC_BLOCK_BYTES);
    }
}

// The hundreds are bc7e's range and the low numbers are the portable backend's, so a blob encoded by one is never mistaken for the other's output. Both remain valid BC7 whatever the stamp says - what the stamp buys is that the cache is wiped rather than read by a decoder that means something else by the same bytes.
//
// <SS:Nexii> Squeeze adaptive quality - a PLAIN BACKEND ID with no quality term, and it must stay that way. Folding the profile in here is what made a mid-session quality change wipe the entire store, which is precisely what adaptive selection would do several times an hour. The profile lives in SSBC7Record::mQuality now, and a record encoded at a low profile is upgraded in place by the idle pass rather than being invalidated wholesale. This value changing again should mean the BLOCK FORMAT changed, never that a setting did.
U32 ssBC7BlockBackendVersion()
{
    return 100;
}

const char* ssBC7BlockBackendName()
{
    return "bc7e";
}

// <SS:Nexii> Squeeze adaptive quality - the bc7e profile each level actually maps to, so the log line that reports a change names the thing that changed rather than an enum ordinal. HIGH is `slow` and not `basic` because slow measurably DOMINATES it: repeatably faster and bit exact on multi-hue blocks, for six tenths of a decibel on smooth content.
const char* ssBC7QualityName(SSBC7Quality quality)
{
    switch (quality)
    {
        case SSBC7_QUALITY_FAST: return "ultrafast";
        case SSBC7_QUALITY_HIGH: return "slow";
        default:                 return "veryfast";
    }
}

bool ssBC7BackendHasQualityProfiles()
{
    return true;
}

// Apache 2.0 asks that the notices travel with the work. The full licence text ships in licenses.txt, which is the copy that satisfies the licence; this shorter line exists so the credit is also somewhere a user will actually look.
const char* ssBC7BlockBackendAttribution()
{
    return "BC7E texture compressor Copyright (C) 2018-2020 Binomial LLC, used under the Apache License 2.0.\n"
           "Built with Intel(R) ISPC, Copyright Intel Corporation, BSD 3-Clause. See licenses.txt.\n";
}
