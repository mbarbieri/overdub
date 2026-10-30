#include "ui/ListeningSettings.h"

#include "core/Described.h"
#include "core/Log.h"
#include "song/TranscribeMix.h"

namespace overdub::ui
{
core::Expected<song::Song> withListeningSetting (const song::Song& song, const intent::ListeningSetting& setting)
{
    using namespace intent;
    return core::visitExhaustively (
        setting,
        core::Overloaded {
            [&] (const SetFocusTrack& s) { return song::setFocusTrack (song, s.trackId); },
            [&] (const SetTranscribeMute& s) { return song::setTranscribeMute (song, s.trackId, s.mute); },
            [&] (const SetTranscribeSolo& s) { return song::setTranscribeSolo (song, s.trackId, s.solo); },
            [&] (const SetOctaveShift& s) { return song::setOctaveShift (song, s.trackId, s.octaves); },
            [&] (const SetBandFilter& s) { return song::setBandFilter (song, s.trackId, { .on = s.on, .lowHz = s.lowHz, .highHz = s.highHz }); },
        });
}

void logListeningSet (const intent::ListeningSetting& setting)
{
    using namespace intent;
    core::visitExhaustively (
        setting,
        core::Overloaded {
            [] (const SetFocusTrack& s) { core::logEvent (core::Level::info, "ui.focus_track_set", { { "track_id", s.trackId.value_or ("") } }); },
            [] (const SetTranscribeMute& s) { core::logEvent (core::Level::debug, "ui.transcribe_mute_set", { { "track_id", s.trackId }, { "mute", s.mute } }); },
            [] (const SetTranscribeSolo& s) { core::logEvent (core::Level::debug, "ui.transcribe_solo_set", { { "track_id", s.trackId }, { "solo", s.solo } }); },
            [] (const SetOctaveShift& s) { core::logEvent (core::Level::info, "ui.octave_shift_set", { { "track_id", s.trackId }, { "octaves", s.octaves } }); },
            [] (const SetBandFilter& s) {
        core::logEvent (core::Level::info, "ui.band_filter_set", { { "track_id", s.trackId }, { "on", s.on }, { "low_hz", s.lowHz }, { "high_hz", s.highHz } });
    },
        });
}
} // namespace overdub::ui
