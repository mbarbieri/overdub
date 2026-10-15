#include "song/Separation.h"
#include "core/LogEvents.h"
#include "song/BackingTrackImport.h"
#include "song/Peaks.h"
#include "song/SeparationModel.h"
#include "song/SeparationRun.h"
#include "song/SongPackage.h"

#include "FakeSeparationHelper.h"
#include "FixtureSongBuilder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// The Song change of a Separation (issue #114) as pure functions, its undo step and files through
// SongPackage::addSeparation, and song::SeparationRun over a fake helper.
using overdub::core::ScopedLogCapture;
using overdub::core::testing::firstEvent;
using namespace overdub::song;
using namespace overdub::song::testing;

namespace
{
// A colour pick that always takes the first choice.
std::size_t firstChoice (std::size_t)
{
    return 0;
}

// The new Tracks' names as issue #136's table gives them: the emoji by code point (so this does
// not share Separation.h's UTF-8 spelling), a space and the part's name, as UTF-8.
std::vector<std::string> partTrackNames()
{
    const std::vector<std::pair<juce::juce_wchar, juce::String>> table {
        { 0x1f3bb, "Bass" },
        { 0x1f941, "Drums" },
        { 0x1f3b6, "Other" },
        { 0x1f3a4, "Vocals" },
        { 0x1f3b8, "Guitar" },
        { 0x1f3b9, "Piano" },
    };
    std::vector<std::string> names;
    for (const auto& [emoji, name] : table)
        names.push_back ((juce::String::charToString (emoji) + " " + name).toStdString());
    return names;
}

// The names a Separation gave its Tracks before issue #136.
const std::vector<std::string> plainPartTrackNames { "Bass", "Drums", "Other", "Vocals", "Guitar", "Piano" };

std::vector<std::string> joined (std::vector<std::string> first, const std::vector<std::string>& second, const std::vector<std::string>& third = {})
{
    first.insert (first.end(), second.begin(), second.end());
    first.insert (first.end(), third.begin(), third.end());
    return first;
}

std::string fileBytes (const juce::File& file)
{
    juce::MemoryBlock block;
    REQUIRE (file.loadFileAsData (block));
    return { static_cast<const char*> (block.getData()), block.getSize() };
}

// A Song by hand: "Band Mix" holds two Clips of one Backing Track, with gain, fades and offsets
// away from their defaults; "Vocals" records Takes; "Click" plays another Backing Track.
Song handBuiltSong()
{
    Song song = makeEmptySong (44100);
    song.backingTracks = {
        BackingTrack { .id = "backing-mix", .originalFilename = "Band Mix.mp3", .originalExtension = "mp3", .originalSampleRate = 48000, .channels = 2, .formatName = "MP3", .lengthSamples = 441000 },
        BackingTrack { .id = "backing-click", .originalFilename = "Click.wav", .originalExtension = "wav", .originalSampleRate = 44100, .channels = 1, .formatName = "WAV", .lengthSamples = 44100 },
    };
    song.takes = { Take { .id = "take-1", .trackId = "track-vocals", .passId = "pass-1", .file = "audio/take-1.wav", .lengthSamples = 1000 } };
    song.tracks = {
        Track {
            .id = "track-mix",
            .name = "Band Mix",
            .volumeDb = -4.5,
            .pan = 0.25,
            .insertChain = { InsertSlot { .id = "slot-1", .component = { "aufx", "dely", "appl" }, .name = "AUDelay" } },
            .clips = {
                Clip { .id = "clip-1", .source = { ClipSourceKind::backing, "backing-mix" }, .timelineStart = 0, .sourceOffset = 0, .length = 22050, .gainDb = -3.0, .fadeInLength = 100, .fadeOutLength = 200 },
                Clip { .id = "clip-2", .source = { ClipSourceKind::backing, "backing-mix" }, .timelineStart = 30000, .sourceOffset = 22050, .length = 10000, .gainDb = 2.0, .fadeInLength = 50, .fadeOutLength = 0 },
            },
        },
        Track { .id = "track-vocals", .name = "Vocals", .armed = true, .inputChannels = { 0 }, .clips = { Clip { .id = "clip-3", .source = { ClipSourceKind::take, "take-1" }, .length = 1000 } } },
        Track { .id = "track-click", .name = "Click", .clips = { Clip { .id = "clip-4", .source = { ClipSourceKind::backing, "backing-click" }, .length = 44100 } } },
    };
    return song;
}

std::vector<BackingTrack> sixParts()
{
    std::vector<BackingTrack> parts;
    for (const auto& part : separationParts)
        parts.push_back (BackingTrack {
            .id = "part-" + std::string (part.fileStem),
            .originalFilename = std::string (part.fileStem) + ".wav",
            .originalExtension = "wav",
            .originalSampleRate = 44100,
            .channels = 2,
            .formatName = "WAV",
            .lengthSamples = 441000,
            .separatedFrom = { "backing-mix", "bs_roformer_sw.fp32" },
        });
    return parts;
}

std::vector<std::string> trackNames (const Song& song)
{
    std::vector<std::string> names;
    for (const auto& track : song.tracks)
        names.push_back (track.name);
    return names;
}

const Track& trackNamed (const Song& song, const std::string& name)
{
    const auto found = std::ranges::find (song.tracks, name, &Track::name);
    REQUIRE (found != song.tracks.end());
    return *found;
}

} // namespace

TEST_CASE ("a Separation adds six Tracks named with each part's emoji and name, in the helper's order, directly below the source Track, which is muted", "[song][separation]")
{
    const auto before = handBuiltSong();

    const auto separated = withSeparation (before, "track-mix", sixParts(), firstChoice);

    REQUIRE (separated.has_value());
    CHECK (trackNames (*separated) == joined ({ "Band Mix" }, partTrackNames(), { "Vocals", "Click" }));
    CHECK (separated->tracks.at (7).id == "track-vocals");
    CHECK (separated->tracks.at (8).id == "track-click");

    auto source = separated->tracks.at (0);
    CHECK (source.mute);
    source.mute = false;
    CHECK (source == before.tracks.at (0)); // name, Clips, Insert Chain and mix kept

    auto expectedBackingTracks = before.backingTracks;
    for (const auto& part : sixParts())
        expectedBackingTracks.push_back (part);
    CHECK (separated->backingTracks == expectedBackingTracks);
    CHECK (separated->takes == before.takes);
}

TEST_CASE ("each new Track starts at clean mixer defaults with a copy of every source Clip playing its part", "[song][separation]")
{
    const auto before = handBuiltSong();
    const auto parts = sixParts();

    const auto separated = withSeparation (before, "track-mix", parts, firstChoice);

    REQUIRE (separated.has_value());
    std::vector<Id> clipIds;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const auto& track = separated->tracks.at (i + 1);
        INFO ("Track " << track.name);
        CHECK (track.name == partTrackNames()[i]);
        CHECK_THAT (track.volumeDb, Catch::Matchers::WithinAbs (0.0, 0.0));
        CHECK_THAT (track.pan, Catch::Matchers::WithinAbs (0.0, 0.0));
        CHECK_FALSE (track.mute);
        CHECK_FALSE (track.solo);
        CHECK_FALSE (track.armed);
        CHECK (track.inputChannels.empty());
        CHECK (track.insertChain.empty());
        REQUIRE (track.clips.size() == before.tracks.at (0).clips.size());
        for (std::size_t c = 0; c < track.clips.size(); ++c)
        {
            auto copy = track.clips[c];
            CHECK (copy.source == ClipSource { ClipSourceKind::backing, parts[i].id });
            CHECK (copy.id != before.tracks.at (0).clips[c].id);
            clipIds.push_back (copy.id);
            copy.id = before.tracks.at (0).clips[c].id;
            copy.source = before.tracks.at (0).clips[c].source;
            CHECK (copy == before.tracks.at (0).clips[c]); // start, offset, length, gain, fades
        }
    }
    std::ranges::sort (clipIds);
    CHECK (std::ranges::adjacent_find (clipIds) == clipIds.end()); // every copy has its own id
}

TEST_CASE ("only a Track whose Clips all play one Backing Track can be separated", "[song][separation]")
{
    auto song = handBuiltSong();
    song.tracks.push_back (Track { .id = "track-empty", .name = "Empty" });
    song.tracks.push_back (Track { .id = "track-lost", .name = "Lost", .clips = { Clip { .id = "clip-7", .source = { ClipSourceKind::backing, "backing-gone" } } } });
    song.tracks.push_back (Track { .id = "track-mixed",
                                   .name = "Mixed",
                                   .clips = { Clip { .id = "clip-5", .source = { ClipSourceKind::backing, "backing-mix" } }, Clip { .id = "clip-6", .source = { ClipSourceKind::backing, "backing-click" } } } });

    CHECK (separableBackingTrack (song, "track-mix") == Id ("backing-mix"));
    CHECK (separableBackingTrack (song, "track-click") == Id ("backing-click"));
    CHECK (separableBackingTrack (song, "track-gone").error().code == "song.track_not_found");
    for (const auto& [trackId, reason] : std::vector<std::pair<Id, std::string>> { { "track-empty", "no_clips" }, { "track-vocals", "take_clips" }, { "track-mixed", "several_backing_tracks" }, { "track-lost", "backing_track_missing" } })
    {
        INFO ("Track " << trackId);
        const auto refused = separableBackingTrack (song, trackId);
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.track_not_separable");
        CHECK (refused.error().fields.at ("reason") == reason);
        CHECK (withSeparation (song, trackId, sixParts(), firstChoice).error().code == "song.track_not_separable");
    }
    CHECK (withSeparation (song, "track-mix", { sixParts().front() }, firstChoice).error().code == "song.separation_parts_invalid");
}

TEST_CASE ("a Separation's undo step is labelled with the source Backing Track's original file name", "[song][separation]")
{
    CHECK (separationStepLabel (handBuiltSong().backingTracks.front()) == "Separate Band Mix.mp3");
}

TEST_CASE ("the six Tracks of a Separation get six different colours that no other Track of the Song has", "[song][separation][colour]")
{
    auto before = handBuiltSong();
    before.tracks.at (0).colour = "#a8d8b9"; // palette[0]
    before.tracks.at (1).colour = "#a6d4f2"; // palette[2]
    before.tracks.at (2).colour = "#d3b5e8"; // palette[4]

    const auto separated = withSeparation (before, "track-mix", sixParts(), firstChoice);

    REQUIRE (separated.has_value());
    std::vector<std::string> newColours;
    for (std::size_t i = 1; i <= separationParts.size(); ++i)
        newColours.push_back (separated->tracks.at (i).colour);
    // Each takes the first colour still free, the earlier new Tracks' colours included.
    CHECK (newColours == std::vector<std::string> { "#f4a6a6", "#f5e19c", "#9fdcd4", "#f6c39a", "#a7b8e8", "#d4e8a0" }); // palette[1, 3, 5, 6, 7, 8]
}

TEST_CASE ("a Separation with fewer free palette colours than parts gives the free ones first, then any palette colour", "[song][separation][colour]")
{
    auto before = handBuiltSong();
    while (before.tracks.size() < 10)
        before.tracks.push_back (Track { .id = "track-extra-" + std::to_string (before.tracks.size()), .name = "Extra" });
    for (std::size_t i = 0; i < before.tracks.size(); ++i)
        before.tracks[i].colour = std::string (trackColourPalette[i]); // palette[10] and [11] stay free
    std::vector<std::size_t> counts;
    const PickIndex lastChoice = [&counts] (std::size_t count) {
        counts.push_back (count);
        return count - 1;
    };

    const auto separated = withSeparation (before, "track-mix", sixParts(), lastChoice);

    REQUIRE (separated.has_value());
    std::vector<std::string> newColours;
    for (std::size_t i = 1; i <= separationParts.size(); ++i)
        newColours.push_back (separated->tracks.at (i).colour);
    CHECK (newColours == std::vector<std::string> { "#eeb1e0", "#bcb3ef", "#eeb1e0", "#eeb1e0", "#eeb1e0", "#eeb1e0" }); // palette[11], [10], then the whole palette's last
    CHECK (counts == std::vector<std::size_t> { 2, 1, 12, 12, 12, 12 });
}

namespace
{
// The fixture Song (Band Mix at 44.1 kHz) with six part WAVs beside it, as the helper writes them.
struct PartFilesFixture
{
    std::unique_ptr<FixtureSong> fixture = FixtureSongBuilder {}.sampleRate (44100).backingTrack ("Band Mix", 2.0, 220.0).build();
    std::vector<juce::File> partFiles;

    PartFilesFixture()
    {
        for (std::size_t part = 0; part < separationParts.size(); ++part)
            partFiles.push_back (fixture->writeSineFile (std::string (separationParts[part].fileStem) + ".wav", 2.0, partFrequencies[part]));
    }

    SongPackage& package() { return *fixture->package; }
};
} // namespace

TEST_CASE ("SongPackage::addSeparation adds the parts as Backing Tracks with the import's files and their origin, as one undo step", "[song][separation][package]")
{
    PartFilesFixture f;
    const auto sourceTrack = f.fixture->trackNamed ("Band Mix");
    const auto sourceBackingTrackId = sourceTrack.clips.front().source.id;
    const auto before = f.package().song();

    const auto added = f.package().addSeparation (sourceTrack.id, f.partFiles, "bs_roformer_sw.fp32");

    REQUIRE (added.has_value());
    REQUIRE (added->size() == 6);
    CHECK (f.package().undoLabel() == "Separate Band Mix.wav");
    const auto& song = f.package().song();
    CHECK (trackNames (song) == joined ({ "Band Mix" }, partTrackNames()));
    for (std::size_t i = 0; i < added->size(); ++i)
    {
        const auto backingTrack = std::ranges::find (song.backingTracks, (*added)[i], &BackingTrack::id);
        REQUIRE (backingTrack != song.backingTracks.end());
        INFO ("part " << backingTrack->originalFilename);
        CHECK (backingTrack->originalFilename == std::string (separationParts[i].fileStem) + ".wav");
        CHECK (backingTrack->separatedFrom == SeparationOrigin { sourceBackingTrackId, "bs_roformer_sw.fp32" });
        CHECK (backingTrack->lengthSamples == 88200);
        CHECK (originalFileFor (f.package().location(), *backingTrack).existsAsFile());
        CHECK (derivedFileFor (f.package().location(), *backingTrack).existsAsFile());
        CHECK (peaksFileFor (f.package().location(), backingTrack->id).existsAsFile());
        CHECK (trackNamed (song, partTrackNames()[i]).clips.front().source.id == backingTrack->id);
    }

    REQUIRE (f.package().undo().has_value());
    CHECK (f.package().song() == before);
}

TEST_CASE ("a Separation's origin survives saving, closing and reopening the Song", "[song][separation][package]")
{
    PartFilesFixture f;
    const auto sourceTrack = f.fixture->trackNamed ("Band Mix");
    REQUIRE (f.package().addSeparation (sourceTrack.id, f.partFiles, "bs_roformer_sw.fp32").has_value());
    const auto separated = f.package().song();
    const auto location = f.package().location();

    f.fixture->package.reset();
    auto reopened = SongPackage::open (location);

    REQUIRE (reopened.has_value());
    CHECK (reopened->notices.empty());
    CHECK (reopened->package->song() == separated);
    const auto separatedFromSource = std::ranges::count_if (reopened->package->song().backingTracks, [&] (const BackingTrack& backingTrack) {
        return backingTrack.separatedFrom == SeparationOrigin { sourceTrack.clips.front().source.id, "bs_roformer_sw.fp32" };
    });
    CHECK (separatedFromSource == 6);
    f.fixture->package = std::move (reopened->package);
}

TEST_CASE ("a Separation's emoji Track names are stored as UTF-8 in song.json and survive saving and reopening byte for byte", "[song][separation][package]")
{
    PartFilesFixture f;
    REQUIRE (f.package().addSeparation (f.fixture->trackNamed ("Band Mix").id, f.partFiles, "bs_roformer_sw.fp32").has_value());
    const auto location = f.package().location();
    const auto songJson = location.getChildFile ("song.json");
    const auto savedBytes = fileBytes (songJson);
    for (const auto& name : partTrackNames())
    {
        INFO ("Track " << name);
        CHECK (savedBytes.contains ("\"" + name + "\"")); // the UTF-8 bytes themselves, not \u escapes
    }

    f.fixture->package.reset();
    auto reopened = SongPackage::open (location);

    REQUIRE (reopened.has_value());
    CHECK (trackNames (reopened->package->song()) == joined ({ "Band Mix" }, partTrackNames()));
    CHECK (fileBytes (songJson) == savedBytes);

    // Written again from the reopened Song.
    REQUIRE (reopened->package->undo().has_value());
    REQUIRE (reopened->package->redo().has_value());
    CHECK (fileBytes (songJson) == savedBytes);
    f.fixture->package = std::move (reopened->package);
}

TEST_CASE ("a Song separated before Track names had emoji opens with its Track names unchanged", "[song][separation][package]")
{
    PartFilesFixture f;
    REQUIRE (f.package().addSeparation (f.fixture->trackNamed ("Band Mix").id, f.partFiles, "bs_roformer_sw.fp32").has_value());
    auto separatedBefore = f.package().song();
    for (std::size_t i = 0; i < plainPartTrackNames.size(); ++i)
        separatedBefore.tracks.at (i + 1).name = plainPartTrackNames[i];
    REQUIRE (f.package().edit (separatedBefore, "Separate Band Mix.wav").has_value());
    const auto location = f.package().location();

    f.fixture->package.reset();
    auto reopened = SongPackage::open (location);

    REQUIRE (reopened.has_value());
    CHECK (reopened->notices.empty());
    CHECK (trackNames (reopened->package->song()) == joined ({ "Band Mix" }, plainPartTrackNames));
    CHECK (reopened->package->song() == separatedBefore);
    f.fixture->package = std::move (reopened->package);
}

TEST_CASE ("a Separation of a Track that is not separable writes no files and leaves the Song unchanged", "[song][separation][package]")
{
    PartFilesFixture f;
    const auto before = f.package().song();

    const auto refused = f.package().addSeparation ("track-gone", f.partFiles, "bs_roformer_sw.fp32");
    CHECK (refused.error().code == "song.track_not_found");
    const auto tooFew = f.package().addSeparation (f.fixture->trackNamed ("Band Mix").id, { f.partFiles.front() }, "bs_roformer_sw.fp32");
    CHECK (tooFew.error().code == "song.separation_parts_invalid");

    CHECK (f.package().song() == before);
    CHECK (f.package().location().getChildFile ("backing").getNumberOfChildFiles (juce::File::findFiles) == 2); // the Band Mix import's
}

namespace
{
struct RunFixture
{
    std::unique_ptr<FixtureSong> fixture = FixtureSongBuilder {}.sampleRate (44100).backingTrack ("Band Mix", 2.0, 220.0).build();
    std::shared_ptr<FakeHelperLog> log = std::make_shared<FakeHelperLog>();
    SeparationTools tools;

    explicit RunFixture (FakeHelperScript script = {})
    {
        tools.model = writeStandInModel (fixture->folder);
        tools.startHelper = fakeSeparationHelper (script, log);
        tools.workFolder = fixture->folder.getChildFile ("work");
    }

    SongPackage& package() { return *fixture->package; }
    const Id& sourceTrackId() const { return fixture->trackNamed ("Band Mix").id; }
};

// Advances the run until it ends; fails after `maxPolls`.
overdub::core::Expected<std::vector<Id>> runToEnd (SeparationRun& run, SeparationRun::Clock::time_point now, int maxPolls = 20)
{
    for (int poll = 0; poll < maxPolls; ++poll)
        if (auto ended = run.advance (now))
            return std::move (*ended);
    FAIL ("the Separation did not end");
    return {};
}
} // namespace

TEST_CASE ("a SeparationRun starts the helper on the source's original file and reports its chunks as progress", "[song][separation][run]")
{
    RunFixture f;
    const auto start = SeparationRun::Clock::now();

    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, start);

    REQUIRE (run.has_value());
    REQUIRE (f.log->starts.size() == 1);
    const auto& source = f.package().song().backingTracks.front();
    CHECK (f.log->starts[0].model == f.tools.model);
    CHECK (f.log->starts[0].input == originalFileFor (f.package().location(), source)); // not the derived WAV
    CHECK (f.log->starts[0].outputFolder.isAChildOf (f.tools.workFolder));
    CHECK ((*run)->sourceName() == "Band Mix.wav");
    CHECK ((*run)->progress().chunks == 0);

    CHECK_FALSE ((*run)->advance (start).has_value()); // separation.started
    CHECK ((*run)->progress().chunks == 3);
    CHECK_FALSE ((*run)->advance (start).has_value()); // chunk 1
    CHECK ((*run)->progress().chunksDone == 1);
    CHECK_FALSE ((*run)->advance (start).has_value()); // chunk 2
    CHECK_THAT ((*run)->progress().fraction(), Catch::Matchers::WithinAbs (2.0 / 3.0, 1.0e-12));
}

TEST_CASE ("a SeparationRun that ends adds the parts, logs its boundaries and deletes the helper's output folder", "[song][separation][run]")
{
    RunFixture f;
    ScopedLogCapture capture;
    const auto start = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, start);
    REQUIRE (run.has_value());
    const auto outputFolder = (*run)->outputFolder();

    const auto ended = runToEnd (**run, start + std::chrono::seconds (90));

    REQUIRE (ended.has_value());
    CHECK (ended->size() == 6);
    CHECK (f.package().song().tracks.size() == 7);
    CHECK_FALSE (outputFolder.exists());

    const auto sourceBackingTrackId = f.package().song().backingTracks.front().id;
    const auto started = firstEvent (capture, "song.separation_started");
    CHECK (started.at ("lvl") == "info");
    CHECK (started.at ("track_id") == f.sourceTrackId());
    CHECK (started.at ("source_backing_track_id") == sourceBackingTrackId);
    CHECK (started.at ("model") == "bs_roformer_sw.fp32");
    const auto finished = firstEvent (capture, "song.separation_finished");
    CHECK (finished.at ("lvl") == "info");
    CHECK (finished.at ("source_backing_track_id") == sourceBackingTrackId);
    CHECK (finished.at ("backing_track_ids") == nlohmann::json (*ended));
    CHECK (finished.at ("model") == "bs_roformer_sw.fp32");
    CHECK (finished.at ("duration_ms") == 90000);
}

TEST_CASE ("a SeparationRun refuses a missing model and a Track it cannot separate without starting the helper", "[song][separation][run]")
{
    RunFixture f;
    const auto now = SeparationRun::Clock::now();

    auto missingModel = f.tools;
    missingModel.model = f.fixture->folder.getChildFile ("models/none.onnx");
    const auto noModel = SeparationRun::start (f.package(), f.sourceTrackId(), missingModel, now);
    CHECK (noModel.error().code == "song.separation_model_missing");
    CHECK (noModel.error().fields.at ("path") == missingModel.model.getFullPathName().toStdString());

    auto noHelper = f.tools;
    noHelper.startHelper = {};
    CHECK (SeparationRun::start (f.package(), f.sourceTrackId(), noHelper, now).error().code == "song.separation_unavailable");
    CHECK (SeparationRun::start (f.package(), "track-gone", f.tools, now).error().code == "song.track_not_found");
    CHECK (f.log->starts.empty());
}

namespace
{
// Every file in the package, as paths relative to it, sorted. A *.tmp is left out: an injected
// write failure leaves it on purpose, as a crash would (song/AtomicWrite.h), and
// SongPackage::open deletes it.
std::vector<std::string> packageFiles (const SongPackage& package)
{
    std::vector<std::string> paths;
    for (const auto& file : package.location().findChildFiles (juce::File::findFiles, true))
        if (file.getFileExtension() != ".tmp")
            paths.push_back (file.getRelativePathFrom (package.location()).toStdString());
    std::ranges::sort (paths);
    return paths;
}

// What a Separation that does not finish must leave exactly as it was.
struct PackageSnapshot
{
    Song song;
    bool canUndo = false;
    std::string undoLabel;
    bool canRedo = false;
    std::vector<std::string> files;

    explicit PackageSnapshot (const SongPackage& package)
        : song (package.song()),
          canUndo (package.canUndo()),
          undoLabel (package.undoLabel()),
          canRedo (package.canRedo()),
          files (packageFiles (package))
    {
    }

    void checkUnchanged (const SongPackage& package) const
    {
        CHECK (package.song() == song);
        CHECK (package.canUndo() == canUndo);
        CHECK (package.undoLabel() == undoLabel);
        CHECK (package.canRedo() == canRedo);
        CHECK (packageFiles (package) == files);
    }
};

const std::string inferenceFailedLine = R"({"error":"separation.inference_failed","message":"out of memory"})";
} // namespace

TEST_CASE ("a helper that exits non-zero, dies from a signal, or ends without every part leaves the Song, history and package unchanged", "[song][separation][run]")
{
    const auto script = GENERATE (FakeHelperScript { .exitCode = 1, .errorLine = inferenceFailedLine },
                                  FakeHelperScript { .signal = 9 },
                                  FakeHelperScript { .partsWritten = 0 },
                                  FakeHelperScript { .partsWritten = 4 });
    CAPTURE (script.exitCode, script.signal, script.partsWritten);
    RunFixture f (script);
    const PackageSnapshot before (f.package());
    const auto now = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, now);
    REQUIRE (run.has_value());
    const auto outputFolder = (*run)->outputFolder();

    const auto ended = runToEnd (**run, now);

    REQUIRE_FALSE (ended.has_value());
    CHECK (ended.error().code == "song.separation_failed");
    if (script.exitCode == 1)
    {
        CHECK (ended.error().fields.at ("exit_code") == 1);
        CHECK (ended.error().fields.at ("signal") == 0);
        CHECK (ended.error().fields.at ("helper_error") == "separation.inference_failed");
        CHECK (ended.error().fields.at ("helper_message") == "out of memory");
        CHECK (ended.error().fields.at ("helper_error_line") == inferenceFailedLine);
    }
    else if (script.signal != 0)
    {
        CHECK (ended.error().fields.at ("exit_code") == -1);
        CHECK (ended.error().fields.at ("signal") == 9);
        CHECK (ended.error().fields.at ("helper_error_line") == "");
    }
    else
    {
        CHECK (ended.error().fields.at ("reason") == "part_missing");
        CHECK (ended.error().fields.at ("path").get<std::string>().ends_with (std::string (separationParts[script.partsWritten].fileStem) + ".wav"));
    }
    before.checkUnchanged (f.package());
    CHECK_FALSE (outputFolder.exists());
}

TEST_CASE ("a helper that refuses the model is song.separation_model_invalid, so the user is told to install it", "[song][separation][run]")
{
    const auto helperError = GENERATE (std::string ("separation.model_missing"), std::string ("separation.model_unloadable"), std::string ("separation.model_mismatch"));
    const auto line = R"({"error":")" + helperError + R"(","message":"no"})";
    RunFixture f (FakeHelperScript { .exitCode = 1, .errorLine = line });
    const PackageSnapshot before (f.package());
    const auto now = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, now);
    REQUIRE (run.has_value());

    const auto ended = runToEnd (**run, now);

    REQUIRE_FALSE (ended.has_value());
    CHECK (ended.error().code == "song.separation_model_invalid");
    CHECK (ended.error().fields.at ("helper_error") == helperError);
    CHECK (ended.error().fields.at ("exit_code") == 1);
    before.checkUnchanged (f.package());
}

TEST_CASE ("a helper that never starts leaves no output folder and the Song unchanged", "[song][separation][run]")
{
    RunFixture f (FakeHelperScript { .startError = overdub::core::Error { "song.separation_start_failed", "injected", { { "path", "/nowhere" }, { "errno", 2 } } } });
    const PackageSnapshot before (f.package());

    const auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, SeparationRun::Clock::now());

    REQUIRE_FALSE (run.has_value());
    CHECK (run.error().code == "song.separation_start_failed");
    REQUIRE (f.log->starts.size() == 1);
    CHECK_FALSE (f.log->starts[0].outputFolder.exists());
    before.checkUnchanged (f.package());
}

TEST_CASE ("progress lines that are not JSON events are skipped and logged, and the Separation goes on", "[song][separation][run]")
{
    RunFixture f (FakeHelperScript { .malformedLines = true });
    ScopedLogCapture capture;
    const auto now = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, now);
    REQUIRE (run.has_value());

    CHECK_FALSE ((*run)->advance (now).has_value()); // separation.started
    CHECK_FALSE ((*run)->advance (now).has_value()); // garbage, then chunk 1
    CHECK ((*run)->progress().chunksDone == 1);
    CHECK ((*run)->progress().chunks == 3);
    const auto ended = runToEnd (**run, now);

    REQUIRE (ended.has_value());
    CHECK (ended->size() == 6);
    const auto ignored = firstEvent (capture, "song.separation_line_ignored");
    CHECK (ignored.at ("lvl") == "warn");
    CHECK (ignored.at ("track_id") == f.sourceTrackId());
    CHECK (ignored.at ("line") == "onnxruntime warning: not json");
}

TEST_CASE ("cancelling a SeparationRun mid-run kills the helper, deletes its output and leaves the Song, history and package unchanged", "[song][separation][run]")
{
    RunFixture f;
    const PackageSnapshot before (f.package());
    ScopedLogCapture capture;
    const auto start = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, start);
    REQUIRE (run.has_value());
    const auto outputFolder = (*run)->outputFolder();
    CHECK_FALSE ((*run)->advance (start).has_value()); // started
    CHECK_FALSE ((*run)->advance (start).has_value()); // chunk 1
    // A part the helper had written, and one it was writing, before the cancel.
    REQUIRE (outputFolder.createDirectory().wasOk());
    REQUIRE (outputFolder.getChildFile ("bass.wav").replaceWithText ("part"));
    REQUIRE (outputFolder.getChildFile ("drums.wav.tmp").replaceWithText ("part"));

    (*run)->cancel (start + std::chrono::seconds (12));

    CHECK (f.log->killed);
    CHECK_FALSE (outputFolder.exists());
    CHECK_FALSE ((*run)->advance (start).has_value()); // over: nothing more
    before.checkUnchanged (f.package());
    const auto cancelled = firstEvent (capture, "song.separation_cancelled");
    CHECK (cancelled.at ("lvl") == "info");
    CHECK (cancelled.at ("track_id") == f.sourceTrackId());
    CHECK (cancelled.at ("source_backing_track_id") == f.package().song().backingTracks.front().id);
    CHECK (cancelled.at ("chunks_done") == 1);
    CHECK (cancelled.at ("chunks") == 3);
    CHECK (cancelled.at ("duration_ms") == 12000);
}

TEST_CASE ("a failure writing the parts into the package after the helper succeeded leaves no undo step and no file", "[song][separation][run][package]")
{
    // The 3rd part's derived WAV fails, or the history step fails after every part is written.
    const auto failHistory = GENERATE (false, true);
    CAPTURE (failHistory);
    RunFixture f;
    const auto location = f.package().location();
    f.fixture->package.reset();
    PackageOptions options;
    auto backingWrites = std::make_shared<int> (0);
    options.beforeRename = [failHistory, backingWrites] (const juce::File& destination) -> std::optional<overdub::core::Error> {
        const auto folder = destination.getParentDirectory().getFileName();
        if (failHistory && folder == "history")
            return overdub::core::Error { "song.write_failed", "injected", {} };
        if (! failHistory && folder == "backing" && ++*backingWrites == 6)
            return overdub::core::Error { "song.write_failed", "injected", {} };
        return std::nullopt;
    };
    auto reopened = SongPackage::open (location, options);
    REQUIRE (reopened.has_value());
    f.fixture->package = std::move (reopened->package);
    const PackageSnapshot before (f.package());
    const auto now = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, now);
    REQUIRE (run.has_value());
    const auto outputFolder = (*run)->outputFolder();

    const auto ended = runToEnd (**run, now);

    REQUIRE_FALSE (ended.has_value());
    CHECK (ended.error().code == "song.write_failed");
    before.checkUnchanged (f.package());
    CHECK_FALSE (outputFolder.exists());
}

TEST_CASE ("the app checks the installed model by its ONNX metadata before starting the helper", "[song][separation][model]")
{
    RunFixture f;
    const auto folder = f.fixture->folder.getChildFile ("other-models");
    REQUIRE (folder.createDirectory().wasOk());

    CHECK (checkSeparationModel (f.tools.model).has_value());

    auto metadata = overdub::separation::testing::separationModelMetadata();
    metadata["sources"] = R"(["bass","drums","other","vocals","guitar","piano"])"; // spacing is not compared
    CHECK (checkSeparationModel (writeStandInModel (folder, metadata)).has_value());

    const auto missing = checkSeparationModel (folder.getChildFile ("none.onnx"));
    CHECK (missing.error().code == "song.separation_model_missing");

    const auto text = folder.getChildFile ("text.onnx");
    REQUIRE (text.replaceWithText ("stand-in"));
    const auto notOnnx = checkSeparationModel (text);
    CHECK (notOnnx.error().code == "song.separation_model_invalid");
    CHECK (notOnnx.error().fields.at ("reason") == "not_onnx");

    struct Case
    {
        std::string key;
        std::optional<std::string> value; // nullopt: the key is absent
        std::string actual;
    };
    for (const auto& c : std::vector<Case> {
             { "architecture", "htdemucs", "htdemucs" },
             { "architecture", std::nullopt, "missing" },
             { "sources", R"(["vocals","other"])", R"(["vocals","other"])" },
         })
    {
        INFO ("key " << c.key << " actual " << c.actual);
        auto wrong = overdub::separation::testing::separationModelMetadata();
        if (c.value.has_value())
            wrong[c.key] = *c.value;
        else
            wrong.erase (c.key);
        const auto refused = checkSeparationModel (writeStandInModel (folder, wrong));
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.separation_model_invalid");
        CHECK (refused.error().fields.at ("key") == c.key);
        CHECK (refused.error().fields.at ("actual") == c.actual);
    }
}

TEST_CASE ("a SeparationRun refuses a model that is not the expected graph without starting the helper", "[song][separation][run][model]")
{
    RunFixture f;
    auto wrong = overdub::separation::testing::separationModelMetadata();
    wrong["architecture"] = "htdemucs";
    f.tools.model = writeStandInModel (f.fixture->folder, wrong);

    const auto refused = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, SeparationRun::Clock::now());

    CHECK (refused.error().code == "song.separation_model_invalid");
    CHECK (f.log->starts.empty());
}

TEST_CASE ("destroying a running SeparationRun kills the helper and deletes its output folder", "[song][separation][run]")
{
    RunFixture f;
    const auto now = SeparationRun::Clock::now();
    auto run = SeparationRun::start (f.package(), f.sourceTrackId(), f.tools, now);
    REQUIRE (run.has_value());
    const auto outputFolder = (*run)->outputFolder();
    REQUIRE (outputFolder.createDirectory().wasOk());

    run->reset();

    CHECK (f.log->killed);
    CHECK_FALSE (outputFolder.exists());
}
