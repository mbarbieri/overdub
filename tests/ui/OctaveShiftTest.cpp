#include "ui/TranscribePage.h"

#include "../song/AudioMeasurements.h"
#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "song/TranscribeMix.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;
using core::ScopedLogCapture;

// The Focus Track's octave shift (issue #233, ADR 0010): each Track keeps its own (−1..+2, outside
// history, saved), and only the Focus Track's is heard, only in Transcribe mode, through a render
// made in one pass; while it renders that Track plays silence and the column shows a spinner.
// Driven through MainComponent::run and clicks on a fixture Song of two Backing Tracks, "Bass" and
// "Drums", each the same 2 s 110 Hz sine (0.25) on its own Track.
namespace
{
constexpr double sineFrequency = 110.0;

std::unique_ptr<song::testing::FixtureSong> bassAndDrums()
{
    return song::testing::FixtureSongBuilder {}.backingTrack ("Bass", 2.0, sineFrequency).backingTrack ("Drums", 2.0, sineFrequency).build();
}

IntentResult runControl (MainWindowFixture& f, const Intent& intent)
{
    return f.window->run (intent, Origin::control);
}

void run (MainWindowFixture& f, SongIntent intent)
{
    REQUIRE (runControl (f, std::move (intent)).has_value());
}

void showMode (MainWindowFixture& f, Mode mode)
{
    REQUIRE (runControl (f, WindowIntent { intent::ShowMode { mode } }).has_value());
}

TranscribeColumn& column (MainWindowFixture& f)
{
    return f.window->songView()->transcribePage.column;
}

// The column's button for `octaves` (−1..+2).
juce::TextButton& octaveButton (MainWindowFixture& f, int octaves)
{
    return column (f).octaveButtons[(std::size_t) (octaves - song::minOctaveShift)];
}

// Polls, as the window's timer does, until no octave-shift render runs.
void tickUntilOctaveRendered (MainWindowFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (f.openSong->listening().renders().isOctaveRendering() && std::chrono::steady_clock::now() < deadline)
    {
        f.window->tick();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    REQUIRE_FALSE (f.openSong->listening().renders().isOctaveRendering());
}

// Plays about 0.8 s from the start (without polling) and returns the last 12000 output samples
// of channel 0: the middle of the sines, away from both ends of a render.
std::vector<float> heardFromStart (MainWindowFixture& f)
{
    run (f, SongIntent { intent::Seek { 0 } });
    if (! f.openSong->isPlaying())
        run (f, SongIntent { intent::PlayOrStop {} });
    f.device.run (f.engine, 150);
    const auto& output = f.device.outputHistory[0];
    REQUIRE (output.size() >= 12000);
    std::vector<float> heard { output.end() - 12000, output.end() };
    f.openSong->stop();
    f.device.run (f.engine, 1); // the engine reports stopped from its next block on
    return heard;
}

double heardFrequency (MainWindowFixture& f)
{
    const auto heard = heardFromStart (f);
    return song::testing::measuredFrequency (heard.data(), 0, (int) heard.size(), f.song().sampleRate);
}

float peakOf (const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (const auto sample : samples)
        peak = std::max (peak, std::abs (sample));
    return peak;
}

double centsBetween (double frequency, double reference)
{
    return 1200.0 * std::log2 (frequency / reference);
}

// Closes the window's Song, which writes song.json, and opens its package again.
void reopenSong (MainWindowFixture& f)
{
    const auto location = f.openSong->package().location();
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    auto reopened = song::SongPackage::open (location);
    REQUIRE (reopened.has_value());
    f.openSong = std::make_unique<OpenSong> (std::move (reopened->package), AudioBinding { &f.engine, {}, { "In 1", "In 2" } });
    f.window->setOpenSong (f.openSong.get());
}

struct ExportFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-octave-shift-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};
} // namespace

TEST_CASE ("the right column shows −1, 0, +1 and +2 for the Focus Track, its own shift on, all disabled without a Focus Track", "[ui][transcribe][octave]")
{
    MainWindowFixture f { bassAndDrums() };
    showMode (f, Mode::transcribe);

    for (int octaves = song::minOctaveShift; octaves <= song::maxOctaveShift; ++octaves)
    {
        CHECK (isShown (octaveButton (f, octaves)));
        CHECK_FALSE (octaveButton (f, octaves).isEnabled());
    }
    CHECK (octaveButton (f, -1).getButtonText() == juce::String::fromUTF8 ("\xe2\x88\x92"
                                                                           "1"));
    CHECK (octaveButton (f, 0).getButtonText() == "0");
    CHECK (octaveButton (f, 1).getButtonText() == "+1");
    CHECK (octaveButton (f, 2).getButtonText() == "+2");
    CHECK (isShown (column (f).octaveCaption));

    run (f, SongIntent { intent::SetFocusTrack { f.trackNamed ("Bass").id } });

    for (int octaves = song::minOctaveShift; octaves <= song::maxOctaveShift; ++octaves)
    {
        CHECK (octaveButton (f, octaves).isEnabled());
        CHECK (octaveButton (f, octaves).getToggleState() == (octaves == 0));
    }

    const auto undoLabel = f.openSong->package().undoLabel();
    const ScopedLogCapture capture;
    click (octaveButton (f, 2));

    CHECK (f.trackNamed ("Bass").octaveShift == 2);
    CHECK (f.trackNamed ("Drums").octaveShift == 0);
    CHECK (octaveButton (f, 2).getToggleState());
    CHECK_FALSE (octaveButton (f, 0).getToggleState());
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    CHECK (std::ranges::any_of (capture.events(), [] (const nlohmann::json& event) { return event.at ("event") == "ui.intent_run" && event.at ("op") == "set_octave_shift"; }));
    CHECK (std::ranges::any_of (capture.events(), [&] (const nlohmann::json& event) {
        return event.at ("event") == "ui.octave_shift_set" && event.at ("track_id") == f.trackNamed ("Bass").id && event.at ("octaves") == 2;
    }));
}

TEST_CASE ("a 110 Hz sine on the Focus Track at +1 is heard at 220 Hz in Transcribe mode, and the same sine on another Track is unchanged", "[ui][transcribe][octave][engine]")
{
    MainWindowFixture f { bassAndDrums() };
    const auto bassId = f.trackNamed ("Bass").id;
    const auto drumsId = f.trackNamed ("Drums").id;
    showMode (f, Mode::transcribe);
    run (f, SongIntent { intent::SetFocusTrack { bassId } }); // soloes Bass alone
    const ScopedLogCapture capture;

    run (f, SongIntent { intent::SetOctaveShift { bassId, 1 } });

    // While it renders, the Bass plays silence and the column shows a spinner.
    CHECK (f.openSong->listening().renders().isOctaveRendering());
    CHECK (f.openSong->heard().octaveShiftedTrack == bassId);
    f.window->refresh();
    CHECK (isShown (column (f).octaveSpinner));
    CHECK (peakOf (heardFromStart (f)) == 0.0f);

    tickUntilOctaveRendered (f);
    f.window->refresh();
    CHECK_FALSE (isShown (column (f).octaveSpinner));
    // One render, of the Bass's one source, made in one pass from its derived audio.
    const auto events = capture.events();
    const auto started = std::ranges::find_if (events, [] (const nlohmann::json& event) { return event.at ("event") == "song.practice_speed_started"; });
    REQUIRE (started != events.end());
    CHECK (started->at ("octave_shift") == 1);
    CHECK (started->at ("speed_percent") == 100);
    CHECK (started->at ("renders") == 1);

    // ADR 0005: within 5 cents.
    CHECK_THAT (centsBetween (heardFrequency (f), 2.0 * sineFrequency), WithinAbs (0.0, 5.0));

    SECTION ("the other Track's same sine, heard with the Bass still the Focus Track, is unchanged")
    {
        run (f, SongIntent { intent::SetTranscribeSolo { bassId, false } });
        run (f, SongIntent { intent::SetTranscribeSolo { drumsId, true } });
        CHECK (f.song().focusTrackId == bassId);
        CHECK_THAT (centsBetween (heardFrequency (f), sineFrequency), WithinAbs (0.0, 5.0));
    }

    SECTION ("Edit and Record never hear the shift")
    {
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { drumsId, true } } });
        showMode (f, Mode::edit);
        CHECK_FALSE (f.openSong->heard().octaveShiftedTrack.has_value());
        CHECK_THAT (centsBetween (heardFrequency (f), sineFrequency), WithinAbs (0.0, 5.0));
        showMode (f, Mode::record);
        CHECK_THAT (centsBetween (heardFrequency (f), sineFrequency), WithinAbs (0.0, 5.0));
        // Back in Transcribe mode the render is there still: heard at once.
        showMode (f, Mode::transcribe);
        CHECK_FALSE (f.openSong->listening().renders().isOctaveRendering());
        CHECK_THAT (centsBetween (heardFrequency (f), 2.0 * sineFrequency), WithinAbs (0.0, 5.0));
    }

    SECTION ("an export from Transcribe mode never hears the shift")
    {
        ExportFolder folder;
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { drumsId, true } } });
        showMode (f, Mode::transcribe);
        const auto destination = folder.root.getChildFile ("Song.wav");
        setExportDestination (*f.window, destination);
        run (f, SongIntent { intent::Export {} });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
        {
            f.runBlocks (1);
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        REQUIRE_FALSE (f.openSong->isExporting());
        const auto decoded = song::decodeAudioFile (destination, f.song().sampleRate);
        REQUIRE (decoded.has_value());
        const auto exported = decoded->audio.getReadPointer (0);
        CHECK_THAT (centsBetween (song::testing::measuredFrequency (exported, 24000, 72000, f.song().sampleRate), sineFrequency), WithinAbs (0.0, 5.0));
    }
}

TEST_CASE ("each Track keeps its own octave shift: moving the Focus brings back that Track's, and reopening the Song restores every Track's", "[ui][transcribe][octave][persistence]")
{
    MainWindowFixture f { bassAndDrums() };
    const auto bassId = f.trackNamed ("Bass").id;
    const auto drumsId = f.trackNamed ("Drums").id;
    showMode (f, Mode::transcribe);

    run (f, SongIntent { intent::SetFocusTrack { bassId } });
    click (octaveButton (f, 1));
    run (f, SongIntent { intent::SetFocusTrack { drumsId } });
    CHECK (octaveButton (f, 0).getToggleState()); // the Drums' own, not the Bass's
    CHECK (f.openSong->heard().octaveShiftedTrack == std::nullopt);
    click (octaveButton (f, -1));
    CHECK (f.openSong->heard().octaveShiftedTrack == drumsId);

    run (f, SongIntent { intent::SetFocusTrack { bassId } });

    CHECK (octaveButton (f, 1).getToggleState());
    CHECK (f.openSong->heard().octaveShiftedTrack == bassId);
    CHECK (f.trackNamed ("Bass").octaveShift == 1);
    CHECK (f.trackNamed ("Drums").octaveShift == -1);
    tickUntilOctaveRendered (f);
    CHECK_THAT (centsBetween (heardFrequency (f), 2.0 * sineFrequency), WithinAbs (0.0, 5.0));

    SECTION ("an undo keeps the shifts as they are")
    {
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { drumsId, true } } });
        run (f, SongIntent { intent::Undo {} });
        CHECK (f.trackNamed ("Bass").octaveShift == 1);
        CHECK (f.trackNamed ("Drums").octaveShift == -1);
    }

    SECTION ("reopening the Song restores every Track's shift")
    {
        reopenSong (f);
        showMode (f, Mode::edit);
        showMode (f, Mode::transcribe);
        CHECK (f.trackNamed ("Bass").octaveShift == 1);
        CHECK (f.trackNamed ("Drums").octaveShift == -1);
        CHECK (f.song().focusTrackId == bassId);
        CHECK (octaveButton (f, 1).getToggleState());
        CHECK (f.openSong->heard().octaveShiftedTrack == bassId);
        tickUntilOctaveRendered (f);
        CHECK_THAT (centsBetween (heardFrequency (f), 2.0 * sineFrequency), WithinAbs (0.0, 5.0));
    }
}

TEST_CASE ("set_octave_shift refuses a shift outside −1..+2 and an unknown Track, changing nothing", "[ui][transcribe][octave]")
{
    MainWindowFixture f { bassAndDrums() };
    const auto bassId = f.trackNamed ("Bass").id;

    for (const auto octaves : { -2, 3 })
    {
        const auto refused = runControl (f, SongIntent { intent::SetOctaveShift { bassId, octaves } });
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.octave_shift_out_of_range");
        CHECK (refused.error().fields.at ("octaves") == octaves);
    }
    const auto unknown = runControl (f, SongIntent { intent::SetOctaveShift { "track-gone", 1 } });
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == "song.track_not_found");
    CHECK (f.trackNamed ("Bass").octaveShift == 0);
}
