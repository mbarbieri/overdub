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
    const PracticeRenderKey key { .speedPercent = 80, .transpose = { .semitones = 2 } };

    const auto missing = missingPracticeRenders (f.root, f.folder, f.song(), key);

    REQUIRE (missing.size() == 2); // take-b plays in no Clip
    CHECK (missing[0].source == f.band());
    CHECK (missing[0].from == derivedFileFor (f.root, f.song().backingTracks[0]));
    CHECK (missing[0].to == f.folder.getChildFile ("backing-" + f.band().id + ".transpose+2.speed80.wav"));
    CHECK (missing[0].transpose == overdub::song::Transpose { .semitones = 2 });
    CHECK (missing[1].source == PracticeFixture::takeA());
    CHECK (missing[1].from == f.root.getChildFile ("audio/take-a.wav"));
    CHECK (missing[1].to == f.folder.getChildFile ("take-take-a.speed80.wav"));
    CHECK (missing[1].transpose == overdub::song::Transpose { .semitones = 0 });

    REQUIRE (missing[1].to.replaceWithText ("rendered"));
    const auto stillMissing = missingPracticeRenders (f.root, f.folder, f.song(), key);
    REQUIRE (stillMissing.size() == 1);
    CHECK (stillMissing[0].source == f.band());
    CHECK (practiceRendersOf (f.folder, f.song(), key) == std::map<ClipSource, juce::File> { { PracticeFixture::takeA(), missing[1].to } });

    CHECK (missingPracticeRenders (f.root, f.folder, f.song(), {}).empty());
}

TEST_CASE ("an octave shift needs a render of every source even at 100%, shifted by 12 semitones per octave on top of the Transpose", "[song][practice_speed][octave]")
{
    PracticeFixture f;
    CHECK_FALSE (needsPracticeRender ({}));
    CHECK (needsPracticeRender ({ .octaveShift = -1 }));
    CHECK (needsPracticeRender ({ .speedPercent = 80 }));

    const PracticeRenderKey key { .speedPercent = 100, .transpose = { .semitones = 2, .cents = 10 }, .octaveShift = 1 };
    const auto missing = missingPracticeRenders (f.root, f.folder, f.song(), key);

    REQUIRE (missing.size() == 2);
    CHECK (missing[0].source == f.band());
    CHECK (missing[0].to == f.folder.getChildFile ("backing-" + f.band().id + ".transpose+2+10c.octave+1.speed100.wav"));
    CHECK (missing[0].transpose == overdub::song::Transpose { .semitones = 14, .cents = 10 });
    CHECK (missing[1].source == PracticeFixture::takeA());
    CHECK (missing[1].to == f.folder.getChildFile ("take-take-a.octave+1.speed100.wav"));
    CHECK (missing[1].transpose == overdub::song::Transpose { .semitones = 12 });

    const auto down = missingPracticeRenders (f.root, f.folder, f.song(), { .speedPercent = 80, .octaveShift = -1 });
    REQUIRE (down.size() == 2);
    CHECK (down[1].to.getFileName() == "take-take-a.octave-1.speed80.wav");
    CHECK (down[1].transpose == overdub::song::Transpose { .semitones = -12 });
    // The unshifted renders are other files.
    CHECK (practiceRenderFileFor (f.folder, PracticeFixture::takeA(), { .speedPercent = 80 }).getFileName() == "take-take-a.speed80.wav");
}

TEST_CASE ("a Take's render serves every Transpose, a Backing Track's only its own", "[song][practice_speed]")
{
    PracticeFixture f;
    for (const auto& job : missingPracticeRenders (f.root, f.folder, f.song(), { .speedPercent = 80, .transpose = { .semitones = 0 } }))
        REQUIRE (job.to.replaceWithText ("rendered"));

    const auto missing = missingPracticeRenders (f.root, f.folder, f.song(), { .speedPercent = 80, .transpose = { .semitones = -1 } });

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

    CHECK (deletePracticeRendersExcept (f.folder, f.song(), { .speedPercent = 80, .transpose = { .semitones = 3 } }) == 3);

    CHECK (practiceRendersOf (f.folder, f.song(), { .speedPercent = 80, .transpose = { .semitones = 3 } }).size() == 2);
    CHECK (f.folder.findChildFiles (juce::File::findFiles, false).size() == 2);
    CHECK (deletePracticeRendersExcept (f.folder, f.song(), {}) == 2);
}

TEST_CASE ("a Practice Speed run renders every job on a worker and logs its start and end", "[song][practice_speed]")
{
    PracticeFixture f;
    const PracticeRenderKey key { .speedPercent = 80, .transpose = { .semitones = 1 } };
    overdub::core::ScopedLogCapture capture;

    auto run = PracticeSpeedRun::start (f.song().sampleRate, key, std::nullopt, missingPracticeRenders (f.root, f.folder, f.song(), key), PracticeSpeedRun::Clock::now());
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
    const PracticeRenderKey key { .speedPercent = 120, .transpose = { .semitones = 0 } };
    const auto jobs = missingPracticeRenders (f.root, f.folder, f.song(), key);
    REQUIRE (f.root.getChildFile ("audio/take-a.wav").deleteFile());

    auto run = PracticeSpeedRun::start (f.song().sampleRate, key, std::nullopt, jobs, PracticeSpeedRun::Clock::now());
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    REQUIRE_FALSE (ended->has_value());
    CHECK (ended->error().code == "song.practice_render_failed");
    CHECK (ended->error().fields.at ("source_kind") == "take");
    CHECK (ended->error().fields.at ("source_id") == "take-a");
    CHECK (run->failedSource() == ClipSource { ClipSourceKind::take, "take-a" });
}

TEST_CASE ("a Loop's window is the Loop widened by 1 s on each side, from 0", "[song][practice_speed][loop]")
{
    CHECK (loopRenderWindow (96000, 144000, 48000) == SampleRange { 48000, 192000 });
    CHECK (loopRenderWindow (12000, 24000, 48000) == SampleRange { 0, 72000 });
    CHECK (loopRenderWindow (44100, 88200, 44100) == SampleRange { 0, 132300 });
}

TEST_CASE ("a source's range in a Loop's window spans the source samples its Clips play there", "[song][practice_speed][loop]")
{
    PracticeFixture f;
    auto song = f.song();
    // take-a plays from 0.5 s of its file at Song time 2 s for 1 s, and again from 0 at 6 s.
    auto& clips = song.tracks.back().clips;
    clips[0].timelineStart = 96000;
    clips[0].sourceOffset = 24000;
    clips[0].length = 24000;
    auto again = clips[0];
    again.id = "clip-a2";
    again.timelineStart = 288000;
    again.sourceOffset = 0;
    clips.push_back (again);

    // Song time 2.25..2.75 s is 0.75..1.25 s of the file; 2.25..7 s spans both Clips.
    CHECK (sourceRangeIn (song, PracticeFixture::takeA(), { 108000, 132000 }) == SampleRange { 36000, 48000 });
    CHECK (sourceRangeIn (song, PracticeFixture::takeA(), { 108000, 336000 }) == SampleRange { 0, 48000 });
    CHECK (sourceRangeIn (song, PracticeFixture::takeA(), { 0, 96000 }) == std::nullopt);
    CHECK (sourceRangeIn (song, { ClipSourceKind::take, "take-b" }, { 0, 480000 }) == std::nullopt);
}

TEST_CASE ("in a Loop's window each source a Clip plays there needs a render of only its range there", "[song][practice_speed][loop]")
{
    PracticeFixture f;
    const PracticeRenderKey key { .speedPercent = 80, .transpose = { .semitones = 2 } };
    // The Band Clip plays 0..1 s, the take-a Clip 0..1 s: a window of 0.25..0.5 s.
    const SampleRange window { 12000, 24000 };

    const auto missing = missingPracticeRenders (f.root, f.folder, f.song(), key, window);

    REQUIRE (missing.size() == 2);
    CHECK (missing[0].source == f.band());
    CHECK (missing[0].sourceRange == SampleRange { 12000, 24000 });
    CHECK (missing[0].to == f.folder.getChildFile ("backing-" + f.band().id + ".transpose+2.speed80.range12000-24000.wav"));
    CHECK (missing[0].transpose == overdub::song::Transpose { .semitones = 2 });
    CHECK (missing[1].source == PracticeFixture::takeA());
    CHECK (missing[1].to == f.folder.getChildFile ("take-take-a.speed80.range12000-24000.wav"));
    // The whole Song's jobs have no range.
    for (const auto& job : missingPracticeRenders (f.root, f.folder, f.song(), key))
        CHECK_FALSE (job.sourceRange.has_value());

    // A range render plays in its window; a whole render, once there, wins and needs no range.
    REQUIRE (missing[0].to.replaceWithText ("rendered"));
    REQUIRE (missing[1].to.replaceWithText ("rendered"));
    CHECK (missingPracticeRenders (f.root, f.folder, f.song(), key, window).empty());
    CHECK (practiceRendersOf (f.folder, f.song(), key, window).at (f.band()) == missing[0].to);
    CHECK (practiceRendersOf (f.folder, f.song(), key).empty());
    const auto whole = practiceRenderFileFor (f.folder, f.band(), key);
    REQUIRE (whole.replaceWithText ("rendered"));
    CHECK (practiceRendersOf (f.folder, f.song(), key, window).at (f.band()) == whole);
    CHECK (deletePracticeRendersExcept (f.folder, f.song(), key, window) == 1); // the Band's range render
    CHECK_FALSE (missing[0].to.exists());
    CHECK (missing[1].to.exists());
}

TEST_CASE ("a Practice Speed run of a Loop's range renders logs its scope and window", "[song][practice_speed][loop]")
{
    PracticeFixture f;
    const PracticeRenderKey key { .speedPercent = 80, .transpose = { .semitones = 0 } };
    const SampleRange window { 12000, 36000 };
    overdub::core::ScopedLogCapture capture;

    auto loopRun = PracticeSpeedRun::start (f.song().sampleRate, key, window, missingPracticeRenders (f.root, f.folder, f.song(), key, window), PracticeSpeedRun::Clock::now());
    CHECK (loopRun->loopWindow() == window);
    REQUIRE (runToEnd (*loopRun).has_value());
    auto songRun = PracticeSpeedRun::start (f.song().sampleRate, key, window, missingPracticeRenders (f.root, f.folder, f.song(), key), PracticeSpeedRun::Clock::now());
    REQUIRE (runToEnd (*songRun).has_value());

    std::vector<nlohmann::json> started;
    for (const auto& event : capture.events())
        if (event.at ("event") == "song.practice_speed_started")
            started.push_back (event);
    REQUIRE (started.size() == 2);
    CHECK (started[0].at ("scope") == "loop");
    CHECK (started[0].at ("window_start") == 12000);
    CHECK (started[0].at ("window_end") == 36000);
    CHECK (started[0].at ("renders") == 2);
    CHECK (started[1].at ("scope") == "song");
    CHECK_FALSE (started[1].contains ("window_start"));
}
