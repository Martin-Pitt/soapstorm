/**
 * @file class1/deferred/ssSurfaceCommitF.glsl
 * @brief Atmo Magic: put a reworked specular buffer back into the gbuffer.
 *
 * The wetness pass cannot write the specular attachment while it is reading
 * it, so it works into a scratch target and this puts the result back. A draw
 * rather than a texture copy: glCopyTexSubImage2D has to be told which texture
 * it is copying into by way of whichever texture unit happens to be active,
 * and the renderer's own binding cache is entitled to skip the call that sets
 * that. Writing through the framebuffer says where the pixels go in the only
 * terms the driver cannot misread.
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

// <SS:Nexii> Atmo Magic surface field

// Four outputs so the locations the linker hands out line up with the gbuffer's attachments. Only the second is written; the rest are masked off with glDrawBuffers at the call site, and writes to a
// buffer set to GL_NONE go nowhere, so the diffuse, normal and emissive attachments are not so much as touched.
out vec4 frag_data[4];

in vec2 vary_fragcoord;

uniform sampler2D ssCommitSource;

// Diagnostic. Above zero, the diffuse attachment is painted a flat magenta as well as the specular being written. Albedo multiplies straight into the final colour on every path there is, so this
// cannot be mistaken for a subtle lighting change, cannot be swallowed by an overcast sky, and cannot be argued with. It separates "nothing this pass writes lands anywhere" from "the specular buffer
// lands and the lighting does nothing with it", which are the only two possibilities left and want opposite fixes.
uniform float ssCommitDebugPaint;

void main()
{
    frag_data[1] = texture(ssCommitSource, vary_fragcoord.xy);

    if (ssCommitDebugPaint > 0.0)
    {
        frag_data[0] = vec4(1.0, 0.0, 1.0, 0.0);

        // The diffuse paint above is already proven to reach the screen (the freeze-frame test). This is the same proof for the OTHER channel this pass writes - the one the wetness effect actually
        // uses - which has never independently been checked. ORM = (0 occlusion, 0 roughness, 1 metal): every PBR surface should go a hard mirror, and every legacy surface should carry a strange
        // blue-tinted specular. If this channel reaches the screen the same way diffuse did, this is unmistakable regardless of lighting or haze.
        frag_data[1] = vec4(0.0, 0.0, 1.0, 0.0);
    }
}

// </SS:Nexii>
