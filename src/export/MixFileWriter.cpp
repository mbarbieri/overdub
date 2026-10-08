#include "export/MixFileWriter.h"

#include <AudioToolbox/AudioToolbox.h>

#include <cstddef>
#include <vector>

namespace overdub::exporting
{
namespace
{
core::Error writeFailed (const juce::File& file, const char* step, OSStatus status)
{
    return core::Error {
        "export.write_failed",
        std::string ("could not write the exported mix (") + step + ")",
        { { "path", file.getFullPathName().toStdString() }, { "step", step }, { "os_status", (std::int64_t) status } },
    };
}

AudioStreamBasicDescription wav24 (int sampleRate, int numChannels)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 24;
    format.mChannelsPerFrame = (UInt32) numChannels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 3 * (UInt32) numChannels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}

// Float, non-interleaved: what write() is handed.
AudioStreamBasicDescription floatClient (int sampleRate, int numChannels)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = (UInt32) numChannels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof (float);
    format.mBytesPerPacket = sizeof (float);
    return format;
}
} // namespace

struct MixFileWriter::Handle
{
    ExtAudioFileRef ref = nullptr;

    ~Handle()
    {
        if (ref != nullptr)
            ExtAudioFileDispose (ref);
    }
};

MixFileWriter::MixFileWriter (juce::File destination, std::unique_ptr<Handle> openHandle, int channels)
    : file (std::move (destination)), handle (std::move (openHandle)), numChannels (channels)
{
}

MixFileWriter::~MixFileWriter() = default;

core::Expected<std::unique_ptr<MixFileWriter>> MixFileWriter::create (const juce::File& file, ExportFormat format, int sampleRate, int numChannels)
{
    if (format != ExportFormat::wav && format != ExportFormat::m4a)
        return std::unexpected (core::Error { "export.format_unavailable", "this format is not an audio file", { { "format", formatName (format) } } });

    AudioFileTypeID fileType = kAudioFileWAVEType;
    AudioStreamBasicDescription fileFormat = wav24 (sampleRate, numChannels);
    if (format == ExportFormat::m4a)
    {
        fileType = kAudioFileM4AType;
        fileFormat = {};
        fileFormat.mSampleRate = sampleRate;
        fileFormat.mFormatID = kAudioFormatMPEG4AAC;
        fileFormat.mChannelsPerFrame = (UInt32) numChannels;
        UInt32 size = sizeof (fileFormat);
        if (const auto status = AudioFormatGetProperty (kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &fileFormat); status != noErr)
            return std::unexpected (writeFailed (file, "create", status));
    }

    auto handle = std::make_unique<Handle>();
    const auto path = file.getFullPathName();
    const auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path.toRawUTF8(), (CFIndex) path.getNumBytesAsUTF8(), false);
    const auto created = ExtAudioFileCreateWithURL (url, fileType, &fileFormat, nullptr, kAudioFileFlags_EraseFile, &handle->ref);
    CFRelease (url);
    if (created != noErr)
    {
        handle->ref = nullptr;
        return std::unexpected (writeFailed (file, "create", created));
    }

    const auto client = floatClient (sampleRate, numChannels);
    if (const auto status = ExtAudioFileSetProperty (handle->ref, kExtAudioFileProperty_ClientDataFormat, sizeof (client), &client); status != noErr)
        return std::unexpected (writeFailed (file, "client_format", status));

    if (format == ExportFormat::m4a)
    {
        // The encoder's bit rate is set on ExtAudioFile's converter, then the converter is told to
        // take the new configuration (a null config array).
        AudioConverterRef converter = nullptr;
        UInt32 size = sizeof (converter);
        if (const auto status = ExtAudioFileGetProperty (handle->ref, kExtAudioFileProperty_AudioConverter, &size, &converter); status != noErr)
            return std::unexpected (writeFailed (file, "bit_rate", status));
        UInt32 bitRate = aacBitRate;
        if (const auto status = AudioConverterSetProperty (converter, kAudioConverterEncodeBitRate, sizeof (bitRate), &bitRate); status != noErr)
            return std::unexpected (writeFailed (file, "bit_rate", status));
        CFArrayRef config = nullptr;
        if (const auto status = ExtAudioFileSetProperty (handle->ref, kExtAudioFileProperty_ConverterConfig, sizeof (config), &config); status != noErr)
            return std::unexpected (writeFailed (file, "bit_rate", status));
    }

    return std::unique_ptr<MixFileWriter> (new MixFileWriter (file, std::move (handle), numChannels));
}

core::Expected<void> MixFileWriter::write (const float* const* channels, int numSamples)
{
    if (handle == nullptr || handle->ref == nullptr)
        return std::unexpected (writeFailed (file, "write", kAudio_FileNotFoundError));

    std::vector<std::byte> listStorage (offsetof (AudioBufferList, mBuffers) + sizeof (AudioBuffer) * (std::size_t) numChannels);
    auto* list = reinterpret_cast<AudioBufferList*> (listStorage.data());
    list->mNumberBuffers = (UInt32) numChannels;
    for (int c = 0; c < numChannels; ++c)
    {
        list->mBuffers[c].mNumberChannels = 1;
        list->mBuffers[c].mDataByteSize = (UInt32) ((std::size_t) numSamples * sizeof (float));
        list->mBuffers[c].mData = const_cast<float*> (channels[c]);
    }
    if (const auto status = ExtAudioFileWrite (handle->ref, (UInt32) numSamples, list); status != noErr)
        return std::unexpected (writeFailed (file, "write", status));
    return {};
}

core::Expected<void> MixFileWriter::close()
{
    if (handle == nullptr || handle->ref == nullptr)
        return {};
    const auto status = ExtAudioFileDispose (handle->ref);
    handle->ref = nullptr;
    if (status != noErr)
        return std::unexpected (writeFailed (file, "close", status));
    return {};
}
} // namespace overdub::exporting
