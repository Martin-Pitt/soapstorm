/**
 * @file ssassetlist.h
 * @brief Atmo Magic: the parts of an ordered asset list that do not care what
 *        kind of asset it holds.
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

#ifndef SS_ASSETLIST_H
#define SS_ASSETLIST_H

// <SS:Nexii> Atmo Magic ordered asset lists
//
// Sounds and textures are stored the same way and edited the same way: a
// comma separated run of asset ids, reordered by dragging, added to from
// inventory, named from whatever inventory item happens to point at them.
// None of that is about what the asset IS - so it lives here once rather
// than twice, and the two editors are left holding only the parts that
// genuinely differ, which is how a row draws itself and what previewing one
// means.

#include "lluuid.h"

#include <string>
#include <vector>

typedef std::vector<LLUUID> SSAssetList;

// What a list means when something asks it for an entry.
//
// A property of the SLOT rather than of its contents, and set where the slot
// is declared: a footstep grid is random because a step draws one sound, an
// ambient bed is sequenced because a bed plays through. Neither is a
// preference.
enum SSAssetListMode
{
    SS_ASSET_SEQUENCE = 0,  // take the list in order
    SS_ASSET_RANDOM         // take one from it per use
};

const char* ss_asset_mode_key(SSAssetListMode mode);
SSAssetListMode ss_asset_mode_from_key(const std::string& key);

// The comma separated form the presets serialise. Order survives both, in
// either mode - it is what a sequence plays in, and for a random slot it is
// still the arrangement the author chose.
SSAssetList ss_asset_list_parse(const std::string& csv);
std::string ss_asset_list_str(const SSAssetList& list);

// The name of an inventory item pointing at this asset, or empty if the agent
// holds none.
//
// Empty is not an error. An asset is usable from its id whether or not
// anybody owns an item for it, so an id typed in by hand, or arriving in a
// preset from someone else, is perfectly good and simply has no name here.
// This is for READING a list, never for deciding what may go in one.
std::string ss_asset_name(const LLUUID& id);

// Compares two names the way a person reading a numbered set would.
//
// Digit runs compare as NUMBERS, so "Step 2" comes before "Step 10" and both
// before "Step 11" - which plain string order gets wrong, comparing "1"
// against "2" one character at a time and putting 10 in the middle. Zero
// padding falls out of the same rule rather than needing its own: "01" and
// "1" are the same number.
bool ss_natural_less(const std::string& a, const std::string& b);

// </SS:Nexii>

#endif
