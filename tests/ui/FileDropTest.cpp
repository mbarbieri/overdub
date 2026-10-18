#include "ui/FileDrop.h"

#include "core/LogEvents.h"
#include "export/MixRender.h"
#include "song/FakeSeparationHelper.h"
#include "song/FixtureSongBuilder.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

// Importing by dropping files on the Edit page's Timeline (issue #123). JUCE routes a real drop
// through a window peer, which the harness has none of, so the tests call the Timeline's
// FileDragAndDropTarget methods as JUCE would.
using namespace overdub;
using overdub::core::ScopedLogCapture;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
juce::StringArray pathsOf (const std::vector<juce::File>& files)
{
    juce::StringArray paths;
    for (const auto& file : files)
        paths.add (file.getFullPathName());
    return paths;
}

std::vector<std::string> trackNames (const song::Song& song)
{
    std::vector<std::string> names;
    for (const auto& track : song.tracks)
        names.push_back (track.name);
    return names;
}
} // namespace

TEST_CASE ("only dropped files with an importable audio extension are imported, in drop order", "[ui][drop]")
{
    const auto fixture = song::testing::threeTrackFixture();
    const auto folder = fixture->folder;
    const auto drums = folder.getChildFile ("Drums.WAV");
    const auto notes = folder.getChildFile ("notes.txt");
    const auto bass = folder.getChildFile ("Bass.mp3");
    const auto aFolder = folder.getChildFile ("Loops.wav");
    REQUIRE (drums.replaceWithText ("x"));
    REQUIRE (notes.replaceWithText ("x"));
    REQUIRE (bass.replaceWithText ("x"));
    REQUIRE (aFolder.createDirectory().wasOk());

    const auto files = importableAudioFiles ({ drums.getFullPathName(), notes.getFullPathName(), aFolder.getFullPathName(), bass.getFullPathName() });

    CHECK (files == std::vector<juce::File> { drums, bass });
}

TEST_CASE ("the Import dialog offers every importable extension", "[ui][drop]")
{
    CHECK (importableAudioPattern() == "*.wav;*.aif;*.aiff;*.aifc;*.caf;*.m4a;*.mp3;*.flac");
}

TEST_CASE ("dropping audio files on the Timeline imports each as its own Track and undo step", "[ui][drop]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto drums = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    const auto bass = f.fixture->writeSineFile ("Bass.wav", 1.0, 55.0);
    const auto namesBefore = trackNames (f.song());
    const auto paths = pathsOf ({ drums, bass });

    REQUIRE (timeline.isInterestedInFileDrag (paths));
    timeline.filesDropped (paths, 10, 10);

    auto expectedNames = namesBefore;
    expectedNames.push_back ("Drums");
    expectedNames.push_back ("Bass");
    CHECK (trackNames (f.song()) == expectedNames);
    for (const auto& name : { "Drums", "Bass" })
    {
        const auto& clips = f.trackNamed (name).clips;
        REQUIRE (clips.size() == 1);
        CHECK (clips[0].timelineStart == 0);
        CHECK (clips[0].source.kind == song::ClipSourceKind::backing);
    }
    CHECK (f.openSong->package().undoLabel() == "Import Bass.wav");
    REQUIRE (f.openSong->undo().has_value());
    CHECK (f.openSong->package().undoLabel() == "Import Drums.wav");
}

TEST_CASE ("a drop imports only its audio files and says nothing about the rest", "[ui][drop]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto drums = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    const auto notes = f.fixture->folder.getChildFile ("notes.txt");
    const auto loops = f.fixture->folder.getChildFile ("Loops");
    REQUIRE (notes.replaceWithText ("not audio"));
    REQUIRE (loops.createDirectory().wasOk());
    const auto tracksBefore = f.song().tracks.size();
    const auto paths = pathsOf ({ notes, drums, loops });

    ScopedLogCapture capture;
    REQUIRE (timeline.isInterestedInFileDrag (paths));
    timeline.filesDropped (paths, 10, 10);

    REQUIRE (f.song().tracks.size() == tracksBefore + 1);
    CHECK (f.song().tracks.back().name == "Drums");
    CHECK (f.alertsShown.empty());
    const auto dropped = core::testing::firstEvent (capture, "ui.files_dropped");
    CHECK (dropped.at ("dropped") == 3);
    CHECK (dropped.at ("importable") == 1);
    CHECK (dropped.at ("accepted") == true);
    CHECK (std::ranges::none_of (capture.events(), [] (const auto& event) { return event.at ("event") == "ui.alert_shown"; }));
}

TEST_CASE ("a drag with no audio file is refused and leaves the Song unchanged", "[ui][drop]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto notes = f.fixture->folder.getChildFile ("notes.txt");
    const auto loops = f.fixture->folder.getChildFile ("Loops.wav");
    REQUIRE (notes.replaceWithText ("not audio"));
    REQUIRE (loops.createDirectory().wasOk());
    const auto songBefore = f.song();
    const auto paths = pathsOf ({ notes, loops });

    CHECK_FALSE (timeline.isInterestedInFileDrag (paths));
    timeline.fileDragEnter (paths, 10, 10);
    CHECK_FALSE (timeline.isDropHighlighted());
    ScopedLogCapture capture;
    timeline.filesDropped (paths, 10, 10);
    CHECK (f.song() == songBefore);
    const auto dropped = core::testing::firstEvent (capture, "ui.files_dropped");
    CHECK (dropped.at ("importable") == 0);
    CHECK (dropped.at ("accepted") == false);
}

TEST_CASE ("the Timeline is highlighted while an audio file drag hovers", "[ui][drop]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto paths = pathsOf ({ f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0) });
    REQUIRE_FALSE (timeline.isDropHighlighted());

    timeline.fileDragEnter (paths, 10, 10);
    CHECK (timeline.isDropHighlighted());
    timeline.fileDragExit (paths);
    CHECK_FALSE (timeline.isDropHighlighted());

    timeline.fileDragEnter (paths, 10, 10);
    timeline.filesDropped (paths, 10, 10);
    CHECK_FALSE (timeline.isDropHighlighted());
}

TEST_CASE ("a dropped file that fails to decode is an Alert, and the other files are imported", "[ui][drop][messages]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto broken = f.fixture->folder.getChildFile ("Broken.wav");
    REQUIRE (broken.replaceWithText ("not audio"));
    const auto drums = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    const auto tracksBefore = f.song().tracks.size();

    ScopedLogCapture capture;
    timeline.filesDropped (pathsOf ({ broken, drums }), 10, 10);

    const auto shown = core::testing::firstEvent (capture, "ui.alert_shown");
    REQUIRE_FALSE (shown.is_null());
    CHECK (shown.at ("code") == "song.import_unreadable");
    CHECK (f.alertsShown.size() == 1);
    REQUIRE (f.song().tracks.size() == tracksBefore + 1);
    CHECK (f.song().tracks.back().name == "Drums");
}

TEST_CASE ("while a Pass runs, a drag is refused and nothing is imported", "[ui][drop]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto paths = pathsOf ({ f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0) });
    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());
    const auto songBefore = f.song();

    CHECK_FALSE (timeline.isInterestedInFileDrag (paths));
    timeline.fileDragEnter (paths, 10, 10);
    CHECK_FALSE (timeline.isDropHighlighted());
    timeline.filesDropped (paths, 10, 10);
    CHECK (f.song() == songBefore);
}

TEST_CASE ("while an Export runs, a drag is refused and nothing is imported", "[ui][drop]")
{
    MainWindowFixture f;
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto paths = pathsOf ({ f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0) });
    REQUIRE (f.openSong->startExport ({ exporting::ExportFormat::wav, f.fixture->folder.getChildFile ("Song.wav") }).has_value());
    REQUIRE (f.openSong->isExporting());
    const auto songBefore = f.song();

    CHECK_FALSE (timeline.isInterestedInFileDrag (paths));
    timeline.fileDragEnter (paths, 10, 10);
    CHECK_FALSE (timeline.isDropHighlighted());
    timeline.filesDropped (paths, 10, 10);
    CHECK (f.song() == songBefore);
}

TEST_CASE ("while a Separation runs, a drag is refused and nothing is imported", "[ui][drop]")
{
    MainWindowFixture f { song::testing::FixtureSongBuilder {}.sampleRate (song::testing::helperSampleRate).backingTrack ("Band Mix", 2.0, 220.0).build() };
    auto& timeline = f.window->songView()->editPage.timeline;
    f.openSong->setSeparationTools (song::SeparationTools {
        .model = song::testing::writeStandInModel (f.fixture->folder),
        .startHelper = song::testing::fakeSeparationHelper ({}, std::make_shared<song::testing::FakeHelperLog>()),
        .workFolder = f.fixture->folder.getChildFile ("work"),
    });
    const auto paths = pathsOf ({ f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0) });
    REQUIRE (f.openSong->startSeparation (f.trackNamed ("Band Mix").id).has_value());
    REQUIRE (f.openSong->isSeparating());
    const auto songBefore = f.song();

    CHECK_FALSE (timeline.isInterestedInFileDrag (paths));
    timeline.fileDragEnter (paths, 10, 10);
    CHECK_FALSE (timeline.isDropHighlighted());
    timeline.filesDropped (paths, 10, 10);
    CHECK (f.song() == songBefore);
}

TEST_CASE ("snapshot: the Edit page while an audio file drag hovers the Timeline", "[ui][drop][snapshot]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    f.window->songView()->editPage.timeline.fileDragEnter (pathsOf ({ f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0) }), 10, 10);
    CHECK (writeSnapshot (*f.window, "issue-123", "timeline-drop-highlight").existsAsFile());
}
