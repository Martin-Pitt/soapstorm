/**
 * @file ssatmolandscapeobject.cpp
 * @brief Atmo Magic: the landscape runtime object - a viewer-local mesh volume.
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

#include "ssatmolandscapeobject.h"

#include "llviewerregion.h"

#include "llmath.h"
#include "llmaterial.h"
#include "lltextureentry.h"
#include "llvolume.h"

namespace
{
    bool near_v3(const LLVector3& a, const LLVector3& b, F32 eps = 1e-4f)
    {
        return llabs(a.mV[VX] - b.mV[VX]) < eps
            && llabs(a.mV[VY] - b.mV[VY]) < eps
            && llabs(a.mV[VZ] - b.mV[VZ]) < eps;
    }

    bool near_v3d(const LLVector3d& a, const LLVector3d& b, F64 eps = 1e-3)
    {
        return llabs(a.mdV[VX] - b.mdV[VX]) < eps
            && llabs(a.mdV[VY] - b.mdV[VY]) < eps
            && llabs(a.mdV[VZ] - b.mdV[VZ]) < eps;
    }

    bool near_quat(const LLQuaternion& a, const LLQuaternion& b, F32 eps = 1e-4f)
    {
        return llabs(a.mQ[VX] - b.mQ[VX]) < eps
            && llabs(a.mQ[VY] - b.mQ[VY]) < eps
            && llabs(a.mQ[VZ] - b.mQ[VZ]) < eps
            && llabs(a.mQ[VW] - b.mQ[VW]) < eps;
    }

    bool face_equiv(const SSAtmoEnvLandscapeFace& a, const SSAtmoEnvLandscapeFace& b)
    {
        return a.mIndex == b.mIndex
            && a.mTexture == b.mTexture
            && a.mMaterial == b.mMaterial
            && a.mAlphaMode == b.mAlphaMode
            && near_v3(LLVector3(a.mRepeats.mV[VX], a.mRepeats.mV[VY], a.mRepeats.mV[VZ]), LLVector3(b.mRepeats.mV[VX], b.mRepeats.mV[VY], b.mRepeats.mV[VZ]))
            && llabs(a.mRepeats.mV[VW] - b.mRepeats.mV[VW]) < 1e-4f
            && llabs(a.mRotation - b.mRotation) < 1e-4f
            && near_v3(LLVector3(a.mColor.mV[VR], a.mColor.mV[VG], a.mColor.mV[VB]), LLVector3(b.mColor.mV[VR], b.mColor.mV[VG], b.mColor.mV[VB]))
            && llabs(a.mColor.mV[VA] - b.mColor.mV[VA]) < 1e-4f;
    }
}

SSAtmoLandscapeObject::SSAtmoLandscapeObject(const LLUUID& id, LLViewerRegion* regionp, const SSAtmoEnvLandscape& record)
    : LLVOVolume(id, LL_PCODE_VOLUME, regionp)
{
    // Local content: every server send touching this object is gated on ssIsLocalContent().
    ssSetLocalContent(true);

    // The stock editor's manipulator and menu permission checks read these VO flags
    // (permMove/permModify/permCopy/permYouOwner). A local object's permissions come from
    // the author's captured item metadata rather than a sim, and authoring the scenery the
    // author dropped is always allowed - so the object carries full perms in its flags.
    mFlags |= FLAGS_OBJECT_YOU_OWNER
        | FLAGS_OBJECT_MODIFY
        | FLAGS_OBJECT_COPY
        | FLAGS_OBJECT_MOVE
        | FLAGS_OBJECT_TRANSFER
        | FLAGS_OBJECT_ANY_OWNER
        | FLAGS_OBJECT_OWNER_MODIFY;

    applyRecord(record);
}

void SSAtmoLandscapeObject::applyRecord(const SSAtmoEnvLandscape& record)
{
    const bool mesh_changed = record.mMeshId != mAuthored.mMeshId;
    mAuthored = record;
    if (mesh_changed)
    {
        applyMesh(record);
    }
    applyPlacement(record);
    mAppliedFaces = -1;
}

void SSAtmoLandscapeObject::applyMesh(const SSAtmoEnvLandscape& record)
{
    // A mesh volume is a mesh by its sculpt entry: sculpt type MESH plus the asset's uuid.
    // Everything else stays the default box params the ctor created - for a mesh volume the
    // asset is the geometry. setVolume drives the whole stock fetch path (gMeshRepo.loadMesh
    // through the volume-coupled delivery).
    LLVolumeParams params = getVolume()->getParams();
    params.setSculptID(record.mMeshId, LL_SCULPT_TYPE_MESH);
    setVolume(params, 0);
}

void SSAtmoLandscapeObject::applyPlacement(const SSAtmoEnvLandscape& record)
{
    if (record.mLocked)
    {
        setPositionRegion(record.mLockedOffset);
    }
    else
    {
        setPositionGlobal(record.mFreeGlobal);
    }
    setRotation(record.mRotation);
    setScale(record.mScale);
}

void SSAtmoLandscapeObject::applyFaces()
{
    const S32 num = getNumFaces();
    if (num <= 0)
    {
        return;
    }
    if (num == mAppliedFaces)
    {
        return;
    }

    const S32 max_face = (S32)mAuthored.mFaces.size();

    // The sparse list is indexed by face; a hand-edited block may omit the index (then it
    // applies to its array position). Build the mapping once per apply.
    std::vector<S32> by_face(static_cast<size_t>(num), -1);
    for (S32 i = 0; i < max_face; ++i)
    {
        const SSAtmoEnvLandscapeFace& f = mAuthored.mFaces[static_cast<size_t>(i)];
        S32 face_index = f.mIndex >= 0 ? f.mIndex : i;
        if (face_index >= 0 && face_index < num)
        {
            by_face[static_cast<size_t>(face_index)] = i;
        }
    }

    for (S32 i = 0; i < num; ++i)
    {
        const S32 src = by_face[static_cast<size_t>(i)];
        if (src < 0)
        {
            continue;
        }
        const SSAtmoEnvLandscapeFace& f = mAuthored.mFaces[static_cast<size_t>(src)];

        // A fully-default block (nothing authored) is skippable - capture only writes blocks
        // that differ from the default texture entry. A tint-only block (texture and
        // material both empty but colour authored) is NOT default, so the test mirrors the
        // TE defaults rather than just the two ids.
        const bool empty = f.mTexture.isNull() && f.mMaterial.isNull()
            && near_v3(LLVector3(f.mRepeats.mV[VX], f.mRepeats.mV[VY], f.mRepeats.mV[VZ]), LLVector3(1.f, 1.f, 0.f))
            && llabs(f.mRepeats.mV[VW]) < 1e-4f
            && llabs(f.mRotation) < 1e-4f
            && f.mColor == LLColor4::white;
        if (empty)
        {
            continue;
        }

        LLTextureEntry te;
        if (i < (S32)getNumTEs())
        {
            te = *getTE((U8)i);
        }

        if (!f.mTexture.isNull())
        {
            te.setID(f.mTexture);
            te.setScaleS(f.mRepeats.mV[VX]);
            te.setScaleT(f.mRepeats.mV[VY]);
            te.setOffsetS(f.mRepeats.mV[VZ]);
            te.setOffsetT(f.mRepeats.mV[VW]);
            te.setRotation(f.mRotation);
        }
        te.setColor(f.mColor);
        if (f.mAlphaMode != 0)
        {
            // Alpha mode lives on the face's material params, not the TE - carry any
            // existing material through and stamp the mode onto it.
            LLMaterialPtr mat = te.getMaterialParams();
            if (mat.isNull())
            {
                mat = new LLMaterial();
            }
            mat->setDiffuseAlphaMode(f.mAlphaMode);
            te.setMaterialParams(mat);
        }

        setTE((U8)i, te);

        if (!f.mMaterial.isNull())
        {
            setRenderMaterialID(i, f.mMaterial, false, true);
        }
    }

    mAppliedFaces = num;
}

bool SSAtmoLandscapeObject::captureToRecord(SSAtmoEnvLandscape& record)
{
    bool changed = false;

    if (record.mLocked)
    {
        const LLVector3 offset = getPositionRegion();
        if (!near_v3(offset, mAuthored.mLockedOffset))
        {
            record.mLockedOffset = offset;
            changed = true;
        }
    }
    else
    {
        const LLVector3d global = getPositionGlobal();
        if (!near_v3d(global, mAuthored.mFreeGlobal))
        {
            record.mFreeGlobal = global;
            changed = true;
        }
    }

    const LLQuaternion rot = getRotation();
    if (!near_quat(rot, mAuthored.mRotation))
    {
        record.mRotation = rot;
        changed = true;
    }

    const LLVector3 scale = getScale();
    if (!near_v3(scale, mAuthored.mScale))
    {
        record.mScale = scale;
        changed = true;
    }

    // Sparse faces: a block is written only when the face differs from the default texture
    // entry (or carries a PBR material). The built list is compared against what we last
    // applied, so an idle object writes nothing and the asset stays clean. Faces are only
    // captured once mesh geometry actually exists - until then the record's authored set is
    // authoritative and must not be replaced by the object's (still empty) face state.
    const S32 num = getNumFaces();
    if (num > 0 && mAppliedFaces >= 0)
    {
        std::vector<SSAtmoEnvLandscapeFace> faces;
        for (S32 i = 0; i < num; ++i)
        {
            LLTextureEntry def;
            LLTextureEntry te = (i < (S32)getNumTEs()) ? *getTE((U8)i) : def;
            const LLUUID mat = getRenderMaterialID((U8)i);
            if (te == def && mat.isNull())
            {
                continue;
            }
            SSAtmoEnvLandscapeFace f;
            f.mIndex = i;
            f.mTexture = te.getID();
            f.mRepeats = LLVector4(te.getScaleS(), te.getScaleT(), te.getOffsetS(), te.getOffsetT());
            f.mRotation = te.getRotation();
            f.mColor = te.getColor();
            f.mAlphaMode = te.getMaterialParams().notNull()
                ? (S32)te.getMaterialParams()->getDiffuseAlphaMode() : 0;
            f.mMaterial = mat;
            faces.push_back(f);
        }

        if (faces.size() != mAuthored.mFaces.size())
        {
            changed = true;
        }
        else
        {
            for (size_t i = 0; i < faces.size(); ++i)
            {
                if (!face_equiv(faces[i], mAuthored.mFaces[i]))
                {
                    changed = true;
                    break;
                }
            }
        }
        if (changed)
        {
            record.mFaces = std::move(faces);
        }
    }

    // Advance the applied snapshot so the next capture diffs against what was just
    // written - an untouched object stays untouched forever after.
    mAuthored = record;

    return changed;
}