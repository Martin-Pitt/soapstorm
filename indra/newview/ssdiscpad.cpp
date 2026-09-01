/**
 * @file ssdiscpad.cpp
 * @brief See ssdiscpad.h.
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

#include "ssdiscpad.h"

#include "ssatmoenvmanager.h"

#include "llimage.h"
#include "llmath.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    // The sample lines are the four cardinal and the four diagonal axes, each rotated off its
    // exact direction by this angle. Diffraction-spike art (star bursts, flare crosses) paints
    // its spikes ALONG those exact axes, and a line that runs along a spike would read the
    // spike's tip as the disc edge - a small rotation slides every line a few texels off the
    // spike's centre by the time it reaches the disc's rim (at 64 texels a 6-degree line sits
    // ~6.7 texels clear of the spoke), so only a spike absurdly wide for its length survives.
    const F32 SS_DISC_PAD_SAMPLE_OFFSET_DEG = 6.f;

    // How many of the sixteen radial samples (eight lines, both directions) must fall in one
    // agreement window for the disc to be trusted. A clean disc shows all sixteen agreeing; a
    // couple of lines clipped by art - a stray ray, a hole, a crescent's gap - still leave a
    // clear majority. Less than this is no consensus, which is the error-out: 0 padding.
    const S32 SS_DISC_PAD_MIN_AGREE = 10;

    // The agreement window's width: samples are "the same disc" when their fractions of the
    // quad differ by no more than this (5% of the quad width, ~8 texels on a 128 texture).
    const F32 SS_DISC_PAD_AGREE_WINDOW = 0.05f;

    // Under this the derived padding is rounding noise - a disc so close to full-bleed that
    // its margin means nothing (half a percent of the quad on each side).
    const F32 SS_DISC_PAD_NOISE_EPS = 0.005f;

    // The asset's own padding ceiling - see SSAtmoEnvCelestialBody::mDiscPadding.
    const F32 SS_DISC_PAD_MAX_PADDING = 0.45f;

    // A raw image this small is still the loading placeholder, not the art.
    const S32 SS_DISC_PAD_MIN_SIDE = 4;

    // How many poll ticks a still-loading derivation may wait before it errors out to 0. At
    // the floaters' half-second cadence this lets a slow disc texture ~15 seconds before
    // giving up and letting the spinner show the full-bleed fallback.
    const S32 SS_DISC_PAD_MAX_ATTEMPTS = 30;

    // The pending pool's cap. A sky import derives the sun and moon together, so a single
    // slot would drop one of the pair whenever both are still loading; the cap only guards
    // against an author running an import on textures that never arrive one after another.
    const S32 SS_DISC_PAD_MAX_PENDING = 8;

    // The still-loading derivations an auto-derive may leave behind, re-checked one by one
    // by ssDiscPadPoll(). A job is superseded when its body gets a newer texture, and a full
    // pool drops the oldest job - the newest disc art is the one whose padding matters.
    struct SsDiscPadPendingJob
    {
        S32 mTrack = -1;
        S32 mBody = -1;
        LLUUID mTexture;
        S32 mAttempts = 0;
    };

    std::vector<SsDiscPadPendingJob> gPendingPads;

    // One radial sample: walks the line from the texture centre and returns the disc as a
    // fraction of the quad (diameter over the frame's shorter side) read along this line, or
    // 1 when the line never leaves the disc before the frame edge - the art fills the quad
    // that way, which is full-bleed. The disc's edge sits one step BEFORE the first texel
    // below the alpha threshold, hence the half-texel correction.
    F32 sampleDiscLine(const LLImageRaw& raw, F32 centre_x, F32 centre_y,
                       F32 dir_x, F32 dir_y, F32 max_t, F32 ref, S32 step_sign, U8 threshold)
    {
        const S32 width = raw.getWidth();
        const S32 height = raw.getHeight();
        const U8* data = raw.getData();

        for (S32 t = 1; t <= (S32)max_t; ++t)
        {
            const S32 px = (S32)llroundf(centre_x + dir_x * (F32)t * (F32)step_sign);
            const S32 py = (S32)llroundf(centre_y + dir_y * (F32)t * (F32)step_sign);
            if (px < 0 || px >= width || py < 0 || py >= height) return 1.f;

            const F32 alpha = (F32)data[(py * width + px) * 4 + 3];
            if (alpha < (F32)threshold)
            {
                const F32 edge_radius = (F32)t - 0.5f;
                return llclamp(2.f * edge_radius / ref, 0.f, 1.f);
            }
        }
        return 1.f;
    }
}

// The analysis itself, agnostic of the asset: reads the texture's decoded pixels and finds
// the disc's share of the quad. See ssdiscpad.h for the status contract.
SSDiscPadStatus ssDiscPadAnalyze(const LLUUID& texture_id, F32& out_padding)
{
    if (texture_id.isNull())
    {
        out_padding = 0.f;
        return SSDiscPadStatus::FAILED;
    }

    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
        texture_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);

    // The decoded pixels, kick-started through the same readback ladder the cloud deck's CPU
    // maps use when the decode has not landed yet (see SSVolCloud::fetchDeckTextures). The
    // fetch priority ask makes a slow texture arrive sooner.
    LLImageRaw* raw = tex ? tex->getRawImage() : nullptr;
    if (tex && (!raw || raw->getWidth() < tex->getWidth()
                     || raw->getHeight() < tex->getHeight()))
    {
        tex->addTextureStats((F32)MAX_IMAGE_AREA);
        tex->readbackRawImage();
        raw = tex->getRawImage();
    }
    if (!raw)
    {
        return SSDiscPadStatus::LOADING;
    }

    const S32 width = raw->getWidth();
    const S32 height = raw->getHeight();
    if (width < SS_DISC_PAD_MIN_SIDE || height < SS_DISC_PAD_MIN_SIDE)
    {
        return SSDiscPadStatus::LOADING;
    }

    // No alpha channel: nothing is transparent, so the whole quad IS the disc - full-bleed
    // is the correct reading, not an error.
    if (raw->getComponents() != 4)
    {
        out_padding = 0.f;
        return SSDiscPadStatus::OK;
    }

    const U8* data = raw->getData();

    const F32 ref = (F32)llmin(width, height); // the frame the disc fractions live in
    const F32 centre_x = (F32)(width - 1) * 0.5f;
    const F32 centre_y = (F32)(height - 1) * 0.5f;
    const F32 max_t = ref * 0.5f;

    // The disc's opacity reference is its own centre: the disc edge is where the alpha falls
    // past half the centre's strength, whatever the art's overall transparency happens to be.
    const U8 centre_alpha = data[((S32)centre_y * width + (S32)centre_x) * 4 + 3];
    if (centre_alpha <= 2)
    {
        // Nothing opaque enough to call a disc (the shader itself discards under ~2/255):
        // the lines have no edge to agree on - error out.
        out_padding = 0.f;
        return SSDiscPadStatus::FAILED;
    }
    const U8 threshold = llmax(centre_alpha / 2, 1);

    // The cardinal and the diagonal axes, each rotated off its exact direction - the slight
    // rotation is the whole point: it keeps the lines clear of diffraction spikes' thin rays
    // along those exact bearings.
    const F32 offset = SS_DISC_PAD_SAMPLE_OFFSET_DEG * DEG_TO_RAD;
    const F32 base_deg[4] = { 0.f, 45.f, 90.f, 135.f };

    std::vector<F32> fractions;
    fractions.reserve(16);
    for (S32 i = 0; i < 4; ++i)
    {
        const F32 theta = base_deg[i] * DEG_TO_RAD + offset;
        const F32 dir_x = cosf(theta);
        const F32 dir_y = sinf(theta);
        fractions.push_back(sampleDiscLine(*raw, centre_x, centre_y, dir_x, dir_y,
                                           max_t, ref, 1, threshold));
        fractions.push_back(sampleDiscLine(*raw, centre_x, centre_y, dir_x, dir_y,
                                           max_t, ref, -1, threshold));
    }

    std::sort(fractions.begin(), fractions.end());

    // Where most lines agree: the densest band of samples within the agreement window. A
    // lone line clipped by a ray or a gap lands outside it and costs nothing; the disc is
    // whatever the majority of the sixteen radial samples read together.
    const S32 count = (S32)fractions.size();
    S32 best_start = 0;
    S32 best_count = 0;
    for (S32 i = 0; i < count; ++i)
    {
        S32 in_window = 0;
        for (S32 j = i; j < count && fractions[j] - fractions[i] <= SS_DISC_PAD_AGREE_WINDOW; ++j)
        {
            ++in_window;
        }
        if (in_window > best_count)
        {
            best_count = in_window;
            best_start = i;
        }
    }

    if (best_count < SS_DISC_PAD_MIN_AGREE)
    {
        LL_WARNS("AtmoMagicEnv") << "Disc padding analysis of " << texture_id
                                 << ": the sample lines cannot agree where the disc is ("
                                 << best_count << " of " << count
                                 << " agree) - erroring out to 0 padding (full-bleed)"
                                 << LL_ENDL;
        out_padding = 0.f;
        return SSDiscPadStatus::FAILED;
    }

    F32 agreed_sum = 0.f;
    S32 agreed_count = 0;
    for (S32 j = best_start; j < count && fractions[j] - fractions[best_start] <= SS_DISC_PAD_AGREE_WINDOW; ++j)
    {
        agreed_sum += fractions[j];
        ++agreed_count;
    }

    const F32 disc_fraction = agreed_sum / (F32)agreed_count;
    F32 padding = llclamp(0.5f * (1.f - disc_fraction), 0.f, SS_DISC_PAD_MAX_PADDING);
    if (padding < SS_DISC_PAD_NOISE_EPS)
    {
        padding = 0.f;
    }

    out_padding = padding;
    return SSDiscPadStatus::OK;
}

namespace
{
    // Writes a derived padding into a live asset body, only while the body still is the one
    // the derivation was requested for - the author may have deleted it or moved on to a
    // different texture while the pixels were decoding.
    void applyDerivedPadding(S32 track_index, S32 body_index, const LLUUID& texture_id, F32 padding)
    {
        SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        if (!mgr || !mgr->hasAsset()) return;

        SSAtmoEnvAsset& asset = mgr->editable();
        if (track_index < 0 || track_index >= (S32)asset.mTracks.size()) return;
        SSAtmoEnvPlanetary& planetary = asset.mTracks[(size_t)track_index].mPlanetary;
        if (body_index < 0 || body_index >= (S32)planetary.mBodies.size()) return;

        SSAtmoEnvCelestialBody& body = planetary.mBodies[(size_t)body_index];
        if (body.mCustomTexture != texture_id) return;

        if (body.mDiscPadding == padding) return;

        LL_INFOS("AtmoMagicEnv") << "Auto-derived disc padding " << padding << " for body '"
                                 << body.mName << "' (track " << track_index << ") from texture "
                                 << texture_id << LL_ENDL;
        body.mDiscPadding = padding;
    }
}

// Derives and applies a body's disc padding when its disc texture changed - a texture pick or
// a sky import adopting the sky's disc art. See ssdiscpad.h.
void ssDiscPadAutoDerive(S32 track_index, S32 body_index, const LLUUID& texture_id)
{
    static LLCachedControl<bool> auto_pad(gSavedSettings, "SSAtmoDiscPadAuto", true);
    if (!auto_pad || texture_id.isNull())
    {
        if (!gPendingPads.empty())
        {
            gPendingPads.clear();
        }
        return;
    }

    // A job for this body is stale the moment its texture changes again - the new texture's
    // derivation below replaces it outright, before the old one's pixels can land late.
    auto superseded = [track_index, body_index](const SsDiscPadPendingJob& job)
    {
        return job.mTrack == track_index && job.mBody == body_index;
    };
    gPendingPads.erase(std::remove_if(gPendingPads.begin(), gPendingPads.end(), superseded),
                       gPendingPads.end());

    F32 padding = 0.f;
    const SSDiscPadStatus status = ssDiscPadAnalyze(texture_id, padding);
    if (status == SSDiscPadStatus::LOADING)
    {
        if ((S32)gPendingPads.size() >= SS_DISC_PAD_MAX_PENDING)
        {
            gPendingPads.erase(gPendingPads.begin()); // the oldest waiting job
        }
        SsDiscPadPendingJob job;
        job.mTrack = track_index;
        job.mBody = body_index;
        job.mTexture = texture_id;
        gPendingPads.push_back(job);
        return;
    }

    applyDerivedPadding(track_index, body_index, texture_id, padding);
}

// Re-checks the still-loading derivations; ticked from the floaters' UI polls.
void ssDiscPadPoll()
{
    static LLCachedControl<bool> auto_pad(gSavedSettings, "SSAtmoDiscPadAuto", true);
    if (!auto_pad)
    {
        gPendingPads.clear();
        return;
    }
    if (gPendingPads.empty())
    {
        return;
    }

    for (size_t i = 0; i < gPendingPads.size();)
    {
        SsDiscPadPendingJob& job = gPendingPads[i];

        F32 padding = 0.f;
        const SSDiscPadStatus status = ssDiscPadAnalyze(job.mTexture, padding);
        if (status == SSDiscPadStatus::LOADING)
        {
            if (++job.mAttempts < SS_DISC_PAD_MAX_ATTEMPTS)
            {
                ++i;
                continue;
            }
            LL_WARNS("AtmoMagicEnv") << "Disc padding analysis of " << job.mTexture
                                     << " never got its pixels - erroring out to 0 padding"
                                     << LL_ENDL;
        }

        applyDerivedPadding(job.mTrack, job.mBody, job.mTexture, padding);
        gPendingPads.erase(gPendingPads.begin() + i);
    }
}