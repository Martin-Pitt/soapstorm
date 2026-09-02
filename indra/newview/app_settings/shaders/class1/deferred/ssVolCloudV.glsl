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

// The far-field squash: x knee, y cap (just inside the far plane), z virtual field radius. Beyond the knee, vertices pull radially toward the camera - each keeps its exact ray, so the
// projected image is identical to the true positions and only the depth compresses - what lets the field read out to 5km through a 2km far plane. vary_world stays the TRUE position: the
// fragment shader samples noise and measures distances in the real world, never the compressed one.
uniform vec3 ss_squash;
uniform vec3 ss_cam_pos;

in vec3 position;
in vec2 texcoord0;
in vec4 diffuse_color;

out vec2 vary_texcoord0;
out vec4 vary_color;

// Where this fragment sits in the world, for the noise lookup. The map is a field the whole sky is carved out of, not a picture of one puff, so it samples by position - see ssVolCloudF.glsl.
out vec3 vary_world;

void main()
{
    // The quads arrive pre-built in world space - the puff field turns its positions camera-facing on the CPU, where it also sorts them back to front, so nothing is left to orient
    // here.
    vec3 rel = position.xyz - ss_cam_pos;
    float d = length(rel);
    vec3 drawn_pos = position.xyz;
    if (d > ss_squash.x && ss_squash.z > ss_squash.x)
    {
        float drawn = ss_squash.x + (d - ss_squash.x) * (ss_squash.y - ss_squash.x) / (ss_squash.z - ss_squash.x);
        drawn = min(drawn, ss_squash.y * 0.999);
        drawn_pos = ss_cam_pos + rel * (drawn / d);
    }
    gl_Position = modelview_projection_matrix * vec4(drawn_pos, 1.0);

    vary_texcoord0 = texcoord0;
    vary_color = diffuse_color;

    // The DRAWN position, deliberately: the fragment shader inverts the squash per fragment to recover the true one. Interpolating the true position as a varying warped it mid-quad -
    // perspective correction follows the drawn geometry, not the true one - which showed as noise swimming on the far rim.
    vary_world = drawn_pos;
}

