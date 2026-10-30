#include "ui/ExportSettings.h"

#include "core/Described.h"
#include "export/MixExport.h"
#include "song/LoopEdits.h"

#include <algorithm>
#include <utility>

namespace overdub::ui
{
core::Expected<song::Song> withExportSetting (song::Song song, const ExportSetting& setting)
{
    using namespace intent;
    return core::visitExhaustively (
        setting,
        core::Overloaded {
            [&] (const SetExportFormat& s) -> core::Expected<song::Song> {
        song.exportFormat = s.format;
        return std::move (song);
    },
            [&] (const SetExportLength& s) -> core::Expected<song::Song> {
        auto changed = song::setExportLoop (std::move (song), s.loopId);
        if (changed.has_value())
            changed->exportLength = s.length;
        return changed;
    },
            [&] (const SetExportDestination& s) -> core::Expected<song::Song> {
        const juce::File destination (s.path.value);
        if (auto folder = exporting::checkExportFolder (destination); ! folder.has_value())
            return std::unexpected (std::move (folder.error()));
        song.exportDestination = destination.getFullPathName().toStdString();
        return std::move (song);
    },
            [&] (const SetExportVideoTake& s) -> core::Expected<song::Song> {
        if (std::ranges::contains (song.videoTakes, s.videoTakeId, &song::VideoTake::id))
            song.exportVideoTakeId = s.videoTakeId;
        return std::move (song);
    },
        });
}
} // namespace overdub::ui
