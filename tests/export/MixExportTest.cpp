#include "export/MixExport.h"
#include "export/MixFileWriter.h"

#include "core/Log.h"
#include "engine/EngineFixture.h"
#include "engine/TestProcessors.h"
#include "song/AudioFileDecode.h"
#include "ui/ExportSheet.h"

#include <AudioToolbox/AudioToolbox.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// Export to WAV and M4A (issue #37), as round trips (ADR 0005): a mix with a click at a known
// sample is exported through the real offline render and ExtAudioFile, then decoded with the
// import decoder and measured.
using overdub::exporting::ExportFormat;
using overdub::exporting::ExportJob;
using overdub::exporting::ExportRequest;
using overdub::exporting::MixRender;
using namespace overdub::engine::testing;
namespace core = overdub::core;
namespace song = overdub::song;

namespace
{
constexpr song::SampleCount clickAt = 12345;
constexpr song::SampleCount clipLength = 48000;

// One Track playing a 1 s Take with a single click (0.8) at `clickAt`.
song::Song clickSong (const TempPackage& package)
{
    auto mix = song::makeEmptySong (testSampleRate);
    const auto take = addTake (mix, package, "take-click", impulse (clipLength, clickAt, 0.8f));
    mix.tracks = { trackWith ("click", { clipOf ("clip-click", take, 0, 0, clipLength) }) };
    return mix;
}

std::int64_t loudestSample (const juce::AudioBuffer<float>& audio, int channel)
{
    const auto* samples = audio.getReadPointer (channel);
    const auto loudest = std::max_element (samples, samples + audio.getNumSamples(), [] (float a, float b) { return std::abs (a) < std::abs (b); });
    return loudest - samples;
}

std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            found.push_back (event);
    return found;
}

// The file's data format as Core Audio reads it.
AudioStreamBasicDescription fileDataFormat (const juce::File& file)
{
    const auto path = file.getFullPathName();
    const auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(), (CFIndex) path.getNumBytesAsUTF8(), false);
    ExtAudioFileRef ref = nullptr;
    const auto opened = ExtAudioFileOpenURL (url, &ref);
    CFRelease (url);
    REQUIRE (opened == noErr);
    AudioStreamBasicDescription format {};
    UInt32 size = sizeof (format);
    REQUIRE (ExtAudioFileGetProperty (ref, kExtAudioFileProperty_FileDataFormat, &size, &format) == noErr);
    ExtAudioFileDispose (ref);
    return format;
}

// The encoder bit rate the file reports (AAC: the esds average bit rate), in bits per second.
UInt32 fileBitRate (const juce::File& file)
{
    const auto path = file.getFullPathName();
    const auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(), (CFIndex) path.getNumBytesAsUTF8(), false);
    AudioFileID audioFile = nullptr;
    const auto opened = AudioFileOpenURL (url, kAudioFileReadPermission, 0, &audioFile);
    CFRelease (url);
    REQUIRE (opened == noErr);
    UInt32 bitRate = 0;
    UInt32 size = sizeof (bitRate);
    REQUIRE (AudioFileGetProperty (audioFile, kAudioFilePropertyBitRate, &size, &bitRate) == noErr);
    AudioFileClose (audioFile);
    return bitRate;
}
} // namespace

TEST_CASE ("a WAV export is 24-bit at the Song rate, as long as the Song span, with the click at its sample", "[export][wav]")
{
    TempPackage package;
    const auto mix = clickSong (package);
    MixRender render { mix, package.root, 256, {} };
    const auto destination = package.root.getChildFile ("exports/Click Song.wav");
    REQUIRE (destination.getParentDirectory().createDirectory().wasOk());

    core::ScopedLogCapture capture;
    const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::wav, destination });
    REQUIRE (exported.has_value());
    CHECK (exported->file == destination);
    CHECK (exported->samples == clipLength);
    REQUIRE (destination.existsAsFile());
    CHECK (destination.getParentDirectory().findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no).size() == 1); // no partial file left

    const auto format = fileDataFormat (destination);
    CHECK (format.mFormatID == kAudioFormatLinearPCM);
    CHECK ((format.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0);
    CHECK (format.mBitsPerChannel == 24);
    CHECK ((int) format.mSampleRate == testSampleRate);
    CHECK (format.mChannelsPerFrame == 2);

    const auto decoded = song::decodeAudioFile (destination, testSampleRate);
    REQUIRE (decoded.has_value());
    REQUIRE (decoded->audio.getNumSamples() == clipLength);
    for (int channel = 0; channel < 2; ++channel)
    {
        CAPTURE (channel);
        CHECK (loudestSample (decoded->audio, channel) == clickAt);
        CHECK (std::abs (decoded->audio.getSample (channel, (int) clickAt) - 0.8f * centrePanGain) < 1.0e-6f);
        CHECK (nonZeroSamples ({ decoded->audio.getReadPointer (channel), decoded->audio.getReadPointer (channel) + clipLength }) == std::vector<std::int64_t> { clickAt });
    }

    const auto started = eventsNamed (capture, "export.started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["format"] == "wav");
    CHECK (started[0]["samples"] == clipLength);
    CHECK (started[0]["sample_rate"] == testSampleRate);
    CHECK (started[0]["path"] == destination.getFullPathName().toStdString());
    const auto finished = eventsNamed (capture, "export.finished");
    REQUIRE (finished.size() == 1);
    CHECK (finished[0]["format"] == "wav");
    CHECK (finished[0]["samples"] == clipLength);
    CHECK (finished[0]["speed"].get<double>() > 1.0); // faster than real time
}

TEST_CASE ("an export into a folder that does not exist fails with export.folder_missing and makes no folder", "[export][folder]")
{
    TempPackage package;
    const auto mix = clickSong (package);
    MixRender render { mix, package.root, 256, {} };
    const auto folder = package.root.getChildFile ("gone");

    const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::wav, folder.getChildFile ("Click Song.wav") });

    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.folder_missing");
    CHECK (exported.error().fields.at ("path") == folder.getFullPathName().toStdString());
    CHECK (! folder.exists());
}

TEST_CASE ("an M4A export is AAC and decodes to the Song span's length with the click at its sample", "[export][m4a]")
{
    TempPackage package;
    const auto mix = clickSong (package);
    MixRender render { mix, package.root, 256, {} };
    const auto destination = package.root.getChildFile ("Click Song.m4a");

    const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::m4a, destination });
    REQUIRE (exported.has_value());
    REQUIRE (destination.existsAsFile());

    const auto format = fileDataFormat (destination);
    CHECK (format.mFormatID == kAudioFormatMPEG4AAC);
    CHECK ((int) format.mSampleRate == testSampleRate);
    CHECK (format.mChannelsPerFrame == 2);

    // ExtAudioFile trims the encoder priming and the padding at the end.
    const auto decoded = song::decodeAudioFile (destination, testSampleRate);
    REQUIRE (decoded.has_value());
    CHECK (decoded->audio.getNumSamples() == clipLength);
    for (int channel = 0; channel < 2; ++channel)
        CHECK (loudestSample (decoded->audio, channel) == clickAt);
}

TEST_CASE ("an M4A export of loud noise averages 256 kbit/s", "[export][m4a]")
{
    TempPackage package;
    auto mix = song::makeEmptySong (testSampleRate);
    juce::Random random (37);
    std::vector<std::vector<float>> noise (2, std::vector<float> (4 * testSampleRate));
    for (auto& channel : noise)
        for (auto& sample : channel)
            sample = random.nextFloat() - 0.5f;
    const auto take = addMultichannelTake (mix, package, "take-noise", noise);
    mix.tracks = { trackWith ("noise", { clipOf ("clip-noise", take, 0, 0, take.lengthSamples) }) };
    MixRender render { mix, package.root, 256, {} };
    const auto destination = package.root.getChildFile ("noise.m4a");
    REQUIRE (overdub::exporting::exportMix (render, ExportRequest { ExportFormat::m4a, destination }).has_value());

    // The encoder's long-term average follows the requested rate (128 kbit/s measures about 133).
    const auto bitRate = (double) fileBitRate (destination);
    CHECK (bitRate > 0.9 * overdub::exporting::aacBitRate);
    CHECK (bitRate < 1.1 * overdub::exporting::aacBitRate);
}

TEST_CASE ("export progress rises to 1 block by block", "[export][progress]")
{
    TempPackage package;
    const auto mix = clickSong (package);
    MixRender render { mix, package.root, 256, {} };
    std::vector<double> seen;
    const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::wav, package.root.getChildFile ("progress.wav") }, [&seen] (double fraction) { seen.push_back (fraction); });
    REQUIRE (exported.has_value());

    REQUIRE (seen.size() == (std::size_t) ((clipLength + 255) / 256));
    CHECK (std::ranges::is_sorted (seen));
    CHECK (seen.front() > 0.0);
    CHECK (seen.front() < 0.01);
    CHECK (seen.back() >= 1.0);
}

TEST_CASE ("a stopped export, an empty Song and MP4 leave no file", "[export][errors]")
{
    TempPackage package;

    SECTION ("stopped part way")
    {
        const auto mix = clickSong (package);
        MixRender render { mix, package.root, 256, {} };
        const auto destination = package.root.getChildFile ("stopped.wav");
        int blocks = 0;
        const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::wav, destination }, {}, [&blocks] { return ++blocks > 10; });
        REQUIRE (! exported.has_value());
        CHECK (exported.error().code == "export.cancelled");
        CHECK (! destination.exists());
        CHECK (! destination.getSiblingFile (".stopped.wav.partial").exists());
    }

    SECTION ("a Song without Clips")
    {
        MixRender render { song::makeEmptySong (testSampleRate), package.root, 256, {} };
        const auto destination = package.root.getChildFile ("empty.wav");
        const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::wav, destination });
        REQUIRE (! exported.has_value());
        CHECK (exported.error().code == "export.nothing_to_export");
        CHECK (! destination.exists());
    }

    SECTION ("MP4, which is not an audio file (exportMovie writes it)")
    {
        const auto mix = clickSong (package);
        MixRender render { mix, package.root, 256, {} };
        const auto destination = package.root.getChildFile ("movie.mp4");
        const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::mp4, destination });
        REQUIRE (! exported.has_value());
        CHECK (exported.error().code == "export.format_unavailable");
        CHECK (! destination.exists());
    }
}

TEST_CASE ("an export job writes the file on its own thread and reports when it is done", "[export][job]")
{
    TempPackage package;
    const auto mix = clickSong (package);
    const auto destination = package.root.getChildFile ("job.m4a");
    ExportJob job { std::make_unique<MixRender> (mix, package.root, 256, overdub::engine::InsertFactory {}), ExportRequest { ExportFormat::m4a, destination } };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (! job.isDone() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    REQUIRE (job.isDone());
    CHECK (job.progress() >= 1.0);
    auto result = job.takeResult();
    REQUIRE (result.has_value());
    REQUIRE (result->has_value());
    CHECK ((*result)->file == destination);
    CHECK (destination.existsAsFile());
    CHECK (! job.takeResult().has_value()); // taken once
}

TEST_CASE ("the export format names, extensions and default file", "[export][format]")
{
    using overdub::exporting::defaultExportFile;
    using overdub::exporting::withFormatExtension;
    const juce::File music ("/Users/someone/Music/Overdub");

    CHECK (withFormatExtension (music.getChildFile ("Song.wav"), ExportFormat::m4a) == music.getChildFile ("Song.m4a"));
    CHECK (withFormatExtension (music.getChildFile ("Song"), ExportFormat::wav) == music.getChildFile ("Song.wav"));
    CHECK (defaultExportFile (music, "My Song", ExportFormat::wav) == music.getChildFile ("My Song.wav"));
    CHECK (defaultExportFile (music, "", ExportFormat::m4a) == music.getChildFile ("Untitled.m4a"));
    CHECK (defaultExportFile (music, "My Song", ExportFormat::mp4) == music.getChildFile ("My Song.mp4"));
}

TEST_CASE ("a Song without Clips whose Master chain reports a tail: the page and the export agree it is not exported", "[export][span]")
{
    TempPackage package;
    auto mix = song::makeEmptySong (testSampleRate);
    mix.tracks = { trackWith ("empty", {}) };
    const overdub::engine::InsertFactory masterEcho = [] (const song::Id& trackId, const std::vector<song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> chain;
        if (trackId.empty())
            chain.push_back (std::make_unique<EchoProcessor> (480, testSampleRate));
        return chain;
    };

    MixRender render { mix, package.root, 256, masterEcho };
    REQUIRE (render.tailSamples() == 480); // the tail alone is non-zero: this is the case of issue #95
    const auto destination = package.root.getChildFile ("tail-only.wav");

    core::ScopedLogCapture capture;
    const auto exported = overdub::exporting::exportMix (render, ExportRequest { ExportFormat::wav, destination });

    REQUIRE (! exported.has_value());
    CHECK (exported.error().code == "export.nothing_to_export");
    CHECK (! destination.exists());
    CHECK (eventsNamed (capture, "export.started").empty());
    CHECK (overdub::ui::exportSpanText (overdub::exporting::lastClipEnd (mix), render.sampleRate()) == "Nothing to export: the Song has no Clips");
}
