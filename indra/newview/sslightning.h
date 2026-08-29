/**
 * @file sslightning.h
 * @brief Atmo Magic: lightning model - scheduling, channels, discharge phases.
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

#ifndef SS_LIGHTNING_H
#define SS_LIGHTNING_H

#include "llsingleton.h"
#include "v3math.h"
#include "v3color.h"
#include "v4color.h"
#include "v4math.h"

#include <vector>

class SSRandStream;
class LLHUDText;

enum SSStrikeKind : U8
{
    STRIKE_SHEET = 0,

    STRIKE_FORK,

    STRIKE_GROUND,

    STRIKE_KIND_COUNT
};

struct SSStrikeNode
{
    LLVector3 mPos;
    S32 mParent = -1;
    F32 mWidth = 1.f;
    F32 mReachedAt = 0.f;
    bool mTrunk = false;

    mutable F32 mOcc = 1.f;
};

struct SSStrike
{
    SSStrikeKind mKind = STRIKE_SHEET;

    F64 mFireAt = 0.0;
    F64 mCreatedAt = 0.0;

    F32 mT = 0.f;

    F32 mIntensity = 1.f;

    LLVector3 mOrigin;
    LLVector3 mGround;
    F32 mDistanceM = 0.f;
    bool mAudible = false;

    std::vector<SSStrikeNode> mChannel;

    F32 mChannelBrightness = 0.f;

    // Branch exclusion for ground strikes, filled in by buildChannel: a cone about the
    // main line's foot and a floor over the attachment that forked channels must stay out of.
    bool mBranchLimits = false;
    LLVector3 mBranchConeApex;
    LLVector3 mBranchConeAxis;
    F32 mBranchConeDot = 0.f;
    F32 mBranchFloorZ = 0.f;

    static const S32 MAX_STROKES = 4;
    S32 mStrokeCount = 0;
    F32 mStrokeAt[MAX_STROKES] = { 0.f };
    F32 mStrokeBright[MAX_STROKES] = { 0.f };

    F32 mLeaderProgress = 0.f;

    F32 mFlash = 0.f;

    F32 mCharge = 0.f;
    bool mChargeSent = false;

    LLHUDText* mDebugText = nullptr;

    mutable F32 mOccAt = -1.0e9f;
    mutable LLVector3 mOccCam;

    bool mDone = false;
    bool mThunderSent = false;
    bool mSparksSent = false;
};

class SSLightning : public LLSingleton<SSLightning>
{
    LLSINGLETON_EMPTY_CTOR(SSLightning);

public:
    void idle(F32 dt);

    const std::vector<SSStrike>& strikes() const { return mStrikes; }

    F32 flash() const { return mFlash; }

    const LLVector3& flashDirection() const { return mFlashDir; }

    S32 sceneLights(std::vector<LLVector4>& out_pos_radius,
                    std::vector<LLColor3>& out_color, S32 max_count) const;

    void triggerNow();

    void triggerGroundNow();

    void clear();

    S32 liveCount() const { return (S32)mStrikes.size(); }
    F64 nextStrikeIn() const;
    static const char* kindName(SSStrikeKind k);
    static const LLColor4& kindDebugColor(SSStrikeKind k);

    // The pending-strike debug overlay (markers and countdown label) hides this long before
    // impact, so the preview does not sit on top of the strike it announced.
    static constexpr F32 MARKER_HIDE_S = 0.5f;

private:
    void spawn(F32 intensity, F64 fire_at, F32 force_bearing = -1.f, F32 force_dist = -1.f,
               SSStrikeKind force_kind = STRIKE_KIND_COUNT, const LLVector3* force_ground = nullptr);
    void buildChannel(SSStrike& strike, F32 intensity);

    void growPath(SSStrike& strike, S32 parent,
                  const LLVector3& from, const LLVector3& to,
                  S32 levels, F32 width_start, F32 width_end,
                  F32 t_start, F32 t_end, bool trunk,
                  SSRandStream& rng, std::vector<S32>& out_nodes);

    void growBranches(SSStrike& strike, const std::vector<S32>& along,
                      S32 depth, S32 levels, F32 intensity, SSRandStream& rng,
                      F32 fecundity = 1.f);
    void advance(SSStrike& strike, F32 dt);

    std::vector<SSStrike> mStrikes;

    F64 mNextStrikeAt = -1.0;
    bool mPrepared = false;
    F32 mFlash = 0.f;
    LLVector3 mFlashDir;
};

#endif
