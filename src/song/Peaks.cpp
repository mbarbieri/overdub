#include "song/Peaks.h"

#include "song/AudioFileDecode.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace overdub::song
{
namespace
{
constexpr char peaksMagic[4] = { 'O', 'V', 'P', 'K' };
constexpr std::uint32_t peaksFormatVersion = 1;
constexpr std::size_t headerBytes = 4 + 4 + 4 + 4 + 8;
constexpr std::uint32_t maxPeakChannels = 2;

std::size_t peakCountFor (SampleCount lengthSamples)
{
    return (std::size_t) ((lengthSamples + samplesPerPeak - 1) / samplesPerPeak);
}

// Plain little-endian byte appends and reads; x86_64 and arm64 are both little-endian.
template <typename T>
void append (std::string& bytes, T value)
{
    char raw[sizeof (T)];
    std::memcpy (raw, &value, sizeof (T));
    bytes.append (raw, sizeof (T));
}

template <typename T>
T readAt (std::string_view bytes, std::size_t offset)
{
    T value;
    std::memcpy (&value, bytes.data() + offset, sizeof (T));
    return value;
}

core::Error corrupt (const char* reason)
{
    return core::Error { "song.peaks_corrupt", "the peaks file is corrupt", { { "reason", reason } } };
}
} // namespace

Peaks computePeaks (const juce::AudioBuffer<float>& audio)
{
    Peaks peaks;
    peaks.lengthSamples = audio.getNumSamples();
    const auto peakCount = peakCountFor (peaks.lengthSamples);
    for (int c = 0; c < audio.getNumChannels(); ++c)
    {
        const float* samples = audio.getReadPointer (c);
        std::vector<PeakRange> ranges (peakCount);
        for (std::size_t block = 0; block < peakCount; ++block)
        {
            const auto start = (int) block * samplesPerPeak;
            const auto end = std::min (start + samplesPerPeak, audio.getNumSamples());
            const auto [minimum, maximum] = std::minmax_element (samples + start, samples + end);
            ranges[block] = PeakRange { *minimum, *maximum };
        }
        peaks.channels.push_back (std::move (ranges));
    }
    return peaks;
}

std::string encodePeaks (const Peaks& peaks)
{
    std::string bytes (peaksMagic, sizeof (peaksMagic));
    append<std::uint32_t> (bytes, peaksFormatVersion);
    append<std::uint32_t> (bytes, (std::uint32_t) samplesPerPeak);
    append<std::uint32_t> (bytes, (std::uint32_t) peaks.channels.size());
    append<std::int64_t> (bytes, peaks.lengthSamples);
    for (const auto& channel : peaks.channels)
        for (const auto& range : channel)
        {
            append<float> (bytes, range.minimum);
            append<float> (bytes, range.maximum);
        }
    return bytes;
}

core::Expected<Peaks> decodePeaks (std::string_view bytes)
{
    if (bytes.size() < headerBytes)
        return std::unexpected (corrupt ("truncated_header"));
    if (std::memcmp (bytes.data(), peaksMagic, sizeof (peaksMagic)) != 0)
        return std::unexpected (corrupt ("bad_magic"));
    if (readAt<std::uint32_t> (bytes, 4) != peaksFormatVersion)
        return std::unexpected (corrupt ("unknown_version"));
    if (readAt<std::uint32_t> (bytes, 8) != (std::uint32_t) samplesPerPeak)
        return std::unexpected (corrupt ("samples_per_peak"));
    const auto channelCount = readAt<std::uint32_t> (bytes, 12);
    const auto lengthSamples = readAt<std::int64_t> (bytes, 16);
    if (channelCount < 1 || channelCount > maxPeakChannels || lengthSamples < 0)
        return std::unexpected (corrupt ("bad_header"));

    const auto peakCount = peakCountFor (lengthSamples);
    if (bytes.size() != headerBytes + channelCount * peakCount * 2 * sizeof (float))
        return std::unexpected (corrupt ("size_mismatch"));

    Peaks peaks;
    peaks.lengthSamples = lengthSamples;
    auto offset = headerBytes;
    for (std::uint32_t c = 0; c < channelCount; ++c)
    {
        std::vector<PeakRange> ranges (peakCount);
        for (auto& range : ranges)
        {
            range.minimum = readAt<float> (bytes, offset);
            range.maximum = readAt<float> (bytes, offset + sizeof (float));
            offset += 2 * sizeof (float);
        }
        peaks.channels.push_back (std::move (ranges));
    }
    return peaks;
}

juce::File peaksFileFor (const juce::File& packageRoot, const Id& mediaId)
{
    return packageRoot.getChildFile ("peaks").getChildFile (juce::String (mediaId) + ".peaks");
}

core::Expected<void> writePeaksFile (const juce::File& peaksFile, const juce::AudioBuffer<float>& audio, const BeforeRenameHook& beforeRename)
{
    if (const auto created = peaksFile.getParentDirectory().createDirectory(); created.failed())
        return std::unexpected (writeFailed (peaksFile.getParentDirectory(), "create_folder", 0));
    return writeFileAtomically (peaksFile, encodePeaks (computePeaks (audio)), beforeRename);
}

core::Expected<Peaks> loadOrRebuildPeaks (const juce::File& packageRoot, const Id& mediaId, const juce::File& mediaFile, const BeforeRenameHook& beforeRename)
{
    const auto file = peaksFileFor (packageRoot, mediaId);
    const char* reason = "missing";
    if (file.existsAsFile())
    {
        juce::MemoryBlock contents;
        if (file.loadFileAsData (contents))
        {
            auto decoded = decodePeaks (std::string_view (static_cast<const char*> (contents.getData()), contents.getSize()));
            if (decoded.has_value())
                return decoded;
        }
        reason = "corrupt";
    }

    const auto audio = readWavFile (mediaFile);
    if (! audio.has_value())
        return std::unexpected (audio.error());
    if (auto written = writePeaksFile (file, *audio, beforeRename); ! written.has_value())
        return std::unexpected (written.error());
    core::logEvent (core::Level::info, "song.peaks_rebuilt", { { "media_id", mediaId }, { "reason", reason }, { "path", pathOf (file) } });
    return computePeaks (*audio);
}
} // namespace overdub::song
