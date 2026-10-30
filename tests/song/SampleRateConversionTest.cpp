#include "song/SampleRateConversion.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "song/BackingTrackImport.h"
#include "song/Peaks.h"
#include "song/SongPackage.h"

#include "AudioMeasurements.h"
#include "FullSong.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

using overdub::core::ScopedLogCapture;
using overdub::song::BackingTrack;
using overdub::song::Clip;
using overdub::song::ClipSource;
using overdub::song::ClipSourceKind;
using overdub::song::ConvertedMedia;
using overdub::song::convertedSamples;
using overdub::song::convertedSong;
using overdub::song::SampleCount;
using overdub::song::SampleRateConversionRun;
using overdub::song::Song;
using overdub::song::SongPackage;
using overdub::song::Take;
using namespace overdub::song::testing;

namespace
{
// A 44.1 kHz Song package in a temp folder with one Backing Track (a 1 kHz sine whose original is
// at 48 kHz) and one Take of a single click, each on its own Track.
struct ConversionFixture
{
    static constexpr int songRate = 44100;
    static constexpr int originalRate = 48000;
    static constexpr int clickAt = 22050; // half a second into the Take, at the Song's rate
    static constexpr SampleCount takeLength = songRate;
    static constexpr SampleCount takeOrigin = 88200;

    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-conversion-" + juce::Uuid().toString());
    std::unique_ptr<SongPackage> package;
    std::string backingTrackId;
    std::string takeId = overdub::song::makeId();

    ConversionFixture()
    {
        REQUIRE (folder.createDirectory().wasOk());
        auto created = SongPackage::create (folder.getChildFile ("Song.overdub"), overdub::song::makeEmptySong (songRate));
        REQUIRE (created.has_value());
        package = std::move (*created);

        juce::AudioBuffer<float> sine (1, originalRate * 2);
        for (int i = 0; i < sine.getNumSamples(); ++i)
            sine.setSample (0, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / originalRate));
        const auto sineFile = folder.getChildFile ("Sine.wav");
        REQUIRE (overdub::song::writeFloatWavAtomically (sineFile, sine, originalRate, {}).has_value());
        auto imported = package->importBackingTrack (sineFile);
        REQUIRE (imported.has_value());
        backingTrackId = *imported;

        juce::AudioBuffer<float> click (1, (int) takeLength);
        click.clear();
        click.setSample (0, clickAt, 1.0f);
        REQUIRE (overdub::song::writeFloatWavAtomically (package->location().getChildFile ("audio/" + takeId + ".wav"), click, songRate, {}).has_value());

        auto song = package->song();
        song.passes.push_back ({ .id = "pass-1", .startSample = takeOrigin, .recordedAt = "2026-10-18T10:00:00Z" });
        song.takes.push_back ({ .id = takeId, .trackId = "track-guitar", .passId = "pass-1", .file = "audio/" + takeId + ".wav", .channels = 1, .lengthSamples = takeLength, .origin = takeOrigin });
        overdub::song::Track guitar;
        guitar.id = "track-guitar";
        guitar.name = "Guitar";
        guitar.clips.push_back ({ .id = "clip-take", .source = { ClipSourceKind::take, takeId }, .timelineStart = takeOrigin, .sourceOffset = 0, .length = takeLength });
        song.tracks.push_back (guitar);
        song.settings.playhead = 44100;
        REQUIRE (package->edit (song, "Record Pass 1").has_value());
    }

    ~ConversionFixture()
    {
        package.reset();
        folder.deleteRecursively();
    }

    // Runs a conversion to the end, as the window's timer would.
    overdub::core::Expected<void> convert (int toRate)
    {
        auto started = SampleRateConversionRun::start (*package, toRate, SampleRateConversionRun::Clock::now());
        REQUIRE (started.has_value());
        auto& run = **started;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto ended = run.advance (SampleRateConversionRun::Clock::now()))
                return *ended;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        FAIL ("the conversion did not end");
        return {};
    }

    std::vector<juce::File> mediaFiles() const
    {
        std::vector<juce::File> files;
        for (const auto* folderName : { "audio", "backing", "peaks" })
            for (const auto& entry : juce::RangedDirectoryIterator (package->location().getChildFile (folderName), false, "*", juce::File::findFiles))
                files.push_back (entry.getFile());
        std::ranges::sort (files, {}, &juce::File::getFullPathName);
        return files;
    }

    juce::AudioBuffer<float> read (const juce::File& file) const
    {
        auto audio = overdub::song::readWavFile (file);
        REQUIRE (audio.has_value());
        return std::move (*audio);
    }

    const Take& onlyTake() const
    {
        REQUIRE (package->song().takes.size() == 1);
        return package->song().takes.front();
    }

    const BackingTrack& onlyBackingTrack() const
    {
        REQUIRE (package->song().backingTracks.size() == 1);
        return package->song().backingTracks.front();
    }
};

int loudestSample (const juce::AudioBuffer<float>& audio)
{
    int loudest = 0;
    for (int i = 0; i < audio.getNumSamples(); ++i)
        if (std::abs (audio.getSample (0, i)) > std::abs (audio.getSample (0, loudest)))
            loudest = i;
    return loudest;
}

std::vector<overdub::core::Fields> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<overdub::core::Fields> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

//==============================================================================
// The converted Song

TEST_CASE ("a sample count converts to the nearest sample at the new rate", "[song][conversion]")
{
    CHECK (convertedSamples (44100, 44100, 48000) == 48000);
    CHECK (convertedSamples (88150, 44100, 48000) == 95946);
    CHECK (convertedSamples (-50, 44100, 48000) == -54);
    CHECK (convertedSamples (48000, 48000, 44100) == 44100);
    CHECK (convertedSamples (0, 44100, 96000) == 0);
}

TEST_CASE ("converting a Song scales every sample position and length to the new rate", "[song][conversion]")
{
    const auto song = fullyPopulatedSong(); // 44.1 kHz
    const auto converted = convertedSong (song, 48000, {});

    CHECK (converted.sampleRate == 48000);
    CHECK (converted.settings.playhead == 144000);
    CHECK (converted.passes[0].startSample == 96000);
    CHECK (converted.passes[1].startSample == 192000);

    CHECK (converted.takes[0].id == "take-1"); // no new media: missing media keeps its id
    CHECK (converted.takes[0].lengthSamples == 480000);
    CHECK (converted.takes[0].origin == 95946);
    CHECK (converted.takes[0].compensation.reportedLatencySamples == 139);
    CHECK (converted.takes[0].compensation.calibrationOffsetSamples == -54);
    CHECK (converted.backingTracks[0].lengthSamples == 8640000);

    CHECK (converted.videoTakes[0].startSample == 96000);
    CHECK (converted.videoTakes[0].calibrationOffsetSamples == 1920);
    CHECK (std::abs (converted.videoTakes[0].songSamplesPerMovieSecond - 44100.25 * 48000.0 / 44100.0) < 1e-9);

    const auto& clip = converted.tracks[0].clips[0]; // clip-b: 88200 + 440000 from offset 50
    CHECK (clip.id == "clip-b");
    CHECK (clip.timelineStart == 96000);
    CHECK (clip.sourceOffset == 54);
    CHECK (clip.length == 574912 - 96000);
    CHECK (clip.fadeInLength == 480);
    CHECK (clip.fadeOutLength == 960);

    // Nothing else changes.
    CHECK (converted.tracks.size() == song.tracks.size());
    CHECK (converted.tracks[0].insertChain == song.tracks[0].insertChain);
    CHECK (converted.master == song.master);
    CHECK (converted.settings.exportVideoTakeId == song.settings.exportVideoTakeId);
}

TEST_CASE ("converting a Song points its Clips and Separation origins at the new media", "[song][conversion]")
{
    const auto song = fullyPopulatedSong();
    ConvertedMedia media;
    auto take = song.takes[0];
    take.id = "take-1-48k";
    take.file = "audio/take-1-48k.wav";
    take.lengthSamples = 480000;
    media.takes["take-1"] = take;
    auto backing = song.backingTracks[0];
    backing.id = "backing-1-48k";
    backing.lengthSamples = 8640000;
    media.backingTracks["backing-1"] = backing;

    const auto converted = convertedSong (song, 48000, media);
    CHECK (converted.takes[0].id == "take-1-48k");
    CHECK (converted.takes[0].file == "audio/take-1-48k.wav");
    CHECK (converted.takes[0].origin == 95946);
    CHECK (converted.takes[1].id == "take-2");
    CHECK (converted.tracks[0].clips[0].source == ClipSource { ClipSourceKind::take, "take-1-48k" });
    CHECK (converted.tracks[0].clips[1].source == ClipSource { ClipSourceKind::take, "take-2" });
    CHECK (converted.backingTracks[0].id == "backing-1-48k");
    CHECK (converted.backingTracks[1].separatedFrom.backingTrackId == "backing-1-48k");
}

TEST_CASE ("Clips that touch still touch after a conversion, and a Clip stays inside its media", "[song][conversion]")
{
    Song song = overdub::song::makeEmptySong (44100);
    song.takes.push_back ({ .id = "take", .file = "audio/take.wav", .lengthSamples = 1001 });
    overdub::song::Track track;
    track.id = "track";
    track.clips.push_back ({ .id = "first", .source = { ClipSourceKind::take, "take" }, .timelineStart = 1, .sourceOffset = 0, .length = 500 });
    track.clips.push_back ({ .id = "second", .source = { ClipSourceKind::take, "take" }, .timelineStart = 501, .sourceOffset = 500, .length = 501, .fadeInLength = 400, .fadeOutLength = 400 });
    song.tracks.push_back (track);

    ConvertedMedia media;
    media.takes["take"] = { .id = "take-48k", .file = "audio/take-48k.wav", .lengthSamples = 1088 }; // one short of 1089.4
    const auto converted = convertedSong (song, 48000, media);
    const auto& first = converted.tracks[0].clips[0];
    const auto& second = converted.tracks[0].clips[1];

    CHECK (first.timelineStart + first.length == second.timelineStart);
    CHECK (second.sourceOffset == 544);
    CHECK (second.sourceOffset + second.length == 1088);
    CHECK (second.fadeInLength + second.fadeOutLength <= second.length);
    CHECK (second.fadeInLength == 435);
}

TEST_CASE ("an undo or redo across a conversion keeps the playhead at the same time", "[song][conversion][history]")
{
    auto at48k = convertedSong (fullyPopulatedSong(), 48000, {});
    at48k.settings.playhead = 96000;
    const auto restored = overdub::song::keepStateOutsideHistory (fullyPopulatedSong(), at48k);
    CHECK (restored.sampleRate == 44100);
    CHECK (restored.settings.playhead == 88200);
}

//==============================================================================
// Converting a package

TEST_CASE ("a conversion resamples a Take, keeping its click on the converted sample", "[song][conversion]")
{
    ConversionFixture f;
    REQUIRE (f.convert (48000).has_value());

    const auto& take = f.onlyTake();
    CHECK (take.id != f.takeId);
    CHECK (take.file == "audio/" + take.id + ".wav");
    CHECK (take.lengthSamples == 48000);
    CHECK (take.origin == 96000);
    const auto audio = f.read (f.package->location().getChildFile (take.file));
    CHECK (audio.getNumSamples() == 48000);
    CHECK (loudestSample (audio) == convertedSamples (ConversionFixture::clickAt, 44100, 48000));
    CHECK (overdub::song::peaksFileFor (f.package->location(), take.id).existsAsFile());

    const auto& clip = f.package->song().tracks.back().clips.front();
    CHECK (clip.source.id == take.id);
    CHECK (clip.timelineStart == 96000);
    CHECK (clip.length == 48000);
}

TEST_CASE ("a conversion derives a Backing Track again from its original, so it is not resampled twice", "[song][conversion]")
{
    ConversionFixture f;
    const auto originalFile = overdub::song::originalFileFor (f.package->location(), f.onlyBackingTrack());
    REQUIRE (f.convert (48000).has_value());

    const auto& backingTrack = f.onlyBackingTrack();
    CHECK (backingTrack.id != f.backingTrackId);
    CHECK (backingTrack.originalFilename == "Sine.wav");
    CHECK (backingTrack.originalSampleRate == 48000);
    CHECK (backingTrack.lengthSamples == 96000);
    CHECK (overdub::song::originalFileFor (f.package->location(), backingTrack).hasIdenticalContentTo (originalFile));

    // The original is at 48 kHz: derived at 48 kHz it is the original, sample for sample.
    const auto derived = f.read (overdub::song::derivedFileFor (f.package->location(), backingTrack));
    const auto original = f.read (originalFile);
    REQUIRE (derived.getNumSamples() == original.getNumSamples());
    CHECK (std::equal (derived.getReadPointer (0), derived.getReadPointer (0) + derived.getNumSamples(), original.getReadPointer (0)));
    CHECK (std::abs (measuredFrequency (derived.getReadPointer (0), 2000, 94000, 48000) - 1000.0) <= 0.01);
}

TEST_CASE ("a conversion is one undo step: undo restores the Song and its files exactly, redo applies it again", "[song][conversion][history]")
{
    ConversionFixture f;
    const auto before = f.package->song();
    const auto filesBefore = f.mediaFiles();
    REQUIRE (f.convert (48000).has_value());
    const auto after = f.package->song();
    CHECK (f.package->undoLabel() == "Convert to 48000 Hz");

    REQUIRE (f.package->undo().has_value());
    CHECK (f.package->song() == before);
    for (const auto& file : filesBefore)
        CHECK (file.existsAsFile());

    REQUIRE (f.package->redo().has_value());
    CHECK (f.package->song() == after);
    CHECK (f.read (f.package->location().getChildFile (f.onlyTake().file)).getNumSamples() == 48000);
}

TEST_CASE ("a conversion logs its rates, files and duration", "[song][conversion]")
{
    ConversionFixture f;
    ScopedLogCapture capture;
    REQUIRE (f.convert (48000).has_value());

    const auto started = eventsNamed (capture, "song.sample_rate_conversion_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0].at ("from_rate") == 44100);
    CHECK (started[0].at ("to_rate") == 48000);
    CHECK (started[0].at ("files") == 2);
    const auto converted = eventsNamed (capture, "song.sample_rate_converted");
    REQUIRE (converted.size() == 1);
    CHECK (converted[0].at ("files") == 2);
    CHECK (converted[0].contains ("duration_ms"));
}

TEST_CASE ("a conversion to the Song's own rate or to no rate is refused", "[song][conversion]")
{
    ConversionFixture f;
    const auto same = SampleRateConversionRun::start (*f.package, 44100, SampleRateConversionRun::Clock::now());
    REQUIRE_FALSE (same.has_value());
    CHECK (same.error().code == "song.sample_rate_unchanged");
    const auto none = SampleRateConversionRun::start (*f.package, 0, SampleRateConversionRun::Clock::now());
    REQUIRE_FALSE (none.has_value());
    CHECK (none.error().code == "song.sample_rate_invalid");
}

TEST_CASE ("a cancelled conversion leaves the Song and its files as they were", "[song][conversion]")
{
    ConversionFixture f;
    const auto before = f.package->song();
    const auto filesBefore = f.mediaFiles();
    ScopedLogCapture capture;

    auto started = SampleRateConversionRun::start (*f.package, 48000, SampleRateConversionRun::Clock::now());
    REQUIRE (started.has_value());
    (*started)->cancel (SampleRateConversionRun::Clock::now());

    CHECK_FALSE ((*started)->advance (SampleRateConversionRun::Clock::now()).has_value());
    CHECK (f.package->song() == before);
    CHECK (f.mediaFiles() == filesBefore);
    CHECK (eventsNamed (capture, "song.sample_rate_conversion_cancelled").size() == 1);
}

TEST_CASE ("a Take that cannot be read fails the conversion and leaves the Song and its files as they were", "[song][conversion]")
{
    ConversionFixture f;
    REQUIRE (f.package->location().getChildFile (f.onlyTake().file).replaceWithText ("not audio"));
    const auto before = f.package->song();
    const auto filesBefore = f.mediaFiles();

    const auto converted = f.convert (48000);
    REQUIRE_FALSE (converted.has_value());
    CHECK (converted.error().code == "song.conversion_failed");
    CHECK (converted.error().fields.at ("media_id") == f.takeId);
    CHECK (converted.error().fields.at ("step") == "decode");
    CHECK (f.package->song() == before);
    CHECK (f.mediaFiles() == filesBefore);
}

TEST_CASE ("a Take whose file is missing keeps its id and stays missing after a conversion", "[song][conversion]")
{
    ConversionFixture f;
    REQUIRE (f.package->location().getChildFile (f.onlyTake().file).deleteFile());
    REQUIRE (f.convert (48000).has_value());
    CHECK (f.onlyTake().id == f.takeId);
    CHECK (f.onlyTake().lengthSamples == 48000);
}

TEST_CASE ("a Backing Track resampled by a conversion keeps its frequency within ADR 0005's tolerance", "[song][conversion]")
{
    ConversionFixture f; // its original is a 1 kHz sine at 48 kHz
    REQUIRE (f.convert (88200).has_value());

    const auto& backingTrack = f.onlyBackingTrack();
    CHECK (backingTrack.lengthSamples == 176400);
    const auto derived = f.read (overdub::song::derivedFileFor (f.package->location(), backingTrack));
    REQUIRE (derived.getNumSamples() == 176400);
    CHECK (std::abs (measuredFrequency (derived.getReadPointer (0), 4000, 172000, 88200) - 1000.0) <= 0.01);
}
