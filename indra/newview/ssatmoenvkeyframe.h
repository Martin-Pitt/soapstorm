/**
 * @file ssatmoenvkeyframe.h
 * @brief Atmo Magic: generic per-parameter keyframe container. See
 *        doc/atmo_magic_environment.md "Keyframes" - there is no
 *        whole-sky-snapshot keyframe like EEP; every parameter keeps its own
 *        independent set, and the editing rule is:
 *          - no keyframes at all: a plain permanent value
 *          - editing while the timeline head sits exactly on a keyframe:
 *            edits that keyframe
 *          - editing anywhere else: inserts a new keyframe at the head
 *        Non-tweenable values (the HOLD curve) step at the keyframe rather
 *        than blend - this is what an enum override (e.g. a forced
 *        precipitation type) uses, since there is no sensible value "between"
 *        Rain and Hail.
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

#ifndef SS_ATMOENVKEYFRAME_H
#define SS_ATMOENVKEYFRAME_H

// <SS:Nexii> Atmo Magic: per-parameter keyframes

#include "llsd.h"
#include "llsdutil.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3color.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Curve between this keyframe and the next. Hidden behind a simple UI in
// v1 (no graph editor) - Ease is the sensible default, Linear is offered
// for when a straight ramp actually reads better, and Hold is what a
// non-blendable value (a string/enum override) is forced to regardless of
// what gets stored, since there is no "70% of the way from Rain to Hail".
enum class SSAtmoEnvCurve : U8
{
    EASE   = 0,
    LINEAR = 1,
    HOLD   = 2
};

inline std::string ss_atmoenv_curve_name(SSAtmoEnvCurve c)
{
    switch (c)
    {
        case SSAtmoEnvCurve::LINEAR: return "linear";
        case SSAtmoEnvCurve::HOLD:   return "hold";
        default:                    return "ease";
    }
}

inline SSAtmoEnvCurve ss_atmoenv_curve_from_name(const std::string& name)
{
    if (name == "linear") return SSAtmoEnvCurve::LINEAR;
    if (name == "hold")   return SSAtmoEnvCurve::HOLD;
    return SSAtmoEnvCurve::EASE;
}

template <typename T>
struct SSAtmoEnvKeyframe
{
    // Position in the day cycle as a fraction, 0.0 to 1.0 - never seconds.
    // This is what makes a track's day length a pure playback-speed control:
    // stretching the day cannot move a keyframe relative to the cycle,
    // because a keyframe does not know how long the cycle is. (It used to
    // be absolute seconds, and every one of the resulting bugs - the
    // preview head sliding to a different time of day when day length
    // changed, keyframes past the new length silently wrapping via fmod -
    // was the same bug wearing a different hat.) Same reasoning EEP applies
    // to its own day-cycle frames, which are also normalised.
    F64 mTime = 0.0;
    T mValue{};
    SSAtmoEnvCurve mCurve = SSAtmoEnvCurve::EASE;
};

// Blend trait: numeric types lerp; anything this isn't specialised for
// falls back to returning `a` unconditionally, i.e. Hold behaviour even if
// a curve on the keyframe claims otherwise. This is what makes a
// SSAtmoEnvKeyframed<std::string> safe to use for an enum override without a
// separate code path - the container is the same, only the value type
// decides whether blending can ever mean anything.
template <typename T>
inline T ss_atmoenv_lerp(const T& a, const T& b, F32 t)
{
    return (T)(a + (b - a) * t);
}

template <>
inline std::string ss_atmoenv_lerp<std::string>(const std::string& a, const std::string& /*b*/, F32 /*t*/)
{
    return a;
}

// Curve a freshly created keyframe gets, per value type - the companion of
// the lerp trait above. A type that cannot blend (a string enum, an asset
// id) gets HOLD, so the stored curve tells the truth about what evaluation
// will actually do, rather than recording an "ease" the lerp specialisation
// silently ignores - which also lets the floater's ghost overlay draw these
// as the value-in-force-across-a-span markers they really are.
template <typename T>
inline SSAtmoEnvCurve ss_atmoenv_default_curve() { return SSAtmoEnvCurve::EASE; }

template <>
inline SSAtmoEnvCurve ss_atmoenv_default_curve<std::string>() { return SSAtmoEnvCurve::HOLD; }

// An asset id has no meaningful midpoint either - blending halfway between
// two normal maps is not a texture, it is nonsense - so like std::string
// this holds rather than interpolates regardless of the curve on the
// keyframe. LLColor3/LLVector2 fall through to the generic template above:
// both have the operator+/operator-/operator*(F32) it needs, and both
// genuinely do interpolate componentwise.
template <>
inline LLUUID ss_atmoenv_lerp<LLUUID>(const LLUUID& a, const LLUUID& /*b*/, F32 /*t*/)
{
    return a;
}

template <>
inline SSAtmoEnvCurve ss_atmoenv_default_curve<LLUUID>() { return SSAtmoEnvCurve::HOLD; }

// Near-equality trait, the companion collapseIfConstant() compares with.
// The generic form is for plain scalars; colours and vectors compare
// componentwise; an asset id or a string enum has no meaningful epsilon,
// so those compare exactly (their specialisations ignore it), the same
// way their lerp specialisations hold rather than blend.
template <typename T>
inline bool ss_atmoenv_near_equal(const T& a, const T& b, F32 epsilon)
{
    return std::fabs((F64)(a - b)) <= (F64)epsilon;
}

template <>
inline bool ss_atmoenv_near_equal<LLColor3>(const LLColor3& a, const LLColor3& b, F32 epsilon)
{
    return std::fabs(a.mV[0] - b.mV[0]) <= epsilon
        && std::fabs(a.mV[1] - b.mV[1]) <= epsilon
        && std::fabs(a.mV[2] - b.mV[2]) <= epsilon;
}

template <>
inline bool ss_atmoenv_near_equal<LLVector2>(const LLVector2& a, const LLVector2& b, F32 epsilon)
{
    return std::fabs(a.mV[0] - b.mV[0]) <= epsilon
        && std::fabs(a.mV[1] - b.mV[1]) <= epsilon;
}

template <>
inline bool ss_atmoenv_near_equal<LLUUID>(const LLUUID& a, const LLUUID& b, F32 /*epsilon*/)
{
    return a == b;
}

template <>
inline bool ss_atmoenv_near_equal<std::string>(const std::string& a, const std::string& b, F32 /*epsilon*/)
{
    return a == b;
}

// value_to_sd / value_from_sd traits - specialise per stored type. Kept as
// free functions rather than a functor passed to every call site, since a
// keyframed field's type doesn't change after it's declared.
template <typename T> LLSD ss_atmoenv_value_to_sd(const T& v);
template <typename T> T ss_atmoenv_value_from_sd(const LLSD& sd, const T& fallback);

template <> inline LLSD ss_atmoenv_value_to_sd<F32>(const F32& v) { return (LLSD::Real)v; }
template <> inline F32 ss_atmoenv_value_from_sd<F32>(const LLSD& sd, const F32& fallback)
{
    return sd.isReal() || sd.isInteger() ? (F32)sd.asReal() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<std::string>(const std::string& v) { return v; }
template <> inline std::string ss_atmoenv_value_from_sd<std::string>(const LLSD& sd, const std::string& fallback)
{
    return sd.isString() ? sd.asString() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<bool>(const bool& v) { return v; }
template <> inline bool ss_atmoenv_value_from_sd<bool>(const LLSD& sd, const bool& fallback)
{
    return sd.isBoolean() ? sd.asBoolean() : fallback;
}

template <> inline LLSD ss_atmoenv_value_to_sd<LLUUID>(const LLUUID& v) { return v; }
template <> inline LLUUID ss_atmoenv_value_from_sd<LLUUID>(const LLSD& sd, const LLUUID& fallback)
{
    return sd.isUUID() ? sd.asUUID() : fallback;
}

// Colour and 2D vector both serialise as a plain array of reals rather than
// LLSD's own colour/vector types: an Atmo Magic notecard is meant to stay
// hand-editable, and [0.1, 0.2, 0.3] reads as obviously-a-colour where a
// packed binary blob does not.
template <> inline LLSD ss_atmoenv_value_to_sd<LLColor3>(const LLColor3& v)
{
    LLSD sd = LLSD::emptyArray();
    sd.append((LLSD::Real)v.mV[0]);
    sd.append((LLSD::Real)v.mV[1]);
    sd.append((LLSD::Real)v.mV[2]);
    return sd;
}
template <> inline LLColor3 ss_atmoenv_value_from_sd<LLColor3>(const LLSD& sd, const LLColor3& fallback)
{
    if (!sd.isArray() || sd.size() < 3) return fallback;
    return LLColor3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal());
}

template <> inline LLSD ss_atmoenv_value_to_sd<LLVector2>(const LLVector2& v)
{
    LLSD sd = LLSD::emptyArray();
    sd.append((LLSD::Real)v.mV[0]);
    sd.append((LLSD::Real)v.mV[1]);
    return sd;
}
template <> inline LLVector2 ss_atmoenv_value_from_sd<LLVector2>(const LLSD& sd, const LLVector2& fallback)
{
    if (!sd.isArray() || sd.size() < 2) return fallback;
    return LLVector2((F32)sd[0].asReal(), (F32)sd[1].asReal());
}

// A single keyframable parameter. Every time here is a phase in [0, 1) -
// see SSAtmoEnvKeyframe::mTime. The loop length is therefore always exactly
// 1.0 and never has to be passed in, which is what stops a caller from
// evaluating a field against a different day length than the one its
// keyframes were authored under.
template <typename T>
class SSAtmoEnvKeyframed
{
public:
    explicit SSAtmoEnvKeyframed(const T& default_value = T()) : mPlainValue(default_value) {}

    bool hasKeyframes() const { return !mKeyframes.empty(); }
    size_t keyframeCount() const { return mKeyframes.size(); }
    const std::vector<SSAtmoEnvKeyframe<T>>& keyframes() const { return mKeyframes; }

    // The evaluated value at a point in the cycle. The span from the last
    // keyframe forward to the first keyframe of the next cycle is one more
    // interpolated segment, not a flat hold - a keyframe near midnight and
    // one near dawn ease between each other through the wrap the same as
    // any other pair.
    T valueAt(F64 phase) const
    {
        if (mKeyframes.empty()) return mPlainValue;
        if (mKeyframes.size() == 1) return mKeyframes.front().mValue;

        phase = wrapPhase(phase);

        const SSAtmoEnvKeyframe<T>& first = mKeyframes.front();
        const SSAtmoEnvKeyframe<T>& last  = mKeyframes.back();

        // Outside [first, last] means we're in the wrap segment - last,
        // forward through the end of the cycle, to first-of-the-next.
        // Shifting by one whole cycle puts both ends on the same continuous
        // number line, so the interpolation below needs no special case for
        // which side of the boundary we're actually on.
        if (phase < first.mTime || phase > last.mTime)
        {
            if (last.mCurve == SSAtmoEnvCurve::HOLD) return last.mValue;

            const F64 span = (first.mTime + 1.0) - last.mTime;
            const F64 elapsed = (phase < first.mTime) ? (phase + 1.0 - last.mTime)
                                                      : (phase - last.mTime);
            F32 t = span > 0.0 ? (F32)(elapsed / span) : 0.f;
            if (last.mCurve == SSAtmoEnvCurve::EASE)
            {
                t = t * t * (3.f - 2.f * t); // smoothstep
            }
            return ss_atmoenv_lerp(last.mValue, first.mValue, t);
        }

        for (size_t i = 0; i + 1 < mKeyframes.size(); ++i)
        {
            const SSAtmoEnvKeyframe<T>& a = mKeyframes[i];
            const SSAtmoEnvKeyframe<T>& b = mKeyframes[i + 1];
            if (phase < a.mTime || phase > b.mTime) continue;

            if (a.mCurve == SSAtmoEnvCurve::HOLD) return a.mValue;

            const F64 span = b.mTime - a.mTime;
            F32 t = span > 0.0 ? (F32)((phase - a.mTime) / span) : 0.f;
            if (a.mCurve == SSAtmoEnvCurve::EASE)
            {
                t = t * t * (3.f - 2.f * t); // smoothstep
            }
            return ss_atmoenv_lerp(a.mValue, b.mValue, t);
        }
        return mKeyframes.back().mValue;
    }

    // Default tolerance for "is the head on this keyframe". A phase, so
    // 0.001 is a thousandth of the cycle - about 14 seconds of a 4-hour day,
    // and far finer than the 1/32 grid the floater's scrubber snaps to.
    static constexpr F64 PHASE_EPSILON = 0.001;

    bool hasKeyframeAt(F64 phase, F64 epsilon = PHASE_EPSILON) const
    {
        return findAt(wrapPhase(phase), epsilon) >= 0;
    }

    // The editing rule from the design doc, in one place so every widget
    // that edits a keyframable field goes through the same logic rather
    // than each reimplementing "am I on a keyframe right now".
    void setValueAtHead(F64 head_phase, const T& value, F64 epsilon = PHASE_EPSILON)
    {
        if (mKeyframes.empty())
        {
            mPlainValue = value;
            return;
        }

        head_phase = wrapPhase(head_phase);

        const S32 at = findAt(head_phase, epsilon);
        if (at >= 0)
        {
            mKeyframes[at].mValue = value;
            return;
        }

        insertKeyframe(head_phase, value, ss_atmoenv_default_curve<T>());
    }

    // The keyframe-diamond toggle. Adding one at a bare value promotes the
    // value at that instant into the first keyframe rather than reaching
    // back for whatever mPlainValue happened to hold; removing the last one
    // does the reverse, so toggling never causes a visible jump either way.
    void toggleKeyframeAtHead(F64 head_phase, F64 epsilon = PHASE_EPSILON)
    {
        head_phase = wrapPhase(head_phase);

        const S32 at = findAt(head_phase, epsilon);
        if (at >= 0)
        {
            mKeyframes.erase(mKeyframes.begin() + at);
            if (mKeyframes.empty())
            {
                mPlainValue = valueAt(head_phase);
            }
            return;
        }

        insertKeyframe(head_phase, valueAt(head_phase), ss_atmoenv_default_curve<T>());
    }

    // Bulk-seeding companion (see addKeyframesFromSky): if every keyframe
    // holds effectively the same value, the field is a constant wearing
    // animation clothing - collapse it back to a plain value so it doesn't
    // carry N redundant keyframes into every notecard it's saved to.
    // Comparison is the type's near-equality trait: componentwise for
    // colours/vectors, exact for ids and strings (see ss_atmoenv_near_equal).
    // A single keyframe collapses too - it evaluates identically everywhere
    // by definition.
    void collapseIfConstant(F32 epsilon)
    {
        if (mKeyframes.empty()) return;
        const T first = mKeyframes.front().mValue;
        for (size_t i = 1; i < mKeyframes.size(); ++i)
        {
            if (!ss_atmoenv_near_equal(mKeyframes[i].mValue, first, epsilon)) return;
        }
        mPlainValue = first;
        mKeyframes.clear();
    }

    void setCurveAt(F64 phase, SSAtmoEnvCurve curve, F64 epsilon = PHASE_EPSILON)
    {
        const S32 at = findAt(wrapPhase(phase), epsilon);
        if (at >= 0) mKeyframes[at].mCurve = curve;
    }

    // Chevron navigation: the nearest keyframe strictly after/before head.
    // Nothing found ahead wraps to the first keyframe (equivalently,
    // nothing found behind wraps to the last) - the cycle loops, so "next"
    // past the last keyframe is "the first one again" rather than a dead
    // end. Returns head_phase unchanged if there are no keyframes at all.
    F64 nextKeyframeTime(F64 head_phase) const
    {
        if (mKeyframes.empty()) return head_phase;
        head_phase = wrapPhase(head_phase);
        for (const SSAtmoEnvKeyframe<T>& kf : mKeyframes)
        {
            if (kf.mTime > head_phase + 1e-6) return kf.mTime;
        }
        return mKeyframes.front().mTime;
    }

    F64 prevKeyframeTime(F64 head_phase) const
    {
        if (mKeyframes.empty()) return head_phase;
        head_phase = wrapPhase(head_phase);
        for (auto it = mKeyframes.rbegin(); it != mKeyframes.rend(); ++it)
        {
            if (it->mTime < head_phase - 1e-6) return it->mTime;
        }
        return mKeyframes.back().mTime;
    }

    LLSD asLLSD() const
    {
        if (mKeyframes.empty())
        {
            // A bare value serialises as itself, not a one-element wrapper -
            // a notecard for an environment with nothing keyframed should
            // read like plain settings, not like a degenerate animation.
            return ss_atmoenv_value_to_sd(mPlainValue);
        }

        LLSD sd = LLSD::emptyMap();
        LLSD kfs = LLSD::emptyArray();
        for (const SSAtmoEnvKeyframe<T>& kf : mKeyframes)
        {
            LLSD entry = LLSD::emptyMap();
            entry["time"] = kf.mTime;
            entry["value"] = ss_atmoenv_value_to_sd(kf.mValue);
            entry["curve"] = ss_atmoenv_curve_name(kf.mCurve);
            kfs.append(entry);
        }
        sd["keyframes"] = kfs;
        return sd;
    }

    void fromLLSD(const LLSD& sd, const T& fallback)
    {
        mKeyframes.clear();

        if (sd.isMap() && sd.has("keyframes") && sd["keyframes"].isArray())
        {
            for (const LLSD& entry : llsd::inArray(sd["keyframes"]))
            {
                SSAtmoEnvKeyframe<T> kf;
                kf.mTime = wrapPhase(entry.has("time") ? entry["time"].asReal() : 0.0);
                kf.mValue = ss_atmoenv_value_from_sd<T>(entry["value"], fallback);
                kf.mCurve = ss_atmoenv_curve_from_name(entry.has("curve") ? entry["curve"].asString() : "ease");
                mKeyframes.push_back(kf);
            }
            std::sort(mKeyframes.begin(), mKeyframes.end(),
                      [](const SSAtmoEnvKeyframe<T>& a, const SSAtmoEnvKeyframe<T>& b) { return a.mTime < b.mTime; });
            mPlainValue = mKeyframes.empty() ? fallback : mKeyframes.front().mValue;
            return;
        }

        // Anything else (a bare scalar, or a shape this version doesn't
        // recognise) is treated as a plain value rather than a hard parse
        // failure - a field this container doesn't understand shouldn't
        // sink the whole asset.
        mPlainValue = ss_atmoenv_value_from_sd<T>(sd, fallback);
    }

private:
    // Any phase folded into [0, 1). Every public entry point runs its input
    // through this, so callers can hand over a raw scrubber value or an
    // accumulated wall-clock fraction without pre-normalising it.
    static F64 wrapPhase(F64 phase)
    {
        phase = std::fmod(phase, 1.0);
        if (phase < 0.0) phase += 1.0;
        return phase;
    }

    S32 findAt(F64 time, F64 epsilon) const
    {
        for (size_t i = 0; i < mKeyframes.size(); ++i)
        {
            if (std::fabs(mKeyframes[i].mTime - time) < epsilon) return (S32)i;
        }
        return -1;
    }

    void insertKeyframe(F64 time, const T& value, SSAtmoEnvCurve curve)
    {
        SSAtmoEnvKeyframe<T> kf;
        kf.mTime = time;
        kf.mValue = value;
        kf.mCurve = curve;

        auto it = std::lower_bound(mKeyframes.begin(), mKeyframes.end(), time,
            [](const SSAtmoEnvKeyframe<T>& k, F64 t) { return k.mTime < t; });
        mKeyframes.insert(it, kf);
    }

    T mPlainValue{};
    std::vector<SSAtmoEnvKeyframe<T>> mKeyframes; // kept sorted by mTime
};

// </SS:Nexii>

#endif // SS_ATMOENVKEYFRAME_H
