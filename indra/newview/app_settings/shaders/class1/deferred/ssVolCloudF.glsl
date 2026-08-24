/**
 * @file ssVolCloudF.glsl
 * @brief Atmo Magic volumetric cloud field - camera-facing puffs.
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

// <SS:Nexii> Atmo Magic volumetric cloud field

out vec4 frag_color;

uniform sampler2D diffuseMap;

in vec2 vary_texcoord0;
in vec4 vary_color;

void main()
{
    vec4 c = texture(diffuseMap, vary_texcoord0.xy);

    // The cloud art is a soft alpha shape; below a whisker of it there is
    // nothing to draw and blending it only costs fill.
    float a = c.a * vary_color.a;
    if (a <= 2.0 / 255.0)
    {
        discard;
    }

    // Colour comes from the CPU per puff - one sun/ambient mix per puff
    // rather than per fragment. A puff is a stand-in for a body of cloud
    // hundreds of metres across; shading it per fragment would be spending
    // real work to vary something that is one lump of vapour at this scale.
    frag_color = vec4(c.rgb * vary_color.rgb, a);
}

// </SS:Nexii>
