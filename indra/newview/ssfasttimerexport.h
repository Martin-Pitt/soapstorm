/**
 * @file ssfasttimerexport.h
 * @brief Offline export of Fast Timer (BlockTimer) frame stats for lag spike analysis.
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
 *
 * The Firestorm Team and the Soapstorm Project
 * $/LicenseInfo$
 */

#ifndef SS_FASTTIMEREXPORT_H
#define SS_FASTTIMEREXPORT_H

#include <string>

// Captures the completed frame's fast timer sums once per frame, driven by
// SSFastTimerExport. Output is JSONL (one JSON object per line) so offline
// tooling can process it without an LLSD parser:
//   {"meta":{"date":...,"viewer":...,"tree":{"Timer":"Parent",...}}}   header, repeats when the timer set grows (last one wins)
//   {"f":<frame>,"t":<secs>,"ms":<frame_ms>,"timers":[["Name",total,self,calls],...]}
//   {"f":<frame>,"t":<secs>,"mark":"<label>"}                          user mark, flushed immediately
class SSFastTimerExport
{
public:
    static void recordFrame();
    static void mark(const std::string& label);
    static void exportRecent();
    static void flush();
};

#endif // SS_FASTTIMEREXPORT_H
