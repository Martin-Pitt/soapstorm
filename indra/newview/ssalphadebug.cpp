/**
 * @file ssalphadebug.cpp
 * @brief TEMPORARY diagnostic - reports why a named prim's face is being drawn in the alpha pool
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

// <SS:Nexii> TEMPORARY diagnostic, not part of any Soapstorm workstream. The question it exists to answer: a sim-surround megaprim's ground texture renders as if alpha blended, but only while the agent stands in a region other than the one the linkset's root prim sits in, with no distance component. The suspected mechanism is in LLPipeline::getPoolTypeFromTE, which reads `if (alpha && mat)` - a face whose Blinn-Phong material has not arrived yet has a null `mat`, so the diffuse alpha mode is never consulted, and a texture that merely HAS an alpha channel falls through to POOL_ALPHA. That failure is one-directional: alpha starts true and only the material can turn it off, so a missing material can never make a face wrongly opaque, only wrongly transparent. This dump is meant to confirm or refute that, and to separate it from the competing explanation that the material does arrive but the face's pool assignment is never recomputed. It touches no rendering behaviour - it reads state and prints.
//
// Trigger: right-click the prim. Selecting it makes the sim send ObjectProperties, and the name arrives on the selection node a moment later - so the tick watches the selection each frame and fires the moment a node's name matches SSAlphaDebugName. No menu entry needed, and no guessing at which prim is which by size.

#include "llviewerprecompiledheaders.h"

#include "ssalphadebug.h"

#include "llagent.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llface.h"
#include "llgltfmaterial.h"
#include "llmaterial.h"
#include "llselectmgr.h"
#include "llstring.h"
#include "lltextureentry.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"

#include <string>

namespace
{
    bool sDeclared = false;
    LLUUID sLastDumped;

    // Declared here rather than in settings.xml because the whole file is meant to be deleted, and a stray setting left behind in a shipped settings.xml outlives the code that read it.
    void declareOnce()
    {
        if (sDeclared) return;
        sDeclared = true;
        gSavedSettings.declareString("SSAlphaDebugName", "mountains", "TEMPORARY: dump the alpha-pool diagnostic when a selected prim's name contains this, case insensitive. Empty disables.", LLControlVariable::PERSIST_NO);
        gSavedSettings.declareBOOL("SSAlphaDebugDump", false, "TEMPORARY: set true to dump the whole current selection regardless of name; self-clears.", LLControlVariable::PERSIST_NO);
    }

    const char* poolName(U32 pool)
    {
        switch (pool)
        {
            case LLDrawPool::POOL_SIMPLE:                   return "SIMPLE";
            case LLDrawPool::POOL_FULLBRIGHT:               return "FULLBRIGHT";
            case LLDrawPool::POOL_BUMP:                     return "BUMP";
            case LLDrawPool::POOL_MATERIALS:                return "MATERIALS";
            case LLDrawPool::POOL_GLTF_PBR:                 return "GLTF_PBR";
            case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:      return "GLTF_PBR_ALPHA_MASK";
            case LLDrawPool::POOL_TERRAIN:                  return "TERRAIN";
            case LLDrawPool::POOL_GRASS:                    return "GRASS";
            case LLDrawPool::POOL_TREE:                     return "TREE";
            case LLDrawPool::POOL_ALPHA_MASK:               return "ALPHA_MASK";
            case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:    return "FULLBRIGHT_ALPHA_MASK";
            case LLDrawPool::POOL_AVATAR:                   return "AVATAR";
            case LLDrawPool::POOL_GLOW:                     return "GLOW";
            case LLDrawPool::POOL_ALPHA_PRE_WATER:          return "ALPHA_PRE_WATER";
            case LLDrawPool::POOL_ALPHA_POST_WATER:         return "ALPHA_POST_WATER";
            case LLDrawPool::POOL_ALPHA:                    return "ALPHA";
            case 0:                                         return "none";
            default:                                        return "other";
        }
    }

    const char* alphaModeName(U8 mode)
    {
        switch (mode)
        {
            case LLMaterial::DIFFUSE_ALPHA_MODE_NONE:       return "NONE";
            case LLMaterial::DIFFUSE_ALPHA_MODE_BLEND:      return "BLEND";
            case LLMaterial::DIFFUSE_ALPHA_MODE_MASK:       return "MASK";
            case LLMaterial::DIFFUSE_ALPHA_MODE_EMISSIVE:   return "EMISSIVE";
            case LLMaterial::DIFFUSE_ALPHA_MODE_DEFAULT:    return "DEFAULT";
            default:                                        return "?";
        }
    }

    // LLWorld::deactivateRegion drops a region from the active list without destroying it, so an object can still be in the world while its region has gone quiet. Worth naming explicitly, because a neighbour circuit that drops and reconnects is the state in which that region's materials would be missing while its objects are still on screen.
    bool isActiveRegion(LLViewerRegion* regionp)
    {
        if (!regionp) return false;
        for (LLViewerRegion* r : LLWorld::instance().getRegionList())
        {
            if (r == regionp) return true;
        }
        return false;
    }

    // The regions the viewer currently holds a connection to. Printed first because the suspected mechanism is region-scoped - the RenderMaterials capability is fetched per region - and because a neighbour whose circuit has just dropped and come back is exactly the state in which a region's materials would be missing while its objects are still on screen.
    void dumpRegions()
    {
        LLViewerRegion* agent_region = gAgent.getRegion();
        LL_INFOS("SSAlphaDebug") << "regions held:" << LL_ENDL;
        for (LLViewerRegion* regionp : LLWorld::instance().getRegionList())
        {
            if (!regionp) continue;
            const bool is_agent = (regionp == agent_region);
            const std::string caps_url = regionp->getCapability("RenderMaterials");
            LL_INFOS("SSAlphaDebug") << (is_agent ? "  * " : "    ")
                << "\"" << regionp->getName() << "\""
                << "  alive=" << (regionp->isAlive() ? "Y" : "N")
                << " caps=" << (regionp->capabilitiesReceived() ? "Y" : "N")
                << " RenderMaterials=" << (caps_url.empty() ? "MISSING" : "have")
                << " cachedObjects=" << regionp->getNumOfActiveCachedObjects()
                << (is_agent ? "   <- agent is here" : "")
                << LL_ENDL;
        }
    }

    void dumpFace(LLViewerObject* obj, U8 te_idx)
    {
        const LLTextureEntry* te = obj->getTE(te_idx);
        LLViewerTexture* imagep = obj->getTEImage(te_idx);
        if (!te || !imagep)
        {
            LL_INFOS("SSAlphaDebug") << "    face " << (S32)te_idx << ": te=" << (te ? "ok" : "NULL") << " image=" << (imagep ? "ok" : "NULL") << LL_ENDL;
            return;
        }

        LLMaterial* mat = te->getMaterialParams().get();
        const LLMaterialID& mat_id = te->getMaterialID();
        LLGLTFMaterial* gltf_mat = te->getGLTFRenderMaterial();

        // The same arithmetic LLPipeline::getPoolTypeFromTE performs, restated so the report can name which term made the decision rather than only the outcome.
        const bool color_alpha = te->getColor().mV[3] < 0.999f;
        const bool tex_alpha = (imagep->getComponents() == 4 && imagep->getType() != LLViewerTexture::MEDIA_TEXTURE) || (imagep->getComponents() == 2);

        // What the pipeline would decide for this face right now, and what it actually decided when the face was built. A disagreement means the material arrived after the fact and nothing asked for the pool assignment to be revisited, which is a different bug from the material never arriving at all.
        const U32 pool_now = gPipeline.getPoolTypeFromTE(te, imagep);
        U32 pool_face = 0;
        bool have_face = false;
        if (obj->mDrawable.notNull())
        {
            for (S32 f = 0; f < obj->mDrawable->getNumFaces(); ++f)
            {
                LLFace* face = obj->mDrawable->getFace(f);
                if (face && face->getTEOffset() == (S32)te_idx)
                {
                    pool_face = face->getPoolType();
                    have_face = true;
                    break;
                }
            }
        }

        std::string verdict;
        if (!color_alpha && !tex_alpha)
        {
            verdict = "opaque by both terms - not an alpha candidate";
        }
        else if (color_alpha)
        {
            verdict = "face colour alpha is below 1.0 - blended regardless of material, this is the user's own setting";
        }
        else if (mat_id.isNull())
        {
            verdict = "NO MATERIAL ASSIGNED AT ALL - alpha mode was never set on this face, so the texture's alpha channel alone puts it in the alpha pool. This is stock behaviour, not a missing fetch.";
        }
        else if (!mat)
        {
            verdict = "MATERIAL ASSIGNED BUT NOT ARRIVED - getDiffuseAlphaMode is never consulted, face falls to POOL_ALPHA. THIS IS THE SUSPECTED BUG.";
        }
        else if (mat->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            verdict = "material says BLEND - correctly blended";
        }
        else
        {
            verdict = "material present and says " + std::string(alphaModeName(mat->getDiffuseAlphaMode())) + " - alpha correctly suppressed";
        }

        LL_INFOS("SSAlphaDebug") << "    face " << (S32)te_idx
            << ": tex " << imagep->getID().asString().substr(0, 8)
            << " comps=" << (S32)imagep->getComponents()
            << " " << imagep->getWidth() << "x" << imagep->getHeight()
            << " discard=" << imagep->getDiscardLevel()
            << " fmt=0x" << std::hex << (U32)imagep->getPrimaryFormat() << std::dec
            << LL_ENDL;
        LL_INFOS("SSAlphaDebug") << "             matID=" << (mat_id.isNull() ? std::string("null") : mat_id.asString().substr(0, 8))
            << " matParams=" << (mat ? "PRESENT" : "NULL")
            << " alphaMode=" << (mat ? alphaModeName(mat->getDiffuseAlphaMode()) : "unknown")
            << " cutoff=" << (mat ? (S32)mat->getAlphaMaskCutoff() : -1)
            << " gltf=" << (gltf_mat ? "PRESENT" : "null")
            << LL_ENDL;
        LL_INFOS("SSAlphaDebug") << "             colourAlpha=" << te->getColor().mV[3]
            << " texAlphaTerm=" << (tex_alpha ? "Y" : "N")
            << " bump=" << (S32)te->getBumpmap()
            << " shiny=" << (S32)te->getShiny()
            << " fullbright=" << (S32)te->getFullbright()
            << LL_ENDL;
        LL_INFOS("SSAlphaDebug") << "             pool now=" << poolName(pool_now)
            << " | pool on the built face=" << (have_face ? poolName(pool_face) : "no face")
            << ((have_face && pool_face != pool_now) ? "   <- STALE, the face was assigned before the material landed" : "")
            << LL_ENDL;
        LL_INFOS("SSAlphaDebug") << "             " << verdict << LL_ENDL;
    }

    // name is whatever the selection node knows, which stays empty until the sim's ObjectProperties reply lands.
    void dumpObject(LLViewerObject* obj, const std::string& name)
    {
        if (!obj || obj->isDead()) return;

        LLViewerRegion* agent_region = gAgent.getRegion();
        LLViewerRegion* obj_region = obj->getRegion();
        const bool cross_region = (obj_region && agent_region && obj_region != agent_region);
        const LLVector3& scale = obj->getScale();

        LL_INFOS("SSAlphaDebug") << "  ---- \"" << (name.empty() ? std::string("(name not received)") : name) << "\""
            << "  " << obj->getID().asString().substr(0, 8)
            << " local " << obj->getLocalID()
            << " scale <" << scale.mV[0] << ", " << scale.mV[1] << ", " << scale.mV[2] << ">"
            << " tes=" << (S32)obj->getNumTEs()
            << ((obj->getRootEdit() != obj) ? " (child of a linkset)" : " (root)")
            << LL_ENDL;
        LL_INFOS("SSAlphaDebug") << "       region \"" << (obj_region ? obj_region->getName() : std::string("NONE")) << "\""
            << (cross_region ? "   <- DIFFERENT REGION FROM THE AGENT" : "   (same region as the agent)")
            << (isActiveRegion(obj_region) ? "" : "  [REGION NOT IN ACTIVE LIST]")
            << " drawable=" << (obj->mDrawable.notNull() ? "present" : "NULL")
            << (obj->mDrawable.notNull() ? (obj->mDrawable->isVisible() ? " visible=Y" : " visible=N") : "")
            << (obj->mDrawable.notNull() && obj->mDrawable->isState(LLDrawable::FORCE_INVISIBLE) ? " FORCE_INVISIBLE" : "")
            << LL_ENDL;

        for (U8 te_idx = 0; te_idx < obj->getNumTEs(); ++te_idx)
        {
            dumpFace(obj, te_idx);
        }
    }
}

void ssAlphaDebugTick()
{
    declareOnce();

    // Both routes need a world and a selection to read.
    if (!LLWorld::instanceExists() || !gAgent.getRegion() || !LLSelectMgr::instanceExists()) return;

    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (selection.isNull() || selection->getObjectCount() == 0)
    {
        // Deselecting re-arms the name trigger, so right-clicking the same prim again after crossing a region border reports afresh rather than staying silent.
        sLastDumped.setNull();
        return;
    }

    const bool force = gSavedSettings.getBOOL("SSAlphaDebugDump");
    if (force)
    {
        gSavedSettings.setBOOL("SSAlphaDebugDump", false);
    }

    std::string want = gSavedSettings.getString("SSAlphaDebugName");
    LLStringUtil::toLower(want);

    bool printed_header = false;
    for (LLObjectSelection::iterator iter = selection->begin(); iter != selection->end(); ++iter)
    {
        LLSelectNode* node = *iter;
        if (!node) continue;
        LLViewerObject* obj = node->getObject();
        if (!obj || obj->isDead()) continue;

        if (!force)
        {
            // The name only turns up once ObjectProperties comes back, which is why this is a per-frame poll rather than a one-shot: the frame the name lands is the frame this fires.
            if (want.empty() || node->mName.empty()) continue;
            std::string have = node->mName;
            LLStringUtil::toLower(have);
            if (have.find(want) == std::string::npos) continue;
            if (obj->getID() == sLastDumped) continue;
            sLastDumped = obj->getID();
        }

        if (!printed_header)
        {
            printed_header = true;
            LL_INFOS("SSAlphaDebug") << "==== alpha-pool diagnostic ====" << LL_ENDL;
            dumpRegions();
        }
        dumpObject(obj, node->mName);
    }

    if (printed_header)
    {
        LL_INFOS("SSAlphaDebug") << "==== end ====" << LL_ENDL;
    }
    else if (force)
    {
        LL_INFOS("SSAlphaDebug") << "selection held no reportable objects" << LL_ENDL;
    }
}
// </SS:Nexii>
