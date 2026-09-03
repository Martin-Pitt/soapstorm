/**
 * @file llmultidraw.cpp
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

#include "linden_common.h"

#include "llmultidraw.h"

#include "llglheaders.h"

// <SS:Nexii> AzdoGaMa, see doc/azdo_bindless_textures.md

LLGLMultiDraw::~LLGLMultiDraw()
{
    if (mIndirectBuffer != 0)
    {
        glDeleteBuffers(1, &mIndirectBuffer);
        mIndirectBuffer = 0;
    }
}

void LLGLMultiDraw::clear()
{
    mCommands.clear();
}

void LLGLMultiDraw::addDrawRange(GLuint count, GLuint first_index, GLint base_vertex, GLuint base_instance)
{
    Command cmd;
    cmd.mCount = count;
    cmd.mInstanceCount = 1;
    cmd.mFirstIndex = first_index;
    cmd.mBaseVertex = (GLuint)base_vertex;
    cmd.mBaseInstance = base_instance;
    mCommands.push_back(cmd);
}

void LLGLMultiDraw::draw()
{
#if !LL_AZDO_GL_ENTRY_POINTS_AVAILABLE
    // GL 4.3 multi-draw indirect is not available on this platform; the caller decides
    // batching based on capability flags, so this state should never be reachable.
    LL_ERRS("Render") << "LLGLMultiDraw::draw called without multi-draw indirect support" << LL_ENDL;
    clear();
    return;
#else
    if (mCommands.empty())
    {
        return;
    }

    if (mIndirectBuffer == 0)
    {
        glGenBuffers(1, &mIndirectBuffer);
    }

    // save the current GL_DRAW_INDIRECT_BUFFER binding - nothing else in the renderer
    // uses this binding point today, but a save/restore keeps the state machine clean
    GLint prev_binding = 0;
    glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &prev_binding);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, mIndirectBuffer);
    // orphaning + upload in one call; multi-draw indirect takes the buffer data as
    // GPU-consumed state, and the next batch rewrites it wholesale
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(Command) * mCommands.size(), mCommands.data(), GL_DYNAMIC_DRAW);

    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const GLvoid*)0, (GLsizei)mCommands.size(), 0);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, prev_binding);
#endif
}
// </SS:Nexii>