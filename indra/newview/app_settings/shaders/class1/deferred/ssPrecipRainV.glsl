/**
 * @file ssPrecipRainV.glsl
 * @brief Atmo Magic rain particle vertex shader (SS:Nexii)
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

uniform mat4 modelview_matrix;
uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec4 diffuse_color;
in vec2 texcoord0;

void calcAtmospherics(vec3 inPositionEye);

out vec3 vary_position;
out vec4 vertex_color;
out vec2 vary_texcoord0;

void main()
{
    vec4 vert = vec4(position.xyz, 1.0);
    vec4 pos = modelview_matrix * vert;
    gl_Position = modelview_projection_matrix * vert;

    vary_position = pos.xyz;
    vary_texcoord0 = texcoord0;

    calcAtmospherics(pos.xyz);

    vertex_color = diffuse_color;
}
