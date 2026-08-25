/**
 * @file ssLightningF.glsl
 * @brief Atmo Magic lightning - channel ribbons and charge sparks.
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

// <SS:Nexii> Atmo Magic lightning

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_texcoord0;
in vec4 vary_color;

// An electric-line texture, tiled along the ribbon (u across the width, v
// down the length). ss_use_tex 0 falls back to the procedural core below, so
// the bolt draws with no asset configured at all.
uniform sampler2D diffuseMap;
uniform float ss_use_tex;

void main()
{
    // Across the ribbon: 0 at one edge, 1 at the other, core in the middle.
    float across = abs(vary_texcoord0.x * 2.0 - 1.0);

    float core;
    if (ss_use_tex > 0.5)
    {
        // The texture's own alpha is the channel shape; its red carries any
        // filament detail the author drew. Multiplied by the width falloff so
        // a rectangular texture still ends softly at the ribbon's edge.
        vec4 tex = texture(diffuseMap, vary_texcoord0);
        core = tex.a * max(tex.r, 0.35) * (1.0 - across * across);
    }
    else
    {
        // A hot filament inside a soft sheath. The fourth power is what
        // makes the middle read as a discharge rather than as a painted
        // stripe - the falloff from a line source really is this steep.
        float sheath = 1.0 - across;
        core = sheath * sheath * sheath * sheath;
    }

    // Drawn additively, so colour IS brightness and alpha is unused by the
    // blend - carried anyway for anything that keys on it. vary_color's rgb
    // arrives premultiplied with the strike's brightness by the CPU.
    vec3 col = vary_color.rgb * core;

    frag_color = vec4(col, core * vary_color.a);
}

// </SS:Nexii>
