/**
 * @file ssfasttimerexport.cpp
 * @brief See ssfasttimerexport.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssfasttimerexport.h"

// <SS:Nexii> Fast timer offline capture. Main thread only: recordFrame() runs in
// the mainloop right after BlockTimer::logStats(), menu callbacks are main thread.
// Streaming writes are buffered in memory and appended in chunks so the file I/O
// cost does not land on every frame; marks bypass the buffer (and flush) so they
// survive a crash. Unlike the stock LogPerformance .slp path this needs no restart:
// the stream file opens when the setting is first seen on, closes (and flushes the
// buffer) when it goes off.

#include "llappviewer.h"    // FTM_FRAME, gFrameCount
#include "lldate.h"
#include "lldir.h"
#include "llfasttimer.h"
#include "llfile.h"
#include "llformat.h"
#include "lltimer.h"
#include "lltracerecording.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"

#include <ctime>
#include <deque>

namespace
{
    std::deque<std::string> sRing;
    llofstream              sStream;
    std::string             sStreamPath;
    std::string             sStreamBuffer;
    bool                    sStreamOpen = false;
    S64                     sTreeCount = -1;   // timer handle count at last meta emit
    LLTimer                 sClock;
    constexpr size_t        STREAM_FLUSH_BYTES = 512 * 1024;

    std::string fileTimestamp()
    {
        time_t now = time(nullptr);
        struct tm* ltm = localtime(&now);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", ltm);
        return std::string(stamp);
    }

    std::string jsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 4);
        for (char c : in)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20)
                    {
                        out += llformat("\\u%04x", c);
                    }
                    else
                    {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    }

    // Timer names can be duplicated across TUs (e.g. "Append Chat Message"),
    // so frame data is an array of [name, total, self, calls] rather than a map.
    void appendFrameLine(std::string& line, const std::string& mark_label = std::string())
    {
        LLTrace::Recording& last = LLTrace::get_frame_recording().getLastRecording();

        std::string entries;
        F64 frame_ms = 0.0;

        for (auto& base : LLTrace::BlockTimerStatHandle::instance_snapshot())
        {
            // because of indirect derivation from LLInstanceTracker, have to downcast
            LLTrace::BlockTimerStatHandle& timer = static_cast<LLTrace::BlockTimerStatHandle&>(base);
            S32 calls = (S32)last.getSum(timer.callCount());
            if (!calls) continue;

            F64Seconds total = last.getSum(timer);
            F64Seconds self = last.getSum(timer.selfTime());
            if (timer.getName() == "Frame")
            {
                frame_ms = F64Milliseconds(total).value();
            }
            entries += llformat(",[\"%s\",%.6f,%.6f,%d]",
                                jsonEscape(timer.getName()).c_str(),
                                total.value(), self.value(), calls);
        }

        line = llformat("{\"f\":%u,\"t\":%.3f,\"ms\":%.2f,\"timers\":[",
                        gFrameCount, sClock.getElapsedTimeF32(), frame_ms);
        line += entries;
        if (mark_label.empty())
        {
            line += "]}";
        }
        else
        {
            line += llformat("],\"mark\":\"%s\"}", jsonEscape(mark_label).c_str());
        }
        line += "\n";
    }

    void writeMeta(std::ostream& os)
    {
        // processTimes() bootstraps/refreshes the parent links; cheap and safe
        // to call from the mainloop thread at any point.
        LLTrace::BlockTimer::processTimes();

        std::string tree;
        bool first = true;
        for (LLTrace::block_timer_tree_df_iterator_t it = LLTrace::begin_block_timer_tree_df(FTM_FRAME);
             it != LLTrace::end_block_timer_tree_df();
             ++it)
        {
            LLTrace::BlockTimerStatHandle* timerp = (*it);
            LLTrace::BlockTimerStatHandle* parentp = timerp->getParent();
            // the root timer's parent points to itself; treat that as top of tree
            std::string parent_name = (parentp && parentp != parentp->getParent())
                                          ? parentp->getName() : std::string("");
            if (!first) tree += ",";
            first = false;
            tree += llformat("\"%s\":\"%s\"", jsonEscape(timerp->getName()).c_str(), jsonEscape(parent_name).c_str());
        }

        os << llformat("{\"meta\":{\"date\":\"%s\",\"viewer\":\"%s\",\"tree\":{%s}}}\n",
                       LLDate::now().asString().c_str(),
                       jsonEscape(LLVersionInfo::instance().getChannelAndVersion()).c_str(),
                       tree.c_str());
    }

    void openStream()
    {
        sStreamPath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS,
            llformat("ssfasttimers_%s.jsonl", fileTimestamp().c_str()));
        sStream.open(sStreamPath, std::ios::out | std::ios::trunc);
        sStreamOpen = sStream.is_open();
        if (sStreamOpen)
        {
            writeMeta(sStream);
            sTreeCount = (S64)LLTrace::BlockTimerStatHandle::instance_tracker_t::instanceCount();
            LL_INFOS("FastTimers") << "SSFastTimerExport streaming to " << sStreamPath << LL_ENDL;
        }
        else
        {
            LL_WARNS("FastTimers") << "SSFastTimerExport could not open " << sStreamPath << LL_ENDL;
        }
    }

    void flushStream()
    {
        if (sStreamOpen && !sStreamBuffer.empty())
        {
            sStream << sStreamBuffer;
            sStreamBuffer.clear();
            sStream.flush();
        }
    }

    void closeStream()
    {
        if (!sStreamOpen) return;
        flushStream();
        sStream.close();
        sStreamOpen = false;
        LL_INFOS("FastTimers") << "SSFastTimerExport stream closed: " << sStreamPath << LL_ENDL;
    }
}

void SSFastTimerExport::recordFrame()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSFastTimerExport", false);
    static bool was_enabled = false;

    if (!enabled())
    {
        if (was_enabled)
        {
            closeStream();
            sRing.clear();
            was_enabled = false;
        }
        return;
    }
    if (!was_enabled)
    {
        openStream();
        was_enabled = true;
    }

    // re-emit the tree when the timer handle set grows (lazy statics can appear mid-session)
    S64 count = (S64)LLTrace::BlockTimerStatHandle::instance_tracker_t::instanceCount();
    if (count != sTreeCount)
    {
        writeMeta(sStream);
        sTreeCount = count;
    }

    std::string line;
    appendFrameLine(line);

    static LLCachedControl<U32> ring_frames(gSavedSettings, "SSFastTimerExportBufferFrames", 600);
    sRing.push_back(line);
    while (sRing.size() > ring_frames())
    {
        sRing.pop_front();
    }

    sStreamBuffer += line;
    if (sStreamBuffer.size() >= STREAM_FLUSH_BYTES)
    {
        flushStream();
    }
}

void SSFastTimerExport::mark(const std::string& label)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSFastTimerExport", false);
    if (!enabled())
    {
        LL_INFOS("FastTimers") << "SSFastTimerExport mark ignored: enable SSFastTimerExport first" << LL_ENDL;
        return;
    }

    std::string line;
    appendFrameLine(line, label.empty() ? "mark" : label);
    sRing.push_back(line);
    if (sStreamOpen)
    {
        // marks flush immediately so a crash cannot lose the correlation point
        sStream << sStreamBuffer << line;
        sStreamBuffer.clear();
        sStream.flush();
    }
}

void SSFastTimerExport::exportRecent()
{
    if (sRing.empty())
    {
        LL_INFOS("FastTimers") << "SSFastTimerExport: no frames captured; enable SSFastTimerExport" << LL_ENDL;
        return;
    }

    std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS,
        llformat("ssfasttimers_recent_%s.jsonl", fileTimestamp().c_str()));
    llofstream os(path, std::ios::out | std::ios::trunc);
    if (!os.is_open())
    {
        LL_WARNS("FastTimers") << "SSFastTimerExport could not open " << path << LL_ENDL;
        return;
    }

    writeMeta(os);
    for (const std::string& line : sRing)
    {
        os << line;
    }
    os.flush();
    os.close();

    LL_INFOS("FastTimers") << "SSFastTimerExport dumped " << sRing.size() << " recent frames to " << path << LL_ENDL;
}

void SSFastTimerExport::flush()
{
    closeStream();
}
