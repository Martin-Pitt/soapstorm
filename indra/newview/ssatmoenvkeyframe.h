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

// A single keyframable parameter. Time is whatever unit the caller's
// timeline uses (seconds into the day-cycle loop, in practice); valueAt()
// takes the loop length explicitly since every real caller has one (a
// track's own day length), and nextKeyframeTime()/prevKeyframeTime() wrap
// to the first/last keyframe regardless, which needs no length at all.
template <typename T>
class SSAtmoEnvKeyframed
{
public:
    explicit SSAtmoEnvKeyframed(const T& default_value = T()) : mPlainValue(default_value) {}

    bool hasKeyframes() const { return !mKeyframes.empty(); }
    size_t keyframeCount() const { return mKeyframes.size(); }
    const std::vector<SSAtmoEnvKeyframe<T>>& keyframes() const { return mKeyframes; }

    // The evaluated value at an arbitrary time. loop_length > 0 treats the
    // timeline as a loop (the day-cycle case every real caller actually
    // has): the span from the last keyframe forward to the first keyframe
    // of the *next* cycle is one more interpolated segment, not a flat
    // hold - a keyframe near midnight and one near dawn should ease
    // between each other through the wrap, the same as any other pair.
    // loop_length <= 0 (the default) keeps the old clamp-at-the-ends
    // behaviour, for a caller that genuinely isn't looping.
    T valueAt(F64 time, F64 loop_length = 0.0) const
    {
        if (mKeyframes.empty()) return mPlainValue;
        if (mKeyframes.size() == 1) return mKeyframes.front().mValue;

        if (loop_length > 0.0)
        {
            time = std::fmod(time, loop_length);
            if (time < 0.0) time += loop_length;

            const SSAtmoEnvKeyframe<T>& first = mKeyframes.front();
            const SSAtmoEnvKeyframe<T>& last = mKeyframes.back();

            // Outside [first, last] means we're in the wrap segment -
            // last, forward through the loop boundary, to first-of-the-
            // next-cycle. Shifting time (and, for the "before first"
            // case, the local position within the segment) by
            // loop_length puts both ends on the same continuous number
            // line so the interpolation math below doesn't need a special
            // case for which side of midnight we're actually on.
            if (time < first.mTime || time > last.mTime)
            {
                const F64 span = (first.mTime + loop_length) - last.mTime;
                const F64 elapsed = (time < first.mTime) ? (time + loop_length - last.mTime)
                                                          : (time - last.mTime);
                if (last.mCurve == SSAtmoEnvCurve::HOLD) return last.mValue;

                F32 t = span > 0.0 ? (F32)(elapsed / span) : 0.f;
                if (last.mCurve == SSAtmoEnvCurve::EASE)
                {
                    t = t * t * (3.f - 2.f * t); // smoothstep
                }
                return ss_atmoenv_lerp(last.mValue, first.mValue, t);
            }
        }
        else
        {
            if (time <= mKeyframes.front().mTime) return mKeyframes.front().mValue;
            if (time >= mKeyframes.back().mTime) return mKeyframes.back().mValue;
        }

        for (size_t i = 0; i + 1 < mKeyframes.size(); ++i)
        {
            const SSAtmoEnvKeyframe<T>& a = mKeyframes[i];
            const SSAtmoEnvKeyframe<T>& b = mKeyframes[i + 1];
            if (time < a.mTime || time > b.mTime) continue;

            if (a.mCurve == SSAtmoEnvCurve::HOLD) return a.mValue;

            const F64 span = b.mTime - a.mTime;
            F32 t = span > 0.0 ? (F32)((time - a.mTime) / span) : 0.f;
            if (a.mCurve == SSAtmoEnvCurve::EASE)
            {
                t = t * t * (3.f - 2.f * t); // smoothstep
            }
            return ss_atmoenv_lerp(a.mValue, b.mValue, t);
        }
        return mKeyframes.back().mValue;
    }

    bool hasKeyframeAt(F64 time, F64 epsilon = 0.01) const
    {
        return findAt(time, epsilon) >= 0;
    }

    // The editing rule from the design doc, in one place so every widget
    // that edits a keyframable field goes through the same logic rather
    // than each reimplementing "am I on a keyframe right now".
    void setValueAtHead(F64 head_time, const T& value, F64 epsilon = 0.01)
    {
        if (mKeyframes.empty())
        {
            mPlainValue = value;
            return;
        }

        const S32 at = findAt(head_time, epsilon);
        if (at >= 0)
        {
            mKeyframes[at].mValue = value;
            return;
        }

        insertKeyframe(head_time, value, SSAtmoEnvCurve::EASE);
    }

    // The keyframe-diamond toggle. Adding one at a bare value promotes the
    // value at that instant into the first keyframe rather than reaching
    // back for whatever mPlainValue happened to hold; removing the last one
    // does the reverse, so toggling never causes a visible jump either way.
    // loop_length must match whatever the caller evaluates this field with
    // elsewhere (valueAt's own default of 0 would silently switch to
    // clamp-at-the-ends semantics for the value captured here otherwise -
    // wrong on a looping day cycle, which every real caller has).
    void toggleKeyframeAtHead(F64 head_time, F64 loop_length = 0.0, F64 epsilon = 0.01)
    {
        const S32 at = findAt(head_time, epsilon);
        if (at >= 0)
        {
            mKeyframes.erase(mKeyframes.begin() + at);
            if (mKeyframes.empty())
            {
                mPlainValue = valueAt(head_time, loop_length);
            }
            return;
        }

        insertKeyframe(head_time, valueAt(head_time, loop_length), SSAtmoEnvCurve::EASE);
    }

    void setCurveAt(F64 time, SSAtmoEnvCurve curve, F64 epsilon = 0.01)
    {
        const S32 at = findAt(time, epsilon);
        if (at >= 0) mKeyframes[at].mCurve = curve;
    }

    // Chevron navigation: the nearest keyframe strictly after/before head.
    // Nothing found ahead wraps to the first keyframe (equivalently,
    // nothing found behind wraps to the last) - the timeline itself loops,
    // so "next" past the last keyframe is "the first one again" rather than
    // a dead end. Returns head_time unchanged if there are no keyframes at
    // all to jump to.
    F64 nextKeyframeTime(F64 head_time) const
    {
        if (mKeyframes.empty()) return head_time;
        for (const SSAtmoEnvKeyframe<T>& kf : mKeyframes)
        {
            if (kf.mTime > head_time + 1e-6) return kf.mTime;
        }
        return mKeyframes.front().mTime;
    }

    F64 prevKeyframeTime(F64 head_time) const
    {
        if (mKeyframes.empty()) return head_time;
        for (auto it = mKeyframes.rbegin(); it != mKeyframes.rend(); ++it)
        {
            if (it->mTime < head_time - 1e-6) return it->mTime;
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
                kf.mTime = entry.has("time") ? entry["time"].asReal() : 0.0;
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
