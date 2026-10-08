#include "ui/RecordView.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <string>

using overdub::song::ClipSourceKind;
using overdub::song::Peaks;
using overdub::song::SampleCount;
using overdub::song::Song;
using namespace overdub::ui;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int rate = 48000;

SampleCount seconds (double s)
{
    return (SampleCount) std::llround (s * rate);
}

// Generated peaks: a mono source of `length` samples whose every 256-sample block reaches `level`.
Peaks constantPeaks (SampleCount length, float level)
{
    Peaks peaks;
    peaks.lengthSamples = length;
    peaks.channels.assign (1, std::vector<overdub::song::PeakRange> ((std::size_t) ((length + 255) / 256), { -level, level }));
    return peaks;
}

overdub::song::Clip clipOf (const std::string& id, ClipSourceKind kind, const std::string& sourceId, SampleCount start, SampleCount length)
{
    overdub::song::Clip clip;
    clip.id = id;
    clip.source = { kind, sourceId };
    clip.timelineStart = start;
    clip.length = length;
    return clip;
}

overdub::song::Track trackOf (const std::string& id, std::vector<overdub::song::Clip> clips)
{
    overdub::song::Track track;
    track.id = id;
    track.name = id;
    track.clips = std::move (clips);
    return track;
}

// Peaks by media id, as OpenSong::peaks hands them out.
struct PeaksTable
{
    std::map<std::string, Peaks> byId;
    Peaks none;

    PeaksLookup lookup() const
    {
        return [this] (const overdub::song::ClipSource& source) -> const Peaks& {
            const auto found = byId.find (source.id);
            return found != byId.end() ? found->second : none;
        };
    }
};

// Music 0-4 s, silence 4-8 s, music 8-12 s: two Backing Track Clips at 0.5 on one Track.
Song songWithABreak (PeaksTable& peaks)
{
    auto song = overdub::song::makeEmptySong (rate);
    peaks.byId["backing-a"] = constantPeaks (seconds (4), 0.5f);
    peaks.byId["backing-b"] = constantPeaks (seconds (4), 0.5f);
    song.tracks.push_back (trackOf ("band", { clipOf ("a", ClipSourceKind::backing, "backing-a", 0, seconds (4)), clipOf ("b", ClipSourceKind::backing, "backing-b", seconds (8), seconds (4)) }));
    return song;
}
} // namespace

TEST_CASE ("the look-ahead window spans 30 s with the playhead at 20%", "[ui][record_view]")
{
    const auto window = lookAheadWindow (seconds (10), rate);
    CHECK (window.start == seconds (4));
    CHECK (window.length == seconds (30));
    CHECK_THAT (lookAheadX (window, seconds (10), 1000), WithinAbs (200.0, 1e-9));
    CHECK_THAT (lookAheadX (window, seconds (4), 1000), WithinAbs (0.0, 1e-9));
    CHECK_THAT (lookAheadX (window, seconds (34), 1000), WithinAbs (1000.0, 1e-9));

    // At the Song start the window reaches 6 s before it; the playhead stays at 20%.
    const auto atStart = lookAheadWindow (0, rate);
    CHECK (atStart.start == -seconds (6));
    CHECK_THAT (lookAheadX (atStart, 0, 500), WithinAbs (100.0, 1e-9));
}

TEST_CASE ("the combined envelope follows the audible Backing Track Clips' peaks", "[ui][record_view]")
{
    PeaksTable peaks;
    auto song = songWithABreak (peaks);

    SECTION ("each Clip's peaks where it plays, nothing between or after")
    {
        const auto envelope = backingEnvelope (song, peaks.lookup());
        CHECK (envelope.levels.size() == (std::size_t) (seconds (12) / 256));
        CHECK (envelopeLevelAt (envelope, seconds (1)) == 0.5f);
        CHECK (envelopeLevelAt (envelope, seconds (5)) == 0.0f);
        CHECK (envelopeLevelAt (envelope, seconds (9)) == 0.5f);
        CHECK (envelopeLevelAt (envelope, seconds (13)) == 0.0f);
        CHECK (envelopeLevelAt (envelope, -1) == 0.0f);
    }
    SECTION ("Clip gain and Track volume scale it; a fade-in starts it at 0")
    {
        song.tracks[0].clips[0].gainDb = -6.0;
        song.tracks[0].volumeDb = -6.0;
        song.tracks[0].clips[1].fadeInLength = seconds (2);
        const auto envelope = backingEnvelope (song, peaks.lookup());
        const auto sixDb = std::pow (10.0, -6.0 / 20.0);
        CHECK_THAT (envelopeLevelAt (envelope, seconds (1)), WithinAbs (0.5 * sixDb * sixDb, 1e-6));
        CHECK (envelopeLevelAt (envelope, seconds (8)) == 0.0f);
        CHECK_THAT (envelopeLevelAt (envelope, seconds (9.6)), WithinAbs (0.5 * 0.8 * sixDb, 1e-6)); // 1.6 s into the 2 s fade
    }
    SECTION ("a muted Track, or one silenced by another's solo, is not heard")
    {
        song.tracks[0].mute = true;
        CHECK (backingEnvelope (song, peaks.lookup()).levels == std::vector<float> ((std::size_t) (seconds (12) / 256), 0.0f));
        song.tracks[0].mute = false;
        song.tracks.push_back (trackOf ("guitar", {}));
        song.tracks[1].solo = true;
        CHECK (backingEnvelope (song, peaks.lookup()).levels == std::vector<float> ((std::size_t) (seconds (12) / 256), 0.0f));
    }
    SECTION ("a Take Clip on top hides the Backing Track under it, and Take Clips never count")
    {
        peaks.byId["take"] = constantPeaks (seconds (2), 0.9f);
        song.tracks[0].clips.push_back (clipOf ("t", ClipSourceKind::take, "take", seconds (1), seconds (2)));
        song.tracks.push_back (trackOf ("guitar", { clipOf ("t2", ClipSourceKind::take, "take", seconds (5), seconds (2)) }));
        const auto envelope = backingEnvelope (song, peaks.lookup());
        CHECK (envelopeLevelAt (envelope, seconds (0.5)) == 0.5f);
        CHECK (envelopeLevelAt (envelope, seconds (2)) == 0.0f);
        CHECK (envelopeLevelAt (envelope, seconds (3.5)) == 0.5f);
        CHECK (envelopeLevelAt (envelope, seconds (6)) == 0.0f);
    }
    SECTION ("Tracks combine by their loudest")
    {
        peaks.byId["strings"] = constantPeaks (seconds (6), 0.25f);
        song.tracks.push_back (trackOf ("strings", { clipOf ("s", ClipSourceKind::backing, "strings", seconds (2), seconds (6)) }));
        const auto envelope = backingEnvelope (song, peaks.lookup());
        CHECK (envelopeLevelAt (envelope, seconds (3)) == 0.5f);
        CHECK (envelopeLevelAt (envelope, seconds (5)) == 0.25f);
    }
    SECTION ("a Backing Track without peaks is silent")
    {
        peaks.byId.clear();
        CHECK (envelopeLevelAt (backingEnvelope (song, peaks.lookup()), seconds (1)) == 0.0f);
    }
}

TEST_CASE ("the countdown says when the Backing Tracks fall silent or come back", "[ui][record_view]")
{
    PeaksTable peaks;
    const auto envelope = backingEnvelope (songWithABreak (peaks), peaks.lookup());

    const auto fromStart = musicCountdown (envelope, 0, rate);
    CHECK (fromStart.soundingNow);
    CHECK (fromStart.changeIn == seconds (4));
    CHECK (musicCountdownText (fromStart, rate) == "Silence in 4.0 s");

    const auto inTheBreak = musicCountdown (envelope, seconds (4.9), rate);
    CHECK_FALSE (inTheBreak.soundingNow);
    CHECK (inTheBreak.changeIn == seconds (3.1));
    CHECK (musicCountdownText (inTheBreak, rate) == "Music back in 3.1 s");

    // Rounded down to a tenth.
    CHECK (musicCountdownText (musicCountdown (envelope, seconds (9) - 1, rate), rate) == "Silence in 3.0 s");
    CHECK (musicCountdownText (musicCountdown (envelope, seconds (9) + 1, rate), rate) == "Silence in 2.9 s");

    // Past the end: silence for good.
    CHECK (musicCountdownText (musicCountdown (envelope, seconds (20), rate), rate) == "Silence");
}

TEST_CASE ("the countdown only looks as far as the window: 24 s ahead", "[ui][record_view]")
{
    PeaksTable peaks;
    auto song = overdub::song::makeEmptySong (rate);
    peaks.byId["long"] = constantPeaks (seconds (60), 0.5f);
    song.tracks.push_back (trackOf ("band", { clipOf ("a", ClipSourceKind::backing, "long", seconds (30), seconds (30)) }));
    const auto envelope = backingEnvelope (song, peaks.lookup());

    CHECK (musicCountdownText (musicCountdown (envelope, seconds (5), rate), rate) == "Silence");
    CHECK (musicCountdownText (musicCountdown (envelope, seconds (6), rate), rate) == "Music back in 24.0 s");
    CHECK (musicCountdownText (musicCountdown (envelope, seconds (31), rate), rate) == "Music continues");
}

TEST_CASE ("the strip's columns hold the loudest level over their share of the window", "[ui][record_view]")
{
    PeaksTable peaks;
    const auto envelope = backingEnvelope (songWithABreak (peaks), peaks.lookup());

    // 30 columns of 1 s from -6 s: music in columns 6-9 (0-4 s) and 14-17 (8-12 s).
    const auto columns = lookAheadColumns (envelope, lookAheadWindow (0, rate), 30);
    REQUIRE (columns.size() == 30);
    for (int column = 0; column < 30; ++column)
    {
        const auto music = (column >= 6 && column < 10) || (column >= 14 && column < 18);
        CHECK_THAT (columns[(std::size_t) column], WithinAbs (music ? 0.5 : 0.0, 0.0));
    }
}

TEST_CASE ("the strip's view shows the pre-roll countdown and the Pass to tint", "[ui][record_view]")
{
    PeaksTable peaks;
    const auto envelope = backingEnvelope (songWithABreak (peaks), peaks.lookup());

    const auto preRolling = lookAheadView (envelope, RecordTransport { .playhead = seconds (0.5), .recordPoint = seconds (2) }, rate, 400);
    CHECK (preRolling.columns.size() == 400);
    CHECK (preRolling.window == lookAheadWindow (seconds (0.5), rate));
    CHECK (preRolling.recordPoint == seconds (2));
    CHECK (preRolling.preRollText == "Pass starts in 1.5 s");
    CHECK (preRolling.countdown == "Silence in 3.5 s");
    CHECK_FALSE (preRolling.passStart.has_value());

    const auto recording = lookAheadView (envelope, RecordTransport { .playhead = seconds (3), .passStart = seconds (2) }, rate, 400);
    CHECK (recording.passStart == seconds (2));
    CHECK (recording.preRollText.empty());
}

TEST_CASE ("the overview spans the whole Song and maps x to Song samples", "[ui][record_view]")
{
    PeaksTable peaks;
    auto song = songWithABreak (peaks);

    // At least 30 s; longer Songs, or a playhead past the end, stretch it.
    CHECK (overviewLength (song, 0) == seconds (30));
    song.tracks[0].clips[1].timelineStart = seconds (40);
    CHECK (overviewLength (song, 0) == seconds (44));
    CHECK (overviewLength (song, seconds (50)) == seconds (50));

    CHECK (overviewSampleAt (250, 1000, seconds (40)) == seconds (10));
    CHECK (overviewSampleAt (-20, 1000, seconds (40)) == 0);
    CHECK (overviewSampleAt (1200, 1000, seconds (40)) == seconds (40));
    CHECK_THAT (overviewX (seconds (10), 1000, seconds (40)), WithinAbs (250.0, 1e-9));
}
