/**
 * @file ssatmolandscapeobject.h
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

#ifndef SS_ATMO_LANDSCAPE_OBJECT_H
#define SS_ATMO_LANDSCAPE_OBJECT_H

#include "lluuid.h"
#include "llvovolume.h"

#include "ssatmoenvasset.h"

class LLViewerRegion;

// <SS:Nexii> The landscape LOD stretch: stock LOD switching is dialed for region viewing
// (roughly 0-256 m); landscape scenery is viewed across 0-2048 m, so the object's distance
// term is multiplied by this factor (an 8x stretch) - the ssLODDistanceScale hook in
// LLVOVolume folds it into the stock formula, Mesh-detail preference and DebugObjectLODs
// untouched. A starting curve: see doc/atmo_landscape/design_synthesis.md.
constexpr F32 SS_LANDSCAPE_LOD_STRETCH = 0.125f;

// A landscape object is the SSWater story applied to volumes: a real LLVOVolume that the
// pcode factory cannot build (it reuses the stock LL_PCODE_VOLUME), so the landscape world
// news it directly and hands it to gObjectList.adoptViewerObject. Local-content flagged, so
// every server send touching it is gated away; full-perms flagged, so the stock editor's
// permission checks treat it as fully editable; its own volume params (sculpt id = the mesh
// asset) drive the entire stock fetch/LOD/face/pool pipeline.
//
// The record the object was last applied with is COPIED in, never pointed at: the live
// records live in the working asset's track vector, and a pointer into that vector would
// dangle the instant the floater adds or removes a record.
class SSAtmoLandscapeObject : public LLVOVolume
{
public:
    SSAtmoLandscapeObject(const LLUUID& id, LLViewerRegion* regionp, const SSAtmoEnvLandscape& record);

    // Re-apply placement + face state from a (possibly updated) record. The mesh volume
    // params are only re-set when the asset id changes - identical params re-hit the repo's
    // cached system volume, but the rebuild is worth skipping.
    void applyRecord(const SSAtmoEnvLandscape& record);

    // Per-frame: write the object's transform + sparse face state back into a record
    // copy. Returns true when anything changed - the reconcile funnel's dirty signal.
    // Non-const: the applied snapshot mAuthored is advanced to match what was written,
    // so an unchanged object keeps reporting unchanged.
    bool captureToRecord(SSAtmoEnvLandscape& record);

    // Faces materialise when mesh LOD geometry lands; the world calls this every frame
    // until the applied face count matches the volume's.
    void applyFaces();

    // The adoption key.
    const LLUUID& meshId() const { return mAuthored.mMeshId; }

    // The relaxed landscape LOD range.
    F32 ssLODDistanceScale() const override { return SS_LANDSCAPE_LOD_STRETCH; }

private:
    void applyMesh(const SSAtmoEnvLandscape& record);
    void applyPlacement(const SSAtmoEnvLandscape& record);

    // The authored snapshot this object last applied (or captured). Copy semantics on
    // purpose - see the class note.
    SSAtmoEnvLandscape mAuthored;

    // TE count the record's faces were last applied to; -1 until the first apply.
    S32 mAppliedFaces = -1;
};

#endif // SS_ATMO_LANDSCAPE_OBJECT_H