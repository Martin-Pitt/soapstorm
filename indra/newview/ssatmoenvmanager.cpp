/**
 * @file ssatmoenvmanager.cpp
 * @brief See ssatmoenvmanager.h.
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

#include "ssprecippreset.h"

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

// Nothing loads implicitly - v3 is opt-in end to end.
SSAtmoEnvManager::SSAtmoEnvManager()
{
}

// Whether the working asset differs from its load-time baseline.
bool SSAtmoEnvManager::isModified() const
{
    if (!mHasAsset) return false;
    return !llsd_equals(mWorking.asLLSD(), mBaseline.asLLSD());
}

// Back to the load-time copy.
void SSAtmoEnvManager::revertToBaseline()
{
    if (!mHasAsset) return;
    mWorking = mBaseline;
}

namespace
{
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

// Finds (or on first ever use creates) the inventory folder v3 notecards live in, async.
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

    gInventory.createNewCategory(settings_folder, LLFolderType::FT_NONE, ATMO_FOLDER_NAME,
        [on_ready](const LLUUID& new_cat_id)
        {
            if (on_ready) on_ready(new_cat_id);
        });
}

namespace
{
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
    // The shipped skies the stock day cycle seeds from.
    const S32 STOCK_SEED_SKY_COUNT = 4;
    const char* const STOCK_SEED_SKY_ID[STOCK_SEED_SKY_COUNT] = {
        "7250bab8-0a2c-0cb7-8161-6717e194da43",
        "db8115a4-9549-9f7d-97ca-a791d0a99a0f",
        "cd8afef7-4276-3f46-6122-6165d97f3e87",
        "7b43eefd-f390-0c79-c30e-a03b3e0ef9c8"
    };

    const char* const STOCK_SEED_SKY_NAME[STOCK_SEED_SKY_COUNT] = {
        "Daylight", "Night", "Sunrise", "Sunset"
    };

    struct SeedSkyCollector
    {
        std::vector<LLSettingsSky::ptr_t> mSkies;
        std::vector<std::string> mNames;
        std::vector<bool> mDone;
        S32 mPending = 0;
    };

    // Log label for a requested sky: the caller's name when it has one, otherwise an index.
    std::string seedSkyLabel(const SeedSkyCollector& skies, size_t slot)
    {
        if (slot < skies.mNames.size() && !skies.mNames[slot].empty())
        {
            return skies.mNames[slot];
        }
        return llformat("sky %d", (S32)slot);
    }

    // Fetches a list of skies by asset id and calls on_done once every fetch has settled. A failed
    // fetch logs and leaves a null slot; the seed builder decides what a hole means.
    void fetchSeedSkies(const std::vector<LLUUID>& asset_ids,
                        const std::vector<std::string>& names,
                        std::function<void(const SeedSkyCollector&)> on_done)
    {
        auto collector = std::make_shared<SeedSkyCollector>();
        collector->mSkies.resize(asset_ids.size());
        collector->mNames.resize(asset_ids.size());
        for (size_t slot = 0; slot < asset_ids.size() && slot < names.size(); ++slot)
        {
            collector->mNames[slot] = names[slot];
        }
        collector->mDone.resize(asset_ids.size(), false);
        collector->mPending = (S32)asset_ids.size();

        for (size_t slot = 0; slot < asset_ids.size(); ++slot)
        {
            LLSettingsVOBase::getSettingsAsset(asset_ids[slot],
                [collector, slot, on_done](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
                {
                    if (collector->mDone[slot]) return;
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
                        LL_WARNS("AtmoMagicEnv") << "Could not fetch seed sky " << seedSkyLabel(*collector, slot)
                                                 << " sky " << asset_id << " (status " << status
                                                 << "); seeding the new Atmo v3 environment without it" << LL_ENDL;
                    }

                    if (--collector->mPending > 0) return;

                    if (on_done) on_done(*collector);
                });
        }
    }

    // Even-spread fallback placement when no sun exists to measure the seed skies against:
    // one sky per equal slice of the arrived set.
    F64 seedSkyEvenPhase(size_t slot, size_t count)
    {
        return count > 0 ? ss_atmoenv_snap_phase((F64)slot / (F64)count) : 0.0;
    }

    // Where a fetched sky's own sun stands, in the observer frame.
    LLVector3 seedSkySunDirection(const LLSettingsSky& sky)
    {
        LLVector3 dir = LLVector3::x_axis * sky.getSunRotation();
        if (dir.normalize() < 0.0001f) dir = LLVector3::z_axis;
        return dir;
    }

    void seedSkyPhases(const SSAtmoEnvTrack& track, const SeedSkyCollector& skies,
                       std::vector<F64>& out_phase)
    {
        const size_t count = skies.mSkies.size();

        out_phase.resize(count);
        for (size_t slot = 0; slot < count; ++slot)
        {
            out_phase[slot] = seedSkyEvenPhase(slot, count);
        }

        SSAtmoEnvResolvedBody sun;
        SSAtmoEnvResolvedBody moon;
        SSAtmoEnvPlanetaryResolver::resolveLightRoles(track.mPlanetary, sun, moon);
        if (sun.mBodyIndex < 0) return;

        const S32 home = track.mPlanetary.homeBodyIndex();
        const F32 tilt = (home >= 0)
            ? track.mPlanetary.mBodies[static_cast<size_t>(home)].mAxialTiltDeg : 0.f;
        const F32 lat = (home >= 0)
            ? track.mPlanetary.mBodies[static_cast<size_t>(home)].mLatitudeDeg : 0.f;

        std::vector<F64> measured(count);
        for (size_t slot = 0; slot < count; ++slot)
        {
            measured[slot] = out_phase[slot];
            if (!skies.mSkies[slot]) continue;

            measured[slot] = ss_atmoenv_snap_phase(
                SSAtmoEnvPlanetaryResolver::phaseForSunDirection(
                    sun.mDirection, tilt, lat, seedSkySunDirection(*skies.mSkies[slot])));
        }

        const SSAtmoEnvDiurnalArc arc =
            SSAtmoEnvPlanetaryResolver::diurnalArc(sun.mDirection, tilt, lat);

        {
            const F32 LOW_SUN_SIN = 0.25f;

            for (size_t a = 0; a < count; ++a)
            {
                if (!skies.mSkies[a]) continue;
                const F32 sin_a = seedSkySunDirection(*skies.mSkies[a]).mV[VZ];
                if (llabs(sin_a) > LOW_SUN_SIN) continue;

                for (size_t b = a + 1; b < count; ++b)
                {
                    if (!skies.mSkies[b]) continue;
                    const F32 sin_b = seedSkySunDirection(*skies.mSkies[b]).mV[VZ];
                    if (llabs(sin_b) > LOW_SUN_SIN) continue;

                    F64 gap = llabs(measured[a] - measured[b]);
                    gap = llmin(gap, 1.0 - gap);
                    if (gap >= 0.08) continue;

                    F64 rising = 0.0, setting = 0.0;
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(arc, sin_a, true, rising);
                    SSAtmoEnvPlanetaryResolver::phaseForElevation(arc, sin_b, false, setting);

                    LL_INFOS("AtmoMagicEnv") << "Seed skies " << seedSkyLabel(skies, a) << " and "
                        << seedSkyLabel(skies, b) << " have their suns at the same height; putting "
                        << seedSkyLabel(skies, a) << " on the rising side (" << rising << ") and "
                        << seedSkyLabel(skies, b) << " on the setting side (" << setting << ")" << LL_ENDL;

                    measured[a] = ss_atmoenv_snap_phase(rising);
                    measured[b] = ss_atmoenv_snap_phase(setting);
                }
            }
        }

        const F64 SEED_PHASE_MIN_GAP = 1.0 / (F64)SS_ATMOENV_PREVIEW_STEPS;

        std::vector<size_t> order;
        for (size_t slot = 0; slot < count; ++slot)
        {
            if (skies.mSkies[slot]) order.push_back(slot);
        }
        std::sort(order.begin(), order.end(),
                  [&measured](size_t a, size_t b) { return measured[a] < measured[b]; });

        for (size_t k = 1; k < order.size(); ++k)
        {
            const size_t prev = order[k - 1];
            const size_t here = order[k];
            const F64 gap = measured[here] - measured[prev];
            if (gap >= SEED_PHASE_MIN_GAP) continue;

            const F64 pushed = ss_atmoenv_snap_phase(measured[prev] + SEED_PHASE_MIN_GAP);
            LL_INFOS("AtmoMagicEnv") << "Seed skies " << seedSkyLabel(skies, prev) << " and "
                << seedSkyLabel(skies, here) << " measure to the same point in the cycle ("
                << measured[here] << "); moving the second to " << pushed
                << " so both survive" << LL_ENDL;
            measured[here] = pushed;
        }

        if (!order.empty())
        {
            const size_t last = order.back();
            measured[last] = ss_atmoenv_snap_phase(
                llmin(measured[last], 1.0 - SEED_PHASE_MIN_GAP));
        }

        out_phase = measured;
    }

    // The seeded default asset: a day cycle from whichever seed skies arrived, or plain defaults from none.
    SSAtmoEnvAsset buildSeededDefault(const SeedSkyCollector& skies)
    {
        SSAtmoEnvAsset def = SSAtmoEnvAsset::makeDefault();
        if (def.mTracks.empty()) return def;

        std::vector<size_t> arrived;
        for (size_t slot = 0; slot < skies.mSkies.size(); ++slot)
        {
            if (skies.mSkies[slot]) arrived.push_back(slot);
        }
        if (arrived.empty()) return def;

        SSAtmoEnvTrack& ground = def.mTracks[0];
        if (arrived.size() == 1)
        {
            const LLSettingsSky::ptr_t& sky = skies.mSkies[arrived[0]];
            ground.mAtmosphere.fromSettingsSky(*sky);
            ground.mCloudDome.fromSettingsSky(*sky);
            return def;
        }

        std::vector<F64> phase;
        seedSkyPhases(ground, skies, phase);

        for (size_t slot : arrived)
        {
            ground.mAtmosphere.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
            ground.mCloudDome.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
        }

        ground.mCloudDome.mNoiseTexture =
            SSAtmoEnvKeyframed<LLUUID>(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_LAYERED));

        ground.mAtmosphere.collapseConstantKeyframes();
        ground.mCloudDome.collapseConstantKeyframes();
        return def;
    }

    // <SS:Nexii> The template's atmosphere columns as a TINT over the seeded cycle: each column
    // divides its value out of the reference sky (Daylight - the "no mood" anchor the dial was
    // tuned against), and the factor multiplies every stamped keyframe, so the archetype's mood
    // rides the whole day - Alien World stays alien at dawn and dusk - instead of flattening the
    // cycle back to a constant.
    void tintSeededCycle(SSAtmoEnvTrack& track, const SSAtmoEnvTemplate& tmpl, const LLSettingsSky& reference)
    {
        const LLColor3 ref_horizon = reference.getBlueHorizon();
        const LLColor3 ref_density = reference.getBlueDensity();
        const LLColor3 horizon_factor(tmpl.mBlueHorizon.mV[0] / llmax(ref_horizon.mV[0], 0.01f),
                                      tmpl.mBlueHorizon.mV[1] / llmax(ref_horizon.mV[1], 0.01f),
                                      tmpl.mBlueHorizon.mV[2] / llmax(ref_horizon.mV[2], 0.01f));
        const LLColor3 density_factor(tmpl.mBlueDensity.mV[0] / llmax(ref_density.mV[0], 0.01f),
                                      tmpl.mBlueDensity.mV[1] / llmax(ref_density.mV[1], 0.01f),
                                      tmpl.mBlueDensity.mV[2] / llmax(ref_density.mV[2], 0.01f));
        const F32 haze_factor   = tmpl.mHazeDensity  / llmax(reference.getHazeDensity(), 0.001f);
        const F32 maxalt_factor = tmpl.mMaxAltitudeM / llmax(reference.getMaxY(), 1.f);
        const F32 cover_factor  = tmpl.mDomeCoverage / llmax(reference.getCloudShadow(), 0.01f);

        for (SSAtmoEnvKeyframe<LLColor3>& kf : track.mAtmosphere.mBlueHorizon.keyframes())
        {
            kf.mValue = LLColor3(kf.mValue.mV[0] * horizon_factor.mV[0],
                                 kf.mValue.mV[1] * horizon_factor.mV[1],
                                 kf.mValue.mV[2] * horizon_factor.mV[2]);
        }
        for (SSAtmoEnvKeyframe<LLColor3>& kf : track.mAtmosphere.mBlueDensity.keyframes())
        {
            kf.mValue = LLColor3(kf.mValue.mV[0] * density_factor.mV[0],
                                 kf.mValue.mV[1] * density_factor.mV[1],
                                 kf.mValue.mV[2] * density_factor.mV[2]);
        }
        for (SSAtmoEnvKeyframe<F32>& kf : track.mAtmosphere.mHazeDensity.keyframes())
        {
            kf.mValue = llmax(kf.mValue * haze_factor, 0.f);
        }
        for (SSAtmoEnvKeyframe<F32>& kf : track.mAtmosphere.mMaxAltitude.keyframes())
        {
            kf.mValue = llmax(kf.mValue * maxalt_factor, 1.f);
        }
        for (SSAtmoEnvKeyframe<F32>& kf : track.mCloudDome.mCoverage.keyframes())
        {
            kf.mValue = llclamp(kf.mValue * cover_factor, 0.f, 1.f);
        }
    }
}

// Creates a plain midday-defaults environment: no fetching, no seeding.
void SSAtmoEnvManager::createEmptyNotecard(const LLUUID& parent_id,
                                           std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
}

// Seeds a day cycle from a list of the author's own skies, the same measure-and-stamp algorithm
// the stock seed uses. An empty list makes the empty environment.
void SSAtmoEnvManager::createFromSkies(const std::vector<LLUUID>& sky_asset_ids,
                                       const std::vector<std::string>& sky_names,
                                       const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    if (sky_asset_ids.empty())
    {
        createEmptyNotecard(parent_id, on_created);
        return;
    }

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; creating Atmo v3 environment with built-in defaults instead of the chosen skies" << LL_ENDL;
        writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
        return;
    }

    fetchSeedSkies(sky_asset_ids, sky_names,
        [parent_id, on_created](const SeedSkyCollector& skies)
        {
            writeDefaultNotecard(buildSeededDefault(skies), parent_id, on_created);
        });
}

// The stock create path: the four shipped seed skies.
void SSAtmoEnvManager::createDefaultNotecard(const LLUUID& parent_id,
                                         std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    std::vector<LLUUID> ids;
    std::vector<std::string> names;
    ids.reserve(STOCK_SEED_SKY_COUNT);
    names.reserve(STOCK_SEED_SKY_COUNT);
    for (S32 slot = 0; slot < STOCK_SEED_SKY_COUNT; ++slot)
    {
        ids.push_back(LLUUID(STOCK_SEED_SKY_ID[slot]));
        names.push_back(STOCK_SEED_SKY_NAME[slot]);
    }

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; creating Atmo v3 environment with built-in defaults instead of the stock sky cycle" << LL_ENDL;
        writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
        return;
    }

    fetchSeedSkies(ids, names,
        [parent_id, on_created](const SeedSkyCollector& skies)
        {
            writeDefaultNotecard(buildSeededDefault(skies), parent_id, on_created);
        });
}

// The template seed: the template's world settings overwrite wholesale, and the track's sky
// reseeds as the stock four-sky day cycle with the template's atmosphere columns tinted over it -
// see the header note. Falls back to the plain constant template when the skies cannot be fetched.
void SSAtmoEnvManager::applyTemplateToTrack(SSAtmoEnvAsset& asset, S32 track_index, const std::string& key,
                                            std::function<void(bool success)> on_done)
{
    const SSAtmoEnvTemplate* tmpl = ssAtmoEnvFindTemplate(key);
    if (!tmpl || track_index < 0 || track_index >= static_cast<S32>(asset.mTracks.size()))
    {
        if (on_done) on_done(false);
        return;
    }

    SSAtmoEnvTrack& track = asset.mTracks[static_cast<size_t>(track_index)];

    if (!gAssetStorage)
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable; seeding the template with its constant sky instead of the stock day cycle" << LL_ENDL;
        ssAtmoEnvApplyTemplate(track, key);
        if (on_done) on_done(true);
        return;
    }

    std::vector<LLUUID> ids;
    std::vector<std::string> names;
    ids.reserve(STOCK_SEED_SKY_COUNT);
    names.reserve(STOCK_SEED_SKY_COUNT);
    for (S32 slot = 0; slot < STOCK_SEED_SKY_COUNT; ++slot)
    {
        ids.push_back(LLUUID(STOCK_SEED_SKY_ID[slot]));
        names.push_back(STOCK_SEED_SKY_NAME[slot]);
    }

    fetchSeedSkies(ids, names,
        [tmpl, &track, on_done, key](const SeedSkyCollector& skies)
        {
            ssAtmoEnvApplyTemplateWorld(track, *tmpl);

            std::vector<size_t> arrived;
            for (size_t slot = 0; slot < skies.mSkies.size(); ++slot)
            {
                if (skies.mSkies[slot]) arrived.push_back(slot);
            }

            if (arrived.empty())
            {
                // No seed sky arrived: the template's constant sky is all there is.
                ssAtmoEnvApplyTemplate(track, key);
                if (on_done) on_done(true);
                return;
            }

            std::vector<F64> phase;
            seedSkyPhases(track, skies, phase);

            for (size_t slot : arrived)
            {
                track.mAtmosphere.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
                track.mCloudDome.addKeyframesFromSky(*skies.mSkies[slot], phase[slot]);
            }

            track.mCloudDome.mNoiseTexture =
                SSAtmoEnvKeyframed<LLUUID>(LLUUID(SSAtmoEnvCloudDome::CLOUD_TEXTURE_LAYERED));

            // The tint reference: the Daylight slot when it arrived, the first arrival otherwise.
            const LLSettingsSky& reference =
                *(skies.mSkies[0] ? skies.mSkies[0] : skies.mSkies[arrived.front()]);

            tintSeededCycle(track, *tmpl, reference);

            track.mAtmosphere.collapseConstantKeyframes();
            track.mCloudDome.collapseConstantKeyframes();

            if (on_done) on_done(true);
        });
}

// Maps an EEP day cycle over: every sky keyframe on its ground-level track is stamped into the
// ground track at the day cycle's own keyframe time, so the authored timings carry across. The
// skies' own noise textures are kept (no layered-noise override) - this is an authored asset, not
// a seed.
void SSAtmoEnvManager::createFromDayCycle(const LLUUID& day_cycle_asset_id,
                                          const LLUUID& parent_id,
                                          std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created)
{
    if (!gAssetStorage || day_cycle_asset_id.isNull())
    {
        LL_WARNS("AtmoMagicEnv") << "Asset system unavailable or no day cycle given; creating Atmo v3 environment with built-in defaults" << LL_ENDL;
        createEmptyNotecard(parent_id, on_created);
        return;
    }

    LLSettingsVOBase::getSettingsAsset(day_cycle_asset_id,
        [parent_id, on_created](LLUUID asset_id, LLSettingsBase::ptr_t settings, S32 status, LLExtStat)
        {
            LLSettingsDay::ptr_t day;
            if (!status && settings)
            {
                day = std::dynamic_pointer_cast<LLSettingsDay>(settings);
            }
            if (!day)
            {
                LL_WARNS("AtmoMagicEnv") << "Could not fetch day cycle " << asset_id
                                         << " (status " << status
                                         << "); creating an empty Atmo v3 environment instead" << LL_ENDL;
                writeDefaultNotecard(SSAtmoEnvAsset::makeDefault(), parent_id, on_created);
                return;
            }

            SSAtmoEnvAsset def = SSAtmoEnvAsset::makeDefault();
            if (def.mTracks.empty())
            {
                writeDefaultNotecard(def, parent_id, on_created);
                return;
            }

            const LLSettingsDay::CycleTrack_t& frames =
                day->getCycleTrackConst(LLSettingsDay::TRACK_GROUND_LEVEL);
            if (frames.empty())
            {
                LL_WARNS("AtmoMagicEnv") << "Day cycle " << asset_id
                                         << " has no sky keyframes; creating an empty Atmo v3 environment instead" << LL_ENDL;
                writeDefaultNotecard(def, parent_id, on_created);
                return;
            }

            SSAtmoEnvTrack& ground = def.mTracks[0];
            if (frames.size() == 1)
            {
                LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(frames.begin()->second);
                if (sky)
                {
                    ground.mAtmosphere.fromSettingsSky(*sky);
                    ground.mCloudDome.fromSettingsSky(*sky);
                }
            }
            else
            {
                for (const LLSettingsDay::CycleTrack_t::value_type& frame : frames)
                {
                    LLSettingsSky::ptr_t sky = std::dynamic_pointer_cast<LLSettingsSky>(frame.second);
                    if (!sky) continue;

                    const F64 phase = ss_atmoenv_snap_phase((F64)frame.first);
                    ground.mAtmosphere.addKeyframesFromSky(*sky, phase);
                    ground.mCloudDome.addKeyframesFromSky(*sky, phase);
                }
            }

            ground.mAtmosphere.collapseConstantKeyframes();
            ground.mCloudDome.collapseConstantKeyframes();

            writeDefaultNotecard(def, parent_id, on_created);
        });
}

// Takes ownership of a just-created notecard as the live asset.
void SSAtmoEnvManager::adoptCreated(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)
{
    mItemID = item_id;
    mAssetID = asset_id;
    mBaseline = asset;

    mFromParcel = false;
    mWorking = asset;
    mHasAsset = true;
    mStatus = "Ready.";
}

// Writes the working asset as a NEW notecard item.
void SSAtmoEnvManager::saveNotecard(const std::string& name)
{
    if (!mHasAsset) return;

    std::string save_name = name;
    LLStringUtil::trim(save_name);
    if (save_name.empty()) save_name = "Atmo Environment";

    mWorking.mName = save_name;

    // Self-containment is a save-time property: whatever the keyframes name travels with the asset.
    ssAtmoEnvEmbedReferencedPrecipTypes(mWorking);

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
                if (item_id.notNull()) mItemID = item_id;
                if (asset_id.notNull()) mAssetID = asset_id;
            });
    });
}

// Overwrites the loaded notecard item's asset in place.
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
            SSAtmoEnvManager::getInstance()->mAssetID = new_asset_id;
        },
        nullptr);
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

// Loads a notecard item's asset, with an optional completion callback.
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

// Fires and clears the pending load callback.
void SSAtmoEnvManager::finishLoad(bool success)
{
    std::function<void(bool)> cb;
    cb.swap(mLoadCompleteCallback);
    if (cb) cb(success);
}

// Requests the notecard asset body from the asset system.
void SSAtmoEnvManager::loadFromAssetId(const LLUUID& asset_id)
{
    mPendingID = asset_id;
    mPendingItemID.setNull();
    mStatus = "loading environment...";

    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        return;
    }

    gAssetStorage->getAssetData(mPendingID, LLAssetType::AT_NOTECARD,
                                &SSAtmoEnvManager::onAssetLoaded, nullptr, true);
}

// Asset arrival: unwrap the notecard, parse, adopt; failures leave the current state untouched.
void SSAtmoEnvManager::onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status)
{
    SSAtmoEnvManager* self = SSAtmoEnvManager::getInstance();

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

// Parses notecard text and adopts it as the live asset.
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

// Adopts an already-parsed external document (Bridge fetch path).
bool SSAtmoEnvManager::applyExternalLLSD(const LLUUID& source_id, const LLSD& sd)
{
    mAssetID = source_id;
    mItemID.setNull();
    return adoptParsedAsset(sd);
}

// Adopts external notecard text (parcel discovery path).
bool SSAtmoEnvManager::applyExternalNotecardText(const LLUUID& source_id, const std::string& text)
{
    mAssetID = source_id;
    mItemID.setNull();
    return applyNotecardText(text, false);
}

// Drops the asset and restores stock EEP behaviour.
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
    SSPrecipPresetManager::instance().clearEnvironmentPresets();
    clearPreviewPhaseOverride();
}

// The one adoption point: validate, install, and set the modification baseline.
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
    // The environment's own precipitation types have to be live before anything resolves one.
    ssAtmoEnvStagePrecipTypes(mWorking);
    return true;
}
