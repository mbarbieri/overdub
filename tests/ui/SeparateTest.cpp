#include "ui/MainComponent.h"

#include "core/LogEvents.h"
#include "export/MixRender.h"
#include "separation/OnnxTestModel.h"
#include "song/BackingTrackImport.h"
#include "song/FakeSeparationHelper.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// Separate... on a Track (issue #114) through the real window, with a fake helper that writes six
// known sines (ADR 0007: no test runs the real model): the modal dialog and its progress, the Song
// change, the undo step, the persisted origin, the logs, and each new Backing Track playing its
// sine (a null test, ADR 0005).
using overdub::core::ScopedLogCapture;
using overdub::core::testing::firstEvent;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using namespace overdub::song::testing;
namespace song = overdub::song;

namespace
{
// The names themselves are specified in tests/song/SeparationTest.cpp.
std::vector<std::string> songTrackNamesAfter()
{
    std::vector<std::string> names { "Band Mix" };
    for (const auto& part : song::separationParts)
        names.emplace_back (part.trackName);
    names.emplace_back ("Guitar");
    return names;
}

const std::string bassTrackName { song::separationParts.front().trackName };

// Band Mix: 2 s at 44.1 kHz, the helper's rate, so each part's derived WAV is its sine unchanged.
std::unique_ptr<FixtureSong> bandMixSong()
{
    return FixtureSongBuilder {}.sampleRate (helperSampleRate).backingTrack ("Band Mix", 2.0, 220.0).track ("Guitar", true, { 0 }).build();
}

struct SeparateFixture
{
    MainWindowFixture f { bandMixSong() };
    std::shared_ptr<FakeHelperLog> log = std::make_shared<FakeHelperLog>();
    std::vector<std::vector<TrackMenuItem>> menusShown;
    std::optional<std::size_t> menuPick = 0; // what the user picks in the next Track menu

    explicit SeparateFixture (FakeHelperScript script = {})
    {
        f.openSong->setSeparationTools (song::SeparationTools {
            .model = writeStandInModel (f.fixture->folder),
            .startHelper = fakeSeparationHelper (script, log),
            .workFolder = f.fixture->folder.getChildFile ("work"),
        });
        f.window->chooseTrackMenuItem = [this] (const std::vector<TrackMenuItem>& items, std::function<void (std::optional<std::size_t>)> done) {
            menusShown.push_back (items);
            done (menuPick);
        };
    }

    // A right-click on the Track's row, which opens its menu.
    void rightClickRow (const std::string& trackName)
    {
        auto& row = f.rowNamed (trackName);
        row.mouseDown (mouseEventAt (row, row.getLocalBounds().getCentre().toFloat(), juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier)));
    }

    // Separate... on the Track, then window polls until the dialog is gone.
    void separate (const std::string& trackName)
    {
        rightClickRow (trackName);
        REQUIRE (f.openSong->isSeparating());
        for (int poll = 0; poll < 20 && f.openSong->isSeparating(); ++poll)
            f.window->tick();
        REQUIRE_FALSE (f.openSong->isSeparating());
    }

    std::vector<std::string> trackNames() const
    {
        std::vector<std::string> names;
        for (const auto& track : f.song().tracks)
            names.push_back (track.name);
        return names;
    }

    const song::BackingTrack& backingTrackOf (const song::Track& track) const
    {
        REQUIRE_FALSE (track.clips.empty());
        const auto& backingTracks = f.song().backingTracks;
        const auto found = std::ranges::find (backingTracks, track.clips.front().source.id, &song::BackingTrack::id);
        REQUIRE (found != backingTracks.end());
        return *found;
    }
};

// The Song's mix with only this Track soloed, rendered offline through the engine's graph.
std::vector<std::vector<float>> renderSoloed (const song::Song& songToRender, const song::Id& trackId, const juce::File& packageRoot)
{
    auto soloed = songToRender;
    for (auto& track : soloed.tracks)
        track.solo = track.id == trackId;
    overdub::exporting::MixRender render { soloed, packageRoot, 256, {} };
    std::vector<std::vector<float>> out (2);
    std::vector<std::vector<float>> block (2, std::vector<float> (256));
    float* pointers[] = { block[0].data(), block[1].data() };
    while (const auto count = render.renderNext (pointers, 256))
        for (std::size_t c = 0; c < 2; ++c)
            out[c].insert (out[c].end(), block[c].begin(), block[c].begin() + count);
    return out;
}

} // namespace

TEST_CASE ("Separate... on a Backing Track's row opens a modal dialog whose progress bar follows the helper", "[ui][separation]")
{
    SeparateFixture s;
    auto& dialog = s.f.window->runDialog;
    CHECK_FALSE (isShown (dialog));

    s.menuPick = std::nullopt; // dismissed
    s.rightClickRow ("Guitar");
    s.rightClickRow ("Band Mix");
    REQUIRE (s.menusShown.size() == 2);
    CHECK (s.menusShown[0] == std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", false, "This Track has no Clips" } });
    CHECK (s.menusShown[1] == std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", true, "" } });
    CHECK_FALSE (s.f.openSong->isSeparating());

    s.menuPick = 0;
    s.rightClickRow ("Band Mix");
    REQUIRE (s.f.openSong->isSeparating());
    CHECK (isShown (dialog));
    CHECK (dialog.getBounds() == s.f.window->getLocalBounds()); // covers the window
    CHECK (dialog.title.getText() == "Separating Band Mix.wav");
    CHECK (dialog.statusLabel.getText() == juce::String::fromUTF8 ("Starting\xe2\x80\xa6"));

    s.f.window->tick(); // separation.started: 3 chunks
    CHECK (dialog.statusLabel.getText() == juce::String::fromUTF8 ("Chunk 0 of 3 \xc2\xb7 0%"));
    s.f.window->tick(); // chunk 1
    CHECK (dialog.progress() > 0.33);
    CHECK (dialog.progress() < 0.34);
    CHECK (dialog.statusLabel.getText() == juce::String::fromUTF8 ("Chunk 1 of 3 \xc2\xb7 33%"));

    // Modal: keys and commands do nothing, so nothing plays, records or changes.
    const auto before = s.f.song();
    CHECK (pressKey (*s.f.window, ' '));
    CHECK (pressKey (*s.f.window, 'r'));
    CHECK (pressKey (*s.f.window, 'z', juce::ModifierKeys (juce::ModifierKeys::commandModifier)));
    s.f.window->perform (Command::playOrStop);
    s.f.window->perform (Command::undo);
    CHECK_FALSE (s.f.openSong->isPlaying());
    CHECK_FALSE (s.f.openSong->isRecording());
    CHECK (s.f.song() == before);
    CHECK (s.f.openSong->record().error().code == "ui.separation_running");

    for (int poll = 0; poll < 5 && s.f.openSong->isSeparating(); ++poll)
        s.f.window->tick();
    CHECK_FALSE (s.f.openSong->isSeparating());
    CHECK_FALSE (isShown (dialog));
}

TEST_CASE ("while a Separation runs, arm, input, pre-roll, Monitoring, the camera and Mixer previews are refused", "[ui][separation]")
{
    SeparateFixture s;
    const auto& guitarId = s.f.trackNamed ("Guitar").id;
    s.rightClickRow ("Band Mix");
    REQUIRE (s.f.openSong->isSeparating());
    const auto before = s.f.song();

    auto quieter = before;
    quieter.master.volumeDb = -60.0;
    CHECK (s.f.openSong->setArmed (guitarId, false).error().code == "ui.separation_running");
    CHECK (s.f.openSong->setInputChannels (guitarId, { 1 }).error().code == "ui.separation_running");
    CHECK (s.f.openSong->setPreRoll (before.preRoll == song::PreRoll::off ? song::PreRoll::oneSecond : song::PreRoll::off).error().code == "ui.separation_running");
    CHECK (s.f.openSong->setMonitoring (guitarId, true).error().code == "ui.separation_running");
    CHECK (s.f.openSong->previewMix (quieter).error().code == "ui.separation_running");
    CHECK (s.f.openSong->setCameraArmed (true).error().code == "ui.separation_running");

    CHECK (s.f.song() == before);
    CHECK_FALSE (s.f.openSong->isMonitoring (guitarId));
    CHECK (s.f.openSong->cameraPreviewLayer() == nullptr);

    for (int poll = 0; poll < 20 && s.f.openSong->isSeparating(); ++poll)
        s.f.window->tick();
    REQUIRE_FALSE (s.f.openSong->isSeparating());
}

TEST_CASE ("a finished Separation adds six Backing Tracks on six Tracks below the source Track, which is muted", "[ui][separation]")
{
    SeparateFixture s;
    const auto source = s.f.trackNamed ("Band Mix");
    const auto sourceBackingTrack = s.backingTrackOf (source);

    s.separate ("Band Mix");

    CHECK (s.trackNames() == songTrackNamesAfter());
    CHECK (s.f.song().backingTracks.size() == 7);
    CHECK (s.f.openSong->package().undoLabel() == "Separate Band Mix.wav");
    CHECK (s.f.alertsShown.empty());

    auto sourceAfter = s.f.trackNamed ("Band Mix");
    CHECK (sourceAfter.mute);
    sourceAfter.mute = false;
    CHECK (sourceAfter == source);

    for (std::size_t i = 1; i <= 6; ++i)
    {
        const auto& track = s.f.song().tracks.at (i);
        INFO ("Track " << track.name);
        CHECK_THAT (track.volumeDb, Catch::Matchers::WithinAbs (0.0, 0.0));
        CHECK_THAT (track.pan, Catch::Matchers::WithinAbs (0.0, 0.0));
        CHECK_FALSE (track.mute);
        CHECK_FALSE (track.solo);
        CHECK_FALSE (track.armed);
        CHECK (track.insertChain.empty());
        REQUIRE (track.clips.size() == source.clips.size());
        auto copy = track.clips.front();
        const auto& part = s.backingTrackOf (track);
        CHECK (part.id != sourceBackingTrack.id);
        CHECK (part.separatedFrom == song::SeparationOrigin { sourceBackingTrack.id, "bs_roformer_sw.fp32" });
        CHECK (copy.id != source.clips.front().id);
        copy.id = source.clips.front().id;
        copy.source = source.clips.front().source;
        CHECK (copy == source.clips.front());
    }
}

TEST_CASE ("each new Backing Track plays its part's sine", "[ui][separation]")
{
    SeparateFixture s;
    s.separate ("Band Mix");
    const auto root = s.f.openSong->package().location();

    for (std::size_t part = 0; part < 6; ++part)
    {
        const auto& track = s.f.song().tracks.at (part + 1);
        INFO ("Track " << track.name);
        const auto& backingTrack = s.backingTrackOf (track);
        CHECK (song::originalFileFor (root, backingTrack).existsAsFile());
        CHECK (song::derivedFileFor (root, backingTrack).existsAsFile());
        CHECK (song::peaksFileFor (root, backingTrack.id).existsAsFile());

        const auto out = renderSoloed (s.f.song(), track.id, root);
        REQUIRE (out[0].size() == (std::size_t) (2 * helperSampleRate));
        float residual = 0.0f;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < out[c].size(); ++i)
                residual = std::max (residual, std::abs (out[c][i] - partSample (part, (std::int64_t) i) * overdub::engine::testing::centrePanGain));
        CHECK (residual <= 1.0e-6f); // -120 dBFS
    }
}

TEST_CASE ("a Separation is one undo step: undo and redo restore the Song exactly and keep the files", "[ui][separation]")
{
    SeparateFixture s;
    const auto before = s.f.song();
    s.separate ("Band Mix");
    const auto separated = s.f.song();
    const auto root = s.f.openSong->package().location();

    s.f.window->perform (Command::undo);
    CHECK (s.f.song() == before);
    CHECK_FALSE (s.f.trackNamed ("Band Mix").mute);
    CHECK (s.f.openSong->package().redoLabel() == "Separate Band Mix.wav");
    for (const auto& backingTrack : separated.backingTracks)
        CHECK (song::derivedFileFor (root, backingTrack).existsAsFile()); // redo plays

    s.f.window->perform (Command::redo);
    CHECK (s.f.song() == separated);
    s.f.window->perform (Command::undo);
    CHECK (s.f.song() == before);
}

TEST_CASE ("the new Backing Tracks' origin survives closing and reopening the Song", "[ui][separation]")
{
    SeparateFixture s;
    const auto sourceBackingTrackId = s.backingTrackOf (s.f.trackNamed ("Band Mix")).id;
    s.separate ("Band Mix");
    const auto separated = s.f.song();
    const auto location = s.f.openSong->package().location();

    s.f.window->setOpenSong (nullptr);
    s.f.openSong.reset();
    auto reopened = song::SongPackage::open (location);

    REQUIRE (reopened.has_value());
    CHECK (reopened->package->song() == separated);
    const auto& backingTracks = reopened->package->song().backingTracks;
    CHECK (backingTracks.front().separatedFrom.isEmpty());
    for (std::size_t i = 1; i < backingTracks.size(); ++i)
        CHECK (backingTracks[i].separatedFrom == song::SeparationOrigin { sourceBackingTrackId, "bs_roformer_sw.fp32" });
}

TEST_CASE ("a Separation is logged at its boundaries under song.*", "[ui][separation]")
{
    SeparateFixture s;
    const auto source = s.f.trackNamed ("Band Mix");
    const auto sourceBackingTrackId = s.backingTrackOf (source).id;
    ScopedLogCapture capture;

    s.separate ("Band Mix");

    const auto started = firstEvent (capture, "song.separation_started");
    CHECK (started.at ("track_id") == source.id);
    CHECK (started.at ("source_backing_track_id") == sourceBackingTrackId);
    CHECK (started.at ("model") == "bs_roformer_sw.fp32");
    const auto finished = firstEvent (capture, "song.separation_finished");
    CHECK (finished.at ("source_backing_track_id") == sourceBackingTrackId);
    CHECK (finished.at ("model") == "bs_roformer_sw.fp32");
    CHECK (finished.at ("duration_ms").is_number_integer());
    std::vector<std::string> newIds;
    for (std::size_t i = 1; i <= 6; ++i)
        newIds.push_back (s.backingTrackOf (s.f.song().tracks.at (i)).id);
    CHECK (finished.at ("backing_track_ids") == nlohmann::json (newIds));
}

TEST_CASE ("a Separation without the model installed is an Alert that says how to install it", "[ui][separation]")
{
    SeparateFixture s;
    s.f.openSong->setSeparationTools (song::SeparationTools { .model = s.f.fixture->folder.getChildFile ("models/none.onnx"), .startHelper = fakeSeparationHelper ({}, s.log) });
    const auto before = s.f.song();
    ScopedLogCapture capture;

    s.rightClickRow ("Band Mix");

    const auto shown = firstEvent (capture, "ui.alert_shown");
    CHECK (shown.at ("code") == "song.separation_model_missing");
    CHECK (shown.at ("tier") == "alert");
    CHECK_FALSE (s.f.openSong->isSeparating());
    CHECK (s.log->starts.empty());
    REQUIRE (s.f.alertsShown.size() == 1);
    CHECK (s.f.alertsShown[0].contains ("make separation-model"));
    CHECK (s.f.song() == before);
}

TEST_CASE ("a snapshot of the Separation dialog over the Record page", "[ui][separation][snapshot]")
{
    SeparateFixture s;
    s.rightClickRow ("Band Mix");
    s.f.window->tick();
    s.f.window->tick();
    s.f.window->tick();
    CHECK (writeSnapshot (*s.f.window, "issue-114", "separation-dialog").existsAsFile());
}

// Issue #136: build/ui-snapshots/issue-136/*.png, for a session to see whether the emoji in the
// Track names draws in colour (Apple Color Emoji fallback) on the Record rows, the Edit headers
// and the Mixer strips.
TEST_CASE ("snapshots of the Tracks a Separation adds, named with emoji, on the Record and Edit pages", "[ui][separation][snapshot]")
{
    SeparateFixture s;
    s.separate ("Band Mix");
    auto& window = *s.f.window;
    window.tick();
    CHECK (writeSnapshot (window, "issue-136", "record-rows").existsAsFile());
    CHECK (writeSnapshot (s.f.rowNamed (bassTrackName), "issue-136", "record-row-bass").existsAsFile());

    REQUIRE (pressKey (window, '2'));
    window.tick();
    CHECK (writeSnapshot (window, "issue-136", "edit-headers-mixer").existsAsFile());
    CHECK (writeSnapshot (s.f.editHeaderNamed (bassTrackName), "issue-136", "edit-header-bass").existsAsFile());
    CHECK (writeSnapshot (*window.editPage.mixer.strips.at (1), "issue-136", "mixer-strip-bass").existsAsFile());
}

TEST_CASE ("a helper that fails is an Alert, the dialog closes and the Song is unchanged", "[ui][separation]")
{
    SeparateFixture s (FakeHelperScript { .exitCode = 1, .errorLine = R"({"error":"separation.inference_failed","message":"out of memory"})" });
    const auto before = s.f.song();
    ScopedLogCapture capture;

    s.separate ("Band Mix");

    const auto shown = firstEvent (capture, "ui.alert_shown");
    CHECK (shown.at ("code") == "song.separation_failed");
    CHECK (shown.at ("tier") == "alert");
    CHECK (s.f.alertsShown.size() == 1);
    CHECK_FALSE (isShown (s.f.window->runDialog));
    CHECK (s.f.song() == before);
}

namespace
{
std::size_t countEvents (const ScopedLogCapture& capture, const std::string& name)
{
    return (std::size_t) std::ranges::count_if (capture.events(), [&name] (const nlohmann::json& event) { return event.at ("event") == name; });
}

// Every file in the package, relative to it, sorted.
std::vector<std::string> packageFiles (const song::SongPackage& package)
{
    std::vector<std::string> paths;
    for (const auto& file : package.location().findChildFiles (juce::File::findFiles, true))
        paths.push_back (file.getRelativePathFrom (package.location()).toStdString());
    std::ranges::sort (paths);
    return paths;
}
} // namespace

TEST_CASE ("the dialog's Cancel kills the helper at once, deletes its output and leaves the Song, history and package unchanged", "[ui][separation][cancel]")
{
    SeparateFixture s;
    const auto before = s.f.song();
    const auto undoLabel = s.f.openSong->package().undoLabel();
    const auto canRedo = s.f.openSong->package().canRedo();
    const auto files = packageFiles (s.f.openSong->package());
    auto& dialog = s.f.window->runDialog;
    ScopedLogCapture capture;
    s.rightClickRow ("Band Mix");
    s.f.window->tick(); // started
    s.f.window->tick(); // chunk 1
    REQUIRE (s.log->starts.size() == 1);
    const auto outputFolder = s.log->starts[0].outputFolder;
    REQUIRE (outputFolder.createDirectory().wasOk());
    REQUIRE (outputFolder.getChildFile ("bass.wav.tmp").replaceWithText ("part"));
    REQUIRE (isShown (dialog.cancelButton));

    dialog.cancelButton.onClick();

    CHECK (s.log->killed);
    CHECK_FALSE (s.f.openSong->isSeparating());
    CHECK_FALSE (isShown (dialog));
    CHECK_FALSE (outputFolder.exists());
    CHECK (s.f.song() == before);
    CHECK (s.f.openSong->package().undoLabel() == undoLabel);
    CHECK (s.f.openSong->package().canRedo() == canRedo);
    CHECK (packageFiles (s.f.openSong->package()) == files);
    CHECK (s.f.alertsShown.empty());
    const auto cancelled = firstEvent (capture, "song.separation_cancelled");
    CHECK (cancelled.at ("track_id") == s.f.trackNamed ("Band Mix").id);
    CHECK (cancelled.at ("chunks_done") == 1);

    s.f.window->tick(); // nothing left to finish
    CHECK (s.f.song() == before);
    CHECK (countEvents (capture, "song.separation_finished") == 0);
}

TEST_CASE ("a helper that fails, crashes, never starts or ends without every part is an Alert, logged once, and leaves the Song unchanged", "[ui][separation][failure]")
{
    struct Case
    {
        FakeHelperScript script;
        std::string code;
    };
    const auto c = GENERATE (
        Case { FakeHelperScript { .exitCode = 1, .errorLine = R"({"error":"separation.inference_failed","message":"out of memory"})" }, "song.separation_failed" },
        Case { FakeHelperScript { .signal = 9 }, "song.separation_failed" },
        Case { FakeHelperScript { .partsWritten = 5 }, "song.separation_failed" },
        Case { FakeHelperScript { .startError = overdub::core::Error { "song.separation_start_failed", "injected", { { "path", "/nowhere" }, { "errno", 2 } } } }, "song.separation_start_failed" });
    CAPTURE (c.code, c.script.exitCode, c.script.signal, c.script.partsWritten);
    SeparateFixture s (c.script);
    const auto before = s.f.song();
    const auto files = packageFiles (s.f.openSong->package());
    ScopedLogCapture capture;

    s.rightClickRow ("Band Mix");
    for (int poll = 0; poll < 20 && s.f.openSong->isSeparating(); ++poll)
        s.f.window->tick();

    CHECK_FALSE (s.f.openSong->isSeparating());
    CHECK_FALSE (isShown (s.f.window->runDialog));
    const auto shown = firstEvent (capture, "ui.alert_shown");
    CHECK (shown.at ("code") == c.code);
    CHECK (shown.at ("tier") == "alert");
    CHECK (s.f.alertsShown.size() == 1);
    CHECK (countEvents (capture, c.code) == 1); // the Error, logged once where it is handled
    const auto logged = firstEvent (capture, c.code);
    CHECK (logged.at ("lvl") == "error");
    if (c.script.exitCode != 0 || c.script.signal != 0)
    {
        CHECK (logged.at ("exit_code") == (c.script.signal != 0 ? -1 : 1));
        CHECK (logged.at ("signal") == c.script.signal);
        CHECK (logged.at ("helper_error_line") == c.script.errorLine);
    }
    CHECK (s.f.song() == before);
    CHECK (packageFiles (s.f.openSong->package()) == files);
    for (const auto& start : s.log->starts)
        CHECK_FALSE (start.outputFolder.exists());
}

TEST_CASE ("a Separation with an installed model that is not the expected graph says how to install it, without starting the helper", "[ui][separation][model]")
{
    SeparateFixture s;
    auto wrong = overdub::separation::testing::separationModelMetadata();
    wrong.erase ("architecture");
    s.f.openSong->setSeparationTools (song::SeparationTools { .model = writeStandInModel (s.f.fixture->folder.getChildFile ("other"), wrong), .startHelper = fakeSeparationHelper ({}, s.log) });
    const auto before = s.f.song();
    ScopedLogCapture capture;

    s.rightClickRow ("Band Mix");

    const auto shown = firstEvent (capture, "ui.alert_shown");
    CHECK (shown.at ("code") == "song.separation_model_invalid");
    CHECK (shown.at ("tier") == "alert");
    CHECK_FALSE (s.f.openSong->isSeparating());
    CHECK (s.log->starts.empty());
    REQUIRE (s.f.alertsShown.size() == 1);
    CHECK (s.f.alertsShown[0].contains ("make separation-model"));
    CHECK (s.f.song() == before);
}

TEST_CASE ("Separate... is disabled with a tooltip during a Pass", "[ui][separation][record]")
{
    SeparateFixture s;
    s.menuPick = 0;
    REQUIRE (s.f.openSong->record().has_value());

    s.rightClickRow ("Band Mix");

    REQUIRE (s.menusShown.size() == 1);
    CHECK (s.menusShown[0] == std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", false, "Stop recording first" } });
    CHECK_FALSE (s.f.openSong->isSeparating());
    CHECK (s.log->starts.empty());
}

TEST_CASE ("the Separation dialog closes only on success, Cancel or failure: clicks and keys do not close it", "[ui][separation]")
{
    SeparateFixture s;
    auto& dialog = s.f.window->runDialog;
    s.rightClickRow ("Band Mix");
    REQUIRE (isShown (dialog));

    dialog.mouseDown (mouseEventAt (dialog, { 5.0f, 5.0f }, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
    pressKey (*s.f.window, juce::KeyPress (juce::KeyPress::escapeKey));
    pressKey (*s.f.window, 'w', juce::ModifierKeys (juce::ModifierKeys::commandModifier));
    s.rightClickRow ("Band Mix"); // the menu does not open under the dialog

    CHECK (isShown (dialog));
    CHECK (s.f.openSong->isSeparating());
    CHECK (s.menusShown.size() == 1);
}

TEST_CASE ("the Edit Timeline scrolls to show the Tracks a Separation adds below the source Track", "[ui][separation][vertical_scroll]")
{
    SeparateFixture s;
    auto& timeline = s.f.window->editPage.timeline;
    // The Timeline gets the Song only in Edit mode: shown once, it knows the Tracks before the
    // Separation, which runs from a Record row.
    REQUIRE (pressKey (*s.f.window, '2'));
    REQUIRE (pressKey (*s.f.window, '1'));

    s.separate ("Band Mix");
    REQUIRE (pressKey (*s.f.window, '2'));

    // Six new Tracks are taller than the rows' viewport: the first of them is at its top.
    REQUIRE (s.trackNames() == songTrackNamesAfter());
    auto& bass = s.f.editHeaderNamed (bassTrackName);
    CHECK (timeline.getLocalArea (&bass, bass.getLocalBounds()).getY() == timeline.tracksViewportBounds().getY());
    CHECK (timeline.verticalScroll() == trackLaneHeight);
}
