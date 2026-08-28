/**
 * @file hudDownsampleF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;

uniform int hud_supersample; // supersample factor, 2 or 4
uniform vec2 hud_texel_size; // 1 / source target dimensions

in vec2 vary_fragcoord;

// <SS:Nexii> Box resolve of the supersampled HUD target. The source holds premultiplied colour in rgb and coverage in alpha (see LLRender::setCoverageAlphaMode), and both are linear in coverage, so a plain unweighted average of the block is the correct resolve for each. Sampling is point filtered, so every tap lands on exactly one source texel and the block is counted once. Rationale in doc/hud_supersampling.md.
void main()
{
    // vary_fragcoord is the destination pixel centre in [0,1]. Walk back to the leading edge of the factor x factor source block it covers, then in by half a texel to land on the first texel's centre.
    vec2 base = vary_fragcoord - (0.5 * float(hud_supersample) - 0.5) * hud_texel_size;

    vec4 sum = vec4(0.0);

    for (int y = 0; y < hud_supersample; ++y)
    {
        for (int x = 0; x < hud_supersample; ++x)
        {
            sum += texture(diffuseRect, base + vec2(float(x), float(y)) * hud_texel_size);
        }
    }

    frag_color = sum / float(hud_supersample * hud_supersample);
}
