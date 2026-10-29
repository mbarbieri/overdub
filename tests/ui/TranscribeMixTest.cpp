#include "ui/TranscribePage.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using core::ScopedLogCapture;

// The Focus Track and the Transcribe Mix (issue #226, ADR 0010): the Transcribe column's Track list
// (◎, M, S), what the engine plays in each mode and Export, what the waveform draws, and that both
// are saved with the Song outside history. Driven through MainComponent::run and clicks on the
// fixture Song (a 2 s Backing Track "Band Mix", a 0.25 sine, and the empty Tracks Guitar and Vocals).
namespace
{
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

TranscribeColumn& showTranscribe (MainWindowFixture& f)
{
    showMode (f, Mode::transcribe);
    return f.window->songView()->transcribePage.column;
}

TranscribeTrackRow& rowNamed (MainWindowFixture& f, const std::string& name)
{
    const auto& trackId = f.trackNamed (name).id;
    for (auto& row : f.window->songView()->transcribePage.column.trackRows)
        if (row->trackId() == trackId)
            return *row;
    FAIL ("no Transcribe row for Track " << name);
    return *f.window->songView()->transcribePage.column.trackRows.front();
}

// The peak of output channel 0 over the last `samples` samples the device rendered.
float recentPeak (const engine::testing::LoopbackDevice& device, std::size_t samples)
{
    const auto& output = device.outputHistory[0];
    REQUIRE (output.size() >= samples);
    float peak = 0.0f;
    for (auto i = output.size() - samples; i < output.size(); ++i)
        peak = std::max (peak, std::abs (output[i]));
    return peak;
}

// Plays from the start for long enough that the plan's latency and the mix's ramp have passed,
// then returns the peak of the last 4096 samples (the 0.25 sine plays for 2 s from 0).
float heardPeakFromStart (MainWindowFixture& f)
{
    run (f, SongIntent { intent::Seek { 0 } });
    if (! f.openSong->isPlaying())
        run (f, SongIntent { intent::PlayOrStop {} });
    f.runBlocks (24);
    return recentPeak (f.device, 4096);
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
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-transcribe-mix-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};

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

float filePeak (const juce::File& file, int sampleRate)
{
    const auto decoded = song::decodeAudioFile (file, sampleRate);
    REQUIRE (decoded.has_value());
    return decoded->audio.getMagnitude (0, 0, decoded->audio.getNumSamples());
}
} // namespace

TEST_CASE ("the Transcribe column lists every Track in Song order with its name, ◎, M and S", "[ui][transcribe][mix]")
{
    MainWindowFixture f;
    auto& column = showTranscribe (f);

    REQUIRE (column.trackRows.size() == f.song().tracks.size());
    for (std::size_t i = 0; i < column.trackRows.size(); ++i)
    {
        CHECK (column.trackRows[i]->trackId() == f.song().tracks[i].id);
        CHECK (isShown (*column.trackRows[i]));
        CHECK (column.trackRows[i]->focusButton.getButtonText() == juce::String::fromUTF8 ("\xe2\x97\x8e"));
        CHECK (column.trackRows[i]->muteButton.getButtonText() == "M");
        CHECK (column.trackRows[i]->soloButton.getButtonText() == "S");
    }
    CHECK (isShown (column.mixCaption));

    SECTION ("a new Track joins the list unmuted and unsoloed")
    {
        run (f, SongIntent { intent::AddTrack {} });
        REQUIRE (column.trackRows.size() == f.song().tracks.size());
        const auto& added = f.song().tracks.back();
        CHECK (column.trackRows.back()->trackId() == added.id);
        CHECK_FALSE (added.transcribeMute);
        CHECK_FALSE (added.transcribeSolo);
        CHECK_FALSE (column.trackRows.back()->muteButton.getToggleState());
        CHECK_FALSE (column.trackRows.back()->soloButton.getToggleState());
    }
}

TEST_CASE ("◎ picks the Focus Track, soloing it alone in the Transcribe Mix; ◎ again clears it and leaves the mix; none is an undo step", "[ui][transcribe][mix][focus]")
{
    MainWindowFixture f;
    showTranscribe (f);
    const auto undoLabel = f.openSong->package().undoLabel();
    const auto canUndo = f.openSong->package().canUndo();
    run (f, SongIntent { intent::SetTranscribeSolo { f.trackNamed ("Band Mix").id, true } });
    run (f, SongIntent { intent::SetTranscribeMute { f.trackNamed ("Vocals").id, true } });
    const ScopedLogCapture capture;

    click (rowNamed (f, "Guitar").focusButton);

    CHECK (f.song().focusTrackId == f.trackNamed ("Guitar").id);
    CHECK (f.trackNamed ("Guitar").transcribeSolo);
    CHECK_FALSE (f.trackNamed ("Band Mix").transcribeSolo);
    CHECK (f.trackNamed ("Vocals").transcribeMute);
    CHECK (rowNamed (f, "Guitar").focusButton.getToggleState());
    CHECK (rowNamed (f, "Guitar").soloButton.getToggleState());
    CHECK_FALSE (rowNamed (f, "Band Mix").soloButton.getToggleState());
    CHECK (std::ranges::any_of (capture.events(), [] (const nlohmann::json& event) { return event.at ("event") == "ui.intent_run" && event.at ("op") == "set_focus_track"; }));

    click (rowNamed (f, "Guitar").focusButton);

    CHECK_FALSE (f.song().focusTrackId.has_value());
    CHECK (f.trackNamed ("Guitar").transcribeSolo);
    CHECK (f.trackNamed ("Vocals").transcribeMute);
    CHECK_FALSE (rowNamed (f, "Guitar").focusButton.getToggleState());

    click (rowNamed (f, "Band Mix").muteButton);
    click (rowNamed (f, "Guitar").soloButton);
    CHECK (f.trackNamed ("Band Mix").transcribeMute);
    CHECK_FALSE (f.trackNamed ("Guitar").transcribeSolo);
    CHECK (rowNamed (f, "Band Mix").muteButton.getToggleState());

    // The Song's own mix and its history never change.
    for (const auto& track : f.song().tracks)
    {
        CHECK_FALSE (track.mute);
        CHECK_FALSE (track.solo);
    }
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    CHECK (f.openSong->package().canUndo() == canUndo);
}

TEST_CASE ("in Transcribe mode a Track muted in the Transcribe Mix is silent while the Song's mute is off; Edit mode and Export still hear it", "[ui][transcribe][mix][engine]")
{
    ExportFolder folder;
    MainWindowFixture f;
    const auto bandId = f.trackNamed ("Band Mix").id;
    REQUIRE (heardPeakFromStart (f) == Catch::Approx (0.25f * std::sqrt (0.5f)).margin (0.02f));

    showTranscribe (f);
    run (f, SongIntent { intent::SetTranscribeMute { bandId, true } });

    CHECK (f.openSong->isTranscribing());
    CHECK_FALSE (f.trackNamed ("Band Mix").mute);
    CHECK (heardPeakFromStart (f) == 0.0f);

    showMode (f, Mode::edit);
    CHECK_FALSE (f.openSong->isTranscribing());
    CHECK (heardPeakFromStart (f) > 0.15f);

    showMode (f, Mode::record);
    CHECK (heardPeakFromStart (f) > 0.15f);

    SECTION ("the Song's own mute is not heard in Transcribe mode, and is in Record mode")
    {
        run (f, SongIntent { intent::SetTranscribeMute { bandId, false } });
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { bandId, true } } });
        CHECK (heardPeakFromStart (f) == 0.0f);
        showMode (f, Mode::transcribe);
        CHECK (heardPeakFromStart (f) > 0.15f);
    }

    SECTION ("an export from Transcribe mode is the Song's own mix")
    {
        showMode (f, Mode::transcribe);
        run (f, SongIntent { intent::PlayOrStop {} });
        f.runBlocks (1);
        const auto destination = folder.root.getChildFile ("Song.wav");
        setExportDestination (*f.window, destination);
        run (f, SongIntent { intent::Export {} });
        tickUntilExported (f);
        CHECK (filePeak (destination, f.song().sampleRate) == Catch::Approx (0.25f * std::sqrt (0.5f)).margin (0.02f));
        // And Transcribe mode hears the Transcribe Mix again once the export is done.
        CHECK (heardPeakFromStart (f) == 0.0f);
    }
}

TEST_CASE ("a soloed Focus Track is all Transcribe mode hears", "[ui][transcribe][mix][engine]")
{
    MainWindowFixture f;
    showTranscribe (f);

    run (f, SongIntent { intent::SetFocusTrack { f.trackNamed ("Guitar").id } });
    CHECK (heardPeakFromStart (f) == 0.0f);

    run (f, SongIntent { intent::SetFocusTrack { f.trackNamed ("Band Mix").id } });
    CHECK (heardPeakFromStart (f) > 0.15f);
}

TEST_CASE ("the waveform shows the Focus Track under its name, and with no Focus Track the heard Transcribe Mix", "[ui][transcribe][mix][page]")
{
    MainWindowFixture f;
    showTranscribe (f);
    auto& page = f.window->songView()->transcribePage;
    const auto bandId = f.trackNamed ("Band Mix").id;
    const auto sine = [&page] { return page.waveformColumns().at (10).maximum; };

    CHECK (page.wave.label() == "Transcribe Mix");
    CHECK (sine() == Catch::Approx (0.25f).margin (0.01f));

    run (f, SongIntent { intent::SetTranscribeMute { bandId, true } });
    CHECK (sine() == Catch::Approx (0.0f));
    run (f, SongIntent { intent::Edit { intent::SetTrackMute { bandId, true } } });
    run (f, SongIntent { intent::SetTranscribeMute { bandId, false } });
    CHECK (sine() == Catch::Approx (0.25f).margin (0.01f)); // the Song's mute is not drawn

    run (f, SongIntent { intent::SetFocusTrack { f.trackNamed ("Guitar").id } });
    CHECK (page.wave.label() == "Guitar");
    CHECK (std::ranges::all_of (page.waveformColumns(), [] (const song::PeakRange& column) { return column.maximum == 0.0f && column.minimum == 0.0f; }));

    run (f, SongIntent { intent::SetFocusTrack { bandId } });
    run (f, SongIntent { intent::SetTranscribeMute { bandId, true } });
    CHECK (page.wave.label() == "Band Mix");
    CHECK (sine() == Catch::Approx (0.25f).margin (0.01f)); // the Focus Track, whatever its mute

    run (f, SongIntent { intent::SetFocusTrack { std::nullopt } });
    CHECK (page.wave.label() == "Transcribe Mix");
    CHECK (sine() == Catch::Approx (0.0f));
}

TEST_CASE ("reopening the Song restores the Focus Track and the Transcribe Mix", "[ui][transcribe][mix][package]")
{
    MainWindowFixture f;
    showTranscribe (f);
    const auto guitarId = f.trackNamed ("Guitar").id;
    const auto bandId = f.trackNamed ("Band Mix").id;
    run (f, SongIntent { intent::SetFocusTrack { guitarId } });
    run (f, SongIntent { intent::SetTranscribeMute { bandId, true } });
    run (f, SongIntent { intent::SetTranscribeSolo { bandId, true } });

    reopenSong (f);

    CHECK (f.song().focusTrackId == guitarId);
    CHECK (f.trackNamed ("Guitar").transcribeSolo);
    CHECK (f.trackNamed ("Band Mix").transcribeMute);
    CHECK (f.trackNamed ("Band Mix").transcribeSolo);
    CHECK_FALSE (f.trackNamed ("Vocals").transcribeSolo);
    CHECK (f.openSong->isTranscribing()); // the window is still in Transcribe mode
    CHECK (rowNamed (f, "Guitar").focusButton.getToggleState());
    CHECK (rowNamed (f, "Band Mix").muteButton.getToggleState());
    CHECK (f.window->songView()->transcribePage.wave.label() == "Guitar");
}

TEST_CASE ("deleting the Focus Track clears it, and undo brings the Track back without its focus", "[ui][transcribe][mix][focus]")
{
    MainWindowFixture f;
    showTranscribe (f);
    const auto guitarId = f.trackNamed ("Guitar").id;
    run (f, SongIntent { intent::SetFocusTrack { guitarId } });

    run (f, SongIntent { intent::DeleteTrack { guitarId } });

    CHECK_FALSE (f.song().focusTrackId.has_value());
    CHECK (f.window->songView()->transcribePage.wave.label() == "Transcribe Mix");
    CHECK (f.window->songView()->transcribePage.column.trackRows.size() == f.song().tracks.size());

    run (f, SongIntent { intent::Undo {} });
    REQUIRE (song::findTrack (f.song(), guitarId) != nullptr);
    CHECK_FALSE (f.song().focusTrackId.has_value());
}
