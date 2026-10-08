#include "song/BackingTrackImport.h"
#include "song/AudioFileDecode.h"
#include "song/SongPackage.h"

#include "EncodedAudioFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::song::Song;
using overdub::song::SongPackage;
using namespace overdub::song::testing;

namespace
{
// A fresh folder under the system temp folder holding a Song package and the files to import.
struct ImportFixture
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("overdub-import-test-" + juce::Uuid().toString());
    juce::File packageRoot = folder.getChildFile ("Song.overdub");

    ImportFixture() { folder.createDirectory(); }
    ~ImportFixture() { folder.deleteRecursively(); }

    juce::File source (const std::string& name) const { return folder.getChildFile ("sources").getChildFile (name); }

    std::unique_ptr<SongPackage> createSong (int sampleRate) const
    {
        auto created = SongPackage::create (packageRoot, overdub::song::makeEmptySong (sampleRate));
        REQUIRE (created.has_value());
        return std::move (*created);
    }
};

std::string importOrFail (SongPackage& package, const juce::File& source)
{
    auto imported = package.importBackingTrack (source);
    if (! imported.has_value())
        FAIL ("import failed: " << imported.error().code << " " << imported.error().fields.dump());
    return *imported;
}

const overdub::song::BackingTrack& backingTrackOf (const Song& song, const std::string& id)
{
    const auto found = std::ranges::find (song.backingTracks, id, &overdub::song::BackingTrack::id);
    REQUIRE (found != song.backingTracks.end());
    return *found;
}

// The derived file as playback reads it.
juce::AudioBuffer<float> derivedAudio (const SongPackage& package, const std::string& backingTrackId)
{
    const auto file = overdub::song::derivedFileFor (package.location(), backingTrackOf (package.song(), backingTrackId));
    auto audio = overdub::song::readWavFile (file);
    REQUIRE (audio.has_value());
    return std::move (*audio);
}

// Frequency from the interpolated positive-going zero crossings in [from, to).
double measuredFrequency (const float* samples, int from, int to, int sampleRate)
{
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    for (int i = from; i + 1 < to; ++i)
    {
        if (samples[i] < 0.0f && samples[i + 1] >= 0.0f)
        {
            const double at = i + (double) -samples[i] / (double) (samples[i + 1] - samples[i]);
            if (first < 0.0)
                first = at;
            last = at;
            ++crossings;
        }
    }
    REQUIRE (crossings > 2);
    return (crossings - 1) / (last - first) * sampleRate;
}
} // namespace

TEST_CASE ("a 48 kHz sine imported into a 44.1 kHz Song keeps its duration and frequency", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Sine.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (48000, 1000.0, 48000) }, 48000);
    auto package = fixture.createSong (44100);

    const auto id = importOrFail (*package, source);

    const auto& backingTrack = backingTrackOf (package->song(), id);
    CHECK (backingTrack.lengthSamples == 44100);
    CHECK (backingTrack.originalSampleRate == 48000);
    CHECK (backingTrack.channels == 1);
    const auto audio = derivedAudio (*package, id);
    REQUIRE (audio.getNumSamples() == 44100);
    REQUIRE (audio.getNumChannels() == 1);
    // Away from the edges, where the converter's filter starts and ends.
    CHECK (std::abs (measuredFrequency (audio.getReadPointer (0), 2000, 42100, 44100) - 1000.0) <= 0.01);
}

namespace
{
// Source clicks at 48 kHz, at multiples of 160 samples so each lands on a whole sample at 44.1 kHz
// (44100 / 48000 = 147 / 160).
const std::vector<std::int64_t> sourceClicks { 24000, 48160, 72320, 96480, 120640 };
constexpr std::int64_t sourceLength = 144000;

std::int64_t atSongRate (std::int64_t sourcePosition, int songRate)
{
    return sourcePosition * songRate / 48000;
}

// The largest |found - expected| over the clicks, where found is the loudest sample within
// ±2000 samples of where the click belongs.
std::int64_t worstClickError (const juce::AudioBuffer<float>& audio, int channel, int songRate)
{
    std::int64_t worst = 0;
    for (const auto click : sourceClicks)
    {
        const auto expected = atSongRate (click, songRate);
        const float* samples = audio.getReadPointer (channel);
        auto found = expected - 2000;
        for (auto i = expected - 2000; i <= expected + 2000; ++i)
            if (std::abs (samples[i]) > std::abs (samples[found]))
                found = i;
        worst = std::max (worst, std::abs (found - expected));
    }
    return worst;
}
} // namespace

TEST_CASE ("a click in a lossless file lands on its source position, sample-exact", "[song][import]")
{
    const auto format = GENERATE (FixtureFormat::wavFloat, FixtureFormat::wavInt16, FixtureFormat::aiffInt16, FixtureFormat::cafFloat, FixtureFormat::flac, FixtureFormat::alac);
    const auto songRate = GENERATE (48000, 44100);
    CAPTURE (nameOf (format), songRate);

    ImportFixture fixture;
    const auto source = fixture.source (std::string ("Clicks.") + extensionOf (format));
    writeEncodedFile (source, format, { clicks (sourceLength, sourceClicks) }, 48000);
    auto package = fixture.createSong (songRate);

    const auto id = importOrFail (*package, source);

    const auto audio = derivedAudio (*package, id);
    CHECK (audio.getNumSamples() == atSongRate (sourceLength, songRate));
    CHECK (backingTrackOf (package->song(), id).lengthSamples == atSongRate (sourceLength, songRate));
    CHECK (worstClickError (audio, 0, songRate) == 0);
}

// ADR 0005: "Imported AAC (M4A) and MP3 click position" is sample-exact, measured by this slice.
constexpr std::int64_t lossyClickTolerance = 0;

TEST_CASE ("a click in an AAC (M4A) file lands on its source position within ADR 0005's tolerance", "[song][import]")
{
    const auto songRate = GENERATE (48000, 44100);
    CAPTURE (songRate);
    ImportFixture fixture;
    const auto source = fixture.source ("Clicks.m4a");
    writeEncodedFile (source, FixtureFormat::aac, { clicks (sourceLength, sourceClicks) }, 48000);
    auto package = fixture.createSong (songRate);

    const auto id = importOrFail (*package, source);

    const auto audio = derivedAudio (*package, id);
    CHECK (audio.getNumSamples() == atSongRate (sourceLength, songRate)); // encoder priming trimmed
    CHECK (backingTrackOf (package->song(), id).formatName == "MPEG-4 AAC");
    CHECK (worstClickError (audio, 0, songRate) <= lossyClickTolerance);
}

TEST_CASE ("a click in an MP3 file lands on its source position within ADR 0005's tolerance", "[song][import]")
{
    const auto songRate = GENERATE (48000, 44100);
    CAPTURE (songRate);
    ImportFixture fixture;
    const auto source = fixture.source ("Clicks.mp3");
    if (! writeMp3File (source, { clicks (sourceLength, sourceClicks) }, 48000))
        SKIP ("lame is not installed (Core Audio has no MP3 encoder to make the fixture with): the MP3 half of ADR 0005's "
              "\"Backing Track import, AAC (M4A) and MP3\" tolerance row is left unverified");
    auto package = fixture.createSong (songRate);

    const auto id = importOrFail (*package, source);

    const auto audio = derivedAudio (*package, id);
    CHECK (audio.getNumSamples() == atSongRate (sourceLength, songRate)); // encoder delay trimmed
    CHECK (backingTrackOf (package->song(), id).formatName == "MPEG Layer 3");
    CHECK (worstClickError (audio, 0, songRate) <= lossyClickTolerance);
}

TEST_CASE ("a stereo file stays stereo with each channel in place", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Stereo.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { clicks (48000, { 1000 }), clicks (48000, { 3000 }, -0.25f) }, 48000);
    auto package = fixture.createSong (48000);

    const auto id = importOrFail (*package, source);

    CHECK (backingTrackOf (package->song(), id).channels == 2);
    const auto audio = derivedAudio (*package, id);
    REQUIRE (audio.getNumChannels() == 2);
    CHECK (audio.getSample (0, 1000) == 0.5f);
    CHECK (audio.getSample (0, 3000) == 0.0f);
    CHECK (audio.getSample (1, 1000) == 0.0f);
    CHECK (audio.getSample (1, 3000) == -0.25f);
}

TEST_CASE ("an import adds a Backing Track, a Track named after the file and one Clip at sample 0, and keeps the original", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix.aiff");
    writeEncodedFile (source, FixtureFormat::aiffInt16, { sine (4800, 440.0, 48000) }, 48000);
    auto package = fixture.createSong (48000);
    ScopedLogCapture capture;

    const auto id = importOrFail (*package, source);

    const auto& song = package->song();
    const auto& backingTrack = backingTrackOf (song, id);
    CHECK (backingTrack.originalFilename == "Band Mix.aiff");
    CHECK (backingTrack.originalExtension == "aiff");
    CHECK (backingTrack.originalSampleRate == 48000);
    CHECK (backingTrack.channels == 1);
    CHECK (backingTrack.formatName == "Linear PCM, 16 bit big-endian signed integer");
    CHECK (backingTrack.lengthSamples == 4800);

    REQUIRE (song.tracks.size() == 1);
    CHECK (song.tracks[0].name == "Band Mix");
    REQUIRE (song.tracks[0].clips.size() == 1);
    const auto& clip = song.tracks[0].clips[0];
    CHECK (clip.source == overdub::song::ClipSource { overdub::song::ClipSourceKind::backing, id });
    CHECK (clip.timelineStart == 0);
    CHECK (clip.sourceOffset == 0);
    CHECK (clip.length == 4800);

    const auto original = overdub::song::originalFileFor (package->location(), backingTrack);
    CHECK (original.getFileName() == juce::String (id) + ".orig.aiff");
    CHECK (original.hasIdenticalContentTo (source));
    CHECK (fixture.packageRoot.getChildFile ("backing").findChildFiles (juce::File::findFiles, false, "*.tmp").isEmpty());

    const auto events = capture.events();
    const auto imported = std::ranges::find_if (events, [] (const auto& e) { return e["event"] == "song.backing_track_imported"; });
    REQUIRE (imported != events.end());
    CHECK ((*imported)["lvl"] == "info");
    CHECK ((*imported)["backing_track_id"] == id);
    CHECK ((*imported)["length_samples"] == 4800);
}

TEST_CASE ("an import is one undo step that undo removes whole and redo brings back", "[song][import][history]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    auto package = fixture.createSong (48000);
    const auto before = package->song();

    const auto id = importOrFail (*package, source);
    const auto after = package->song();

    CHECK (package->undoLabel() == "Import Band Mix.wav");
    REQUIRE (package->undo().has_value());
    CHECK (package->song() == before);
    CHECK_FALSE (package->canUndo());

    REQUIRE (package->redo().has_value());
    CHECK (package->song() == after);
    CHECK (after.backingTracks.at (0).id == id);
}

TEST_CASE ("several files import as several Tracks, each with its Clip at sample 0", "[song][import]")
{
    ImportFixture fixture;
    const auto drums = fixture.source ("Drums.wav");
    const auto bass = fixture.source ("Bass.flac");
    writeEncodedFile (drums, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    writeEncodedFile (bass, FixtureFormat::flac, { sine (9600, 110.0, 48000) }, 48000);
    auto package = fixture.createSong (48000);

    const auto results = package->importBackingTracks ({ drums, bass });

    REQUIRE (results.size() == 2);
    REQUIRE (results[0].has_value());
    REQUIRE (results[1].has_value());
    const auto& song = package->song();
    REQUIRE (song.tracks.size() == 2);
    CHECK (song.tracks[0].name == "Drums");
    CHECK (song.tracks[1].name == "Bass");
    CHECK (song.tracks[0].clips.at (0).source.id == *results[0]);
    CHECK (song.tracks[1].clips.at (0).source.id == *results[1]);
    CHECK (song.tracks[0].clips.at (0).timelineStart == 0);
    CHECK (song.tracks[1].clips.at (0).timelineStart == 0);
    CHECK (song.tracks[1].clips.at (0).length == 9600);
    CHECK (package->undoLabel() == "Import Bass.flac");
}

TEST_CASE ("a 3-channel file is refused with an Error and leaves the Song and package unchanged", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Surround.wav");
    const auto channel = sine (4800, 440.0, 48000);
    writeEncodedFile (source, FixtureFormat::wavFloat, { channel, channel, channel }, 48000);
    auto package = fixture.createSong (48000);
    const auto before = package->song();

    const auto imported = package->importBackingTrack (source);

    REQUIRE_FALSE (imported.has_value());
    CHECK (imported.error().code == "song.import_too_many_channels");
    CHECK (imported.error().fields["channels"] == 3);
    CHECK (package->song() == before);
    CHECK_FALSE (package->canUndo());
    CHECK (fixture.packageRoot.getChildFile ("backing").getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
}

TEST_CASE ("a file Core Audio cannot open is refused with an Error and leaves the Song unchanged", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Notes.mp3");
    REQUIRE (source.getParentDirectory().createDirectory().wasOk());
    REQUIRE (source.replaceWithText ("not audio"));
    auto package = fixture.createSong (48000);
    const auto before = package->song();

    const auto imported = package->importBackingTrack (source);

    REQUIRE_FALSE (imported.has_value());
    CHECK (imported.error().code == "song.import_unreadable");
    CHECK (imported.error().fields["step"] == "open");
    CHECK (package->song() == before);
    CHECK (fixture.packageRoot.getChildFile ("backing").getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
}

TEST_CASE ("a failed history write leaves the Song unchanged and no Backing Track file behind", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    {
        auto package = fixture.createSong (48000);
    }
    overdub::song::PackageOptions options;
    options.beforeRename = [] (const juce::File& destination) -> std::optional<overdub::core::Error> {
        if (destination.getParentDirectory().getFileName() == "history")
            return overdub::core::Error { "song.write_failed", "injected", {} };
        return std::nullopt;
    };
    auto opened = SongPackage::open (fixture.packageRoot, options);
    REQUIRE (opened.has_value());
    auto& package = *opened->package;
    const auto before = package.song();

    const auto imported = package.importBackingTrack (source);

    REQUIRE_FALSE (imported.has_value());
    CHECK (imported.error().code == "song.write_failed");
    CHECK (package.song() == before);
    CHECK (fixture.packageRoot.getChildFile ("backing").getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
    CHECK (fixture.packageRoot.getChildFile ("peaks").getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
}

TEST_CASE ("a media write failure leaves the Song unchanged and no Backing Track file behind", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    {
        auto package = fixture.createSong (48000);
    }
    // The original copy (backing/<id>.orig.wav) or the derived WAV (backing/<id>.wav).
    const bool failOriginal = GENERATE (true, false);
    CAPTURE (failOriginal);
    overdub::song::PackageOptions options;
    options.beforeRename = [failOriginal] (const juce::File& destination) -> std::optional<overdub::core::Error> {
        if (destination.getParentDirectory().getFileName() == "backing" && destination.getFileName().contains (".orig") == failOriginal)
            return overdub::core::Error { "song.write_failed", "injected", {} };
        return std::nullopt;
    };
    auto opened = SongPackage::open (fixture.packageRoot, options);
    REQUIRE (opened.has_value());
    auto& package = *opened->package;
    const auto before = package.song();

    const auto imported = package.importBackingTrack (source);

    REQUIRE_FALSE (imported.has_value());
    CHECK (imported.error().code == "song.write_failed");
    CHECK (package.song() == before);
    CHECK_FALSE (package.canUndo());
    CHECK (fixture.packageRoot.getChildFile ("backing").getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
}

TEST_CASE ("a song.json write failure after the step is committed is reported but the import stands", "[song][import][history]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    {
        auto package = fixture.createSong (48000);
    }
    overdub::song::PackageOptions options;
    options.beforeRename = [] (const juce::File& destination) -> std::optional<overdub::core::Error> {
        if (destination.getFileName() == "song.json")
            return overdub::core::Error { "song.write_failed", "injected", {} };
        return std::nullopt;
    };
    auto opened = SongPackage::open (fixture.packageRoot, options);
    REQUIRE (opened.has_value());
    auto& package = *opened->package;
    ScopedLogCapture capture;

    const auto imported = package.importBackingTrack (source);

    REQUIRE_FALSE (imported.has_value());
    CHECK (imported.error().code == "song.write_failed");
    REQUIRE (package.song().backingTracks.size() == 1);
    const auto backingTrack = package.song().backingTracks[0];
    CHECK (package.song().tracks.size() == 1);
    CHECK (package.undoLabel() == "Import Band Mix.wav");
    CHECK (package.hasPendingWrite());
    CHECK (overdub::song::derivedFileFor (package.location(), backingTrack).existsAsFile());
    CHECK (overdub::song::originalFileFor (package.location(), backingTrack).existsAsFile());

    const auto events = capture.events();
    const auto logged = std::ranges::find_if (events, [] (const auto& e) { return e["event"] == "song.backing_track_imported"; });
    REQUIRE (logged != events.end());
    CHECK ((*logged)["backing_track_id"] == backingTrack.id);
}

TEST_CASE ("a file with no extension is kept as backing/<id>.orig and rebuilds from it on open", "[song][import]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    std::string id;
    {
        auto package = fixture.createSong (48000);
        id = importOrFail (*package, source);
        const auto& backingTrack = backingTrackOf (package->song(), id);
        CHECK (backingTrack.originalExtension.empty());
        CHECK (package->song().tracks.at (0).name == "Band Mix");
        const auto original = overdub::song::originalFileFor (package->location(), backingTrack);
        CHECK (original.getFileName() == juce::String (id) + ".orig");
        CHECK (original.hasIdenticalContentTo (source));
        REQUIRE (overdub::song::derivedFileFor (package->location(), backingTrack).deleteFile());
    }

    auto opened = SongPackage::open (fixture.packageRoot);

    REQUIRE (opened.has_value());
    CHECK_FALSE (opened->package->isBackingTrackMissing (id));
    CHECK (derivedAudio (*opened->package, id).getNumSamples() == 4800);
}

TEST_CASE ("on open a missing derived file is rebuilt from the original", "[song][import][open]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Clicks.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { clicks (sourceLength, sourceClicks) }, 48000);
    std::string id;
    juce::File derived;
    {
        auto package = fixture.createSong (44100);
        id = importOrFail (*package, source);
        derived = overdub::song::derivedFileFor (package->location(), backingTrackOf (package->song(), id));
    }
    REQUIRE (derived.deleteFile());
    ScopedLogCapture capture;

    auto opened = SongPackage::open (fixture.packageRoot);

    REQUIRE (opened.has_value());
    CHECK (opened->notices.empty());
    CHECK_FALSE (opened->package->isBackingTrackMissing (id));
    const auto audio = derivedAudio (*opened->package, id);
    CHECK (audio.getNumSamples() == atSongRate (sourceLength, 44100));
    CHECK (worstClickError (audio, 0, 44100) == 0);

    const auto events = capture.events();
    const auto rebuilt = std::ranges::find_if (events, [] (const auto& e) { return e["event"] == "song.backing_track_rebuilt"; });
    REQUIRE (rebuilt != events.end());
    CHECK ((*rebuilt)["lvl"] == "info");
    CHECK ((*rebuilt)["backing_track_id"] == id);
    CHECK ((*rebuilt)["path"] == derived.getFullPathName().toStdString());
}

TEST_CASE ("on open a Backing Track with neither file is marked missing with a Notice and the Song is kept", "[song][import][open]")
{
    ImportFixture fixture;
    const auto source = fixture.source ("Band Mix.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, { sine (4800, 440.0, 48000) }, 48000);
    std::string id;
    Song imported;
    {
        auto package = fixture.createSong (48000);
        id = importOrFail (*package, source);
        imported = package->song();
        const auto& backingTrack = backingTrackOf (imported, id);
        REQUIRE (overdub::song::derivedFileFor (package->location(), backingTrack).deleteFile());
        REQUIRE (overdub::song::originalFileFor (package->location(), backingTrack).deleteFile());
    }
    ScopedLogCapture capture;

    auto opened = SongPackage::open (fixture.packageRoot);

    REQUIRE (opened.has_value());
    CHECK (opened->package->song() == imported);
    CHECK (opened->package->isBackingTrackMissing (id));
    REQUIRE (opened->notices.size() == 1);
    CHECK (opened->notices[0].code == "song.backing_track_missing");

    const auto events = capture.events();
    const auto missing = std::ranges::find_if (events, [] (const auto& e) { return e["event"] == "song.backing_track_missing"; });
    REQUIRE (missing != events.end());
    CHECK ((*missing)["lvl"] == "warn");
    CHECK ((*missing)["backing_track_id"] == id);
    CHECK ((*missing)["cause"] == "song.read_failed");
}
