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

    // Filled during the same call_once, because the params structs are pure output of the profile initialisers and are then only read.
    ispc::bc7e_compress_block_params sParams;

    // Read once inside the call_once below, so every encode in a session uses the profile the blobs were stamped with.
    SSBC7Quality sQuality = SSBC7_QUALITY_BALANCED;

    // Perceptual weighting is deliberately off. It optimises a luma weighted error that suits albedo, but the same code path also encodes normal and specular maps where the channels are not colour at all and weighting them by human luminance response is simply wrong. Uniform error is correct for every channel type, which is what a backend with no knowledge of its input needs to be.
    const bool SSBC7E_PERCEPTUAL = false;

    // Three of bc7e's seven profiles, chosen from the offline benchmark rather than from their names. Measured at 512x512, single threaded, against a full-mode reference decoder:
    //
    //   ultrafast  73 Mpix/s   matches or beats the portable mode 6 backend everywhere, but shares its one blind spot: a 4x4 of several unrelated hues still scores about 11 dB
    //   veryfast    9 Mpix/s   the partitioned modes arrive and that same block jumps to 54 dB, a 43 dB step, while smooth content gains about 1.5 dB
    //   basic     2.5 Mpix/s   a further 1.8 dB on low saturation content for nearly four times the time
    //
    // BALANCED is the default because that first step is a different kind of improvement from the second: it removes a failure mode rather than sharpening an already good result. Everything past it is ordinary diminishing returns, which is what a user with time to spare can opt into.
    void ssInitBC7E()
    {
        ispc::bc7e_compress_block_init();

        switch (sQuality)
        {
            case SSBC7_QUALITY_FAST:  ispc::bc7e_compress_block_params_init_ultrafast(&sParams, SSBC7E_PERCEPTUAL); break;
            case SSBC7_QUALITY_HIGH:  ispc::bc7e_compress_block_params_init_basic(&sParams, SSBC7E_PERCEPTUAL);     break;
            default:                  ispc::bc7e_compress_block_params_init_veryfast(&sParams, SSBC7E_PERCEPTUAL);  break;
        }
    }

    // bc7e reads pixels as uint32 and writes blocks as uint64. Callers hand us byte pointers, and a byte pointer that happens to be odd would make those casts undefined, so misaligned input is staged through an aligned buffer instead. In practice the encoder wrapper passes vector storage and this never triggers; it exists so that a future caller with a stack buffer fails slowly rather than mysteriously.
    inline bool ssAligned(const void* p, size_t to)
    {
        return (reinterpret_cast<uintptr_t>(p) % to) == 0;
    }
}

void ssBC7EncodeBlocksRGBA(U32 num_blocks, const U8* rgba_blocks, U8* out_blocks)
{
    if (num_blocks == 0)
    {
        return;
    }

    std::call_once(sInitFlag, ssInitBC7E);

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
                               &sParams);

    if (stage_out)
    {
        memcpy(out_blocks, sOutStage.data(), (size_t)num_blocks * SSBC7ENC_BLOCK_BYTES);
    }
}

void ssBC7SetBlockQuality(SSBC7Quality quality)
{
    sQuality = quality;
}

// The hundreds are bc7e's range and the low numbers are the portable backend's, so a blob encoded by one is never mistaken for the other's output; the quality is folded in for the same reason, since a blob encoded at FAST is a different artefact from one encoded at HIGH. Both remain valid BC7 whatever the stamp says - what the stamp buys is that the cache re-encodes at the current setting rather than serving the old one forever.
U32 ssBC7BlockBackendVersion()
{
    return 100 + (U32)sQuality;
}

const char* ssBC7BlockBackendName()
{
    switch (sQuality)
    {
        case SSBC7_QUALITY_FAST: return "bc7e-ultrafast";
        case SSBC7_QUALITY_HIGH: return "bc7e-basic";
        default:                 return "bc7e-veryfast";
    }
}

// Apache 2.0 asks that the notices travel with the work. The full licence text ships in licenses.txt, which is the copy that satisfies the licence; this shorter line exists so the credit is also somewhere a user will actually look.
const char* ssBC7BlockBackendAttribution()
{
    return "BC7E texture compressor Copyright (C) 2018-2020 Binomial LLC, used under the Apache License 2.0.\n"
           "Built with Intel(R) ISPC, Copyright Intel Corporation, BSD 3-Clause. See licenses.txt.\n";
}
