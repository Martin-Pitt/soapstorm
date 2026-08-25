/**
 * @file ssavatarwet.cpp
 * @brief Atmo Magic: avatar wetness. See the header.
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

#include "ssavatarwet.h"

#include "ssatmomagic.h"
#include "ssprecippreset.h"
#include "sssurfacefield.h"

#include "llcharacter.h"
#include "llglslshader.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <algorithm>

// <SS:Nexii> Atmo Magic avatar wetness

namespace
{
    // Beyond this an avatar is not tracked at all. Wetness is a specular change on a body a few pixels tall at that distance; the cost of carrying everyone in the region for it is not repaid.
    const F32 TRACK_RADIUS = 96.f;

    // How long an avatar keeps its state after we stop seeing them. Long enough to cover a teleport-and-return or a walk behind a building, short enough that a busy sim does not accumulate strangers
    // forever.
    const F64 FORGET_SECONDS = 120.0;

    // Seconds of standing in the full downpour to go from dry to soaked, and seconds of standing out of it to dry off again. Drying is deliberately much slower: a soaked coat does not stop being wet
    // the moment the cloud passes, which is the same reason the puddles drain on their own clock.
    const F32 SOAK_SECONDS = 45.f;
    const F32 DRY_SECONDS  = 240.f;

    // How quickly the exposure figure itself moves. Walking through a doorway should read as stepping out of the rain, not as a switch: without this an avatar crossing a lip flickers between
    // sheltered and not on the cell boundary underneath them.
    const F32 EXPOSURE_TAU = 1.5f;

    // Cover clearance: a stored surface this far above someone's head is a roof, and one below it is the ground they are standing on. Between the two the exposure fades rather than stepping, which
    // is what makes the edge of an awning read as an edge rather than a line.
    const F32 COVER_CLEAR = 0.35f;
    const F32 COVER_FADE  = 1.25f;
}

void SSAvatarWet::clear()
{
    mAvatars.clear();
    mShaded.clear();
    mPeakSoak = 0.f;
}

F32 SSAvatarWet::exposureAt(const LLVector3& foot_agent, F32 height) const
{
    const SSSurfaceField::Sample sample = SSSurfaceField::getInstance()->sample(foot_agent);

    // Nothing captured here: no evidence of cover, and open sky is the commoner case by far. Assuming shelter instead would leave avatars bone dry in the middle of a field whenever the capture had
    // not caught up with them.
    if (!sample.mValid) return 1.f;

    // The field stores the top of the column - the thing the weather lands on. If that is above someone's head, it is what is keeping the rain off them; if it is at their feet, it is the ground they
    // are standing on and the sky is open.
    const F32 head_z = foot_agent.mV[VZ] + height;
    const F32 above = sample.mSurfaceZ - head_z;

    if (above <= COVER_CLEAR) return 1.f;
    if (above >= COVER_FADE) return 0.f;
    return 1.f - (above - COVER_CLEAR) / (COVER_FADE - COVER_CLEAR);
}

void SSAvatarWet::idle(F32 dt)
{
    mShaded.clear();

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo || dt <= 0.f) return;

    static LLCachedControl<F32> enabled(gSavedSettings, "SSAtmoWetStrength", 1.f);
    if ((F32)enabled <= 0.f)
    {
        clear();
        return;
    }

    // Only liquid water wets anybody. Snow settles on someone rather than soaking them, and the preset knows which of the two is falling - the same mWetRate the surface field asks about.
    const SSPrecipPreset& preset = atmo->preset();
    const F32 intensity = (atmo->hasWeather() && preset.mWetRate > 0.f)
        ? llclamp(atmo->precipitation(), 0.f, 1.f) : 0.f;

    const F64 now = atmo->sharedTime();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    mPeakSoak = 0.f;

    // Distance-sorted as they are gathered, so the cap below keeps the nearest rather than whichever the character list happened to hold first.
    std::vector<std::pair<F32, Capsule> > candidates;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead()) continue;

        // Control avatars are animesh objects rather than people. They can have wetness too eventually; leaving them out for now keeps the capsule budget for the bodies anyone is looking at.
        if (avatar->isControlAvatar()) continue;

        const LLVector3 pos = avatar->getPositionAgent();
        const F32 dist_sq = (pos - cam).magVecSquared();
        if (dist_sq > TRACK_RADIUS * TRACK_RADIUS) continue;

        // The body as a capsule: from the soles up. mBodySize is the appearance's own measurement, so a tiny avatar gets a tiny capsule and a nine-foot one gets a nine-foot capsule without this
        // having to guess.
        const F32 height = llclamp(avatar->mBodySize.mV[VZ], 0.4f, 4.f);
        const F32 girth = llmax(avatar->mBodySize.mV[VX], avatar->mBodySize.mV[VY]);

        LLVector3 foot = pos;
        foot.mV[VZ] -= avatar->getPelvisToFoot();

        const F32 target = exposureAt(foot, height);

        auto found = mAvatars.find(avatar->getID());
        const bool first_sight = (found == mAvatars.end());

        State& state = mAvatars[avatar->getID()];
        state.mLastSeen = now;

        if (first_sight)
        {
            // Somebody who walks into range has a past. Starting them bone dry and easing up from zero says they were created at the edge of the tracking radius, which is visibly wrong: an avatar
            // rezzing in, or simply walking closer, spends its first half minute drying out in reverse while the weather catches up. Worse, the tracking radius is small enough that ordinary movement
            // crosses it, so the same person could arrive dry more than once in one downpour. So assume they have been where they are for a while, and give them the wetness that implies. Exposure
            // takes its measured value outright rather than fading in from sheltered, and soak takes what standing in this much rain at this much exposure would have reached - which is exactly what
            // the loop below would have converged on anyway, arrived at immediately.
            state.mExposure = target;
            state.mSoak = llclamp(target * intensity, 0.f, 1.f);
        }
        else
        {
            // Exposure eases rather than switching - see EXPOSURE_TAU.
            state.mExposure += (target - state.mExposure)
                * llclamp(dt / EXPOSURE_TAU, 0.f, 1.f);
        }

        // Soaking and drying are one exponential approach each, toward a wet target while it is raining on them and toward dry when it is not. Rate rather than target carries the intensity: a
        // drizzle should eventually soak someone who stands in it all day, just far more slowly than a downpour, and scaling the target instead would cap them permanently damp.
        const F32 drive = state.mExposure * intensity;
        if (drive > 0.f)
        {
            const F32 rate = drive / SOAK_SECONDS;
            state.mSoak += (1.f - state.mSoak) * llclamp(rate * dt, 0.f, 1.f);
        }
        else if (state.mSoak > 0.f)
        {
            state.mSoak -= state.mSoak * llclamp(dt / DRY_SECONDS, 0.f, 1.f);
            if (state.mSoak < 0.002f) state.mSoak = 0.f;
        }

        mPeakSoak = llmax(mPeakSoak, state.mSoak);
        if (state.mSoak <= 0.004f) continue;    // nothing to shade

        Capsule cap;
        cap.mFootAgent = foot;
        cap.mHeight = height;
        // Wide enough to hold clothing, hair and a rigged coat, and no wider: this is a world-space test, so anything else standing inside the capsule shades as though it were part of them.
        cap.mRadius = llclamp(girth * 0.75f, 0.3f, 0.9f);
        cap.mSoak = state.mSoak;

        candidates.push_back(std::make_pair(dist_sq, cap));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const std::pair<F32, Capsule>& a, const std::pair<F32, Capsule>& b)
              { return a.first < b.first; });

    for (size_t i = 0; i < candidates.size() && (S32)i < MAX_SHADED; ++i)
    {
        mShaded.push_back(candidates[i].second);
    }

    // Forget avatars nobody has seen for a while. Done here rather than on a separate timer because this is the only place that knows who was seen.
    for (auto it = mAvatars.begin(); it != mAvatars.end(); )
    {
        it = (now - it->second.mLastSeen > FORGET_SECONDS) ? mAvatars.erase(it) : std::next(it);
    }
}

bool SSAvatarWet::bindForShader(LLGLSLShader& shader) const
{
    static LLStaticHashedString rain_dir("ssRainDir");
    static LLStaticHashedString avatar_count("ssAvatarCount");
    static LLStaticHashedString avatar_pos("ssAvatarPos");
    static LLStaticHashedString avatar_shape("ssAvatarShape");

    // Which way the rain is going, so the windward side of a body wets first.
    LLVector3 dir = SSAtmoMagic::getInstance()->rainDirection();
    if (dir.normVec() < 0.001f) dir.setVec(0.f, 0.f, -1.f);
    shader.uniform3fv(rain_dir, 1, dir.mV);

    const S32 count = llmin((S32)mShaded.size(), MAX_SHADED);
    shader.uniform1i(avatar_count, count);
    if (count <= 0) return false;

    // Two vec4s each: where the body stands and how wet it is. Packed rather than sent as separate arrays so the whole set is two uploads however many avatars are in it.
    F32 pos[MAX_SHADED * 4];
    F32 shape[MAX_SHADED * 4];
    for (S32 i = 0; i < count; ++i)
    {
        const Capsule& cap = mShaded[(size_t)i];
        pos[i * 4 + 0] = cap.mFootAgent.mV[VX];
        pos[i * 4 + 1] = cap.mFootAgent.mV[VY];
        pos[i * 4 + 2] = cap.mFootAgent.mV[VZ];
        pos[i * 4 + 3] = cap.mRadius;

        shape[i * 4 + 0] = cap.mHeight;
        shape[i * 4 + 1] = cap.mSoak;
        shape[i * 4 + 2] = 0.f;
        shape[i * 4 + 3] = 0.f;
    }

    shader.uniform4fv(avatar_pos, count, pos);
    shader.uniform4fv(avatar_shape, count, shape);
    return true;
}

// </SS:Nexii>
