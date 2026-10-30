#include "ui/Listening.h"

#include "core/Log.h"
#include "song/FixtureSongBuilder.h"
#include "song/LoopEdits.h"
#include "song/TranscribeMix.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using overdub::engine::HeardBandFilter;
using overdub::engine::LoopRange;
using overdub::song::Song;
using overdub::ui::Listening;
using namespace overdub::song::testing;

// Listening driven on its own (issue #249): a real package's Song, no engine, no OpenSong, no
// window. The fixture Song's Bass Track is the Focus Track, soloed in the Transcribe Mix, shifted an
// octave down and band filtered, and its active Loop loops from 1 s to 2 s at 60%.
namespace
{
template <typename T>
T unwrap (overdub::core::Expected<T> result)
{
    REQUIRE (result.has_value());
    return std::move (*result);
}

struct ListeningFixture
{
    std::unique_ptr<FixtureSong> fixture = FixtureSongBuilder().backingTrack ("Band", 4.0).backingTrack ("Bass", 4.0, 110.0).build();
    overdub::song::Id bassId = fixture->trackNamed ("Bass").id;
    overdub::song::Id loopId = "loop-1";
    Song song = transcribeSetUp (fixture->song());
    std::unique_ptr<Listening> listening = std::make_unique<Listening> (fixture->package->location(), song, [] {});

    Song transcribeSetUp (Song base) const
    {
        const auto second = base.sampleRate;
        base = unwrap (overdub::song::setFocusTrack (std::move (base), bassId));
        base = unwrap (overdub::song::setTranscribeSolo (std::move (base), bassId, true));
        base = unwrap (overdub::song::setOctaveShift (std::move (base), bassId, -1));
        base = unwrap (overdub::song::setBandFilter (std::move (base), bassId, { .on = true, .lowHz = 200, .highHz = 3200 }));
        base = unwrap (overdub::song::addLoop (std::move (base), second, 2 * second, loopId, 60));
        return unwrap (overdub::song::setActiveLoop (std::move (base), loopId, true));
    }

    // The fixture's Tracks in order: Band, then Bass.
    static const overdub::song::Track& band (const Song& in) { return in.tracks.at (0); }
    const overdub::song::Track& bass (const Song& in) const
    {
        REQUIRE (in.tracks.at (1).id == bassId);
        return in.tracks.at (1);
    }

    int loopSpeed (const Song& in) const { return overdub::song::findLoop (in, loopId)->practiceSpeedPercent; }
};
} // namespace

TEST_CASE ("outside Transcribe mode the Song is heard with its own mix and no listening aid", "[ui][listening]")
{
    ListeningFixture f;

    const auto heard = f.listening->heard (f.song);

    CHECK_FALSE (f.bass (heard.song).solo);
    CHECK_FALSE (heard.song.focusTrackId.has_value());
    CHECK_FALSE (heard.loop.has_value());
    CHECK_FALSE (heard.bandFilter.has_value());
    CHECK_FALSE (heard.octaveShiftedTrack.has_value());
    CHECK (heard.octaveShift == 0);
}

TEST_CASE ("in Transcribe mode the Transcribe Mix, the active Loop and the Focus Track's aids are heard", "[ui][listening]")
{
    ListeningFixture f;
    f.listening->setTranscribing (f.song, true, false);

    const auto heard = f.listening->heard (f.song);

    CHECK_FALSE (f.band (heard.song).solo);
    CHECK (f.bass (heard.song).solo); // the Transcribe Mix soloes the Focus Track
    CHECK_FALSE (f.bass (heard.song).transcribeSolo);
    CHECK (heard.loop == LoopRange { f.song.sampleRate, 2 * f.song.sampleRate });
    CHECK (heard.bandFilter == HeardBandFilter { .trackId = f.bassId, .lowHz = 200, .highHz = 3200 });
    CHECK (heard.octaveShiftedTrack == f.bassId);
    CHECK (heard.octaveShift == -1);
}

TEST_CASE ("a Loop is heard only while it loops, and a band filter only while it is on", "[ui][listening]")
{
    ListeningFixture f;
    f.listening->setTranscribing (f.song, true, false);
    auto notLooping = unwrap (overdub::song::setLooping (f.song, false));
    notLooping = unwrap (overdub::song::setBandFilter (std::move (notLooping), f.bassId, { .on = false, .lowHz = 200, .highHz = 3200 }));

    const auto heard = f.listening->heard (notLooping);

    CHECK_FALSE (heard.loop.has_value());
    CHECK_FALSE (heard.bandFilter.has_value());
    CHECK (heard.octaveShiftedTrack == f.bassId);
}

TEST_CASE ("entering Transcribe mode plays at the looping Loop's Practice Speed and leaving it returns to 100%", "[ui][listening]")
{
    ListeningFixture f;
    REQUIRE (f.listening->practiceSpeed() == 100);

    f.listening->setTranscribing (f.song, true, true);
    CHECK (f.listening->practiceSpeed() == 60);

    f.listening->setTranscribing (f.song, false, true);
    CHECK (f.listening->practiceSpeed() == 100);
}

TEST_CASE ("the Practice Speed stays when it may not change now", "[ui][listening]")
{
    ListeningFixture f;
    REQUIRE_FALSE (f.listening->setPracticeSpeed (f.song, 80).has_value());

    f.listening->setTranscribing (f.song, true, false);
    CHECK (f.listening->practiceSpeed() == 80);

    f.listening->setTranscribing (f.song, false, false);
    CHECK (f.listening->practiceSpeed() == 80);
}

TEST_CASE ("a Practice Speed set while a Loop is heard is handed back as that Loop's", "[ui][listening]")
{
    ListeningFixture f;
    f.listening->setTranscribing (f.song, true, true);
    const overdub::core::ScopedLogCapture capture;

    const auto withLoopSpeed = f.listening->setPracticeSpeed (f.song, 45);

    CHECK (f.listening->practiceSpeed() == 45);
    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "ui.practice_speed_set");
    CHECK (events[0]["from_percent"] == 60);
    CHECK (events[0]["to_percent"] == 45);
    REQUIRE (withLoopSpeed.has_value());
    CHECK (f.loopSpeed (*withLoopSpeed) == 45);
    CHECK (f.listening->setPracticeSpeed (*withLoopSpeed, 45) == std::nullopt);
}

TEST_CASE ("a Practice Speed set while no Loop is heard leaves the Loops as they are", "[ui][listening]")
{
    ListeningFixture f;

    CHECK (f.listening->setPracticeSpeed (f.song, 45) == std::nullopt);
    CHECK (f.listening->practiceSpeed() == 45);
}

TEST_CASE ("hearing a Loop plays at its own Practice Speed", "[ui][listening]")
{
    ListeningFixture f;
    auto notLooping = unwrap (overdub::song::setLooping (f.song, false));
    f.listening->setTranscribing (notLooping, true, true);
    REQUIRE (f.listening->practiceSpeed() == 100);

    f.listening->hearLoopPracticeSpeed (f.song, false);
    CHECK (f.listening->practiceSpeed() == 100);

    f.listening->hearLoopPracticeSpeed (f.song, true);
    CHECK (f.listening->practiceSpeed() == 60);
}
