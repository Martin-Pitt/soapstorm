/**
 * @file ssassetlist.cpp
 * @brief Atmo Magic ordered asset lists. See the header.
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

#include "ssassetlist.h"

#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llstring.h"

#include <algorithm>
#include <map>

// <SS:Nexii> Atmo Magic ordered asset lists

bool ss_natural_less(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;

    while (i < a.size() && j < b.size())
    {
        const bool a_digit = isdigit((unsigned char)a[i]) != 0;
        const bool b_digit = isdigit((unsigned char)b[j]) != 0;

        if (a_digit && b_digit)
        {
            // Take the whole run and compare the values, so the count of digits stops mattering and the number starts.
            size_t ia = i, jb = j;
            while (ia < a.size() && isdigit((unsigned char)a[ia])) ++ia;
            while (jb < b.size() && isdigit((unsigned char)b[jb])) ++jb;

            const std::string na = a.substr(i, ia - i);
            const std::string nb = b.substr(j, jb - j);

            const unsigned long va = strtoul(na.c_str(), NULL, 10);
            const unsigned long vb = strtoul(nb.c_str(), NULL, 10);

            if (va != vb) return va < vb;

            // Equal values, different padding: the shorter form first, so an order exists at all rather than the two comparing as equal and sorting arbitrarily.
            if (na.size() != nb.size()) return na.size() < nb.size();

            i = ia;
            j = jb;
            continue;
        }

        const char ca = (char)tolower((unsigned char)a[i]);
        const char cb = (char)tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb;

        ++i;
        ++j;
    }

    return (a.size() - i) < (b.size() - j);
}

const char* ss_asset_mode_key(SSAssetListMode mode)
{
    return (mode == SS_ASSET_SEQUENCE) ? "sequence" : "random";
}

SSAssetListMode ss_asset_mode_from_key(const std::string& key)
{
    return (key == "sequence") ? SS_ASSET_SEQUENCE : SS_ASSET_RANDOM;
}

SSAssetList ss_asset_list_parse(const std::string& csv)
{
    SSAssetList out;

    size_t start = 0;
    while (start <= csv.size())
    {
        const size_t comma = csv.find(',', start);
        const size_t end = (comma == std::string::npos) ? csv.size() : comma;

        std::string token = csv.substr(start, end - start);
        LLStringUtil::trim(token);

        if (!token.empty() && LLUUID::validate(token))
        {
            out.push_back(LLUUID(token));
        }

        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    return out;
}

std::string ss_asset_list_str(const SSAssetList& seq)
{
    std::string out;
    for (size_t i = 0; i < seq.size(); ++i)
    {
        if (i) out += ",";
        out += seq[i].asString();
    }
    return out;
}

std::string ss_asset_name(const LLUUID& id)
{
    if (id.isNull()) return std::string();

    // Cached, because this is asked once per row per frame and the walk is over the whole inventory. The answer only changes when something is added or removed, which is rare next to how often it is
    // read - and a miss is re-asked rather than remembered, so a name that arrives once the inventory finishes fetching is picked up.
    static std::map<LLUUID, std::string> s_names;

    auto found = s_names.find(id);
    if (found != s_names.end()) return found->second;

    LLViewerInventoryCategory::cat_array_t cats;
    LLViewerInventoryItem::item_array_t items;
    LLAssetIDMatches matches(id);
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items,
                                    LLInventoryModel::EXCLUDE_TRASH, matches);

    if (items.empty()) return std::string();

    const std::string name = items.front()->getName();
    s_names[id] = name;
    return name;
}


// </SS:Nexii>
