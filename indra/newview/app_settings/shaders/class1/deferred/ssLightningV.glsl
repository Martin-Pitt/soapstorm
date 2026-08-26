/**
 * @file ssLightningV.glsl
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

uniform mat4 modelview_projection_matrix;

// The same far-field squash the puff field's vertex shader applies, from the same ss_squash uniform - one mapping, so a bolt inside a far cloud stays inside it in drawn depth too. Per vertex,
// each keeping its exact ray from the camera, which is what replaced the old CPU per-strike scale (that one collapsed everything beyond a limit onto one shell; this compresses progressively and
// a 5km crawler spanning the knee bends correctly through it).
uniform vec3 ss_squash;
uniform vec3 ss_cam_pos;

in vec3 position;
in vec2 texcoord0;
in vec4 diffuse_color;

out vec2 vary_texcoord0;
out vec4 vary_color;

void main()
{
    // Ribbons arrive already built in world space, camera-faced on the CPU alongside everything else about them - same arrangement as the puff field, and for the same reason: the geometry is a
    // handful of quads a few times a minute, not something worth a geometry stage.
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
}

// </SS:Nexii>
