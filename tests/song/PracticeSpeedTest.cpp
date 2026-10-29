#include "song/PracticeSpeed.h"

#include "core/Log.h"
#include "song/AtomicWrite.h"
#include "song/BackingTrackImport.h"

#include "FixtureSongBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <thread>

using namespace overdub::song;
using namespace overdub::song::testing;

// Practice Speed's renders (issue #212, ADR 0009): which sources need one, where they live, and the
// worker that makes them.
namespace
{
// A Song with a Backing Track "Band" (on its own Track, in a Clip) and a 1 s Take "take-a" in a
// Clip on a Track "Guitar", plus a Take "take-b" no Clip plays; and a render folder outside the
// package.
struct PracticeFixture
{
    std::unique_ptr<FixtureSong> fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).track ("Guitar").build();
    juce::File root = fixture->package->location();
    juce::File folder = fixture->folder.getChildFile ("practice");

    PracticeFixture()
    {
        REQUIRE (folder.createDirectory().wasOk());
        auto song = fixture->song();
        for (const auto* id : { "take-a", "take-b" })
        {
            Take take;
            take.id = id;
            take.file = std::string ("audio/") + id + ".wav";
            take.channels = 1;
            take.lengthSamples = 48000;
            juce::AudioBuffer<float> audio (1, 48000);
            audio.clear();
            REQUIRE (writeFloatWavAtomically (root.getChildFile (take.file), audio, 48000, {}).has_value());
            song.takes.push_back (take);
        }
        Clip clip;
        clip.id = "clip-a";
        clip.source = { ClipSourceKind::take, "take-a" };
        clip.length = 48000;
        song.tracks.back().clips.push_back (clip);
        REQUIRE (fixture->package->edit (song, "Add Takes").has_value());
    }

    const Song& song() const { return fixture->song(); }
    ClipSource band() const { return { ClipSourceKind::backing, song().backingTracks.at (0).id }; }
    static ClipSource takeA() { return { ClipSourceKind::take, "take-a" }; }
};

std::optional<overdub::core::Expected<void>> runToEnd (PracticeSpeedRun& run)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto ended = run.advance (PracticeSpeedRun::Clock::now()))
            return ended;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    FAIL ("the Practice Speed run did not end");
    return std::nullopt;
}
} // namespace

TEST_CASE ("Practice Speed is a whole percent from 25 to 125", "[song][practice_speed]")
{
    CHECK (checkPracticeSpeed (25) == 25);
    CHECK (checkPracticeSpeed (50) == 50);
    CHECK (checkPracticeSpeed (125) == 125);
    for (const int outside : { 24, 126, 0, -80 })
    {
        const auto refused = checkPracticeSpeed (outside);
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.practice_speed_out_of_range");
        CHECK (refused.error().fields.at ("speed_percent") == outside);
        CHECK (refused.error().fields.at ("min") == 25);
        CHECK (refused.error().fields.at ("max") == 125);
    }
}

TEST_CASE ("each source a Clip plays needs one render at a Practice Speed, a Backing Track's at the Song's Transpose", "[song][practice_speed]")
{
    PracticeFixture f;
    const PracticeRenderKey key { .speedPercent = 80, .semitones = 2 };

    const auto missing = missingPracticeRenders (f.root, f.folder, f.song(), key);

    REQUIRE (missing.size() == 2); // take-b plays in no Clip
    CHECK (missing[0].source == f.band());
    CHECK (missing[0].from == derivedFileFor (f.root, f.song().backingTracks[0]));
    CHECK (missing[0].to == f.folder.getChildFile ("backing-" + f.band().id + ".transpose+2.speed80.wav"));
    CHECK (missing[0].semitones == 2);
    CHECK (missing[1].source == PracticeFixture::takeA());
    CHECK (missing[1].from == f.root.getChildFile ("audio/take-a.wav"));
    CHECK (missing[1].to == f.folder.getChildFile ("take-take-a.speed80.wav"));
    CHECK (missing[1].semitones == 0);

    REQUIRE (missing[1].to.replaceWithText ("rendered"));
    const auto stillMissing = missingPracticeRenders (f.root, f.folder, f.song(), key);
    REQUIRE (stillMissing.size() == 1);
    CHECK (stillMissing[0].source == f.band());
    CHECK (practiceRendersOf (f.folder, f.song(), key) == std::map<ClipSource, juce::File> { { PracticeFixture::takeA(), missing[1].to } });

    CHECK (missingPracticeRenders (f.root, f.folder, f.song(), {}).empty());
}

TEST_CASE ("a Take's render serves every Transpose, a Backing Track's only its own", "[song][practice_speed]")
{
    PracticeFixture f;
    for (const auto& job : missingPracticeRenders (f.root, f.folder, f.song(), { .speedPercent = 80, .semitones = 0 }))
        REQUIRE (job.to.replaceWithText ("rendered"));

    const auto missing = missingPracticeRenders (f.root, f.folder, f.song(), { .speedPercent = 80, .semitones = -1 });

    REQUIRE (missing.size() == 1);
    CHECK (missing[0].source == f.band());
    CHECK (missing[0].to.getFileName() == juce::String ("backing-" + f.band().id + ".transpose-1.speed80.wav"));
}

TEST_CASE ("only the renders the Song plays at the kept Practice Speed and Transpose stay", "[song][practice_speed]")
{
    PracticeFixture f;
    std::vector<juce::File> written;
    for (const auto key : { PracticeRenderKey { 80, 0 }, PracticeRenderKey { 80, 3 }, PracticeRenderKey { 90, 3 } })
        for (const auto& job : missingPracticeRenders (f.root, f.folder, f.song(), key))
        {
            REQUIRE (job.to.replaceWithText ("rendered"));
            written.push_back (job.to);
        }
    REQUIRE (written.size() == 5); // the Take's render at 80 serves both Transposes

    CHECK (deletePracticeRendersExcept (f.folder, f.song(), { .speedPercent = 80, .semitones = 3 }) == 3);

    CHECK (practiceRendersOf (f.folder, f.song(), { .speedPercent = 80, .semitones = 3 }).size() == 2);
    CHECK (f.folder.findChildFiles (juce::File::findFiles, false).size() == 2);
    CHECK (deletePracticeRendersExcept (f.folder, f.song(), {}) == 2);
}

TEST_CASE ("a Practice Speed run renders every job on a worker and logs its start and end", "[song][practice_speed]")
{
    PracticeFixture f;
    const PracticeRenderKey key { .speedPercent = 80, .semitones = 1 };
    overdub::core::ScopedLogCapture capture;

    auto run = PracticeSpeedRun::start (f.song().sampleRate, key, missingPracticeRenders (f.root, f.folder, f.song(), key), PracticeSpeedRun::Clock::now());
    CHECK (run->key() == key);
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    CHECK (ended->has_value());
    CHECK (missingPracticeRenders (f.root, f.folder, f.song(), key).empty());
    const auto events = capture.events();
    const auto named = [&] (const char* name) { return std::ranges::count_if (events, [name] (const auto& event) { return event.at ("event") == name; }); };
    CHECK (named ("song.practice_speed_started") == 1);
    CHECK (named ("song.practice_speed_rendered") == 1);
    for (const auto& event : events)
        if (event.at ("event") == "song.practice_speed_rendered")
        {
            CHECK (event.at ("speed_percent") == 80);
            CHECK (event.at ("renders") == 2);
            CHECK (event.contains ("duration_ms"));
        }
}

TEST_CASE ("a Practice Speed run that cannot render names the source", "[song][practice_speed]")
{
    PracticeFixture f;
    const PracticeRenderKey key { .speedPercent = 120, .semitones = 0 };
    const auto jobs = missingPracticeRenders (f.root, f.folder, f.song(), key);
    REQUIRE (f.root.getChildFile ("audio/take-a.wav").deleteFile());

    auto run = PracticeSpeedRun::start (f.song().sampleRate, key, jobs, PracticeSpeedRun::Clock::now());
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    REQUIRE_FALSE (ended->has_value());
    CHECK (ended->error().code == "song.practice_render_failed");
    CHECK (ended->error().fields.at ("source_kind") == "take");
    CHECK (ended->error().fields.at ("source_id") == "take-a");
    CHECK (run->failedSource() == ClipSource { ClipSourceKind::take, "take-a" });
}
