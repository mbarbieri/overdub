#include "ui/ListeningRenders.h"

#include "song/AtomicWrite.h"
#include "song/FixtureSongBuilder.h"
#include "song/TranscribeMix.h"
#include "song/Transpose.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using overdub::song::ClipSource;
using overdub::song::ClipSourceKind;
using overdub::song::Transpose;
using overdub::ui::ListeningRenders;
using overdub::ui::ListeningWanted;
using namespace overdub::song::testing;

// The Listening Renders driven on their own (issue #247): a real package's Song and files, no
// engine, no OpenSong. `playHeard` only counts the switches it asks for.
namespace
{
struct RendersFixture
{
    std::unique_ptr<FixtureSong> fixture = FixtureSongBuilder().backingTrack ("Band", 4.0).backingTrack ("Bass", 4.0, 110.0).build();
    juce::File root = fixture->package->location();
    overdub::song::Song song = fixture->song();
    int switches = 0;
    std::unique_ptr<ListeningRenders> renders = std::make_unique<ListeningRenders> (root, song, [this] { ++switches; });

    ~RendersFixture() { renders.reset(); }

    bool rendering() const { return renders->isTransposeRendering() || renders->isPracticeRendering() || renders->isOctaveRendering(); }

    // Advances, as the window's timer does, until no render runs; returns every Error handed back.
    std::vector<overdub::core::Error> advanceUntilRendered (const ListeningWanted& wanted)
    {
        std::vector<overdub::core::Error> errors;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        do
        {
            for (auto& error : renders->advance (song, wanted, ListeningRenders::Clock::now()))
                errors.push_back (std::move (error));
            if (! rendering())
                return errors;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL ("the renders did not finish");
        return errors;
    }

    // Advances until `heard` holds, at most 30 s.
    template <typename Predicate>
    void advanceUntil (const ListeningWanted& wanted, Predicate heard)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (! heard() && std::chrono::steady_clock::now() < deadline)
        {
            REQUIRE (renders->advance (song, wanted, ListeningRenders::Clock::now()).empty());
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        REQUIRE (heard());
    }

    ClipSource bandSource() const { return { ClipSourceKind::backing, song.backingTracks[0].id }; }
    const overdub::song::Id& bassTrackId() const { return fixture->trackNamed ("Bass").id; }

    int samples (double seconds) const { return (int) (seconds * song.sampleRate); }
};
} // namespace

TEST_CASE ("a Transpose is heard only once every Backing Track's render is there", "[ui][listening_renders][transpose]")
{
    RendersFixture f;
    auto transposed = overdub::song::setTranspose (f.song, { .semitones = 3 });
    REQUIRE (transposed.has_value());
    f.song = *transposed;

    f.renders->follow (f.song, {});

    CHECK (f.renders->isTransposeRendering());
    CHECK (f.renders->heardTranspose() == Transpose {});
    CHECK (f.switches == 0);

    CHECK (f.advanceUntilRendered ({}).empty());
    CHECK (f.renders->heardTranspose() == Transpose { .semitones = 3 });
    CHECK (f.switches == 1);
    for (const auto& backingTrack : f.song.backingTracks)
        CHECK (overdub::song::transposedFileFor (f.root, backingTrack, { .semitones = 3 }).existsAsFile());
}

TEST_CASE ("a Practice Speed plays the Loop's window renders first, then the whole Song's", "[ui][listening_renders][practice_speed]")
{
    RendersFixture f;
    const ListeningWanted wanted { .practiceSpeedPercent = 80, .heardLoop = overdub::engine::LoopRange { f.samples (0.5), f.samples (1.0) } };

    f.renders->follow (f.song, wanted);
    CHECK (f.renders->isPracticeRendering());
    CHECK (f.renders->heardPracticeSpeed() == 100);

    // Heard as soon as the Loop's window is rendered, while the whole Song still renders.
    f.advanceUntil (wanted, [&f] { return f.renders->heardPracticeSpeed() == 80; });
    CHECK (f.renders->isPracticeRendering());
    const auto loopPlayback = f.renders->practicePlayback (f.song, wanted);
    CHECK (loopPlayback.speedPercent == 80);
    REQUIRE (loopPlayback.renders.contains (f.bandSource()));
    CHECK (loopPlayback.renders.at (f.bandSource()).getFileName().contains (".range"));

    CHECK (f.advanceUntilRendered (wanted).empty());
    const auto songPlayback = f.renders->practicePlayback (f.song, wanted);
    REQUIRE (songPlayback.renders.contains (f.bandSource()));
    CHECK_FALSE (songPlayback.renders.at (f.bandSource()).getFileName().contains (".range"));
    CHECK (f.switches >= 2);

    const auto folder = f.renders->practiceFolder();
    REQUIRE (folder.isDirectory());
    f.renders.reset();
    CHECK_FALSE (folder.exists());
}

TEST_CASE ("the octave-shifted Track is heard at once and plays its renders for the heard key once they are there", "[ui][listening_renders][octave_shift]")
{
    RendersFixture f;
    auto focused = overdub::song::setFocusTrack (f.song, f.bassTrackId());
    REQUIRE (focused.has_value());
    auto shifted = overdub::song::setOctaveShift (*focused, f.bassTrackId(), 1);
    REQUIRE (shifted.has_value());
    f.song = *shifted;
    const ListeningWanted wanted { .octaveShiftedTrack = f.bassTrackId() };

    f.renders->follow (f.song, wanted);
    CHECK (f.renders->isOctaveRendering());
    const auto whileRendering = f.renders->practicePlayback (f.song, wanted).octaveShifted;
    REQUIRE (whileRendering.has_value());
    CHECK (whileRendering->trackId == f.bassTrackId());
    CHECK (whileRendering->renders.empty());

    CHECK (f.advanceUntilRendered (wanted).empty());
    const auto rendered = f.renders->practicePlayback (f.song, wanted).octaveShifted;
    REQUIRE (rendered.has_value());
    REQUIRE (rendered->renders.size() == 1);
    CHECK (rendered->renders.begin()->second.getFileName().contains ("octave+1.speed100"));
    CHECK (f.switches == 1);
    CHECK (f.renders->heardPracticeSpeed() == 100);
}

TEST_CASE ("a failed render is handed back once and tried again only once its key changes or it is retried", "[ui][listening_renders][practice_speed]")
{
    RendersFixture f;
    // A folder that is not empty where the render's temp file would go: the write fails.
    const auto blocker = overdub::song::tempFileFor (overdub::song::practiceRenderFileFor (f.renders->practiceFolder(), f.bandSource(), { .speedPercent = 80 }));
    REQUIRE (blocker.createDirectory().wasOk());
    REQUIRE (blocker.getChildFile ("in the way").replaceWithText ("x"));
    const ListeningWanted wanted { .practiceSpeedPercent = 80 };

    f.renders->follow (f.song, wanted);
    const auto errors = f.advanceUntilRendered (wanted);

    REQUIRE (errors.size() == 1);
    CHECK (errors[0].code == "song.practice_render_failed");
    CHECK (f.renders->heardPracticeSpeed() == 100);
    f.renders->follow (f.song, wanted);
    CHECK (f.renders->advance (f.song, wanted, ListeningRenders::Clock::now()).empty());
    CHECK_FALSE (f.renders->isPracticeRendering());

    SECTION ("another Practice Speed renders again")
    {
        const ListeningWanted slower { .practiceSpeedPercent = 75 };
        f.renders->follow (f.song, slower);
        CHECK (f.renders->isPracticeRendering());
        CHECK (f.advanceUntilRendered (slower).empty());
        CHECK (f.renders->heardPracticeSpeed() == 75);
    }
    SECTION ("retrying the same Practice Speed renders again")
    {
        REQUIRE (blocker.deleteRecursively());
        f.renders->retryPracticeRenders();
        f.renders->follow (f.song, wanted);
        CHECK (f.renders->isPracticeRendering());
        CHECK (f.advanceUntilRendered (wanted).empty());
        CHECK (f.renders->heardPracticeSpeed() == 80);
    }
}
