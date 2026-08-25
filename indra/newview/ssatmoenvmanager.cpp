/**
 * @file ssatmoenvmanager.cpp
 * @brief Atmo Magic environment manager implementation.
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

#include "ssatmoenvmanager.h"

#include "ssatmoenvplanetarystate.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "llagent.h"
#include "llassetstorage.h"
#include "llenvironment.h"
#include "llfilesystem.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llpermissionsflags.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsettingssky.h"
#include "llsettingsvo.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "roles_constants.h"

// <SS:Nexii> Atmo Magic: environment manager

SSAtmoEnvManager::SSAtmoEnvManager()
{
    // No baseline, no working set. See doc/atmo_magic_environment.md - there is deliberately nothing to fall back to here.
}

bool SSAtmoEnvManager::isModified() const
{
    if (!mHasAsset) return false;
    return !llsd_equals(mWorking.asLLSD(), mBaseline.asLLSD());
}

void SSAtmoEnvManager::revertToBaseline()
{
    if (!mHasAsset) return;
    mWorking = mBaseline;
}

//-----------------------------------------------------------------------------
// Notecard body helpers - shared by createDefaultNotecard() and saveAsNewNotecard(), which differ only in which asset gets serialised.
//-----------------------------------------------------------------------------

namespace
{
    // Wraps a v3 asset's LLSD in the Linden notecard container so the result opens like any other notecard, and fires create_inventory_item + an asset upload the same way v2's
    // SSAtmoTrackManager::exportToNotecard does. name is both the inventory item's name and, if the caller wants it, worth also writing into the asset's own mName before calling this. on_created
    // fires once the asset body has actually finished uploading - not right after the bare inventory-item-metadata create step. Calling it that early was a real bug: the item existed but its content
    // hadn't landed yet, so a caller that immediately tried to load it back (the floater's Create button did exactly this) could race against an item with no asset on it yet and silently do nothing.
    // Either id is null if creation or upload failed.
    void writeAssetAsNotecard(const SSAtmoEnvAsset& asset, const std::string& name,
                               const LLUUID& parent_id_in,
                               std::function<void(const LLUUID& item_id, const LLUUID& asset_id)> on_created)
    {
        std::ostringstream body;
        LLSDSerialize::toPrettyXML(asset.asLLSD(), body);

        LLNotecard nc(LLNotecard::MAX_SIZE);
        nc.setText(body.str());
        std::ostringstream wrapped;
        nc.exportStream(wrapped);
        const std::string asset_text = wrapped.str();

        // parent_id_in is always resolved by the caller by this point - see atmoFolderId() - never defaulted here, since resolving it (creating the folder on a first use) is itself async and this
        // function's own shape stays simpler for not doing that inline.
        const LLUUID parent_id = parent_id_in;

        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
            [asset_text, name, on_created](const LLUUID& new_item_id)
            {
                LLViewerRegion* region = gAgent.getRegion();
                const std::string url = region
                    ? region->getCapability("UpdateNotecardAgentInventory")
                    : std::string();
                if (new_item_id.isNull() || url.empty())
                {
                    LL_WARNS("AtmoMagicEnv") << "Could not create Atmo v3 notecard item" << LL_ENDL;
                    if (on_created) on_created(LLUUID::null, LLUUID::null);
                    return;
                }

                LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
                    new_item_id, LLAssetType::AT_NOTECARD, asset_text,
                    [name, new_item_id, on_created](LLUUID, LLUUID new_asset_id, LLUUID, LLSD)
                    {
                        LL_INFOS("AtmoMagicEnv") << "Saved Atmo v3 environment '" << name
                                                 << "' as asset " << new_asset_id << LL_ENDL;
                        if (on_created) on_created(new_item_id, new_asset_id);
                    },
                    nullptr);

                LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            });

        create_inventory_item(gAgentID, gAgentSessionID, parent_id, LLTransactionID::tnull,
                               name, "Atmo Magic environment", LLAssetType::AT_NOTECARD,
                               LLInventoryType::IT_NOTECARD, NO_INV_SUBTYPE, PERM_ALL, cb);
    }
}

//-----------------------------------------------------------------------------
// Creation
//-----------------------------------------------------------------------------

// static
void SSAtmoEnvManager::atmoFolderId(std::function<void(const LLUUID&)> on_ready)
{
    static const std::string ATMO_FOLDER_NAME = "Atmo Magic";

    const LLUUID settings_folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_SETTINGS);

    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(settings_folder, cats, items);
    if (cats)
    {
        for (LLViewerInventoryCategory* cat : *cats)
        {
            if (cat->getName() == ATMO_FOLDER_NAME)
            {
                if (on_ready) on_ready(cat->getUUID());
                return;
            }
        }
    }

    // Not found - create it. Only reached the first time this is ever called for an account; every call after this finds it above.
    gInventory.createNewCategory(settings_folder, LLFolderType::FT_NONE, ATMO_FOLDER_NAME,
        [on_ready](const LLUUID& new_cat_id)
        {
            if (on_ready) on_ready(new_cat_id);
        });
}

namespace
{
    // The shared tail of createDefaultNotecard(): resolve the destination folder if the caller didn't name one, write, and hand the written asset itself back alongside the ids - see the header on
    // why on_created must receive the exact object that was serialised rather than letting callers regenerate it.
    void writeDefaultNotecard(const SSAtmoEnvAsset& def, const LLUUID& parent_id,
                               std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
    {
        auto write = [def, on_created](const LLUUID& folder_id)
        {
            writeAssetAsNotecard(def, def.mName, folder_id,
                [def, on_created](const LLUUID& item_id, const LLUUID& asset_id)
                {
                    if (on_created) on_created(item_id, asset_id, def);
                });
        };

        if (parent_id.notNull())
        {
            write(parent_id);
            return;
        }
        SSAtmoEnvManager::atmoFolderId(write);
    }
}

namespace
{
    // Creation-time seeding fetches this set of PBR sky assets at once and keyframes each where the track's OWN sun actually puts it - see seedSkyPhases below. These are an authored set, not the
    // viewer's stock skies. The four stock non-legacy ones this replaces are ARCHIVED here rather than deleted, since going back to them is a matter of swapping the list:
    // LLEnvironment::KNOWN_SKY_MIDNIGHT / _SUNRISE / _MIDDAY / _SUNSET Nothing below knows how many skies there are or what times of day they depict - the placement is measured, not assumed - so
    // changing this list is the whole change.
    const S32 SEED_SKY_COUNT = 4;
    const char* const SEED_SKY_ID[SEED_SKY_COUNT] = {
        "7250bab8-0a2c-0cb7-8161-6717e194da43",  // Daylight
        "db8115a4-9549-9f7d-97ca-a791d0a99a0f",  // Night
        "cd8afef7-4276-3f46-6122-6165d97f3e87",  // Sunrise
        "7b43eefd-f390-0c79-c30e-a03b3e0ef9c8"   // Sunset
    };

    // Names for the log only. Nothing reads them to decide placement - the whole point of measuring each sky's own sun is that "Sunrise" lands at sunrise because its sun is at sunrise, not because
    // of what it is called. They are here so a failed fetch or a nudged phase names the sky an author would recognise instead of an index.
    const char* const SEED_SKY_NAME[SEED_SKY_COUNT] = {
        "Daylight", "Night", "Sunrise", "Sunset"
    };

    // Used only when the track has no sun to measure the skies against (a homeless or emitterless world): spread evenly over the cycle in list order, which claims nothing about which sky is which
    // time of day beyond the order they were listed in. Snapped like everything else here - see ss_atmoenv_snap_phase.
    F64 seedSkyEvenPhase(S32 slot)
    {
        return ss_atmoenv_snap_phase((F64)slot / (F64)SEED_SKY_COUNT);
    }

    // Joins the concurrent getSettingsAsset() calls: each callback fills its own slot (or leaves it null on failure) and decrements mPending exactly once - mDone is the guard that makes "exactly
    // once" hold even against a hypothetical double-fire - and whichever callback lands last builds and writes the asset. Held by shared_ptr so it survives however the fetches interleave, including
    // a cached asset resolving synchronously inside the request call.
    struct SeedSkyCollector
    {
        LLSettingsSky::ptr_t mSkies[SEED_SKY_COUNT];
        bool mDone[SEED_SKY_COUNT] = { false };
        S32 mPending = SEED_SKY_COUNT;
    };

    // Where the sun stands in a given sky, as a direction in the observer's sky frame. EEP stores it as a rotation carrying +X onto that direction (see convert_azimuth_and_altitude_to_quat in
    // llsettingssky.cpp) - the same frame and the same convention the applier publishes ours in, so the two are directly comparable.
    LLVector3 seedSkySunDirection(const LLSettingsSky& sky)
    {
        LLVector3 dir = LLVector3::x_axis * sky.getSunRotation();
        if (dir.normalize() < 0.0001f) dir = LLVector3::z_axis;
        return dir;
    }

    // Where each fetched sky belongs on this track's cycle, measured against the track's own sun rather than assumed. A sky was painted for a particular sun position and carries it, so the honest
    // placement is "the phase at which OUR sun stands closest to where THAT sky's sun stands". Matching the whole direction rather than just its height is what makes this work for an arbitrary set
    // of skies: every elevation below the peak happens twice a day, so elevation alone cannot tell a dawn sky from a dusk one, while their suns sit on opposite sides of the sky. Nothing here needs
    // to know what a sky is called or which time of day it was meant to be. A set of six is placed the same way a set of two or twenty would be, and a sky whose sun sits higher than this world's sun
    // ever climbs still lands at the closest approach rather than failing. This is also what makes seeding correct for a world whose noon is not at phase 0.5 - which is any world with an authored
    // orbital phase, the default Earth included: with its 1 AU orbit the sun culminates at phase 0.75, so any fixed-phase placement would put every sky a quarter of a cycle from the sun it
    // describes.
    void seedSkyPhases(const SSAtmoEnvTrack& track, const SeedSkyCollector& skies,
                       F64 (&out_phase)[SEED_SKY_COUNT])
    {
        for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
        {
            out_phase[slot] = seedSkyEvenPhase(slot);
        }

        // Whichever body the renderer will actually light this world with - asked of the same function the applier asks, so seeding can never measure against a different "sun" than the one that
        // rises.
        SSAtmoEnvResolvedBody sun;
        SSAtmoEnvResolvedBody moon;
        SSAtmoEnvPlanetaryResolver::resolveLightRoles(track.mPlanetary, sun, moon);
        if (sun.mBodyIndex < 0) return;

        const S32 home = track.mPlanetary.homeBodyIndex();
        const F32 tilt = (home >= 0)
            ? track.mPlanetary.mBodies[static_cast<size_t>(home)].mAxialTiltDeg : 0.f;
        const F32 lat = (home >= 0)
            ? track.mPlanetary.mBodies[static_cast<size_t>(home)].mLatitudeDeg : 0.f;

        F64 measured[SEED_SKY_COUNT];
        for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
        {
            measured[slot] = out_phase[slot];
            if (!skies.mSkies[slot]) continue;   // never fetched; nothing will be stamped there anyway

            // Snapped to the scrubber's own grid: a phase measured from a world's sun lands anywhere, and a keyframe an author cannot scrub onto is one they cannot edit. Half a stop of error in when
            // a sky appears is invisible; a keyframe you can never select is not.
            measured[slot] = ss_atmoenv_snap_phase(
                SSAtmoEnvPlanetaryResolver::phaseForSunDirection(
                    sun.mDirection, tilt, lat, seedSkySunDirection(*skies.mSkies[slot])));
        }

        // The sun's own daily curve, which the branch split below solves against - see SSAtmoEnvDiurnalArc.
        const SSAtmoEnvDiurnalArc arc =
            SSAtmoEnvPlanetaryResolver::diurnalArc(sun.mDirection, tilt, lat);

        // Skies whose suns sit at the same height get put on OPPOSITE halves of the day before anything else is decided. A sunrise sky and a sunset sky are the same sun at the same elevation - that
        // is what makes them look alike - so measuring the direction alone lands them within a few degrees of each other and the separation pass below then parks them side by side, which reads as
        // the pair having been merged into one. The information that tells them apart is not in the sun's position at all: it is which way the sun is GOING. A static sky cannot say, so the list
        // order decides - the earlier of a colliding low-sun pair takes the rising branch, the later takes the setting one. That is the one place a sky's position in the list is allowed to mean
        // something, and only ever as a tie-break.
        {
            // "Low" is a sun within this of the horizon either way, which is the band the twilight skies live in and nothing else does.
            const F32 LOW_SUN_SIN = 0.25f;   // about 14 degrees

            for (S32 a = 0; a < SEED_SKY_COUNT; ++a)
            {
                if (!skies.mSkies[a]) continue;
                const F32 sin_a = seedSkySunDirection(*skies.mSkies[a]).mV[VZ];
                if (fabsf(sin_a) > LOW_SUN_SIN) continue;

                for (S32 b = a + 1; b < SEED_SKY_COUNT; ++b)
                {
                    if (!skies.mSkies[b]) continue;
                    const F32 sin_b = seedSkySunDirection(*skies.mSkies[b]).mV[VZ];
                    if (fabsf(sin_b) > LOW_SUN_SIN) continue;

                    F64 gap = std::fabs(measured[a] - measured[b]);
                    gap = llmin(gap, 1.0 - gap);
                    if (gap >= 0.08) continue;   // already well apart; leave them

                    F64 rising = 0.0, setting = 0.0;
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(arc, sin_a, true, rising);
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(arc, sin_b, false, setting);

                    LL_INFOS("AtmoMagicEnv") << "Seed skies " << SEED_SKY_NAME[a] << " and "
                        << SEED_SKY_NAME[b] << " have their suns at the same height; putting "
                        << SEED_SKY_NAME[a] << " on the rising side (" << rising << ") and "
                        << SEED_SKY_NAME[b] << " on the setting side (" << setting << ")" << LL_ENDL;

                    measured[a] = ss_atmoenv_snap_phase(rising);
                    measured[b] = ss_atmoenv_snap_phase(setting);
                }
            }
        }

        // Two skies can measure to the same instant - a set with three daylight skies in it very likely has at least two suns within a few degrees of each other. That is not an error and not a
        // reason to throw the measurement away: they really are at the same time of day, and what they differ in is the look. Separating them by the minimum readable gap, in measured order, keeps
        // every sky in the cycle and keeps the order their suns actually put them in. (An earlier version abandoned the whole measurement and spread the set evenly on any collision. One pair landing
        // together would then move every other sky off the phase its own sun asked for, which is a great deal of damage from a little ambiguity.) One scrubber stop: the closest two keyframes can sit
        // and still be separately reachable. Anything finer would put them on the same stop, which is the collision this pass exists to resolve.
        const F64 SEED_PHASE_MIN_GAP = 1.0 / (F64)SS_ATMOENV_PREVIEW_STEPS;

        S32 order[SEED_SKY_COUNT];
        S32 order_count = 0;
        for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
        {
            if (skies.mSkies[slot]) order[order_count++] = slot;
        }
        std::sort(order, order + order_count,
                  [&measured](S32 a, S32 b) { return measured[a] < measured[b]; });

        for (S32 k = 1; k < order_count; ++k)
        {
            const S32 prev = order[k - 1];
            const S32 here = order[k];
            const F64 gap = measured[here] - measured[prev];
            if (gap >= SEED_PHASE_MIN_GAP) continue;

            const F64 pushed = ss_atmoenv_snap_phase(measured[prev] + SEED_PHASE_MIN_GAP);
            LL_INFOS("AtmoMagicEnv") << "Seed skies " << SEED_SKY_NAME[prev] << " and "
                << SEED_SKY_NAME[here] << " measure to the same point in the cycle ("
                << measured[here] << "); moving the second to " << pushed
                << " so both survive" << LL_ENDL;
            measured[here] = pushed;
        }

        // The last one may have been pushed past the end of the cycle by the pass above. Wrapping it would put it before the first sky and undo the ordering that pass just established, so it is
        // clamped just inside instead: phases are a circle, but a keyframe at 1.0 and one at 0.0 are the same keyframe.
        if (order_count > 0)
        {
            const S32 last = order[order_count - 1];
            measured[last] = ss_atmoenv_snap_phase(
                llmin(measured[last], 1.0 - SEED_PHASE_MIN_GAP));
        }

        for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
        {
            out_phase[slot] = measured[slot];
        }
    }

    // The fallback matrix, applied once every fetch has resolved:
    //   two or more -> a day cycle, each arrived sky keyframed at its own
    //                  measured phase; the cycle interpolates across
    //                  whatever did not arrive
    //   exactly 1   -> plain-seed from it - one sky cannot describe a
    //                  cycle, so keyframing it would just wrap a constant
    //                  in animation clothing
    //   none        -> plain makeDefault(), warnings already logged per
    //                  failed fetch
    SSAtmoEnvAsset buildSeededDefault(const SeedSkyCollector& skies)
    {
        SSAtmoEnvAsset def = SSAtmoEnvAsset::makeDefault();
        if (def.mTracks.empty()) return def;

        S32 arrived = 0;
        for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
        {
            if (skies.mSkies[slot]) ++arrived;
        }
        if (arrived == 0) return def;

        SSAtmoEnvTrack& ground = def.mTracks[0];
        if (arrived == 1)
        {
            for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
            {
                if (!skies.mSkies[slot]) continue;
                // The sky's fields split across two structs (haze/lighting vs. the legacy cloud layer), so both seed here - one fetched sky, transcribed in full.
                ground.mAtmosphere.fromSettingsSky(*skies.mSkies[slot]);
                ground.mCloudDome.fromSettingsSky(*skies.mSkies[slot]);
            }
            return def;
        }

        // Measured placement for everything that arrived - see seedSkyPhases. Slots that failed to fetch carry a phase too, but nothing is stamped at them.
        F64 phase[SEED_SKY_COUNT];
        seedSkyPhases(ground, skies, phase);

        for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
        {
            if (!skies.mSkies[slot]) continue;
            ground.mAtmosphere.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
            ground.mCloudDome.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
        }

        // The stock skies each bring their own cloud map, and stamping seven of them leaves the deck changing texture through the day - which is a real thing skies do, but not one anybody asked this
        // to do by default, and it reads as the clouds blinking between shapes rather than evolving. One map for the whole cycle, chosen for being a plausible everyday deck; an author who wants the
        // changes back has a keyframed texture row to put them on.
        ground.mCloudDome.mNoiseTexture =
            SSAtmoEnvKeyframed<LLUUID>(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_LAYERED));

        // Cleanup pass: a field the stamped skies all agree on (the cloud noise map, most of the optics dials) collapses back to a plain value - a constant should not carry four redundant keyframes
        // into every notecard this document ever saves.
        ground.mAtmosphere.collapseConstantKeyframes();
        ground.mCloudDome.collapseConstantKeyframes();
        return def;
    }
}

// static
void SSAtmoEnvManager::createDefaultNotecard(const LLUUID& parent_id,
                                         std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    // Pre-step: fetch EEP's four stock non-legacy skies and seed the ground track's atmosphere + cloud dome as a full day cycle (see buildSeededDefault above), so a fresh environment opens on the
    // Sunrise/Midday/Sunset/Midnight everyone already knows instead of LLSettingsSky's code-baked legacy defaults. Settings assets are immutable and getSettingsAsset() reads through the asset cache,
    // so this is one round trip per sky per cache lifetime, not per creation. Any failure (no asset system, fetch error, or an asset that somehow isn't a sky) degrades per the fallback matrix rather
    // than blocking - creation itself must never be hostage to a fetch.
    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; creating Atmo v3 environment with built-in defaults instead of the stock sky cycle" << LL_ENDL;
        writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
        return;
    }

    LLUUID known_ids[SEED_SKY_COUNT];
    for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
    {
        known_ids[slot] = LLUUID(SEED_SKY_ID[slot]);
    }

    auto collector = std::make_shared<SeedSkyCollector>();
    for (S32 slot = 0; slot < SEED_SKY_COUNT; ++slot)
    {
        LLSettingsVOBase::getSettingsAsset(known_ids[slot],
            [collector, slot, parent_id, on_created](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
            {
                if (collector->mDone[slot]) return; // see SeedSkyCollector - each slot resolves exactly once
                collector->mDone[slot] = true;

                LLSettingsSky::ptr_t sky;
                if (!status && settings)
                {
                    sky = std::dynamic_pointer_cast<LLSettingsSky>(settings);
                }
                if (sky)
                {
                    collector->mSkies[slot] = sky;
                }
                else
                {
                    LL_WARNS("AtmoMagicEnv") << "Could not fetch seed sky " << SEED_SKY_NAME[slot]
                                             << " sky " << asset_id << " (status " << status
                                             << "); seeding the new Atmo v3 environment without it" << LL_ENDL;
                }

                if (--collector->mPending > 0) return;

                // Last fetch in - however they interleaved, this runs once.
                writeDefaultNotecard(buildSeededDefault(*collector), parent_id, on_created);
            });
    }
}

void SSAtmoEnvManager::adoptCreated(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)
{
    mItemID = item_id;
    mAssetID = asset_id;
    mBaseline = asset;

    // Adopted from somewhere. Assume the user put it here: discovery calls noteSource(id, true) straight after its own apply, so the parcel case corrects this a moment later, and everything else - a
    // hand load, a drag-drop, a fresh creation - is correctly the user's and is left alone when they cross a parcel boundary.
    mFromParcel = false;
    mWorking = asset;
    mHasAsset = true;
    mStatus = "Ready.";
}

void SSAtmoEnvManager::saveNotecard(const std::string& name)
{
    if (!mHasAsset) return;

    std::string save_name = name;
    LLStringUtil::trim(save_name);
    if (save_name.empty()) save_name = "Atmo Environment";

    mWorking.mName = save_name;

    // Saving adopts the just-written state as the new baseline immediately - there is no separate "confirm the upload actually landed" step here, matching the v2 floater's behaviour
    // (exportToNotecard has the same shape) - so this happens before the write below even starts, not inside the (async, folder-resolution-dependent) callback.
    mBaseline = mWorking;

    if (mItemID.notNull())
    {
        updateExistingNotecard(save_name);
        return;
    }

    const SSAtmoEnvAsset to_save = mWorking;
    atmoFolderId([this, to_save, save_name](const LLUUID& folder_id)
    {
        writeAssetAsNotecard(to_save, save_name, folder_id,
            [this](const LLUUID& item_id, const LLUUID& asset_id)
            {
                // The very first save after a discovery-only load (mItemID was null): now there is an owned item, so the *next* save updates it in place instead of minting yet another one.
                if (item_id.notNull()) mItemID = item_id;
                if (asset_id.notNull()) mAssetID = asset_id;
            });
    });
}

void SSAtmoEnvManager::updateExistingNotecard(const std::string& name)
{
    const LLUUID item_id = mItemID;
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (item && item->getName() != name
        && gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE))
    {
        LLPointer<LLViewerInventoryItem> new_item = new LLViewerInventoryItem(item);
        new_item->rename(name);
        new_item->updateServer(false);
        gInventory.updateItem(new_item);
        gInventory.notifyObservers();
    }

    std::ostringstream body;
    LLSDSerialize::toPrettyXML(mWorking.asLLSD(), body);
    LLNotecard nc(LLNotecard::MAX_SIZE);
    nc.setText(body.str());
    std::ostringstream wrapped;
    nc.exportStream(wrapped);

    LLViewerRegion* region = gAgent.getRegion();
    const std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
    if (url.empty())
    {
        LL_WARNS("AtmoMagicEnv") << "No UpdateNotecardAgentInventory capability; could not update Atmo v3 notecard in place" << LL_ENDL;
        return;
    }

    LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
        item_id, LLAssetType::AT_NOTECARD, wrapped.str(),
        [name](LLUUID, LLUUID new_asset_id, LLUUID, LLSD)
        {
            LL_INFOS("AtmoMagicEnv") << "Updated Atmo v3 environment '" << name
                                     << "' in place as asset " << new_asset_id << LL_ENDL;
            // Notecard assets are immutable, so an in-place update still lands as a new asset id behind the same item id - keep mAssetID pointing at what's actually current.
            SSAtmoEnvManager::getInstance()->mAssetID = new_asset_id;
        },
        nullptr);
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

//-----------------------------------------------------------------------------
// Loading
//-----------------------------------------------------------------------------

bool SSAtmoEnvManager::loadFromInventory(const LLInventoryItem* item, std::function<void(bool)> on_complete)
{
    if (!item || item->getAssetUUID().isNull()) return false;

    if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE)
        && !gAgent.isGodlike())
    {
        mStatus = "no permission to read that notecard";
        return false;
    }

    mPendingID = item->getAssetUUID();
    mPendingItemID = item->getUUID();
    mStatus = "loading environment...";
    mLoadCompleteCallback = on_complete;

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        finishLoad(false);
        return false;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoEnvManager::onAssetLoaded, nullptr, true);
    return true;
}

void SSAtmoEnvManager::finishLoad(bool success)
{
    std::function<void(bool)> cb;
    cb.swap(mLoadCompleteCallback); // clear before calling - a callback that itself starts another load must not see the old one still set
    if (cb) cb(success);
}

void SSAtmoEnvManager::loadFromAssetId(const LLUUID& asset_id)
{
    // See the header note and the open item in doc/atmo_magic_environment.md: this path does not check ownership, because a parcel-referenced notecard is not necessarily something the agent has a
    // copy of. Kept separate from loadFromInventory() rather than folded together so that distinction stays visible at the call site, not buried in a flag.
    mPendingID = asset_id;
    mPendingItemID.setNull(); // not something the agent owns an item for - see the header note above
    mStatus = "loading environment...";

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        return;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoEnvManager::onAssetLoaded, nullptr, true);
}

// static
void SSAtmoEnvManager::onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status)
{
    SSAtmoEnvManager* self = SSAtmoEnvManager::getInstance();

    // A newer request may have superseded this fetch while it was in flight
    if (asset_id != self->mPendingID) return;
    self->mPendingID.setNull();

    if (status != 0)
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment notecard " << asset_id
                                 << " failed to load, status " << status << LL_ENDL;
        self->mStatus = "notecard unavailable";
        self->finishLoad(false);
        return;
    }

    LLFileSystem file(asset_id, type, LLFileSystem::READ);
    const S32 length = file.getSize();
    if (length <= 0)
    {
        self->mStatus = "notecard empty";
        self->finishLoad(false);
        return;
    }

    std::vector<char> buffer(length + 1);
    file.read((U8*)buffer.data(), length);
    buffer[length] = '\0';

    // Notecard assets are wrapped in the Linden text container; unwrap to the plain body, but tolerate a bare text asset too - same tolerance v2's SSAtmoTrackManager::onNotecardLoaded applies.
    std::string text(buffer.data(), length);
    if (length > 19 && strncmp(buffer.data(), "Linden text version", 19) == 0)
    {
        LLNotecard notecard;
        std::istringstream stream(text);
        if (!notecard.importStream(stream))
        {
            LL_WARNS("AtmoMagicEnv") << "Could not parse Atmo v3 notecard " << asset_id << LL_ENDL;
            self->mStatus = "notecard unreadable";
            self->finishLoad(false);
            return;
        }
        text = notecard.getText();
    }

    self->mAssetID = asset_id;
    self->mItemID = self->mPendingItemID;
    self->mPendingItemID.setNull();
    self->finishLoad(self->applyNotecardText(text, false));
}

bool SSAtmoEnvManager::applyNotecardText(const std::string& text, bool /*from_inventory_permission_check*/)
{
    LLSD sd;
    std::istringstream stream(text);

    bool parsed = false;
    if (text.find("<llsd") != std::string::npos)
    {
        parsed = (LLSDSerialize::fromXML(sd, stream) != LLSDParser::PARSE_FAILURE);
    }
    if (!parsed)
    {
        std::istringstream retry(text);
        parsed = (LLSDSerialize::fromNotation(sd, retry, (S32)text.size()) != LLSDParser::PARSE_FAILURE);
    }

    if (!parsed)
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment notecard is not valid LLSD" << LL_ENDL;
        mStatus = "notecard is not valid LLSD";
        return false;
    }

    return adoptParsedAsset(sd);
}

bool SSAtmoEnvManager::applyExternalLLSD(const LLUUID& source_id, const LLSD& sd)
{
    mAssetID = source_id;
    mItemID.setNull(); // not the agent's own copy - see the header note on mItemID
    return adoptParsedAsset(sd);
}

bool SSAtmoEnvManager::applyExternalNotecardText(const LLUUID& source_id, const std::string& text)
{
    mAssetID = source_id;
    mItemID.setNull();
    return applyNotecardText(text, false);
}

void SSAtmoEnvManager::unload()
{
    if (!mHasAsset) return;

    LL_INFOS("AtmoMagicEnv") << "Unloading the Atmo Magic environment; the world"
                                " falls back to the parcel or region setting" << LL_ENDL;

    mHasAsset = false;
    mWorking = SSAtmoEnvAsset();
    mBaseline = SSAtmoEnvAsset();
    mSourceAssetId.setNull();
    mFromParcel = false;
    clearPreviewPhaseOverride();
}

bool SSAtmoEnvManager::adoptParsedAsset(const LLSD& sd)
{
    SSAtmoEnvAsset parsed_asset;
    std::string error;
    if (!parsed_asset.fromLLSD(sd, error))
    {
        LL_WARNS("AtmoMagicEnv") << "Atmo v3 environment notecard rejected: " << error << LL_ENDL;
        mStatus = "notecard invalid: " + error;
        return false;
    }

    mBaseline = parsed_asset;
    mWorking = parsed_asset;
    mHasAsset = true;
    mStatus = "Ready.";
    return true;
}

// </SS:Nexii>
