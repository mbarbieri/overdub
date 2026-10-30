#include "ui/ExportSheet.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "ui/MainWindowFixture.h"
#include "video/SyntheticMovie.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// Export can write a Loop's range (issue #235, ADR 0010): the Export sheet's length offers the
// Whole Song and "Loop: <name>" per Loop for WAV and MP4 alike, set_export_length takes a Loop id,
// the choice is saved outside history and goes back to the whole Song when that Loop is deleted.
// Exported from Transcribe mode, a Loop's file starts with the impulse at the Loop's start, is as
// long as the Loop to the sample, and hears neither the Transcribe Mix nor Practice Speed.
using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using core::ScopedLogCapture;
using exporting::ExportFormat;
using exporting::SampleRange;
using exporting::VideoLength;

namespace
{
constexpr int rate = 48000;
constexpr song::SampleCount impulseAt = 30000;
constexpr float impulseLevel = 0.5f;
constexpr song::SampleCount loopLength = 24007;

// One Backing Track "Click": 2 s of silence with one impulse at `impulseAt`.
std::unique_ptr<song::testing::FixtureSong> clickSong()
{
    auto fixture = song::testing::FixtureSongBuilder {}.build();
    juce::AudioBuffer<float> audio (1, 2 * rate);
    audio.clear();
    audio.setSample (0, (int) impulseAt, impulseLevel);
    const auto file = fixture->folder.getChildFile ("sources/Click.wav");
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    REQUIRE (song::writeFloatWavAtomically (file, audio, rate, {}).has_value());
    REQUIRE (fixture->package->importBackingTrack (file).has_value());
    return fixture;
}

void run (MainWindowFixture& f, SongIntent intent)
{
    REQUIRE (f.window->run (std::move (intent), Origin::control).has_value());
}

void showMode (MainWindowFixture& f, Mode mode)
{
    REQUIRE (f.window->run (WindowIntent { intent::ShowMode { mode } }, Origin::control).has_value());
}

song::Id addLoop (MainWindowFixture& f, song::SampleCount start, song::SampleCount end)
{
    const auto added = f.window->run (SongIntent { intent::AddLoop { start, end } }, Origin::control);
    REQUIRE (added.has_value());
    REQUIRE (added->createdId.has_value());
    return *added->createdId;
}

juce::ToggleButton& loopButtonNamed (ExportSheet& sheet, const std::string& text)
{
    for (auto& button : sheet.loopButtons)
        if (button->getButtonText().toStdString() == text)
            return *button;
    FAIL ("no length choice " << text);
    return *sheet.loopButtons.front();
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
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-export-loop-" + juce::Uuid().toString());

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

const song::Track& clickTrack (MainWindowFixture& f)
{
    REQUIRE (f.song().tracks.size() == 1);
    return f.song().tracks.front();
}
} // namespace

TEST_CASE ("the span bar and its text show a Loop's range, an MP4's black around the Video Take", "[ui][export][loop]")
{
    const song::Loop solo { .id = "loop-1", .name = "Solo", .start = 3 * rate, .end = 6 * rate };
    const video::VideoTakeSpan passOne { "video-a", "Pass 1", 1 * rate, 5 * rate, true };
    const auto range = SampleRange { solo.start, solo.end };

    CHECK (exportSpanBar (ExportFormat::wav, VideoLength::trimToVideoTake, 4 * rate, std::nullopt, range) == ExportSpanBarState { .scaleEnd = 6 * rate, .exported = range });
    CHECK (exportSpanBar (ExportFormat::mp4, VideoLength::trimToVideoTake, 12 * rate, passOne, range)
           == ExportSpanBarState { .scaleEnd = 12 * rate, .exported = range, .videoTake = SampleRange { 1 * rate, 5 * rate }, .blackOutsideVideo = true });
    CHECK (loopSpanText (solo, std::nullopt, rate) == "Loop: Solo: 0:03.0 \xe2\x80\x93 0:06.0");
    CHECK (loopSpanText (solo, passOne, rate) == "Loop: Solo: 0:03.0 \xe2\x80\x93 0:06.0, black outside Pass 1");
}

TEST_CASE ("the Export sheet's length offers the Whole Song and each Loop for WAV and MP4, saved outside history and after reopening", "[ui][export][loop][choices]")
{
    MainWindowFixture f { clickSong() };
    showMode (f, Mode::transcribe);
    const auto verse = addLoop (f, 0, rate / 2);
    const auto solo = addLoop (f, impulseAt, impulseAt + loopLength);
    run (f, SongIntent { intent::Edit { intent::RenameLoop { solo, "Solo" } } });
    auto& sheet = openExportSheet (*f.window);

    // WAV: Whole Song, then each Loop in time order.
    REQUIRE (sheet.wavButton.getToggleState());
    CHECK (isShown (sheet.lengthHeading));
    CHECK (isShown (sheet.wholeSongButton));
    CHECK (sheet.wholeSongButton.getToggleState());
    CHECK_FALSE (isShown (sheet.trimButton));
    REQUIRE (sheet.loopButtons.size() == 2);
    CHECK (sheet.loopButtons[0]->getButtonText() == "Loop: Loop 1");
    CHECK (sheet.loopButtons[1]->getButtonText() == "Loop: Solo");
    CHECK (isShown (*sheet.loopButtons[1]));

    const auto undoLabel = f.openSong->package().undoLabel();
    const ScopedLogCapture capture;
    click (loopButtonNamed (sheet, "Loop: Solo"));
    CHECK (f.song().settings.exportLoopId == solo);
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    CHECK (std::ranges::any_of (capture.events(), [] (const nlohmann::json& event) { return event.at ("event") == "ui.intent_run" && event.at ("op") == "set_export_length"; }));
    CHECK (sheet.loopButtons[1]->getToggleState());
    CHECK_FALSE (sheet.wholeSongButton.getToggleState());
    REQUIRE (sheet.request().loop.has_value());
    CHECK (sheet.request().loop->range == SampleRange { impulseAt, impulseAt + loopLength });
    CHECK (sheet.spanBar.state().exported == SampleRange { impulseAt, impulseAt + loopLength });
    CHECK (sheet.spanLabel.getText().startsWith ("Loop: Solo: 0:00.6"));

    // An MP4 offers the same Loops after its own two lengths; the stored MP4 length is kept.
    auto changed = f.song();
    video::testing::addSyntheticVideoTake (changed, f.openSong->package().location(), rate / 2, 30);
    REQUIRE (f.openSong->edit (changed, "Add Video Take").has_value());
    click (sheet.mp4Button);
    CHECK (isShown (sheet.trimButton));
    CHECK (isShown (sheet.fullSongButton));
    CHECK_FALSE (isShown (sheet.wholeSongButton));
    CHECK_FALSE (sheet.trimButton.getToggleState());
    CHECK (sheet.loopButtons[1]->getToggleState());
    CHECK (sheet.spanBar.state().blackOutsideVideo);
    click (loopButtonNamed (sheet, "Loop: Loop 1"));
    CHECK (f.song().settings.exportLoopId == verse);
    click (sheet.fullSongButton);
    CHECK_FALSE (f.song().settings.exportLoopId.has_value());
    CHECK (f.song().settings.exportLength == VideoLength::fullSong);
    click (loopButtonNamed (sheet, "Loop: Solo"));
    CHECK (f.song().settings.exportLength == VideoLength::fullSong);

    reopenSong (f);
    CHECK (f.song().settings.exportLoopId == solo);
    auto& reopened = openExportSheet (*f.window);
    CHECK (loopButtonNamed (reopened, "Loop: Solo").getToggleState());

    // The Whole Song (WAV) goes back to the whole Song and keeps the MP4's length.
    click (reopened.wavButton);
    click (reopened.wholeSongButton);
    CHECK_FALSE (f.song().settings.exportLoopId.has_value());
    CHECK (f.song().settings.exportLength == VideoLength::fullSong);
    CHECK (reopened.spanBar.state().exported == SampleRange { 0, exporting::lastClipEnd (f.song()) });
}

TEST_CASE ("a Song without Loops shows no length choice for WAV", "[ui][export][loop][choices]")
{
    MainWindowFixture f { clickSong() };
    auto& sheet = openExportSheet (*f.window);
    CHECK_FALSE (isShown (sheet.lengthHeading));
    CHECK_FALSE (isShown (sheet.wholeSongButton));
    CHECK (sheet.loopButtons.empty());
}

TEST_CASE ("set_export_length takes a Loop id; one the Song does not have is refused and changes nothing", "[ui][export][loop]")
{
    MainWindowFixture f { clickSong() };
    showMode (f, Mode::transcribe);
    const auto solo = addLoop (f, impulseAt, impulseAt + loopLength);

    run (f, SongIntent { intent::SetExportLength { VideoLength::fullSong, solo } });
    CHECK (f.song().settings.exportLoopId == solo);
    CHECK (f.song().settings.exportLength == VideoLength::fullSong);

    const auto refused = f.window->run (SongIntent { intent::SetExportLength { VideoLength::trimToVideoTake, "loop-gone" } }, Origin::control);
    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == "song.loop_not_found");
    CHECK (f.song().settings.exportLoopId == solo);
    CHECK (f.song().settings.exportLength == VideoLength::fullSong);

    // Without loop_id: the whole Song.
    run (f, SongIntent { intent::SetExportLength { VideoLength::fullSong } });
    CHECK_FALSE (f.song().settings.exportLoopId.has_value());
}

TEST_CASE ("deleting the chosen Loop sends the length back to the whole Song, and undoing the delete keeps it there", "[ui][export][loop]")
{
    MainWindowFixture f { clickSong() };
    showMode (f, Mode::transcribe);
    const auto solo = addLoop (f, impulseAt, impulseAt + loopLength);
    run (f, SongIntent { intent::SetExportLength { VideoLength::trimToVideoTake, solo } });
    auto& sheet = openExportSheet (*f.window);
    REQUIRE (sheet.request().loop.has_value());

    run (f, SongIntent { intent::Edit { intent::DeleteLoop { solo } } });
    CHECK_FALSE (f.song().settings.exportLoopId.has_value());
    CHECK_FALSE (sheet.request().loop.has_value());
    CHECK (sheet.loopButtons.empty());
    CHECK (sheet.spanBar.state().exported == SampleRange { 0, exporting::lastClipEnd (f.song()) });

    run (f, SongIntent { intent::Undo {} });
    REQUIRE (f.song().loops.size() == 1);
    CHECK_FALSE (f.song().settings.exportLoopId.has_value());
    CHECK (sheet.wholeSongButton.getToggleState());
}

TEST_CASE ("a Loop exported from Transcribe mode starts with the impulse at its start, is the Loop's length to the sample, and ignores the Transcribe Mix and Practice Speed", "[ui][export][loop][transcribe]")
{
    ExportFolder folder;
    MainWindowFixture f { clickSong() };
    showMode (f, Mode::transcribe);
    const auto solo = addLoop (f, impulseAt, impulseAt + loopLength);
    const auto trackId = clickTrack (f).id;
    // The Transcribe Mix mutes the only Track and the Loop is drilled at half speed.
    run (f, SongIntent { intent::SetTranscribeMute { trackId, true } });
    run (f, SongIntent { intent::SelectLoop { solo } });
    run (f, SongIntent { intent::SetLooping { true } });
    run (f, SongIntent { intent::SetPracticeSpeed { 50 } });
    REQUIRE (f.song().loops.front().settings.practiceSpeedPercent == 50);
    REQUIRE (clickTrack (f).settings.transcribeMute);

    run (f, SongIntent { intent::SetExportLength { VideoLength::trimToVideoTake, solo } });
    const auto destination = folder.root.getChildFile ("Solo.wav");
    setExportDestination (*f.window, destination);
    const ScopedLogCapture capture;
    run (f, SongIntent { intent::Export {} });
    tickUntilExported (f);
    REQUIRE (f.openSong->lastExportedFile() == destination);

    const auto decoded = song::decodeAudioFile (destination, rate);
    REQUIRE (decoded.has_value());
    CHECK (decoded->audio.getNumSamples() == loopLength);
    const auto centrePan = (float) std::sqrt (0.5);
    for (int channel = 0; channel < 2; ++channel)
    {
        CAPTURE (channel);
        CHECK (std::abs (decoded->audio.getSample (channel, 0) - impulseLevel * centrePan) < 1.0e-5f);
        CHECK (decoded->audio.getMagnitude (channel, 1, decoded->audio.getNumSamples() - 1) < 1.0e-6f);
    }
    CHECK (std::ranges::any_of (capture.events(), [&] (const nlohmann::json& event) {
        return event.at ("event") == "export.started" && event.at ("loop_id") == solo && event.at ("start_sample") == impulseAt && event.at ("samples") == loopLength;
    }));
}
