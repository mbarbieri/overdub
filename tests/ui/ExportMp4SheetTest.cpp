#include "ui/ExportSheet.h"

#include "export/MixRender.h"
#include "export/Mp4Decode.h"
#include "ui/ExportSheetSink.h"
#include "ui/MainWindowFixture.h"
#include "video/SyntheticMovie.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// The Export sheet's MP4 (issue #38, ADR 0005 UI amendment): the span bar's ranges for "trim to the
// Video Take" and "full song" (pure), choosing a Video Take and a length on the sheet (component),
// an MP4 exported from the window, and snapshots of the sheet trimmed and full-song.
using overdub::core::ScopedLogCapture;
using overdub::exporting::ExportFormat;
using overdub::exporting::ExportRequest;
using overdub::exporting::SampleRange;
using overdub::exporting::VideoLength;
using overdub::song::SampleCount;
using overdub::ui::ExportSheet;
using overdub::ui::ExportSheetState;
using overdub::ui::ExportSpanBarState;
using overdub::video::VideoTakeSpan;
using namespace overdub::ui::testing;

namespace
{
constexpr SampleCount rate = 48000;

// Pass 1's Video Take: 1 s to 5 s. Pass 2's: 3 s to 13 s, past the Clips' end at 12 s.
const VideoTakeSpan passOne { "video-a", "Pass 1", 1 * rate, 5 * rate, true };
const VideoTakeSpan passTwo { "video-b", "Pass 2", 3 * rate, 13 * rate, true };

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

struct ExportFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-export-mp4-ui-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};

// The window's fixture Song (a 2 s Backing Track) plus two Video Takes, as one undo step:
// Pass A's 4 s from 0.5 s (past the Clips' end), Pass B's 1 s from 0.5 s.
struct Mp4WindowFixture : MainWindowFixture
{
    overdub::song::Id firstTake;
    overdub::song::Id secondTake;

    Mp4WindowFixture()
    {
        auto changed = song();
        const auto root = openSong->package().location();
        firstTake = overdub::video::testing::addSyntheticVideoTake (changed, root, rate / 2, 120);
        secondTake = overdub::video::testing::addSyntheticVideoTake (changed, root, rate / 2, 30);
        REQUIRE (openSong->edit (changed, "Add Video Takes").has_value());
        window->tick();
    }

    ExportSheet& toMp4()
    {
        openExportSheet (*window);
        click (window->songView()->exportSheet.mp4Button);
        REQUIRE (window->songView()->exportSheet.format() == ExportFormat::mp4);
        return window->songView()->exportSheet;
    }
};

void tickUntilExported (MainWindowFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}
} // namespace

TEST_CASE ("the span bar shows the trimmed range as the Video Take's span", "[ui][export][mp4]")
{
    using overdub::ui::exportSpanBar;
    const auto bar = exportSpanBar (ExportFormat::mp4, VideoLength::trimToVideoTake, 12 * rate, passOne);
    CHECK (bar == ExportSpanBarState { .scaleEnd = 12 * rate, .exported = { 1 * rate, 5 * rate }, .videoTake = SampleRange { 1 * rate, 5 * rate }, .blackOutsideVideo = false });

    // A Video Take past the Clips' end widens the scale to its end.
    const auto past = exportSpanBar (ExportFormat::mp4, VideoLength::trimToVideoTake, 12 * rate, passTwo);
    CHECK (past.scaleEnd == 13 * rate);
    CHECK (past.exported == SampleRange { 3 * rate, 13 * rate });
}

TEST_CASE ("the span bar shows the full song from 0, black outside the Video Take", "[ui][export][mp4]")
{
    using overdub::ui::exportSpanBar;
    CHECK (exportSpanBar (ExportFormat::mp4, VideoLength::fullSong, 12 * rate, passOne)
           == ExportSpanBarState { .scaleEnd = 12 * rate, .exported = { 0, 12 * rate }, .videoTake = SampleRange { 1 * rate, 5 * rate }, .blackOutsideVideo = true });
    CHECK (exportSpanBar (ExportFormat::mp4, VideoLength::fullSong, 12 * rate, passTwo).exported == SampleRange { 0, 13 * rate });

    // WAV, M4A, and an MP4 without a Video Take: the whole mix, no video.
    const auto wholeMix = ExportSpanBarState { .scaleEnd = 12 * rate, .exported = { 0, 12 * rate } };
    CHECK (exportSpanBar (ExportFormat::wav, VideoLength::fullSong, 12 * rate, passOne) == wholeMix);
    CHECK (exportSpanBar (ExportFormat::m4a, VideoLength::trimToVideoTake, 12 * rate, passOne) == wholeMix);
    CHECK (exportSpanBar (ExportFormat::mp4, VideoLength::trimToVideoTake, 12 * rate, std::nullopt) == wholeMix);
}

TEST_CASE ("span bar ranges map to pixels over the bar's width", "[ui][export][mp4]")
{
    using overdub::ui::spanBarPixels;
    CHECK (spanBarPixels ({ 0, 12 * rate }, 12 * rate, 600) == juce::Range<int> (0, 600));
    CHECK (spanBarPixels ({ 1 * rate, 5 * rate }, 12 * rate, 600) == juce::Range<int> (50, 250));
    CHECK (spanBarPixels ({ 0, 0 }, 0, 600) == juce::Range<int> (0, 0));
}

TEST_CASE ("the span text names the Video Take and its range", "[ui][export][mp4]")
{
    using overdub::ui::movieSpanText;
    using overdub::ui::videoTakeChoiceText;
    CHECK (movieSpanText (VideoLength::trimToVideoTake, passOne, { 1 * rate, 5 * rate }, (int) rate) == "Trimmed to Pass 1: 0:01.0 \xe2\x80\x93 0:05.0");
    CHECK (movieSpanText (VideoLength::fullSong, passOne, { 0, 12 * rate }, (int) rate) == "Full song: 0:00.0 \xe2\x80\x93 0:12.0, black outside Pass 1");
    CHECK (videoTakeChoiceText (passTwo, (int) rate) == "Pass 2 \xc2\xb7 0:03.0 \xe2\x80\x93 0:13.0");
}

TEST_CASE ("the Video Take shown by default is the Song's stored choice, else the first", "[ui][export][mp4]")
{
    using overdub::ui::defaultExportVideoTake;
    CHECK (defaultExportVideoTake ({ passOne, passTwo }, std::string ("video-b")) == "video-b");
    CHECK (defaultExportVideoTake ({ passOne, passTwo }, std::nullopt) == "video-a");
    CHECK (defaultExportVideoTake ({ passOne, passTwo }, std::string ("gone")) == "video-a");
    CHECK (defaultExportVideoTake ({}, std::string ("video-b")) == std::nullopt);
}

TEST_CASE ("a length toggle the handler does not call back leaves the form as it was", "[ui][export][mp4]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportSheet sheet;
    layOut (sheet, windowWidth, 700);
    sheet.show ({ .clipsEnd = 12 * rate, .sampleRate = (int) rate, .videoTakes = { passOne, passTwo }, .storedVideoTakeId = std::string ("video-b") });
    sheet.chooseFormat (ExportFormat::mp4);
    REQUIRE (isShown (sheet.fullSongButton));
    std::vector<overdub::ui::Intent> refused;
    sheet.onIntent = [&refused] (const overdub::ui::Intent& intent) { refused.push_back (intent); };

    click (sheet.fullSongButton);
    CHECK (refused.size() == 1);
    CHECK (sheet.trimButton.getToggleState());
    CHECK_FALSE (sheet.fullSongButton.getToggleState());
    CHECK (sheet.spanBar.state().exported == SampleRange { 3 * rate, 13 * rate });
}

TEST_CASE ("choosing a Video Take and a length updates the span bar and the export request", "[ui][export][mp4]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportSheet sheet;
    layOut (sheet, windowWidth, 700);
    sheet.setDestination (juce::File ("/tmp/Song.wav"));
    ExportSheetSink sink;
    sink.connect (sheet);
    const auto& stored = sink.videoTakesChosen;
    const auto& requests = sink.requests;
    sheet.show ({ .clipsEnd = 12 * rate, .sampleRate = (int) rate, .videoTakes = { passOne, passTwo }, .storedVideoTakeId = std::string ("video-b") });

    // The Video Take controls show for MP4 only.
    CHECK (! isShown (sheet.videoTakePicker));
    CHECK (! isShown (sheet.trimButton));
    CHECK (sheet.spanBar.state().videoTake == std::nullopt);
    click (sheet.mp4Button);
    REQUIRE (isShown (sheet.videoTakePicker));
    REQUIRE (isShown (sheet.trimButton));
    REQUIRE (isShown (sheet.fullSongButton));
    CHECK (sheet.destination() == juce::File ("/tmp/Song.mp4"));

    // The Song's stored choice, trimmed by default.
    CHECK (sheet.videoTakePicker.getText() == juce::String::fromUTF8 ("Pass 2 \xc2\xb7 0:03.0 \xe2\x80\x93 0:13.0"));
    CHECK (sheet.trimButton.getToggleState());
    CHECK (sheet.spanBar.state().exported == SampleRange { 3 * rate, 13 * rate });
    CHECK (sheet.spanLabel.getText() == juce::String::fromUTF8 ("Trimmed to Pass 2: 0:03.0 \xe2\x80\x93 0:13.0"));

    choose (sheet.videoTakePicker, 0);
    CHECK (stored == std::vector<overdub::song::Id> { "video-a" });
    CHECK (sheet.spanBar.state().exported == SampleRange { 1 * rate, 5 * rate });
    CHECK (sheet.spanBar.state().videoTake == SampleRange { 1 * rate, 5 * rate });

    click (sheet.fullSongButton);
    CHECK (sheet.fullSongButton.getToggleState());
    CHECK (! sheet.trimButton.getToggleState());
    CHECK (sheet.spanBar.state().exported == SampleRange { 0, 12 * rate });
    CHECK (sheet.spanBar.state().blackOutsideVideo);
    CHECK (sheet.spanLabel.getText() == juce::String::fromUTF8 ("Full song: 0:00.0 \xe2\x80\x93 0:12.0, black outside Pass 1"));

    // The sheet's own choice survives a refresh that still carries the old stored one.
    sheet.show ({ .clipsEnd = 12 * rate, .sampleRate = (int) rate, .videoTakes = { passOne, passTwo }, .storedVideoTakeId = std::string ("video-b") });
    CHECK (sheet.spanBar.state().videoTake == SampleRange { 1 * rate, 5 * rate });

    click (sheet.exportButton);
    REQUIRE (requests.size() == 1);
    CHECK (requests[0] == ExportRequest { ExportFormat::mp4, juce::File ("/tmp/Song.mp4"), std::string ("video-a"), VideoLength::fullSong });

    // Back to WAV: the whole mix, and a request without a Video Take.
    click (sheet.wavButton);
    CHECK (! isShown (sheet.videoTakePicker));
    CHECK (sheet.spanBar.state().videoTake == std::nullopt);
    click (sheet.exportButton);
    REQUIRE (requests.size() == 2);
    CHECK (requests[1] == ExportRequest { ExportFormat::wav, juce::File ("/tmp/Song.wav") });
}

TEST_CASE ("an MP4 cannot be exported from a Song without Video Takes", "[ui][export][mp4]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportSheet sheet;
    layOut (sheet, windowWidth, 700);
    sheet.setDestination (juce::File ("/tmp/Song.wav"));
    ExportSheetSink sink;
    sink.connect (sheet);
    sheet.show ({ .clipsEnd = 12 * rate, .sampleRate = (int) rate });
    click (sheet.mp4Button);
    CHECK (isShown (sheet.noVideoTakesLabel));
    CHECK (! isShown (sheet.videoTakePicker));
    CHECK (! sheet.exportButton.isEnabled());

    MainWindowFixture f;
    ScopedLogCapture capture;
    const auto started = f.openSong->startExport ({ ExportFormat::mp4, juce::File ("/tmp/none.mp4"), std::string ("missing"), VideoLength::fullSong });
    REQUIRE_FALSE (started.has_value());
    CHECK (started.error().code == "ui.no_video_take");
    CHECK (! f.openSong->isExporting());
}

TEST_CASE ("the Export sheet exports an MP4 of the chosen Video Take and stores the choice in the Song", "[ui][export][mp4]")
{
    ExportFolder folder;
    Mp4WindowFixture f;
    auto& window = *f.window;
    auto& sheet = f.toMp4();
    sheet.setDestination (folder.root.getChildFile ("Band Mix video"));
    const auto canUndo = f.openSong->package().canUndo();

    // The first Video Take is shown by default; choosing the second stores it, with no undo step.
    CHECK (sheet.videoTakePicker.getSelectedItemIndex() == 0);
    choose (sheet.videoTakePicker, 1);
    CHECK (f.song().exportVideoTakeId == f.secondTake);
    CHECK (f.openSong->package().canUndo() == canUndo);
    CHECK (f.openSong->package().undoLabel() == "Add Video Takes");
    window.tick();
    CHECK (sheet.videoTakePicker.getSelectedItemIndex() == 1);

    ScopedLogCapture capture;
    click (sheet.exportButton);
    tickUntilExported (f);
    REQUIRE (! f.openSong->isExporting());
    CHECK (f.alertsShown.empty());

    const auto file = folder.root.getChildFile ("Band Mix video.mp4");
    REQUIRE (f.openSong->lastExportedFile() == file);
    REQUIRE (file.existsAsFile());
    // Trimmed to the second Video Take: 1 s of its 30 frames.
    CHECK (std::abs (overdub::exporting::testing::movieDurationSeconds (file) - 1.0) < 1.0 / 30.0);
    CHECK (overdub::exporting::testing::decodeVideoFrames (file).size() == 30);

    const auto requested = eventsNamed (capture, "ui.export_requested");
    REQUIRE (requested.size() == 1);
    CHECK (requested[0]["format"] == "mp4");
    CHECK (requested[0]["video_take_id"] == f.secondTake);
    CHECK (requested[0]["length"] == "trim");
    CHECK (eventsNamed (capture, "export.finished").size() == 1);
}

TEST_CASE ("a Song with a Video Take but no Clips exports an MP4 over silence, and WAV and M4A stay a Notice", "[ui][export][mp4]")
{
    ExportFolder folder;
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}.track ("Guitar", true, { 0 }).build() };
    auto& window = *f.window;
    auto changed = f.song();
    REQUIRE (overdub::exporting::lastClipEnd (changed) == 0);
    const auto videoTake = overdub::video::testing::addSyntheticVideoTake (changed, f.openSong->package().location(), rate / 2, 30);
    REQUIRE (f.openSong->edit (changed, "Add Video Take").has_value());
    window.tick();
    openExportSheet (window);
    auto& sheet = window.songView()->exportSheet;

    SECTION ("MP4: the Video Take over a silent mix")
    {
        click (sheet.mp4Button);
        sheet.setDestination (folder.root.getChildFile ("Video only"));
        CHECK (sheet.exportButton.isEnabled());

        ScopedLogCapture capture;
        click (sheet.exportButton);
        tickUntilExported (f);
        REQUIRE (! f.openSong->isExporting());
        CHECK (f.alertsShown.empty());
        CHECK (eventsNamed (capture, "export.nothing_to_export").empty());

        const auto file = folder.root.getChildFile ("Video only.mp4");
        REQUIRE (f.openSong->lastExportedFile() == file);
        REQUIRE (file.existsAsFile());
        // Trimmed to the Video Take: 1 s of its 30 frames, over 1 s of silence.
        CHECK (std::abs (overdub::exporting::testing::movieDurationSeconds (file) - 1.0) < 1.0 / 30.0);
        CHECK (overdub::exporting::testing::decodeVideoFrames (file).size() == 30);
        const auto audio = overdub::exporting::testing::decodeAudio (file, (int) rate);
        REQUIRE (audio.getNumSamples() > 0);
        CHECK (std::abs (audio.getNumSamples() - (int) rate) < (int) rate / 30);
        CHECK (audio.getMagnitude (0, audio.getNumSamples()) <= 1.0e-6f); // -120 dBFS, the null-test bound (ADR 0005)
        const auto requested = eventsNamed (capture, "ui.export_requested");
        REQUIRE (requested.size() == 1);
        CHECK (requested[0]["video_take_id"] == videoTake);
    }

    SECTION ("WAV and M4A: there is no mix to export")
    {
        for (auto* button : { &sheet.wavButton, &sheet.m4aButton })
        {
            openExportSheet (window);
            click (*button);
            const auto file = folder.root.getChildFile (button == &sheet.wavButton ? "Empty.wav" : "Empty.m4a");
            sheet.setDestination (file);
            CAPTURE (file.getFileName());

            ScopedLogCapture capture;
            click (sheet.exportButton);
            tickUntilExported (f);
            REQUIRE (! f.openSong->isExporting());
            CHECK (window.noticeLine.text() == juce::String ("The Song has no Clips to export."));
            const auto refused = eventsNamed (capture, "export.nothing_to_export");
            REQUIRE (refused.size() == 1);
            CHECK (refused[0]["lvl"] == "warn");
            CHECK (! file.exists());
        }
    }
}

TEST_CASE ("on a Song with no Clips, the WAV and M4A span text says there is nothing to export, and MP4 still describes the Video Take's range", "[ui][export][mp4][no_clips_span]")
{
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}.track ("Guitar", true, { 0 }).build() };
    auto& window = *f.window;
    auto changed = f.song();
    REQUIRE (overdub::exporting::lastClipEnd (changed) == 0);
    overdub::video::testing::addSyntheticVideoTake (changed, f.openSong->package().location(), rate / 2, 30);
    REQUIRE (f.openSong->edit (changed, "Add Video Take").has_value());
    window.tick();
    openExportSheet (window);
    auto& sheet = window.songView()->exportSheet;

    for (auto* button : { &sheet.wavButton, &sheet.m4aButton })
    {
        click (*button);
        CAPTURE (button->getButtonText());
        CHECK (sheet.spanLabel.getText() == juce::String ("Nothing to export: the Song has no Clips"));
        CHECK (! sheet.spanLabel.getText().contains ("effect tails"));
    }

    click (sheet.mp4Button);
    REQUIRE (sheet.trimButton.getToggleState());
    CHECK (sheet.spanLabel.getText().startsWith ("Trimmed to "));
    CHECK (sheet.spanLabel.getText().endsWith (juce::String::fromUTF8 (": 0:00.5 \xe2\x80\x93 0:01.5")));
    click (sheet.fullSongButton);
    CHECK (sheet.spanLabel.getText().startsWith (juce::String::fromUTF8 ("Full song: 0:00.0 \xe2\x80\x93 0:01.5, black outside ")));
}

TEST_CASE ("an MP4 of a Video Take that ends before the Song starts is a Notice that says so, not one about Clips", "[ui][export][mp4][outside_song]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    auto changed = f.song();
    REQUIRE (overdub::exporting::lastClipEnd (changed) > 0);
    // 1 s of frames from 2 s before Song sample 0: it ends 1 s before the Song starts.
    const auto videoTake = overdub::video::testing::addSyntheticVideoTake (changed, f.openSong->package().location(), -2 * rate, 30);
    REQUIRE (f.openSong->edit (changed, "Add Video Take").has_value());
    window.tick();
    openExportSheet (window);
    auto& sheet = window.songView()->exportSheet;
    click (sheet.mp4Button);
    REQUIRE (sheet.trimButton.getToggleState());
    const auto file = folder.root.getChildFile ("Before.mp4");
    sheet.setDestination (file);

    ScopedLogCapture capture;
    click (sheet.exportButton);
    tickUntilExported (f);
    REQUIRE (! f.openSong->isExporting());
    CHECK (f.alertsShown.empty());
    CHECK (window.noticeLine.text() == juce::String ("The Video Take ends before the Song starts, so there is nothing to export."));
    CHECK (eventsNamed (capture, "export.nothing_to_export").empty());
    const auto refused = eventsNamed (capture, "export.video_take_outside_song");
    REQUIRE (refused.size() == 1);
    CHECK (refused[0]["lvl"] == "warn");
    CHECK (refused[0]["video_take_id"] == videoTake);
    CHECK (refused[0]["video_take_end_sample"] == -rate);
    CHECK (! file.exists());
}

TEST_CASE ("snapshots of the Export sheet with MP4 trimmed and full-song", "[ui][snapshot]")
{
    const std::string slice = "slice-17";
    ExportFolder folder;
    Mp4WindowFixture f;
    auto& window = *f.window;
    window.exportFolder = folder.root;
    window.setOpenSong (f.openSong.get());
    auto& sheet = f.toMp4();
    choose (sheet.videoTakePicker, 1); // Pass 2's, which ends before the Clips do
    window.tick();
    REQUIRE (sheet.trimButton.getToggleState());
    CHECK (writeSnapshot (window, slice, "export-mp4-trimmed").existsAsFile());

    click (sheet.fullSongButton);
    REQUIRE (sheet.fullSongButton.getToggleState());
    CHECK (writeSnapshot (window, slice, "export-mp4-full-song").existsAsFile());
}
