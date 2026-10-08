#include "song/Peaks.h"
#include "song/AudioFileDecode.h"
#include "song/SongPackage.h"

#include "EncodedAudioFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::song::ClipSource;
using overdub::song::ClipSourceKind;
using overdub::song::PeakRange;
using overdub::song::Peaks;
using overdub::song::SongPackage;
using namespace overdub::song::testing;

namespace
{
// 2.5 blocks of 256 samples. In block b, channel 0 runs a ramp (k - 128) / 128 * a_b over
// k = 0..255 and channel 1 is its negation, with a_b = 0.25 (b + 1). So per block, channel 0 has
// min -a_b and max a_b * 127 / 128, and channel 1 min -a_b * 127 / 128 and max a_b. The last block
// is cut after k = 127, so its max is a_b * (127 - 128) / 128 = -a_b / 128 on channel 0.
constexpr int signalLength = 640;

juce::AudioBuffer<float> rampSignal()
{
    juce::AudioBuffer<float> audio (2, signalLength);
    for (int i = 0; i < signalLength; ++i)
    {
        const float amplitude = 0.25f * (float) (i / 256 + 1);
        const float value = (float) (i % 256 - 128) / 128.0f * amplitude;
        audio.setSample (0, i, value);
        audio.setSample (1, i, -value);
    }
    return audio;
}

Peaks expectedRampPeaks()
{
    return Peaks {
        .lengthSamples = signalLength,
        .channels = {
            { PeakRange { -0.25f, 0.25f * 127.0f / 128.0f }, PeakRange { -0.5f, 0.5f * 127.0f / 128.0f }, PeakRange { -0.75f, -0.75f / 128.0f } },
            { PeakRange { -0.25f * 127.0f / 128.0f, 0.25f }, PeakRange { -0.5f * 127.0f / 128.0f, 0.5f }, PeakRange { 0.75f / 128.0f, 0.75f } },
        },
    };
}

Signal channelsOf (const juce::AudioBuffer<float>& audio)
{
    Signal signal;
    for (int c = 0; c < audio.getNumChannels(); ++c)
        signal.emplace_back (audio.getReadPointer (c), audio.getReadPointer (c) + audio.getNumSamples());
    return signal;
}

struct PackageFixture
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("overdub-peaks-test-" + juce::Uuid().toString());
    juce::File packageRoot = folder.getChildFile ("Song.overdub");

    PackageFixture() { folder.createDirectory(); }
    ~PackageFixture() { folder.deleteRecursively(); }
};

// A 48 kHz Song package with the ramp signal imported as a Backing Track.
std::unique_ptr<SongPackage> songWithImportedRamp (const PackageFixture& fixture, std::string& backingTrackId)
{
    const auto source = fixture.folder.getChildFile ("Ramp.wav");
    writeEncodedFile (source, FixtureFormat::wavFloat, channelsOf (rampSignal()), 48000);
    auto created = SongPackage::create (fixture.packageRoot, overdub::song::makeEmptySong (48000));
    REQUIRE (created.has_value());
    auto imported = (*created)->importBackingTrack (source);
    REQUIRE (imported.has_value());
    backingTrackId = *imported;
    return std::move (*created);
}

nlohmann::json onlyEvent (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    REQUIRE (found.size() == 1);
    return found[0];
}
} // namespace

TEST_CASE ("peaks hold the min and max of every 256 samples of every channel", "[song][peaks]")
{
    CHECK (overdub::song::computePeaks (rampSignal()) == expectedRampPeaks());
}

TEST_CASE ("peaks built from chunks of any size equal the peaks of the whole audio", "[song][peaks]")
{
    const auto audio = rampSignal();
    // Chunk sizes that straddle the 256-sample peak boundaries in different places.
    for (const int chunk : { 1, 7, 100, 256, 300, signalLength })
    {
        overdub::song::PeaksBuilder builder { 2 };
        for (int start = 0; start < signalLength; start += chunk)
        {
            const auto count = std::min (chunk, signalLength - start);
            const float* channels[] = { audio.getReadPointer (0, start), audio.getReadPointer (1, start) };
            builder.append (channels, count);
        }
        CAPTURE (chunk);
        CHECK (builder.finish() == expectedRampPeaks());
    }
}

TEST_CASE ("peaks survive their file encoding", "[song][peaks]")
{
    const auto decoded = overdub::song::decodePeaks (overdub::song::encodePeaks (expectedRampPeaks()));

    REQUIRE (decoded.has_value());
    CHECK (*decoded == expectedRampPeaks());
}

TEST_CASE ("a peaks file that is truncated or not a peaks file is corrupt", "[song][peaks]")
{
    const auto bytes = overdub::song::encodePeaks (expectedRampPeaks());
    auto badMagic = bytes;
    badMagic[0] = 'X';

    for (const auto& corrupt : { bytes.substr (0, bytes.size() - 1), bytes.substr (0, 10), badMagic, bytes + "x" })
    {
        const auto decoded = overdub::song::decodePeaks (corrupt);
        REQUIRE_FALSE (decoded.has_value());
        CHECK (decoded.error().code == "song.peaks_corrupt");
    }
}

TEST_CASE ("an import writes the Backing Track's peaks", "[song][peaks][import]")
{
    PackageFixture fixture;
    std::string id;
    auto package = songWithImportedRamp (fixture, id);
    ScopedLogCapture capture;

    const auto peaks = package->peaks (ClipSource { ClipSourceKind::backing, id });

    REQUIRE (peaks.has_value());
    CHECK (*peaks == expectedRampPeaks());
    CHECK (overdub::song::peaksFileFor (fixture.packageRoot, id).existsAsFile());
    CHECK (std::ranges::none_of (capture.events(), [] (const auto& e) { return e["event"] == "song.peaks_rebuilt"; }));
}

TEST_CASE ("a deleted peaks file is rebuilt on next access and the rebuild is logged", "[song][peaks]")
{
    PackageFixture fixture;
    std::string id;
    auto package = songWithImportedRamp (fixture, id);
    const auto file = overdub::song::peaksFileFor (fixture.packageRoot, id);
    REQUIRE (file.deleteFile());
    ScopedLogCapture capture;

    const auto peaks = package->peaks (ClipSource { ClipSourceKind::backing, id });

    REQUIRE (peaks.has_value());
    CHECK (*peaks == expectedRampPeaks());
    CHECK (file.existsAsFile());
    const auto rebuilt = onlyEvent (capture, "song.peaks_rebuilt");
    CHECK (rebuilt["lvl"] == "info");
    CHECK (rebuilt["media_id"] == id);
    CHECK (rebuilt["reason"] == "missing");
    CHECK (rebuilt["path"] == file.getFullPathName().toStdString());
}

TEST_CASE ("a corrupt peaks file is rebuilt on next access and the rebuild is logged", "[song][peaks]")
{
    PackageFixture fixture;
    std::string id;
    auto package = songWithImportedRamp (fixture, id);
    const auto file = overdub::song::peaksFileFor (fixture.packageRoot, id);
    REQUIRE (file.replaceWithText ("garbage"));
    ScopedLogCapture capture;

    const auto peaks = package->peaks (ClipSource { ClipSourceKind::backing, id });

    REQUIRE (peaks.has_value());
    CHECK (*peaks == expectedRampPeaks());
    CHECK (onlyEvent (capture, "song.peaks_rebuilt")["reason"] == "corrupt");
    const auto reread = package->peaks (ClipSource { ClipSourceKind::backing, id });
    REQUIRE (reread.has_value());
    CHECK (*reread == expectedRampPeaks());
}

TEST_CASE ("a Take's peaks come from the same writer and are rebuilt from its file", "[song][peaks]")
{
    PackageFixture fixture;
    auto song = overdub::song::makeEmptySong (48000);
    song.takes.push_back (overdub::song::Take { .id = "take-1", .file = "audio/take-1.wav", .channels = 2, .lengthSamples = signalLength });
    auto created = SongPackage::create (fixture.packageRoot, song);
    REQUIRE (created.has_value());
    REQUIRE (overdub::song::writeFloatWavAtomically (fixture.packageRoot.getChildFile ("audio/take-1.wav"), rampSignal(), 48000, {}).has_value());

    // Recording at Pass end writes the peaks with writePeaksFile.
    const auto file = overdub::song::peaksFileFor (fixture.packageRoot, "take-1");
    REQUIRE (overdub::song::writePeaksFile (file, rampSignal(), {}).has_value());
    const auto written = (*created)->peaks (ClipSource { ClipSourceKind::take, "take-1" });
    REQUIRE (written.has_value());
    CHECK (*written == expectedRampPeaks());

    REQUIRE (file.deleteFile());
    const auto rebuilt = (*created)->peaks (ClipSource { ClipSourceKind::take, "take-1" });
    REQUIRE (rebuilt.has_value());
    CHECK (*rebuilt == expectedRampPeaks());
}

TEST_CASE ("peaks of media the Song does not have are an Error", "[song][peaks]")
{
    PackageFixture fixture;
    auto created = SongPackage::create (fixture.packageRoot, overdub::song::makeEmptySong (48000));
    REQUIRE (created.has_value());

    const auto peaks = (*created)->peaks (ClipSource { ClipSourceKind::backing, "nope" });

    REQUIRE_FALSE (peaks.has_value());
    CHECK (peaks.error().code == "song.media_unknown");
    CHECK (peaks.error().fields["source_id"] == "nope");
}

TEST_CASE ("peaks are never part of history: undo and redo of an import leave the peaks file alone", "[song][peaks][history]")
{
    PackageFixture fixture;
    std::string id;
    auto package = songWithImportedRamp (fixture, id);
    const auto file = overdub::song::peaksFileFor (fixture.packageRoot, id);
    const auto before = file.getLastModificationTime();

    REQUIRE (package->undo().has_value());
    REQUIRE (package->redo().has_value());

    CHECK (file.existsAsFile());
    CHECK (file.getLastModificationTime() == before);
    const auto historyFiles = fixture.packageRoot.getChildFile ("history").findChildFiles (juce::File::findFiles, false);
    CHECK (std::ranges::none_of (historyFiles, [] (const juce::File& f) { return f.loadFileAsString().contains ("peaks"); }));
}
