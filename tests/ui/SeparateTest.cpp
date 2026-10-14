#include "ui/MainComponent.h"

#include "export/MixRender.h"
#include "song/BackingTrackImport.h"
#include "song/FakeSeparationHelper.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
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
using namespace overdub::ui;
using namespace overdub::ui::testing;
using namespace overdub::song::testing;
namespace song = overdub::song;

namespace
{
const std::vector<std::string> songTrackNamesAfter { "Band Mix", "Bass", "Drums", "Other", "Vocals", "Guitar", "Piano", "Guitar" };

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

nlohmann::json firstEvent (const ScopedLogCapture& capture, const std::string& name)
{
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            return event;
    FAIL ("no event " << name);
    return {};
}
} // namespace

TEST_CASE ("Separate... on a Backing Track's row opens a modal dialog whose progress bar follows the helper", "[ui][separation]")
{
    SeparateFixture s;
    auto& dialog = s.f.window->separationDialog;
    CHECK_FALSE (isShown (dialog));

    s.menuPick = std::nullopt; // dismissed
    s.rightClickRow ("Guitar");
    s.rightClickRow ("Band Mix");
    REQUIRE (s.menusShown.size() == 2);
    CHECK (s.menusShown[0] == std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", false } }); // no Clips
    CHECK (s.menusShown[1] == std::vector<TrackMenuItem> { { TrackMenuAction::separate, "Separate...", true } });
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

TEST_CASE ("a finished Separation adds six Backing Tracks on six Tracks below the source Track, which is muted", "[ui][separation]")
{
    SeparateFixture s;
    const auto source = s.f.trackNamed ("Band Mix");
    const auto sourceBackingTrack = s.backingTrackOf (source);

    s.separate ("Band Mix");

    CHECK (s.trackNames() == songTrackNamesAfter);
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
    CHECK_FALSE (isShown (s.f.window->separationDialog));
    CHECK (s.f.song() == before);
}
