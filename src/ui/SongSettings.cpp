#include "ui/SongSettings.h"

#include "core/Described.h"
#include "core/Log.h"
#include "export/MixExport.h"
#include "song/LoopEdits.h"
#include "song/TranscribeMix.h"

#include <algorithm>
#include <utility>

namespace overdub::ui
{
core::Expected<song::Song> withSongSetting (song::Song song, const intent::SongSetting& setting)
{
    using namespace intent;
    return core::visitExhaustively (
        setting,
        core::Overloaded {
            [&] (const SetArmed& s) -> core::Expected<song::Song> {
        if (auto* track = song::findTrack (song, s.trackId))
            track->settings.armed = s.armed;
        return std::move (song);
    },
            [&] (const SetInputChannels& s) -> core::Expected<song::Song> {
        if (auto* track = song::findTrack (song, s.trackId))
            track->settings.inputChannels = s.channels;
        return std::move (song);
    },
            [&] (const SetPreRoll& s) -> core::Expected<song::Song> {
        song.settings.preRoll = s.preRoll;
        return std::move (song);
    },
            [&] (const SetLooping& s) { return song::setLooping (std::move (song), s.looping); },
            [&] (const SetFocusTrack& s) { return song::setFocusTrack (std::move (song), s.trackId); },
            [&] (const SetTranscribeMute& s) { return song::setTranscribeMute (std::move (song), s.trackId, s.mute); },
            [&] (const SetTranscribeSolo& s) { return song::setTranscribeSolo (std::move (song), s.trackId, s.solo); },
            [&] (const SetOctaveShift& s) { return song::setOctaveShift (std::move (song), s.trackId, s.octaves); },
            [&] (const SetBandFilter& s) { return song::setBandFilter (std::move (song), s.trackId, { .on = s.on, .lowHz = s.lowHz, .highHz = s.highHz }); },
            [&] (const SetExportFormat& s) -> core::Expected<song::Song> {
        song.settings.exportFormat = s.format;
        return std::move (song);
    },
            [&] (const SetExportLength& s) -> core::Expected<song::Song> {
        auto changed = song::setExportLoop (std::move (song), s.loopId);
        if (changed.has_value())
            changed->settings.exportLength = s.length;
        return changed;
    },
            [&] (const SetExportDestination& s) -> core::Expected<song::Song> {
        const juce::File destination (s.path.value);
        if (auto folder = exporting::checkExportFolder (destination); ! folder.has_value())
            return std::unexpected (std::move (folder.error()));
        song.settings.exportDestination = destination.getFullPathName().toStdString();
        return std::move (song);
    },
            [&] (const SetExportVideoTake& s) -> core::Expected<song::Song> {
        if (std::ranges::contains (song.videoTakes, s.videoTakeId, &song::VideoTake::id))
            song.settings.exportVideoTakeId = s.videoTakeId;
        return std::move (song);
    },
        });
}

void logSongSettingSet (const intent::SongSetting& setting)
{
    using namespace intent;
    core::visitExhaustively (
        setting,
        core::Overloaded {
            [] (const SetArmed&) {},
            [] (const SetInputChannels&) {},
            [] (const SetPreRoll&) {},
            [] (const SetLooping& s) { core::logEvent (core::Level::info, "ui.looping_set", { { "looping", s.looping } }); },
            [] (const SetFocusTrack& s) { core::logEvent (core::Level::info, "ui.focus_track_set", { { "track_id", s.trackId.value_or ("") } }); },
            [] (const SetTranscribeMute& s) { core::logEvent (core::Level::debug, "ui.transcribe_mute_set", { { "track_id", s.trackId }, { "mute", s.mute } }); },
            [] (const SetTranscribeSolo& s) { core::logEvent (core::Level::debug, "ui.transcribe_solo_set", { { "track_id", s.trackId }, { "solo", s.solo } }); },
            [] (const SetOctaveShift& s) { core::logEvent (core::Level::info, "ui.octave_shift_set", { { "track_id", s.trackId }, { "octaves", s.octaves } }); },
            [] (const SetBandFilter& s) {
        core::logEvent (core::Level::info, "ui.band_filter_set", { { "track_id", s.trackId }, { "on", s.on }, { "low_hz", s.lowHz }, { "high_hz", s.highHz } });
    },
            [] (const SetExportFormat&) {},
            [] (const SetExportLength&) {},
            [] (const SetExportDestination&) {},
            [] (const SetExportVideoTake&) {},
        });
}
} // namespace overdub::ui
