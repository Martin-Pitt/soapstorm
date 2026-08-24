/**
 * @file ssCelestialV.glsl
 * @brief Atmo Magic: one shader for every celestial disc it draws - the
 *        body in EEP's sun slot, the body in its moon slot, and the
 *        billboards for everything else.
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

// <SS:Nexii> Atmo Magic celestial discs
//
// Its own shader rather than uniforms bolted onto the stock sun and moon
// ones. Three reasons, in order of how much they matter:
//
//  1. A GL uniform nobody sets is ZERO. Adding "how bright" and "how far to
//     drop the quad" uniforms to a stock shader means every call site that
//     binds it - now and in future, ours and upstream's - must remember to
//     set them or draw a black disc at the wrong height. A separate program
//     cannot be bound by accident.
//  2. Atmo Magic wants different constants, not tunable ones. There is no
//     legacy 50m drop here (see below), the terminator softness is a fixed
//     look, and the emissive gain is a fixed multiple. Hardcoding them says
//     so, and keeps them next to the code that reads them.
//  3. It leaves the upstream shaders untouched, so a stock environment
//     renders byte-identically and merges stay clean.
//
// What still arrives by uniform is per-BODY state - where its star is, which
// way its quad faces, whether it lights itself - because that genuinely
// differs from one disc to the next.

uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;

out vec2 vary_texcoord0;

void main()
{
    // No vertex offset of any kind.
    //
    // sunDiscV.glsl subtracts vec3(0, 0, 50) here - a legacy sky fudge, the
    // same 50 the cloud shader carries as vec3(0, 50, 0). At the sun's
    // ~1004m that is 2.85 degrees of elevation, which is invisible when
    // nothing says where the sun ought to be and glaring when an authored
    // orbit does: the disc drew nearly three degrees below the direction it
    // was placed at, while the haze glow around it - which comes from the
    // atmosphere shader and the true direction - stayed put.
    vec4 pos = modelview_projection_matrix * vec4(position.xyz, 1.0);

    // Smashed to the far end of the depth range - the value the stock SUN
    // uses, not the moon's.
    //
    // The three sky depths are stars at 1.0, sun at 0.999999 and moon at
    // 0.999991, and the clouds are at their real geometry depth. That last
    // one is the catch: the dome is ~5000m out, and this near the far plane
    // the depth curve is so compressed that 5000m lands BETWEEN the moon's
    // value and the sun's. A disc at the moon's depth is therefore nearer
    // than the clouds, so it drew in front of them and z-fought wherever
    // the two nearly coincided - the sun flickering through cloud.
    //
    // At the sun's value every disc is comfortably behind the cloud layer
    // (which is the whole separation this needs) and still in front of the
    // stars at 1.0, so a disc continues to occlude the stars behind it.
    pos.z = pos.w * 0.999999;
    gl_Position = pos;

    vary_texcoord0 = texcoord0;
}

// </SS:Nexii>
