#include "ui/MainComponent.h"

#include "song/AudioFileDecode.h"
#include "song/AudioMeasurements.h"
#include "song/BackingTrackImport.h"
#include "song/Transpose.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <thread>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;

// Recording and exporting in a transposed Song (issue #210, ADR 0009): both wait while Transposing,
// an export plays the Backing Tracks at the Song's Transpose, and Takes are never transposed.
namespace
{
struct ExportFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-transpose-export-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};

IntentResult runFromSocket (MainWindowFixture& f, SongIntent intent)
{
    return f.window->run (std::move (intent), Origin::control);
}

// Ticks the window, as its timer does, until playback is at the Song's Transpose.
void tickUntilTransposed (MainWindowFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (f.openSong->isTransposing() && std::chrono::steady_clock::now() < deadline)
    {
        f.window->tick();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    REQUIRE_FALSE (f.openSong->isTransposing());
}

void tickUntilExported (MainWindowFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    REQUIRE_FALSE (f.openSong->isExporting());
}

const juce::String transposingText = juce::String::fromUTF8 ("Transposing\xe2\x80\xa6");
} // namespace

TEST_CASE ("while Transposing, Record and Export are unavailable with the reason Transposing, and available once it is heard", "[ui][transpose][availability]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& sheet = openExportSheet (*f.window);
    setExportDestination (*f.window, folder.root.getChildFile ("Song.wav"));
    REQUIRE (sheet.exportButton.isEnabled());

    REQUIRE (runFromSocket (f, intent::SetTranspose { .semitones = 2 }).has_value());
    f.window->refresh();

    REQUIRE (f.openSong->isTransposing());
    CHECK_FALSE (f.window->topBar.recordButton.isEnabled());
    CHECK (f.window->topBar.recordButton.getTooltip() == transposingText);
    CHECK_FALSE (f.window->topBar.exportButton.isEnabled());
    CHECK (f.window->topBar.exportButton.getTooltip() == transposingText);
    CHECK_FALSE (sheet.exportButton.isEnabled());
    CHECK (sheet.exportButton.getTooltip() == transposingText);
    CHECK_FALSE (sheet.canExport());

    tickUntilTransposed (f);
    f.window->refresh();

    CHECK (f.window->topBar.recordButton.isEnabled());
    CHECK (f.window->topBar.recordButton.getTooltip().isEmpty());
    CHECK (f.window->topBar.exportButton.isEnabled());
    CHECK (sheet.exportButton.isEnabled());
    CHECK (sheet.exportButton.getTooltip().isEmpty());
}

TEST_CASE ("an export plays the Backing Tracks at the Song's Transpose", "[ui][transpose][export]")
{
    constexpr double sineFrequency = 440.0;
    // Whole semitones (issue #209), and a fine tune in cents alone or with semitones (issue #232).
    const auto transpose = GENERATE (intent::SetTranspose { .semitones = 3 }, intent::SetTranspose { .semitones = 0, .cents = 30 }, intent::SetTranspose { .semitones = -12, .cents = -50 });
    INFO (transpose.semitones << " semitones " << transpose.cents.value_or (0) << " cents");
    ExportFolder folder;
    MainWindowFixture f { song::testing::FixtureSongBuilder().backingTrack ("Band", 2.0, sineFrequency).build() };
    const auto destination = folder.root.getChildFile ("Song.wav");
    REQUIRE (runFromSocket (f, transpose).has_value());
    tickUntilTransposed (f);

    setExportDestination (*f.window, destination);
    REQUIRE (runFromSocket (f, intent::Export {}).has_value());
    tickUntilExported (f);

    const auto sampleRate = f.song().sampleRate;
    const auto exported = song::decodeAudioFile (destination, sampleRate);
    REQUIRE (exported.has_value());
    // 523.25 Hz, 447.69 Hz, 213.74 Hz
    const auto expected = sineFrequency * std::pow (2.0, (transpose.semitones + transpose.cents.value_or (0) / 100.0) / 12.0);
    // Measured away from both ends, where the stretcher has the whole spectral window. ADR 0005:
    // within 5 cents.
    const auto measured = song::testing::measuredFrequency (exported->audio.getReadPointer (0), sampleRate / 4, sampleRate * 7 / 4, sampleRate);
    CHECK_THAT (1200.0 * std::log2 (measured / expected), WithinAbs (0.0, 5.0));
}

TEST_CASE ("a Take recorded at Transpose -1 +20 cents plays its own file once Transpose is back at 0, which needs no confirmation", "[ui][transpose][record]")
{
    MainWindowFixture f;
    REQUIRE (runFromSocket (f, intent::SetTranspose { .semitones = -1, .cents = 20 }).has_value());
    tickUntilTransposed (f);
    REQUIRE (runFromSocket (f, intent::Record {}).has_value());
    f.runBlocks (20);
    REQUIRE (runFromSocket (f, intent::PlayOrStop {}).has_value());
    f.runBlocks (1);
    REQUIRE_FALSE (f.openSong->isRecording());
    REQUIRE (f.song().takes.size() == 2);
    const auto root = f.openSong->package().location();
    const song::ClipSource takeSource { song::ClipSourceKind::take, f.song().takes.front().id };
    const auto takeFile = song::mediaFileFor (root, f.song(), takeSource);
    REQUIRE (takeFile.has_value());
    juce::MemoryBlock recorded;
    REQUIRE (takeFile->loadFileAsData (recorded));

    REQUIRE (runFromSocket (f, intent::SetTranspose { 0 }).has_value());
    tickUntilTransposed (f);

    CHECK (f.song().transpose == song::Transpose {});
    CHECK (f.alertsShown.empty());
    CHECK (f.deleteQuestionsAsked.empty());
    CHECK (song::playedFileFor (root, f.song(), takeSource) == takeFile);
    juce::MemoryBlock afterwards;
    REQUIRE (takeFile->loadFileAsData (afterwards));
    CHECK (afterwards == recorded);
}
