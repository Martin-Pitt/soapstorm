/**
 * @file ssazdo.cpp
 * @brief Soapstorm AZDO (approaching zero driver overhead) settings refresh
 *
 * See doc/azdo_bindless_textures.md
 */

#include "llviewerprecompiledheaders.h"

#include "ssazdo.h"

#include "lldrawpool.h"
#include "llglslshader.h"
#include "llviewercontrol.h"
#include "llvertexbuffer.h"

// <SS:Nexii> AzdoGaMa, see doc/azdo_bindless_textures.md

void ss_azdo_refresh_enabled()
{
    LLVertexBuffer::sUsePersistentBuffers = gSavedSettings.getBOOL("SSPersistentBuffers");
    LLRenderPass::sUseMultiDrawIndirect = gSavedSettings.getBOOL("SSMultiDrawIndirect");
    LLGLSLShader::setUseBindlessTextures(gSavedSettings.getBOOL("SSBindlessTextures"));

    LL_INFOS("AzdoGaMa") << "AZDO: bindless textures "
        << (LLGLSLShader::sUseBindlessTextures ? "enabled" : "disabled (unit binding)")
        << ", persistent buffers " << (LLVertexBuffer::sUsePersistentBuffers ? "enabled" : "disabled (glBufferSubData)")
        << ", multi-draw indirect " << (LLRenderPass::sUseMultiDrawIndirect ? "enabled" : "disabled (per-draw submit)") << LL_ENDL;
}
// </SS:Nexii>