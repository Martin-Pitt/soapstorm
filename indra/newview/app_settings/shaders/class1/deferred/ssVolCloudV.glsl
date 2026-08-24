/**
 * @file ssVolCloudV.glsl
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

uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;
in vec4 diffuse_color;

out vec2 vary_texcoord0;
out vec4 vary_color;

// Where this fragment is in the world, for the noise lookup. The map is a
// field the whole sky is carved out of, not a picture of one puff, so it
// has to be sampled by position - see ssVolCloudF.glsl.
out vec3 vary_world;

void main()
{
    // The quads arrive already built in world space - the puff field turns
    // its own positions camera-facing on the CPU, where it is also sorting
    // them back to front, so there is nothing left to orient here.
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);

    vary_texcoord0 = texcoord0;
    vary_color = diffuse_color;
    vary_world = position.xyz;
}

// </SS:Nexii>
