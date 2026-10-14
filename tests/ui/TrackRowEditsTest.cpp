#include "ui/TrackRowEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using overdub::song::Song;
using overdub::song::Track;
using namespace overdub::ui;

namespace
{
Track track (const std::string& id, bool armed, std::vector<int> inputs)
{
    Track t;
    t.id = id;
    t.name = id;
    t.armed = armed;
    t.inputChannels = std::move (inputs);
    return t;
}

Song twoTrackSong()
{
    auto song = overdub::song::makeEmptySong (48000);
    song.tracks = { track ("guitar", false, { 0 }), track ("vocals", true, { 1 }) };
    return song;
}
} // namespace

TEST_CASE ("input choices list every mono input, then every stereo pair, after No input", "[ui][inputs]")
{
    const auto choices = inputChoices ({ "In 1", "In 2", "In 3", "In 4", "In 5" });
    REQUIRE (choices.size() == 8);
    CHECK (choices[0].channels.empty());
    CHECK (choices[0].label == "No input");
    CHECK (choices[1].channels == std::vector<int> { 0 });
    CHECK (choices[1].label == "In 1");
    CHECK (choices[5].channels == std::vector<int> { 4 });
    CHECK (choices[6].channels == std::vector<int> { 0, 1 });
    CHECK (choices[6].label == "In 1 + In 2");
    CHECK (choices[7].channels == std::vector<int> { 2, 3 });

    CHECK (choiceIndexFor (choices, { 2, 3 }) == 7);
    CHECK (choiceIndexFor (choices, {}) == 0);
    CHECK (choiceIndexFor (choices, { 1, 2 }) == std::nullopt);
    CHECK (choiceIndexFor (choices, { 9 }) == std::nullopt);
}

TEST_CASE ("arming and picking an input change only that Track", "[ui][inputs]")
{
    const auto song = twoTrackSong();

    const auto armed = withArmed (song, "guitar", true);
    CHECK (armed.tracks[0].armed);
    CHECK (armed.tracks[1] == song.tracks[1]);

    const auto stereo = withInputChannels (song, "vocals", { 2, 3 });
    CHECK (stereo.tracks[1].inputChannels == std::vector<int> { 2, 3 });
    CHECK (stereo.tracks[0] == song.tracks[0]);

    CHECK (withArmed (song, "missing", true) == song);
}

TEST_CASE ("the engine hears live input only on Armed Tracks with Monitoring on", "[ui][inputs]")
{
    auto song = twoTrackSong();
    song.tracks[0].armed = true;

    const auto heard = songForEngine (song, { "vocals" });
    CHECK (! heard.tracks[0].armed);
    CHECK (heard.tracks[1].armed);
    CHECK (heard.tracks[1].inputChannels == std::vector<int> { 1 });

    const auto none = songForEngine (song, {});
    CHECK (! none.tracks[0].armed);
    CHECK (! none.tracks[1].armed);
}

TEST_CASE ("the Pass being recorded is numbered after the Song's Passes", "[ui][inputs]")
{
    auto song = twoTrackSong();
    CHECK (nextPassNumber (song) == 1);
    song.passes.push_back ({ "pass-a", 0, "2026-10-08T10:00:00Z" });
    CHECK (nextPassNumber (song) == 2);
}

TEST_CASE ("a Track's menu offers Separate... only on a Track whose Clips all play one Backing Track, and its tooltip says why not", "[ui][separation]")
{
    using namespace overdub::song;
    Song song = makeEmptySong (48000);
    song.backingTracks = {
        BackingTrack { .id = "backing-1", .originalFilename = "Band Mix.wav" },
        BackingTrack { .id = "backing-2", .originalFilename = "Click.wav" },
    };
    song.takes = { Take { .id = "take-1", .trackId = "track-vocals", .passId = "pass-1", .lengthSamples = 100 } };
    song.tracks = {
        Track { .id = "track-mix", .name = "Band Mix", .clips = { Clip { .id = "clip-1", .source = { ClipSourceKind::backing, "backing-1" }, .length = 100 } } },
        Track { .id = "track-guitar", .name = "Guitar" },
        Track { .id = "track-vocals",
                .name = "Vocals",
                .clips = { Clip { .id = "clip-2", .source = { ClipSourceKind::backing, "backing-1" }, .length = 100 }, Clip { .id = "clip-3", .source = { ClipSourceKind::take, "take-1" }, .length = 100 } } },
        Track { .id = "track-mixed",
                .name = "Mixed",
                .clips = { Clip { .id = "clip-4", .source = { ClipSourceKind::backing, "backing-1" }, .length = 100 }, Clip { .id = "clip-5", .source = { ClipSourceKind::backing, "backing-2" }, .length = 100 } } },
    };
    const auto disabledBecause = [] (std::string tooltip) {
        return std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", false, std::move (tooltip) } };
    };

    CHECK (trackMenuFor (song, "track-mix", {}) == std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", true, "" } });
    CHECK (trackMenuFor (song, "track-mix", { .passRunning = true }) == disabledBecause ("Stop recording first"));
    CHECK (trackMenuFor (song, "track-mix", { .exportRunning = true }) == disabledBecause ("Wait for the export to finish"));
    CHECK (trackMenuFor (song, "track-guitar", {}) == disabledBecause ("This Track has no Clips"));
    CHECK (trackMenuFor (song, "track-vocals", {}) == disabledBecause ("This Track has recorded Takes"));
    CHECK (trackMenuFor (song, "track-mixed", {}) == disabledBecause ("This Track has more than one Backing Track"));
}
