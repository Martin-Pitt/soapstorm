/**
 * @file sslightning.h
 * @brief Atmo Magic lightning: what strikes, where, when, and what shape it
 *        takes. This is the model only - it decides that a strike happened,
 *        builds its channel, and runs it through the phases a real discharge
 *        goes through. Drawing it and sounding it are elsewhere; both read
 *        what is here.
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

// <SS:Nexii> Atmo Magic lightning

#include "llsingleton.h"
#include "v3math.h"

#include <vector>

// The deterministic stream every Atmo Magic system draws from - defined in
// ssatmomagic.h. Forward declared rather than included: this header is the
// lightning MODEL's public face, and a renderer that includes it has no
// business being handed the whole weather manager to get at it.
class SSRandStream;

// What kind of discharge this is. Real lightning is mostly the first of
// these - the great majority of strikes never reach the ground and are seen
// only as the cloud lighting up from inside - so the weights that pick
// between them are not uniform and should not be.
enum SSStrikeKind : U8
{
    // In-cloud. No visible channel, because the cloud is in the way: what
    // you see is the cloud itself lit from within. Cheapest by far, and the
    // one to lean on for a distant storm.
    STRIKE_SHEET = 0,

    // Cloud to cloud, or cloud to air. A visible forked channel with no
    // ground termination - it wanders, branches, and stops.
    STRIKE_FORK,

    // Cloud to ground. The one with a leader that gropes downward and a
    // return stroke that fires back up when it lands, and the only one that
    // throws sparks or lights the scene properly.
    STRIKE_GROUND,

    STRIKE_KIND_COUNT
};

// One node of a discharge channel. A channel is a tree: mParent indexes back
// toward the cloud, so a renderer can draw segments without needing to know
// anything about how the tree was built.
struct SSStrikeNode
{
    LLVector3 mPos;         // agent space
    S32 mParent = -1;       // index into the channel, -1 for the root
    F32 mWidth = 1.f;       // 1 at the trunk, less on branches
    F32 mReachedAt = 0.f;   // seconds after the leader started, for the crawl
    bool mTrunk = false;    // on the path that actually reaches the ground
};

// A discharge, from the first flicker of the leader to the last of the
// afterglow. Held for its whole life, including the parts of it that are not
// visible, because the sound is still coming.
struct SSStrike
{
    SSStrikeKind mKind = STRIKE_SHEET;

    // When the return stroke fires - the moment anyone would call "the
    // lightning". Everything else is timed relative to it, forwards AND
    // backwards, because a strike is now known before it happens.
    F64 mFireAt = 0.0;
    F64 mCreatedAt = 0.0;

    // Seconds relative to mFireAt. Negative while the strike is still
    // coming: dormant, then charging, then the leader on its way down.
    F32 mT = 0.f;

    F32 mIntensity = 1.f;       // 0..1, how fierce this one is

    LLVector3 mOrigin;          // agent space, up in the cloud
    LLVector3 mGround;          // agent space, where it lands (ground strikes)
    F32 mDistanceM = 0.f;       // from the camera, for delay and attenuation
    bool mAudible = false;      // false past the shadow zone - see the cpp

    std::vector<SSStrikeNode> mChannel;

    // Brightness of the channel itself right now, 0 when nothing is drawn.
    // Follows the phases: dim while the leader propagates, brilliant on each
    // return stroke, decaying between them. The sum of the strokes below;
    // what the cloud deck lights itself by.
    F32 mChannelBrightness = 0.f;

    // The individual return strokes still glowing this frame, for ribbon
    // lightning: the ionised channel is a column of hot air, and hot air
    // drifts with the wind. Strokes come tens of milliseconds apart, so in
    // a crosswind each successive one fires down a channel displaced a
    // little from the last - which observers (and long exposures) see as
    // strokes lying side by side in a smeared ribbon. A renderer draws the
    // channel once per stroke, offset by wind x mStrokeAt.
    static const S32 MAX_STROKES = 4;
    S32 mStrokeCount = 0;
    F32 mStrokeAt[MAX_STROKES] = { 0.f };      // seconds after mFireAt
    F32 mStrokeBright[MAX_STROKES] = { 0.f };  // each stroke's glow right now

    // How far down the channel the leader has got, 0 to 1. Nodes with
    // mReachedAt beyond this have not happened yet and must not be drawn -
    // this is the "forking in the sky before it connects" the eye actually
    // sees.
    F32 mLeaderProgress = 0.f;

    // Sky/cloud lift from this strike right now, 0..1, already including
    // distance falloff. What the sky flash and the in-cloud illumination
    // both read.
    F32 mFlash = 0.f;

    // The anticipation, 0 until the charge begins and rising to 1 as the
    // strike arrives. Zero for the whole life of a strike when the effect
    // is switched off, which it is by default - nothing in real weather
    // announces a strike before it happens.
    //
    // What reads it is the crackle and the sparks gathering at the
    // attachment point, so it is a curve rather than a flag: the point of
    // the effect is that it builds.
    F32 mCharge = 0.f;
    bool mChargeSent = false;

    bool mDone = false;
    bool mThunderSent = false;  // the clap has been handed to the soundscape
    bool mSparksSent = false;   // ground impact effects have been spawned
};

class SSLightning : public LLSingleton<SSLightning>
{
    LLSINGLETON_EMPTY_CTOR(SSLightning);

public:
    // Per-frame driver, called from the manager's idle. Schedules new
    // strikes from the weather state, ages the live ones, retires the spent.
    void idle(F32 dt);

    // Everything currently alive, for a renderer to walk.
    const std::vector<SSStrike>& strikes() const { return mStrikes; }

    // Combined sky flash from every live strike, 0..1. Additive across
    // strikes and clamped, so a severe storm firing several at once reads
    // as one bright sky rather than as separate events fighting.
    F32 flash() const { return mFlash; }

    // Which way the brightest live flash is, for a directional lift. Zero
    // length when nothing is flashing.
    const LLVector3& flashDirection() const { return mFlashDir; }

    // Fire one now, wherever the weather would have put it. For the debug
    // menu and for testing without waiting on a severe sky.
    void triggerNow();

    // Everything gone, silently: teleport, system disabled.
    void clear();

    // For the info overlay.
    S32 liveCount() const { return (S32)mStrikes.size(); }
    F64 nextStrikeIn() const;
    static const char* kindName(SSStrikeKind k);

private:
    void spawn(F32 intensity, F64 fire_at);
    void buildChannel(SSStrike& strike, F32 intensity);

    // One run of channel from a to b, built by midpoint displacement and
    // appended as a chain of nodes hanging off parent. Fills out_nodes with
    // the indices it created, which is what a caller needs to hang further
    // branches off. Returns nothing else: the channel IS the output.
    void growPath(SSStrike& strike, S32 parent,
                  const LLVector3& from, const LLVector3& to,
                  S32 levels, F32 width_start, F32 width_end,
                  F32 t_start, F32 t_end, bool trunk,
                  SSRandStream& rng, std::vector<S32>& out_nodes);

    // Branches off a run, and branches off those, down to depth. Recursion
    // is the point: a channel that splits once looks like a fork, and a
    // channel that splits at every scale looks like lightning.
    void growBranches(SSStrike& strike, const std::vector<S32>& along,
                      S32 depth, S32 levels, F32 intensity, SSRandStream& rng);
    void advance(SSStrike& strike, F32 dt);

    std::vector<SSStrike> mStrikes;

    F64 mNextStrikeAt = -1.0;   // sharedTime, -1 when nothing is scheduled
    bool mPrepared = false;     // the next strike has already been built
    F32 mFlash = 0.f;
    LLVector3 mFlashDir;
};

// </SS:Nexii>

#endif // SS_LIGHTNING_H
