/**
 * @file ssavatarwet.h
 * @brief Atmo Magic: how wet each avatar is, and where on them.
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

#ifndef SS_AVATARWET_H
#define SS_AVATARWET_H

// <SS:Nexii> Atmo Magic avatar wetness

#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"

#include <map>
#include <vector>

class LLGLSLShader;

// Avatars soak, and they do it on their own clock rather than the ground's. The surface field is a 2D thing anchored to the world: it knows what the weather has done to the top of every column, and
// nothing at all about something standing in that column. An avatar used to pick up the ground's wetness for exactly that reason - the field lookup is by XY, so a fragment on someone's shoulder read
// the wet pavement under their feet and shaded like it. That is fixed at the source (ssFieldAt now declines to answer for anything standing above the surface it stored), and this is the intended
// version of the effect it accidentally showed. What it models: - Exposure. Rain has to actually be reaching them: standing under a roof keeps an avatar dry, and the same captured geometry the rain
// shadow uses answers that. - Accumulation. Wetness grows while exposed rather than switching on, and how fast depends on how hard it is coming down. - A vertical gradient along the body. Rain lands
// on someone from above, so shoulders and head darken first, and their feet not far behind - splashback off the ground gets them wet from the other end. The middle of the body fills in last, and
// only really in a downpour. - Decay. It dries off the same way a puddle drains: slowly, and after the rain has stopped rather than with it.
class SSAvatarWet : public LLSingleton<SSAvatarWet>
{
    LLSINGLETON_EMPTY_CTOR(SSAvatarWet);

public:
    // From SSAtmoMagic::idle, alongside the surface field's own tick.
    void idle(F32 dt);

    void clear();

    // One avatar as the shading pass needs it: a capsule to test fragments against, and how soaked the body inside it is.
    struct Capsule
    {
        LLVector3 mFootAgent;   // where the body starts, agent space
        F32 mHeight = 1.8f;     // to the top of the head
        F32 mRadius = 0.45f;    // generous: it has to cover clothing and hair
        F32 mSoak = 0.f;        // 0 dry, 1 as wet as this system goes
    };

    // The nearest few avatars with any wetness on them at all, camera-first. Capped at MAX_SHADED: a crowd is a wall of bodies and the ones behind the front row are not what anyone is looking at,
    // while every one of them would cost the whole screen another capsule test per fragment.
    static const S32 MAX_SHADED = 8;
    const std::vector<Capsule>& shaded() const { return mShaded; }

    // Uploads the above as uniforms. False when there is nothing to shade, in which case the caller can skip its avatar path entirely.
    bool bindForShader(LLGLSLShader& shader) const;

    // Simulation floater / info overlay
    S32 trackedCount() const { return (S32)mAvatars.size(); }
    F32 peakSoak() const { return mPeakSoak; }

private:
    // Per avatar, keyed by id so it survives them going out of range and coming back - someone who ducks behind a building for a moment should still be as wet as they were when they went in.
    struct State
    {
        F32 mSoak = 0.f;
        F32 mExposure = 0.f;    // smoothed, so a doorway is a fade not a step
        F64 mLastSeen = 0.0;
    };

    // How exposed an avatar is to what is falling: 1 under open sky, 0 with solid cover overhead. Read off the same captured surface the rain shadow map is built from - if the surface stored above
    // them is over their head, something is between them and the sky.
    F32 exposureAt(const LLVector3& foot_agent, F32 height) const;

    std::map<LLUUID, State> mAvatars;
    std::vector<Capsule> mShaded;
    F32 mPeakSoak = 0.f;
};

// </SS:Nexii>

#endif // SS_AVATARWET_H
