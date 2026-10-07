#include "song/AudioFileDecode.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <AudioToolbox/AudioToolbox.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace overdub::song
{
namespace
{
core::Error unreadable (const juce::File& file, const char* step, OSStatus status)
{
    return core::Error {
        "song.import_unreadable",
        std::string ("could not decode the audio file (") + step + ")",
        { { "path", pathOf (file) }, { "step", step }, { "os_status", (std::int64_t) status } },
    };
}

// Owns an open ExtAudioFile.
struct ExtAudioFileHandle
{
    ExtAudioFileRef ref = nullptr;

    ~ExtAudioFileHandle()
    {
        if (ref != nullptr)
            ExtAudioFileDispose (ref);
    }
};

std::string formatNameOf (AudioStreamBasicDescription format)
{
    // The name of the data format alone, not of this file's rate and channels.
    format.mSampleRate = 0;
    format.mChannelsPerFrame = 0;
    CFStringRef name = nullptr;
    UInt32 size = sizeof (name);
    if (AudioFormatGetProperty (kAudioFormatProperty_FormatName, sizeof (format), &format, &size, &name) != noErr || name == nullptr)
        return {};
    const auto text = juce::String::fromCFString (name).toStdString();
    CFRelease (name);
    return text;
}

// Float, non-interleaved, at `sampleRate`: what the decoder hands back.
AudioStreamBasicDescription clientFormatFor (int sampleRate, int channels)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = (UInt32) channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof (float);
    format.mBytesPerPacket = sizeof (float);
    return format;
}

// Sets the sample rate converter to maximum quality. Only a rate change has a sample rate
// converter: at the same rate the converter refuses these properties and there is nothing to set.
core::Expected<void> useMaximumConverterQuality (const juce::File& file, ExtAudioFileRef audioFile, double fileRate, int clientRate)
{
    if (fileRate == (double) clientRate)
        return {};

    AudioConverterRef converter = nullptr;
    UInt32 size = sizeof (converter);
    if (const auto status = ExtAudioFileGetProperty (audioFile, kExtAudioFileProperty_AudioConverter, &size, &converter); status != noErr)
        return std::unexpected (unreadable (file, "get_converter", status));
    if (converter == nullptr)
        return {};

    const UInt32 quality = kAudioConverterQuality_Max;
    if (const auto status = AudioConverterSetProperty (converter, kAudioConverterSampleRateConverterQuality, sizeof (quality), &quality); status != noErr)
        return std::unexpected (unreadable (file, "set_converter_quality", status));
    const UInt32 complexity = kAudioConverterSampleRateConverterComplexity_Mastering;
    if (const auto status = AudioConverterSetProperty (converter, kAudioConverterSampleRateConverterComplexity, sizeof (complexity), &complexity); status != noErr)
        return std::unexpected (unreadable (file, "set_converter_complexity", status));

    // ExtAudioFile applies converter changes only when its converter config is set again.
    CFArrayRef config = nullptr;
    if (const auto status = ExtAudioFileSetProperty (audioFile, kExtAudioFileProperty_ConverterConfig, sizeof (config), &config); status != noErr)
        return std::unexpected (unreadable (file, "apply_converter_config", status));
    return {};
}

// Reads every frame ExtAudioFile delivers at the client format.
core::Expected<std::vector<std::vector<float>>> readAllFrames (const juce::File& file, ExtAudioFileRef audioFile, int channels)
{
    constexpr UInt32 chunkFrames = 16384;
    std::vector<std::vector<float>> decoded ((std::size_t) channels);
    std::vector<std::vector<float>> chunk ((std::size_t) channels, std::vector<float> (chunkFrames));
    std::vector<std::byte> listStorage (offsetof (AudioBufferList, mBuffers) + sizeof (AudioBuffer) * (std::size_t) channels);
    auto* list = reinterpret_cast<AudioBufferList*> (listStorage.data());

    for (;;)
    {
        list->mNumberBuffers = (UInt32) channels;
        for (int c = 0; c < channels; ++c)
        {
            list->mBuffers[c].mNumberChannels = 1;
            list->mBuffers[c].mDataByteSize = chunkFrames * sizeof (float);
            list->mBuffers[c].mData = chunk[(std::size_t) c].data();
        }
        UInt32 frames = chunkFrames;
        if (const auto status = ExtAudioFileRead (audioFile, &frames, list); status != noErr)
            return std::unexpected (unreadable (file, "read", status));
        if (frames == 0)
            return decoded;
        for (int c = 0; c < channels; ++c)
            decoded[(std::size_t) c].insert (decoded[(std::size_t) c].end(), chunk[(std::size_t) c].begin(), chunk[(std::size_t) c].begin() + frames);
    }
}

} // namespace

core::Expected<DecodedAudio> decodeAudioFile (const juce::File& file, int sampleRate)
{
    const auto path = file.getFullPathName();
    const auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(), (CFIndex) path.getNumBytesAsUTF8(), false);
    ExtAudioFileHandle audioFile;
    const auto opened = ExtAudioFileOpenURL (url, &audioFile.ref);
    CFRelease (url);
    if (opened != noErr)
        return std::unexpected (unreadable (file, "open", opened));

    AudioStreamBasicDescription fileFormat {};
    UInt32 size = sizeof (fileFormat);
    if (const auto status = ExtAudioFileGetProperty (audioFile.ref, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat); status != noErr)
        return std::unexpected (unreadable (file, "get_file_format", status));

    const int channels = (int) fileFormat.mChannelsPerFrame;
    if (channels > maxImportChannels)
        return std::unexpected (core::Error {
            "song.import_too_many_channels",
            "only mono and stereo files can be imported",
            { { "path", pathOf (file) }, { "channels", channels } },
        });
    if (channels < 1)
        return std::unexpected (unreadable (file, "channel_count", 0));

    auto clientFormat = clientFormatFor (sampleRate, channels);
    if (const auto status = ExtAudioFileSetProperty (audioFile.ref, kExtAudioFileProperty_ClientDataFormat, sizeof (clientFormat), &clientFormat); status != noErr)
        return std::unexpected (unreadable (file, "set_client_format", status));
    if (auto quality = useMaximumConverterQuality (file, audioFile.ref, fileFormat.mSampleRate, sampleRate); ! quality.has_value())
        return std::unexpected (quality.error());

    auto frames = readAllFrames (file, audioFile.ref, channels);
    if (! frames.has_value())
        return std::unexpected (frames.error());

    DecodedAudio decoded;
    decoded.originalSampleRate = (int) std::lround (fileFormat.mSampleRate);
    decoded.channels = channels;
    decoded.formatName = formatNameOf (fileFormat);
    decoded.audio.setSize (channels, (int) (*frames)[0].size());
    for (int c = 0; c < channels; ++c)
        decoded.audio.copyFrom (c, 0, (*frames)[(std::size_t) c].data(), decoded.audio.getNumSamples());
    return decoded;
}

core::Expected<void> writeFloatWavAtomically (const juce::File& destination,
                                              const juce::AudioBuffer<float>& audio,
                                              int sampleRate,
                                              const BeforeRenameHook& beforeRename)
{
    const auto tempFile = tempFileFor (destination);
    tempFile.deleteFile(); // FileOutputStream appends to an existing file
    {
        auto fileStream = std::make_unique<juce::FileOutputStream> (tempFile);
        if (! fileStream->openedOk())
            return std::unexpected (writeFailed (tempFile, "open", 0, fileStream->getStatus().getErrorMessage().toStdString()));
        // Owned by the writer below, which outlives every use of this pointer.
        const auto* openedStream = fileStream.get();
        std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);

        juce::WavAudioFormat wav;
        const auto options = juce::AudioFormatWriterOptions {}
                                 .withSampleRate (sampleRate)
                                 .withNumChannels (audio.getNumChannels())
                                 .withBitsPerSample (32)
                                 .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = wav.createWriterFor (stream, options);
        if (writer == nullptr)
            return std::unexpected (writeFailed (tempFile, "open", 0, "the WAV writer refused the format"));
        if (! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()) || ! writer->flush())
            return std::unexpected (writeFailed (tempFile, "write", 0, openedStream->getStatus().getErrorMessage().toStdString()));
    }
    return renameIntoPlace (tempFile, destination, beforeRename);
}

core::Expected<juce::AudioBuffer<float>> readWavFile (const juce::File& file)
{
    const auto failed = [&] { return std::unexpected (core::Error { "song.read_failed", "could not read a package file", { { "path", pathOf (file) } } }); };
    if (! file.existsAsFile())
        return failed();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader { wav.createReaderFor (new juce::FileInputStream (file), true) };
    if (reader == nullptr || reader->numChannels < 1)
        return failed();

    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    if (! reader->read (&audio, 0, audio.getNumSamples(), 0, true, true))
        return failed();
    return audio;
}
} // namespace overdub::song
