/**
 * @file ssloginavatar.cpp
 * @brief Login-screen avatar preview: capture the user's avatar on logout and
 *        replay it as a live 3D idle on the login screen, old-school menu style.
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

#include "llviewerprecompiledheaders.h"

#include "ssloginavatar.h"

#include "llagent.h"
#include "llanimationstates.h"
#include "lldrawpoolavatar.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llformat.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llimagepng.h"
#include "llrender.h"
#include "llsdserialize.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewerobjectlist.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvertexbuffer.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

using namespace LLAvatarAppearanceDefines;

// <SS:Nexii> Login-avatar preview: capture-on-logout + offline replay on the login screen.
// The bundle lives in LL_PATH_CACHE (available before the account dir is set on the
// next launch), and the replay path reuses the UI-avatar machinery so the offline
// avatar renders exactly like any fully-loaded remote avatar: TE images + visual
// params, keyed by the same baked texture slots.

static const std::string SS_LOGIN_AVATAR_DIR = "ss_login_avatar";
static const std::string SS_LOGIN_AVATAR_VIEW_NAME = "ss_login_avatar_overlay";
static const F32 SS_PREVIEW_ASPECT = 0.8f;          // rt width / height
static const F32 SS_PREVIEW_HEIGHT_FRACTION = 0.72f;
static const F32 SS_PREVIEW_X_FRACTION = 0.03f;

static std::string ss_bundle_path(const std::string& file)
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, SS_LOGIN_AVATAR_DIR, file);
}

//*************************************************************************
// Capture
//*************************************************************************

static bool ss_copy_vfs_asset_to_file(const LLUUID& id, LLAssetType::EType type, const std::string& file)
{
    if (!LLFileSystem::getExists(id, type))
    {
        return false;
    }

    LLFileSystem vfile(id, type, LLFileSystem::READ);
    S32 size = vfile.getSize();
    if (size <= 0)
    {
        return false;
    }

    std::vector<U8> buffer(size);
    if (!vfile.read(&buffer[0], size) || vfile.getLastBytesRead() != size)
    {
        return false;
    }

    llofstream stream(file, std::ios_base::binary);
    stream.write((const char*)&buffer[0], size);
    return stream.good();
}

static void ss_serialize_linkset(LLViewerObject* objp, LLSD& out)
{
    if (!objp || objp->isDead())
    {
        return;
    }

    out["pcode"] = (S32)objp->getPCode();
    out["position"] = objp->getPosition().getValue();
    out["rotation"] = objp->getRotation().getValue();
    out["scale"] = objp->getScale().getValue();

    if (objp->getVolume())
    {
        out["volume"]["path"] = objp->getVolume()->getParams().getPathParams().asLLSD();
        out["volume"]["profile"] = objp->getVolume()->getParams().getProfileParams().asLLSD();
    }

    const LLSculptParams* sculptp = objp->getSculptParams();
    if (sculptp)
    {
        out["sculpt"] = sculptp->asLLSD();
    }
    const LLFlexibleObjectData* flexiblep = objp->getFlexibleObjectData();
    if (flexiblep)
    {
        out["flexible"] = flexiblep->asLLSD();
    }
    const LLLightParams* lightp = objp->getLightParams();
    if (lightp)
    {
        out["light"] = lightp->asLLSD();
    }
    const LLLightImageParams* light_imagep = objp->getLightImageParams();
    if (light_imagep)
    {
        out["light_texture"] = light_imagep->asLLSD();
    }

    // Material texture asset ids; the texture cache persists them and
    // getFetchedTexture resolves them offline.
    S32 num_tes = objp->getNumTEs();
    for (S32 face = 0; face < num_tes; ++face)
    {
        const LLTextureEntry* tep = objp->getTE(face);
        if (tep)
        {
            out["texture"].append(tep->asLLSD());
        }
    }

    for (LLViewerObject::child_list_t::iterator iter = objp->mChildList.begin();
         iter != objp->mChildList.end(); ++iter)
    {
        LLViewerObject* childp = *iter;
        if (childp && !childp->isDead())
        {
            LLSD child;
            ss_serialize_linkset(childp, child);
            out["children"].append(child);
        }
    }
}

static void ss_collect_mesh_ids(LLViewerObject* objp, std::set<LLUUID>& mesh_ids)
{
    if (!objp || objp->isDead())
    {
        return;
    }

    // The sculpt texture of a mesh prim is its mesh asset id.
    const LLSculptParams* sculptp = objp->getSculptParams();
    if (sculptp && sculptp->getSculptTexture().notNull() && sculptp->getSculptType() != SCULPT_TYPE_NONE)
    {
        mesh_ids.insert(sculptp->getSculptTexture());
    }

    for (LLViewerObject::child_list_t::iterator iter = objp->mChildList.begin();
         iter != objp->mChildList.end(); ++iter)
    {
        ss_collect_mesh_ids(*iter, mesh_ids);
    }
}

// static
void SSLoginAvatar::captureOnLogout()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSLoginAvatarPreview", true);
    if (!enabled)
    {
        return;
    }
    if (!isAgentAvatarValid() || gAgent.getRegion() == NULL)
    {
        return;
    }
    LLVOAvatarSelf* avp = gAgentAvatarp;
    if (avp->isEditingAppearance())
    {
        // Do not poison the bundle with the outfit editor's stripped state.
        return;
    }

    const std::string dir = getBundleDir();
    LLFile::mkdir(dir);

    LLSD manifest = LLSD::emptyMap();
    manifest["version"] = 1;

    // Visual params: id/weight pairs; drivers re-propagate on apply.
    LLSD params = LLSD::emptyArray();
    for (LLVisualParam* param = avp->getFirstVisualParam();
         param;
         param = avp->getNextVisualParam())
    {
        LLSD entry;
        entry["id"] = param->getID();
        entry["w"] = param->getWeight();
        params.append(entry);
    }
    manifest["params"] = params;
    manifest["sex"] = (S32)avp->getSex();

    // Baked textures: PNG dump (offline-proof) plus the TE asset id.
    LLSD bakes = LLSD::emptyArray();
    const LLAvatarAppearanceDictionary* dictionary = LLAvatarAppearanceDictionary::getInstance();
    for (S32 i = 0; i < BAKED_NUM_INDICES; ++i)
    {
        const EBakedTextureIndex baked = (EBakedTextureIndex)i;
        const ETextureIndex te = dictionary->bakedToLocalTextureIndex(baked);
        if (!avp->isTextureDefined(te))
        {
            continue;
        }

        LLViewerTexture* tex = avp->getTEImage(te);
        if (!tex)
        {
            continue;
        }

        LLSD entry;
        entry["te"] = (S32)te;
        entry["uuid"] = tex->getID();

        LLViewerFetchedTexture* fetchedp = dynamic_cast<LLViewerFetchedTexture*>(tex);
        if (fetchedp)
        {
            fetchedp->readbackRawImage();
            LLImageRaw* raw = fetchedp->getRawImage();
            if (raw)
            {
                std::string file = llformat("bake_%d.png", i);
                LLPointer<LLImagePNG> png = new LLImagePNG;
                if (png->encode(raw) && png->save(ss_bundle_path(file)))
                {
                    entry["file"] = file;
                }
            }
        }
        bakes.append(entry);
    }
    manifest["bakes"] = bakes;

    // Idle animations: copy the stand family out of the VFS so the replay
    // path can prime it back in even after a VFS prune.
    LLSD anims = LLSD::emptyArray();
    static const LLUUID stand_anims[] = { ANIM_AGENT_STAND, ANIM_AGENT_STAND_1, ANIM_AGENT_STAND_2,
                                          ANIM_AGENT_STAND_3, ANIM_AGENT_STAND_4 };
    for (const LLUUID& id : stand_anims)
    {
        std::string file = "anim_" + id.asString() + ".anim";
        if (ss_copy_vfs_asset_to_file(id, LLAssetType::AT_ANIMATION, ss_bundle_path(file)))
        {
            LLSD entry;
            entry["id"] = id;
            entry["file"] = file;
            anims.append(entry);
        }
    }
    manifest["anims"] = anims;

    // Attachment linksets, serialized in the OXP prim schema.
    std::set<LLUUID> mesh_ids;
    LLSD attachments = LLSD::emptyArray();
    for (auto& point_attachment : avp->mAttachmentPoints)
    {
        LLViewerJointAttachment* jointp = point_attachment.second;
        if (!jointp || jointp->getIsHUDAttachment())
        {
            continue;
        }
        for (LLViewerObject* objp : jointp->mAttachedObjects)
        {
            if (!objp || objp->isDead())
            {
                continue;
            }
            ss_collect_mesh_ids(objp, mesh_ids);
            LLSD entry;
            entry["point"] = point_attachment.first;
            // Record the root's offset in joint space; at replay the joint
            // world transform is applied again from the fresh skeleton.
            LLVector3 jpos = (objp->getRenderPosition() - jointp->getWorldPosition()) * ~jointp->getWorldRotation();
            LLQuaternion jrot = objp->getRenderRotation() * ~jointp->getWorldRotation();
            entry["jpos"] = jpos.getValue();
            entry["jrot"] = jrot.getValue();
            ss_serialize_linkset(objp, entry["root"]);
            attachments.append(entry);
        }
    }
    manifest["attachments"] = attachments;

    // Mesh assets for rigged attachments.
    LLSD meshes = LLSD::emptyArray();
    for (const LLUUID& id : mesh_ids)
    {
        std::string file = "mesh_" + id.asString() + ".mesh";
        if (ss_copy_vfs_asset_to_file(id, LLAssetType::AT_MESH, ss_bundle_path(file)))
        {
            LLSD entry;
            entry["id"] = id;
            entry["file"] = file;
            meshes.append(entry);
        }
    }
    manifest["meshes"] = meshes;

    llofstream out(ss_bundle_path("manifest.xml"));
    LLSDSerialize::toPrettyXML(manifest, out);
}

// static
std::string SSLoginAvatar::getBundleDir()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, SS_LOGIN_AVATAR_DIR);
}

//*************************************************************************
// Replay
//*************************************************************************

static void ss_prime_vfs_asset(const LLSD& entries, LLAssetType::EType type)
{
    if (!entries.isArray())
    {
        return;
    }
    for (auto& entry : entries)
    {
        LLUUID id = entry["id"].asUUID();
        if (id.isNull() || LLFileSystem::getExists(id, type))
        {
            continue;
        }
        llifstream stream(ss_bundle_path(entry["file"].asString()), std::ios_base::binary);
        if (!stream.is_open())
        {
            continue;
        }
        stream.seekg(0, std::ios_base::end);
        std::streamoff size = stream.tellg();
        stream.seekg(0, std::ios_base::beg);
        if (size <= 0)
        {
            continue;
        }
        std::vector<U8> buffer((size_t)size);
        stream.read((char*)&buffer[0], size);
        if (stream.gcount() == size)
        {
            LLFileSystem vfile(id, type, LLFileSystem::WRITE);
            vfile.write(&buffer[0], (S32)size);
        }
    }
}

static LLPointer<LLViewerTexture> ss_load_bake_texture(const LLSD& entry)
{
    std::string file = entry["file"].asString();
    if (!file.empty())
    {
        LLPointer<LLImagePNG> png = new LLImagePNG;
        if (png->load(ss_bundle_path(file)))
        {
            LLPointer<LLImageRaw> raw = new LLImageRaw;
            if (png->decode(raw))
            {
                return LLViewerTextureManager::getLocalTexture(raw, false);
            }
        }
    }
    // Fallback: the J2C of a recent bake usually survives in the texture cache.
    LLUUID id = entry["uuid"].asUUID();
    if (id.notNull())
    {
        return LLPointer<LLViewerTexture>(LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true,
                                                         LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE));
    }
    return NULL;
}

static LLViewerObject* ss_rebuild_prim(const LLSD& prim_sd, LLViewerObject* parent)
{
    LLPCode pcode = (LLPCode)prim_sd["pcode"].asInteger();
    if (pcode != LL_PCODE_VOLUME)
    {
        // Only volumes are worth replaying; anything exotic degrades to a box.
        pcode = LL_PCODE_VOLUME;
    }

    LLViewerObject* objp = gObjectList.createObjectViewer(pcode, NULL);
    if (!objp)
    {
        return NULL;
    }
    objp->createDrawable(&gPipeline);
    if (parent)
    {
        // addChild does setParent plus the parent's child-list bookkeeping.
        parent->addChild(objp);
    }

    if (prim_sd.has("volume"))
    {
        LLVolumeParams volume_params;
        volume_params.getPathParams().fromLLSD(prim_sd["volume"]["path"]);
        volume_params.getProfileParams().fromLLSD(prim_sd["volume"]["profile"]);
        // Detail 3: nothing drives volume LOD updates offline, so build the
        // highest-detail geometry up front.
        objp->setVolume(volume_params, 3, false);
    }

    // Child prims store local transforms relative to their (now set) parent.
    LLVector3 position;
    position.setValue(prim_sd["position"]);
    LLQuaternion rotation;
    rotation.setValue(prim_sd["rotation"]);
    LLVector3 scale;
    scale.setValue(prim_sd["scale"]);
    objp->setPosition(position);
    objp->setRotation(rotation);
    objp->setScale(scale);

    const LLSD& sculpt = prim_sd["sculpt"];
    if (sculpt.isDefined())
    {
        LLSculptParams sculpt_params;
        sculpt_params.fromLLSD(sculpt);
        objp->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt_params, true);
    }
    const LLSD& flexible = prim_sd["flexible"];
    if (flexible.isDefined())
    {
        LLFlexibleObjectData attributes;
        attributes.fromLLSD(flexible);
        objp->setParameterEntry(LLNetworkData::PARAMS_FLEXIBLE, attributes, true);
    }
    const LLSD& light = prim_sd["light"];
    if (light.isDefined())
    {
        LLLightParams light_params;
        light_params.fromLLSD(light);
        objp->setParameterEntry(LLNetworkData::PARAMS_LIGHT, light_params, true);
    }
    const LLSD& light_texture = prim_sd["light_texture"];
    if (light_texture.isDefined())
    {
        LLLightImageParams light_image_params;
        light_image_params.fromLLSD(light_texture);
        objp->setParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE, light_image_params, true);
    }

    const LLSD& textures = prim_sd["texture"];
    S32 num_tes = (S32)textures.size();
    if (num_tes > 0)
    {
        objp->setNumTEs((U8)num_tes);
        for (S32 face = 0; face < num_tes; ++face)
        {
            LLTextureEntry texture_entry;
            if (!texture_entry.fromLLSD(textures[face]))
            {
                continue;
            }
            objp->setTE((U8)face, texture_entry);
            LLUUID tex_id = texture_entry.getID();
            if (tex_id.notNull() && !LLAvatarAppearanceDictionary::isBakedImageId(tex_id))
            {
                objp->setTEImage((U8)face,
                    LLViewerTextureManager::getFetchedTexture(tex_id, FTT_DEFAULT, true,
                                                              LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE));
            }
        }
    }

    objp->updateGeometry(objp->mDrawable);

    const LLSD& children = prim_sd["children"];
    for (auto& child_sd : children)
    {
        ss_rebuild_prim(child_sd, objp);
    }
    return objp;
}

//*************************************************************************
// SSLoginAvatarView
//*************************************************************************

SSLoginAvatarView::SSLoginAvatarView(const LLView::Params& params)
:   LLView(params),
    mBuilt(false),
    mFailed(false),
    mYaw(-0.35f)
{
    // Purely decorative; clicks must fall through to the login menu.
    setMouseOpaque(false);
}

SSLoginAvatarView::~SSLoginAvatarView()
{
    teardown();
}

void SSLoginAvatarView::teardown()
{
    if (mAvatar)
    {
        mAvatar->markDead();
        mAvatar = NULL;
    }
    mTarget.release();
    mBuilt = false;
}

bool SSLoginAvatarView::buildAvatar()
{
    llifstream stream(ss_bundle_path("manifest.xml"));
    if (!stream.is_open())
    {
        return false;
    }
    LLSD manifest;
    if (!LLSDSerialize::fromXML(manifest, stream) || manifest["version"].asInteger() != 1)
    {
        return false;
    }

    // Prime the VFS with the cached anim/mesh assets so the offline asset
    // lookups hit LLFileSystem without a region.
    ss_prime_vfs_asset(manifest["anims"], LLAssetType::AT_ANIMATION);
    ss_prime_vfs_asset(manifest["meshes"], LLAssetType::AT_MESH);

    // The character files and VO class statics are normally initialized in
    // STATE_WORLD_INIT; the login screen needs them first. Both are
    // re-entrant (failed-login reparse), and the world-init pass will run
    // them again after login. One-shot: keep repeat logins from re-parsing
    // the character XML and stacking up region-changed callback connects.
    static bool s_avatar_classes_initialized = false;
    if (!s_avatar_classes_initialized)
    {
        s_avatar_classes_initialized = true;
        LLAvatarAppearance::initClass("avatar_lad.xml", "avatar_skeleton.xml");
        LLVOAvatar::initClass();
    }

    LLVOAvatar* avatarp = (LLVOAvatar*)gObjectList.createObjectViewer(LL_PCODE_LEGACY_AVATAR, NULL,
                                                                     LLViewerObject::CO_FLAG_UI_AVATAR);
    if (!avatarp)
    {
        return false;
    }
    mAvatar = avatarp;
    mAvatar->mSpecialRenderMode = 1; // force motion updates regardless of visibility

    // Appearance: slam params, then let the virtual updateVisualParams
    // re-derive sex and apply skeleton (bone offset) params.
    for (auto& param_sd : manifest["params"])
    {
        mAvatar->setVisualParamWeight(param_sd["id"].asInteger(), (F32)param_sd["w"].asReal());
    }
    mAvatar->updateVisualParams();
    mAvatar->updateOverallAppearance();

    for (auto& bake_sd : manifest["bakes"])
    {
        LLPointer<LLViewerTexture> tex = ss_load_bake_texture(bake_sd);
        if (tex)
        {
            mAvatar->setTEImage((U8)bake_sd["te"].asInteger(), tex);
        }
    }

    // Idle animation. The stand assets were primed into the VFS above, so
    // the keyframe fetch resolves offline; breathe/body-noise/eye are
    // procedural and need nothing.
    mAvatar->startMotion(ANIM_AGENT_STAND);

    for (auto& attachment_sd : manifest["attachments"])
    {
        S32 point = attachment_sd["point"].asInteger();
        LLViewerObject* rootp = ss_rebuild_prim(attachment_sd["root"], NULL);
        auto joint_iter = mAvatar->mAttachmentPoints.find(point);
        if (rootp && joint_iter != mAvatar->mAttachmentPoints.end() && joint_iter->second)
        {
            LLViewerJointAttachment* jointp = joint_iter->second;
            // Re-apply the captured joint-space offset in the fresh
            // skeleton's agent space; attachObject will re-derive it.
            LLVector3 jpos;
            jpos.setValue(attachment_sd["jpos"]);
            LLQuaternion jrot;
            jrot.setValue(attachment_sd["jrot"]);
            rootp->setPosition(jpos * jointp->getWorldRotation() + jointp->getWorldPosition());
            rootp->setRotation(jrot * jointp->getWorldRotation());
            // A unique item id keeps addObject's duplicate-attach path
            // (which needs a sim connection) out of the picture.
            LLUUID item_id;
            item_id.generate();
            rootp->addNVPair("AttachItemID STRING " + item_id.asString());
            mAvatar->ssAttachObjectTo(rootp, point);
        }
        else if (rootp)
        {
            rootp->markDead();
        }
    }

    mAvatar->updateGeometry(mAvatar->mDrawable);
    return true;
}

void SSLoginAvatarView::renderPreview()
{
    const S32 rt_w = 512;
    const S32 rt_h = (S32)(rt_w / SS_PREVIEW_ASPECT);
    if (!mTarget.isComplete() && !mTarget.allocate(rt_w, rt_h, GL_RGBA, true))
    {
        return;
    }

    LLVOAvatar* avatarp = mAvatar;
    if (!avatarp || avatarp->isDead() || avatarp->mDrawable.isNull())
    {
        return;
    }

    // Drive the skeleton by hand; the object-list idle that normally does
    // this does not run before STATE_STARTED.
    avatarp->setPixelAreaAndAngle(gAgent);
    avatarp->updateJointLODs();
    avatarp->updateCharacter(gAgent);
    avatarp->updateLOD();

    // This render happens mid UI pass, so save the matrices/viewport that
    // setPerspective clobbers; restore after flush.
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    GLint saved_viewport[4];
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    S32 saved_ggl_viewport[4] = { gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3] };
    GLfloat saved_clear_color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_clear_color);

    // Transparent clear so the avatar composites over the login screen.
    mTarget.bindTarget();
    glClearColor(0.f, 0.f, 0.f, 0.f);

    LLVector3 target_pos = avatarp->mRoot->getWorldPosition();

    LLQuaternion camera_rot = LLQuaternion(0.1f, LLVector3::y_axis) * LLQuaternion(mYaw, LLVector3::z_axis);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    LLQuaternion av_rot = avatarp->mRoot->getWorldRotation() * camera_rot;
    camera->setOriginAndLookAt(
        target_pos + (LLVector3(2.2f, 0.f, 0.f) * av_rot),
        LLVector3::z_axis,
        target_pos + (LLVector3(0.f, 0.f, 0.55f) * av_rot));
    camera->setViewNoBroadcast(LLViewerCamera::getInstance()->getDefaultFOV());
    camera->setAspect((F32)rt_w / (F32)rt_h);
    camera->setPerspective(false, 0, 0, rt_w, rt_h, false);

    gPipeline.enableLightsPreview();

    LLVertexBuffer::unbind();
    {
        // Depth mask on for both the clear and the render.
        LLGLDepthTest depth(GL_TRUE);
        mTarget.clear();

        LLFace* face = avatarp->mDrawable->getFace(0);
        if (face)
        {
            LLDrawPoolAvatar* avatar_poolp = (LLDrawPoolAvatar*)face->getPool();
            avatarp->dirtyMesh();
            avatar_poolp->renderAvatars(avatarp);
        }
    }

    mTarget.flush();
    // flush() restores the viewport from gGLViewport, which setPerspective
    // just overwrote with the RT size - put the real one back by hand.
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    gGLViewport[0] = saved_ggl_viewport[0];
    gGLViewport[1] = saved_ggl_viewport[1];
    gGLViewport[2] = saved_ggl_viewport[2];
    gGLViewport[3] = saved_ggl_viewport[3];
    glClearColor(saved_clear_color[0], saved_clear_color[1], saved_clear_color[2], saved_clear_color[3]);
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

void SSLoginAvatarView::draw()
{
    if (!mBuilt && !mFailed)
    {
        if (!buildAvatar())
        {
            mFailed = true;
            return;
        }
        mBuilt = true;
    }
    if (!mBuilt || !mAvatar)
    {
        return;
    }

    renderPreview();
    if (!mTarget.isComplete() || mTarget.getTexture() == 0)
    {
        return;
    }

    // Old-school menu framing: full-height bust on the left, drawn over
    // everything in login_panel_holder (added last as a child).
    const LLRect& rect = getRect();
    S32 height = (S32)((F32)rect.getHeight() * SS_PREVIEW_HEIGHT_FRACTION);
    S32 width = (S32)((F32)height * SS_PREVIEW_ASPECT);
    S32 x = rect.mLeft + (S32)((F32)rect.getWidth() * SS_PREVIEW_X_FRACTION);
    S32 y = rect.mBottom + ((rect.getHeight() - height) / 2);

    // The avatar pool bound its own shaders; go back to the UI program for
    // this quad (and leave it bound, as the UI pass expects).
    gUIProgram.bind();
    gGL.getTexUnit(0)->bind(&mTarget);
    LLGLSUIDefault ui_state;
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gGL.begin(LLRender::TRIANGLES);
    // FBO row 0 is the bottom: v=1 at the UI top.
    gGL.texCoord2f(0.f, 1.f); gGL.vertex2i(x, y + height);
    gGL.texCoord2f(0.f, 0.f); gGL.vertex2i(x, y);
    gGL.texCoord2f(1.f, 0.f); gGL.vertex2i(x + width, y);
    gGL.texCoord2f(1.f, 0.f); gGL.vertex2i(x + width, y);
    gGL.texCoord2f(1.f, 1.f); gGL.vertex2i(x + width, y + height);
    gGL.texCoord2f(0.f, 1.f); gGL.vertex2i(x, y + height);
    gGL.end();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

//*************************************************************************
// Overlay lifecycle
//*************************************************************************

// static
void SSLoginAvatar::showOverlay()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSLoginAvatarPreview", true);
    if (!enabled || !gViewerWindow)
    {
        return;
    }
    LLView* holder = gViewerWindow->getLoginPanelHolder();
    if (!holder)
    {
        return;
    }

    SSLoginAvatarView* view = (SSLoginAvatarView*)holder->getChildByName(SS_LOGIN_AVATAR_VIEW_NAME);
    if (view)
    {
        // Re-add so the avatar stays in front of the login panel.
        holder->removeChild(view);
    }
    else
    {
        LLView::Params params;
        params.name(SS_LOGIN_AVATAR_VIEW_NAME);
        params.rect(holder->getLocalRect());
        params.follows.flags(FOLLOWS_ALL);
        view = new SSLoginAvatarView(params);
    }
    holder->addChild(view);
}

// static
void SSLoginAvatar::hideOverlay()
{
    if (!gViewerWindow)
    {
        return;
    }
    LLView* holder = gViewerWindow->getLoginPanelHolder();
    if (!holder)
    {
        return;
    }
    LLView* view = holder->getChildByName(SS_LOGIN_AVATAR_VIEW_NAME);
    if (view)
    {
        holder->removeChild(view);
        delete view;
    }
}
// </SS:Nexii>
