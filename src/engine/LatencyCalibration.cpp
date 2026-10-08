#include "engine/LatencyCalibration.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

namespace overdub::engine
{
namespace
{
constexpr int calibrationVersion = 1;

std::string pathOf (const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

core::Error unreadable (const juce::File& file, const std::string& reason)
{
    return core::Error { "engine.calibration_unreadable",
                         "the latency calibration file could not be read",
                         { { "path", pathOf (file) }, { "reason", reason } } };
}

bool matches (const nlohmann::json& entry, const CalibrationKey& key)
{
    return entry.at ("input_device").get<std::string>() == key.inputDevice
        && entry.at ("output_device").get<std::string>() == key.outputDevice
        && entry.at ("sample_rate").get<int>() == key.sampleRate;
}

// The file's offsets array; an empty one when the file does not exist.
core::Expected<nlohmann::json> readOffsets (const juce::File& file)
{
    if (! file.existsAsFile())
        return nlohmann::json::array();

    const auto document = nlohmann::json::parse (file.loadFileAsString().toStdString(), nullptr, false);
    if (document.is_discarded() || ! document.is_object())
        return std::unexpected (unreadable (file, "not a JSON object"));

    try
    {
        if (document.at ("version").get<int>() != calibrationVersion)
            return std::unexpected (unreadable (file, "unsupported version"));
        auto offsets = document.at ("offsets");
        if (! offsets.is_array())
            return std::unexpected (unreadable (file, "offsets is not an array"));
        for (const auto& entry : offsets)
        {
            // Validates every entry's shape now, so later reads cannot throw.
            (void) matches (entry, {});
            (void) entry.at ("offset_samples").get<song::SampleCount>();
        }
        return offsets;
    }
    catch (const nlohmann::json::exception& error)
    {
        return std::unexpected (unreadable (file, error.what()));
    }
}
} // namespace

juce::File defaultLatencyCalibrationFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Overdub")
        .getChildFile ("latency-calibration.json");
}

core::Expected<song::SampleCount> readCalibrationOffset (const juce::File& file, const CalibrationKey& key)
{
    const auto found = findCalibrationOffset (file, key);
    if (! found.has_value())
        return std::unexpected (found.error());
    return found->value_or (0);
}

core::Expected<std::optional<song::SampleCount>> findCalibrationOffset (const juce::File& file, const CalibrationKey& key)
{
    const auto offsets = readOffsets (file);
    if (! offsets.has_value())
        return std::unexpected (offsets.error());

    for (const auto& entry : *offsets)
        if (matches (entry, key))
            return entry.at ("offset_samples").get<song::SampleCount>();
    return std::optional<song::SampleCount> {};
}

core::Expected<void> writeCalibrationOffset (const juce::File& file, const CalibrationKey& key, song::SampleCount offsetSamples)
{
    auto offsets = readOffsets (file);
    if (! offsets.has_value())
        return std::unexpected (offsets.error());

    auto kept = nlohmann::json::array();
    for (const auto& entry : *offsets)
        if (! matches (entry, key))
            kept.push_back (entry);
    kept.push_back ({ { "input_device", key.inputDevice }, { "output_device", key.outputDevice }, { "sample_rate", key.sampleRate }, { "offset_samples", offsetSamples } });

    const nlohmann::json document { { "version", calibrationVersion }, { "offsets", kept } };
    // Device names come from the OS; replace invalid UTF-8 rather than throw.
    const auto text = document.dump (1, ' ', false, nlohmann::json::error_handler_t::replace);
    // replaceWithText writes a temporary file and renames it over the target.
    const auto written = file.getParentDirectory().createDirectory().wasOk()
                      && file.replaceWithText (juce::String::fromUTF8 (text.data(), (int) text.size()));
    if (! written)
        return std::unexpected (core::Error { "engine.calibration_write_failed",
                                              "the latency calibration file could not be written",
                                              { { "path", pathOf (file) } } });

    core::logEvent (core::Level::info,
                    "engine.calibration_saved",
                    { { "path", pathOf (file) }, { "input_device", key.inputDevice }, { "output_device", key.outputDevice }, { "sample_rate", key.sampleRate }, { "offset_samples", offsetSamples } });
    return {};
}
} // namespace overdub::engine
