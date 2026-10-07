#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Audio fixtures for import tests, generated and encoded inside the test (ADR 0005: no golden
// files). Lossless and AAC files are encoded with ExtAudioFile. Core Audio has no MP3 encoder, so
// MP3 fixtures are encoded by the `lame` command-line tool when it is installed.
namespace overdub::song::testing
{
using Signal = std::vector<std::vector<float>>; // one vector per channel, all the same length

enum class FixtureFormat
{
    wavFloat,
    wavInt16,
    aiffInt16,
    cafFloat,
    flac,
    alac,
    aac
};

inline const char* extensionOf (FixtureFormat format)
{
    switch (format)
    {
        case FixtureFormat::wavFloat:
        case FixtureFormat::wavInt16:
            return "wav";
        case FixtureFormat::aiffInt16:
            return "aiff";
        case FixtureFormat::cafFloat:
            return "caf";
        case FixtureFormat::flac:
            return "flac";
        case FixtureFormat::alac:
        case FixtureFormat::aac:
            return "m4a";
    }
    return "";
}

inline const char* nameOf (FixtureFormat format)
{
    switch (format)
    {
        case FixtureFormat::wavFloat:
            return "WAV float";
        case FixtureFormat::wavInt16:
            return "WAV 16-bit";
        case FixtureFormat::aiffInt16:
            return "AIFF 16-bit";
        case FixtureFormat::cafFloat:
            return "CAF float";
        case FixtureFormat::flac:
            return "FLAC";
        case FixtureFormat::alac:
            return "ALAC";
        case FixtureFormat::aac:
            return "AAC";
    }
    return "";
}

inline AudioStreamBasicDescription linearPcm (double sampleRate, int channels, bool isFloat, bool bigEndian, bool nonInterleaved)
{
    const UInt32 bytes = isFloat ? 4 : 2;
    AudioStreamBasicDescription description {};
    description.mSampleRate = sampleRate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags = (isFloat ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagIsPacked
                             | (bigEndian ? kAudioFormatFlagIsBigEndian : 0) | (nonInterleaved ? kAudioFormatFlagIsNonInterleaved : 0);
    description.mBitsPerChannel = bytes * 8;
    description.mChannelsPerFrame = (UInt32) channels;
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = nonInterleaved ? bytes : bytes * (UInt32) channels;
    description.mBytesPerPacket = description.mBytesPerFrame;
    return description;
}

inline AudioStreamBasicDescription compressed (AudioFormatID formatId, UInt32 flags, double sampleRate, int channels)
{
    AudioStreamBasicDescription description {};
    description.mSampleRate = sampleRate;
    description.mFormatID = formatId;
    description.mFormatFlags = flags;
    description.mChannelsPerFrame = (UInt32) channels;
    UInt32 size = sizeof (description);
    REQUIRE (AudioFormatGetProperty (kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &description) == noErr);
    return description;
}

// Encodes `signal` at `sampleRate` into `file` in `format`.
inline void writeEncodedFile (const juce::File& file, FixtureFormat format, const Signal& signal, int sampleRate)
{
    REQUIRE (! signal.empty());
    const int channels = (int) signal.size();
    AudioFileTypeID fileType = kAudioFileWAVEType;
    AudioStreamBasicDescription fileFormat {};
    switch (format)
    {
        case FixtureFormat::wavFloat:
            fileType = kAudioFileWAVEType;
            fileFormat = linearPcm (sampleRate, channels, true, false, false);
            break;
        case FixtureFormat::wavInt16:
            fileType = kAudioFileWAVEType;
            fileFormat = linearPcm (sampleRate, channels, false, false, false);
            break;
        case FixtureFormat::aiffInt16:
            fileType = kAudioFileAIFFType;
            fileFormat = linearPcm (sampleRate, channels, false, true, false);
            break;
        case FixtureFormat::cafFloat:
            fileType = kAudioFileCAFType;
            fileFormat = linearPcm (sampleRate, channels, true, false, false);
            break;
        case FixtureFormat::flac:
            fileType = kAudioFileFLACType;
            fileFormat = compressed (kAudioFormatFLAC, kAppleLosslessFormatFlag_16BitSourceData, sampleRate, channels);
            break;
        case FixtureFormat::alac:
            fileType = kAudioFileM4AType;
            fileFormat = compressed (kAudioFormatAppleLossless, kAppleLosslessFormatFlag_16BitSourceData, sampleRate, channels);
            break;
        case FixtureFormat::aac:
            fileType = kAudioFileM4AType;
            fileFormat = compressed (kAudioFormatMPEG4AAC, 0, sampleRate, channels);
            break;
    }

    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    file.deleteFile();
    const auto path = file.getFullPathName();
    const auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(), (CFIndex) path.getNumBytesAsUTF8(), false);
    ExtAudioFileRef audioFile = nullptr;
    const auto created = ExtAudioFileCreateWithURL (url, fileType, &fileFormat, nullptr, kAudioFileFlags_EraseFile, &audioFile);
    CFRelease (url);
    REQUIRE (created == noErr);

    auto clientFormat = linearPcm (sampleRate, channels, true, false, true);
    REQUIRE (ExtAudioFileSetProperty (audioFile, kExtAudioFileProperty_ClientDataFormat, sizeof (clientFormat), &clientFormat) == noErr);

    const auto length = signal[0].size();
    std::vector<std::byte> listStorage (offsetof (AudioBufferList, mBuffers) + sizeof (AudioBuffer) * (std::size_t) channels);
    auto* list = reinterpret_cast<AudioBufferList*> (listStorage.data());
    list->mNumberBuffers = (UInt32) channels;
    constexpr std::size_t chunk = 4096;
    for (std::size_t start = 0; start < length; start += chunk)
    {
        const auto frames = std::min (chunk, length - start);
        for (int c = 0; c < channels; ++c)
        {
            list->mBuffers[c].mNumberChannels = 1;
            list->mBuffers[c].mDataByteSize = (UInt32) (frames * sizeof (float));
            list->mBuffers[c].mData = const_cast<float*> (signal[(std::size_t) c].data() + start);
        }
        REQUIRE (ExtAudioFileWrite (audioFile, (UInt32) frames, list) == noErr);
    }
    REQUIRE (ExtAudioFileDispose (audioFile) == noErr);
}

// The `lame` MP3 encoder, if installed.
inline std::optional<juce::File> findLame()
{
    for (const auto* candidate : { "/usr/local/bin/lame", "/opt/homebrew/bin/lame", "/usr/bin/lame" })
        if (juce::File (candidate).existsAsFile())
            return juce::File (candidate);
    return std::nullopt;
}

// Encodes `signal` as a 320 kbit/s CBR MP3 (with the LAME header that carries the encoder delay)
// through a 16-bit WAV. Returns false when lame is not installed.
inline bool writeMp3File (const juce::File& file, const Signal& signal, int sampleRate)
{
    const auto lame = findLame();
    if (! lame.has_value())
        return false;
    const auto wav = file.getSiblingFile (file.getFileNameWithoutExtension() + "-lame-input.wav");
    writeEncodedFile (wav, FixtureFormat::wavInt16, signal, sampleRate);
    juce::ChildProcess process;
    REQUIRE (process.start (juce::StringArray { lame->getFullPathName(), "--quiet", "-b", "128", wav.getFullPathName(), file.getFullPathName() }));
    process.waitForProcessToFinish (60000);
    REQUIRE (process.getExitCode() == 0);
    wav.deleteFile();
    REQUIRE (file.existsAsFile());
    return true;
}

// `length` samples of silence with `amplitude` at each position in `clicks`.
inline std::vector<float> clicks (std::int64_t length, const std::vector<std::int64_t>& positions, float amplitude = 0.5f)
{
    std::vector<float> samples ((std::size_t) length, 0.0f);
    for (const auto position : positions)
        samples[(std::size_t) position] = amplitude;
    return samples;
}

inline std::vector<float> sine (std::int64_t length, double frequency, int sampleRate, float amplitude = 0.5f)
{
    std::vector<float> samples ((std::size_t) length);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * (double) i / sampleRate);
    return samples;
}
} // namespace overdub::song::testing
