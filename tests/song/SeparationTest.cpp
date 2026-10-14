#include "song/Separation.h"
#include "song/BackingTrackImport.h"
#include "song/Peaks.h"
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
using namespace overdub::song;
using namespace overdub::song::testing;

namespace
{
const std::vector<std::string> partTrackNames { "Bass", "Drums", "Other", "Vocals", "Guitar", "Piano" };

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

nlohmann::json firstEvent (const ScopedLogCapture& capture, const std::string& name)
{
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            return event;
    FAIL ("no event " << name);
    return {};
}
} // namespace

TEST_CASE ("a Separation adds six Tracks named after the parts directly below the source Track, which is muted", "[song][separation]")
{
    const auto before = handBuiltSong();

    const auto separated = withSeparation (before, "track-mix", sixParts());

    REQUIRE (separated.has_value());
    CHECK (trackNames (*separated) == std::vector<std::string> { "Band Mix", "Bass", "Drums", "Other", "Vocals", "Guitar", "Piano", "Vocals", "Click" });
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

    const auto separated = withSeparation (before, "track-mix", parts);

    REQUIRE (separated.has_value());
    std::vector<Id> clipIds;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        const auto& track = separated->tracks.at (i + 1);
        INFO ("Track " << track.name);
        CHECK (track.name == partTrackNames[i]);
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
    song.tracks.push_back (Track { .id = "track-mixed",
                                   .name = "Mixed",
                                   .clips = { Clip { .id = "clip-5", .source = { ClipSourceKind::backing, "backing-mix" } }, Clip { .id = "clip-6", .source = { ClipSourceKind::backing, "backing-click" } } } });

    CHECK (separableBackingTrack (song, "track-mix") == Id ("backing-mix"));
    CHECK (separableBackingTrack (song, "track-click") == Id ("backing-click"));
    CHECK (separableBackingTrack (song, "track-gone").error().code == "song.track_not_found");
    for (const auto& [trackId, reason] : std::vector<std::pair<Id, std::string>> { { "track-empty", "no_clips" }, { "track-vocals", "not_one_backing_track" }, { "track-mixed", "not_one_backing_track" } })
    {
        INFO ("Track " << trackId);
        const auto refused = separableBackingTrack (song, trackId);
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.track_not_separable");
        CHECK (refused.error().fields.at ("reason") == reason);
        CHECK (withSeparation (song, trackId, sixParts()).error().code == "song.track_not_separable");
    }
    CHECK (withSeparation (song, "track-mix", { sixParts().front() }).error().code == "song.separation_parts_invalid");
}

TEST_CASE ("a Separation's undo step is labelled with the source Backing Track's original file name", "[song][separation]")
{
    CHECK (separationStepLabel (handBuiltSong().backingTracks.front()) == "Separate Band Mix.mp3");
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
    CHECK (trackNames (song) == std::vector<std::string> { "Band Mix", "Bass", "Drums", "Other", "Vocals", "Guitar", "Piano" });
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
        CHECK (trackNamed (song, partTrackNames[i]).clips.front().source.id == backingTrack->id);
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

TEST_CASE ("a helper that fails, or ends without every part, leaves the Song unchanged", "[song][separation][run]")
{
    const auto script = GENERATE (FakeHelperScript { .exitCode = 1, .errorLine = R"({"error":"separation.inference_failed","message":"out of memory"})" },
                                  FakeHelperScript { .writeParts = false });
    RunFixture f (script);
    const auto before = f.package().song();
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
        CHECK (ended.error().fields.at ("helper_error") == "separation.inference_failed");
        CHECK (ended.error().fields.at ("helper_message") == "out of memory");
    }
    else
    {
        CHECK (ended.error().fields.at ("reason") == "part_missing");
    }
    CHECK (f.package().song() == before);
    CHECK_FALSE (f.package().canRedo());
    CHECK_FALSE (outputFolder.exists());
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
