#include "ui/OpenSong.h"

#include "engine/EngineFixture.h"
#include "song/AtomicWrite.h"
#include "song/AudioFileDecode.h"
#include "song/AudioMeasurements.h"
#include "song/BackingTrackImport.h"
#include "song/FixtureSongBuilder.h"
#include "song/Transpose.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::song::testing;

// Transpose in the open Song (issue #209, ADR 0009): renders run on a worker that poll() drives;
// playback keeps the Transpose it plays (heardTranspose) until every render of the Song's
// Transpose is ready.
namespace
{
// An OpenSong over a Song with two 1 s Backing Tracks, bound to an engine.
struct TransposeFixture
{
    std::unique_ptr<FixtureSong> fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).backingTrack ("Click", 1.0, 1000.0).build();
    juce::File root = fixture->package->location();
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    std::unique_ptr<OpenSong> openSong = std::make_unique<OpenSong> (std::move (fixture->package), AudioBinding { &engine, {}, {} });

    ~TransposeFixture() { openSong.reset(); }

    const overdub::song::Song& song() const { return openSong->song(); }

    void setTranspose (int semitones)
    {
        auto changed = overdub::song::setTranspose (song(), semitones);
        REQUIRE (changed.has_value());
        REQUIRE (openSong->edit (std::move (*changed), overdub::song::transposeLabel (semitones)).has_value());
    }

    // Polls, as the window's timer does, until no render runs; returns every Error poll() handed back.
    std::vector<overdub::core::Error> pollUntilRendered()
    {
        std::vector<overdub::core::Error> errors;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        do
        {
            for (auto& error : openSong->poll())
                errors.push_back (std::move (error));
            if (! openSong->isTransposeRendering())
                return errors;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL ("the Transpose renders did not finish");
        return errors;
    }

    bool hasRenders (int semitones) const
    {
        for (const auto& backingTrack : song().backingTracks)
            if (! overdub::song::transposedFileFor (root, backingTrack, semitones).existsAsFile())
                return false;
        return true;
    }

    // Imports a 2 s sine at `frequency` as a new Backing Track and returns it.
    overdub::song::BackingTrack importSine (double frequency)
    {
        const auto imported = openSong->importFiles ({ fixture->writeSineFile ("Keys.wav", 2.0, frequency) });
        REQUIRE (imported.size() == 1);
        REQUIRE (imported[0].has_value());
        return song().backingTracks.back();
    }

    int rendersInPackage() const
    {
        return root.getChildFile ("backing").findChildFiles (juce::File::findFiles, false, "*.transpose*").size();
    }
};
} // namespace

TEST_CASE ("a Transpose change plays the previous audio until every Backing Track's render is ready", "[ui][open_song][transpose]")
{
    TransposeFixture f;

    f.setTranspose (2);

    CHECK (f.song().transpose == 2);
    CHECK (f.openSong->heardTranspose() == 0);
    CHECK (f.openSong->isTransposing());
    REQUIRE (f.openSong->poll().empty());
    CHECK (f.openSong->isTransposeRendering());
    CHECK (f.openSong->heardTranspose() == 0);

    CHECK (f.pollUntilRendered().empty());
    CHECK (f.openSong->heardTranspose() == 2);
    CHECK_FALSE (f.openSong->isTransposing());
    CHECK (f.hasRenders (2));
}

TEST_CASE ("the package keeps only the current Transpose's renders, and undo renders an earlier value again", "[ui][open_song][transpose]")
{
    TransposeFixture f;
    f.setTranspose (2);
    f.pollUntilRendered();
    f.setTranspose (-5);
    f.pollUntilRendered();

    CHECK (f.openSong->heardTranspose() == -5);
    CHECK (f.hasRenders (-5));
    CHECK (f.rendersInPackage() == 2);

    REQUIRE (f.openSong->undo().has_value());
    CHECK (f.song().transpose == 2);
    CHECK (f.openSong->heardTranspose() == -5);
    REQUIRE (f.openSong->poll().empty());
    CHECK (f.openSong->isTransposeRendering());
    f.pollUntilRendered();

    CHECK (f.openSong->heardTranspose() == 2);
    CHECK (f.hasRenders (2));
    CHECK (f.rendersInPackage() == 2);
}

TEST_CASE ("at Transpose 0 playback uses the derived audio at once and no render stays", "[ui][open_song][transpose]")
{
    TransposeFixture f;
    f.setTranspose (7);
    f.pollUntilRendered();

    f.setTranspose (0);
    REQUIRE (f.openSong->poll().empty());

    CHECK_FALSE (f.openSong->isTransposeRendering());
    CHECK (f.openSong->heardTranspose() == 0);
    CHECK (f.rendersInPackage() == 0);
}

TEST_CASE ("a failed render keeps the previous audio and hands its Error back once", "[ui][open_song][transpose]")
{
    TransposeFixture f;
    // A folder that is not empty where the render's temp file would go: the write fails.
    const auto band = f.song().backingTracks.at (0);
    const auto blocker = overdub::song::tempFileFor (overdub::song::transposedFileFor (f.root, band, 3));
    REQUIRE (blocker.createDirectory().wasOk());
    REQUIRE (blocker.getChildFile ("in the way").replaceWithText ("x"));

    f.setTranspose (3);
    const auto errors = f.pollUntilRendered();

    REQUIRE (errors.size() == 1);
    CHECK (errors[0].code == "song.transpose_render_failed");
    CHECK (errors[0].fields.at ("backing_track_id") == band.id);
    CHECK (f.openSong->heardTranspose() == 0);
    // Not tried again until Transpose changes.
    CHECK (f.openSong->poll().empty());
    CHECK_FALSE (f.openSong->isTransposeRendering());
}

TEST_CASE ("a Song opened at a Transpose plays its derived audio until what is missing is rendered, and drops other renders", "[ui][open_song][transpose]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).build();
    const auto root = fixture->package->location();
    auto changed = overdub::song::setTranspose (fixture->song(), -1);
    REQUIRE (changed.has_value());
    REQUIRE (fixture->package->edit (*changed, overdub::song::transposeLabel (-1)).has_value());
    const auto backingTrack = fixture->song().backingTracks.at (0);
    const auto stale = overdub::song::transposedFileFor (root, backingTrack, 4);
    REQUIRE (stale.replaceWithText ("an older render"));

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    {
        OpenSong openSong { std::move (fixture->package), AudioBinding { &engine, {}, {} } };

        CHECK_FALSE (stale.exists());
        // The derived audio plays until the -1 render is there.
        CHECK (openSong.heardTranspose() == 0);
        CHECK (openSong.isTransposeRendering());
        CHECK (openSong.isTransposing());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while ((openSong.poll(), openSong.isTransposing()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        CHECK_FALSE (openSong.isTransposing());
        CHECK (openSong.heardTranspose() == -1);
        CHECK (overdub::song::transposedFileFor (root, openSong.song().backingTracks.at (0), -1).existsAsFile());
    }
}

TEST_CASE ("a Backing Track imported into a transposed Song is rendered at its Transpose, Transposing from the import on", "[ui][open_song][transpose][import]")
{
    constexpr double sineFrequency = 440.0;
    constexpr int semitones = -3;
    TransposeFixture f;
    f.setTranspose (semitones);
    f.pollUntilRendered();

    const auto keys = f.importSine (sineFrequency);

    // Record and Export read isTransposing() (ui/Availability.h): they wait from the import on.
    CHECK (f.openSong->isTransposing());
    CHECK (f.openSong->isTransposeRendering());
    CHECK (f.openSong->heardTranspose() == semitones);
    CHECK (f.pollUntilRendered().empty());
    CHECK_FALSE (f.openSong->isTransposing());

    const auto rendered = overdub::song::readWavFile (overdub::song::transposedFileFor (f.root, keys, semitones));
    REQUIRE (rendered.has_value());
    const auto sampleRate = f.song().sampleRate;
    const auto expected = sineFrequency * std::pow (2.0, semitones / 12.0); // 369.99 Hz
    // ADR 0005: within 5 cents, away from both ends.
    const auto measured = measuredFrequency (rendered->getReadPointer (0), sampleRate / 4, sampleRate * 7 / 4, sampleRate);
    CHECK_THAT (1200.0 * std::log2 (measured / expected), Catch::Matchers::WithinAbs (0.0, 5.0));
}

TEST_CASE ("undoing an import leaves no render of its Backing Track, whether or not the render had finished", "[ui][open_song][transpose][import]")
{
    const auto renderFinishedFirst = GENERATE (true, false);
    TransposeFixture f;
    f.setTranspose (2);
    f.pollUntilRendered();
    const auto keys = f.importSine (440.0);
    if (renderFinishedFirst)
    {
        f.pollUntilRendered();
        REQUIRE (overdub::song::transposedFileFor (f.root, keys, 2).existsAsFile());
    }

    REQUIRE (f.openSong->undo().has_value());
    // A render of the removed Backing Track is cancelled: Record and Export wait for nothing.
    CHECK_FALSE (f.openSong->isTransposing());
    f.pollUntilRendered();

    CHECK_FALSE (overdub::song::transposedFileFor (f.root, keys, 2).exists());
    CHECK (f.hasRenders (2));
    CHECK (f.rendersInPackage() == 2);
    CHECK_FALSE (f.openSong->isTransposing());
}
