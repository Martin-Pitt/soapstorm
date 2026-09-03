/**
 * @file llmultidraw.h
 * @brief Batching for glMultiDrawElementsIndirect (zero driver overhead draw submission)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Soapstorm contributors
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

#ifndef LL_LLMULTIDRAW_H
#define LL_LLMULTIDRAW_H

#include "llgl.h"

#include <vector>

// <SS:Nexii> AzdoGaMa - accumulate DrawElementsIndirectCommand entries and submit them in
// one glMultiDrawElementsIndirect call, see doc/azdo_bindless_textures.md. Draws added to
// the same batch must share every piece of GL state (bound shader, vertex buffer bindings,
// texture state, matrix uniforms): batching is decided by the caller; this class only
// stores and submits commands.
class LLGLMultiDraw
{
public:
    LLGLMultiDraw() = default;
    ~LLGLMultiDraw();

    // drop all pending commands without issuing them
    void clear();

    bool empty() const { return mCommands.empty(); }
    size_t size() const { return mCommands.size(); }

    // append one draw of `count` indices starting at index `first_index` (in index units,
    // i.e. the value that drawRange calls indices_offset), with a base vertex of
    // `base_vertex` (the value that drawRange calls start)
    void addDrawRange(GLuint count, GLuint first_index, GLint base_vertex, GLuint base_instance = 0);

    // bind the indirect command buffer and issue glMultiDrawElementsIndirect
    // GL_TRIANGLES, GL_UNSIGNED_SHORT. The command buffer binding is saved and restored;
    // the caller must keep the source vertex buffer bound. GL 4.3 is required.
    void draw();

private:
    // DrawElementsIndirectCommand (5 x 32 bit, identical layout in GL spec)
    struct Command
    {
        GLuint mCount;
        GLuint mInstanceCount;
        GLuint mFirstIndex;
        GLuint mBaseVertex; // drawn as GLint in GL; bit pattern identical for non-negative
        GLuint mBaseInstance;
    };

    std::vector<Command> mCommands;
    GLuint mIndirectBuffer = 0;
};
// </SS:Nexii>

#endif