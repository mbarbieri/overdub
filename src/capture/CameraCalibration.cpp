#include "capture/CameraCalibration.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace overdub::capture
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
    return core::Error { "capture.camera_calibration_unreadable",
                         "the camera calibration file could not be read",
                         { { "path", pathOf (file) }, { "reason", reason } } };
}

bool matches (const nlohmann::json& entry, const CameraCalibrationKey& key)
{
    return entry.at ("camera_id").get<std::string>() == key.cameraId
        && entry.at ("output_device").get<std::string>() == key.outputDevice;
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
            (void) entry.at ("offset_ms").get<double>();
        }
        return offsets;
    }
    catch (const nlohmann::json::exception& error)
    {
        return std::unexpected (unreadable (file, error.what()));
    }
}
} // namespace

juce::File defaultCameraCalibrationFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Overdub")
        .getChildFile ("camera-calibration.json");
}

core::Expected<double> readCameraCalibration (const juce::File& file, const CameraCalibrationKey& key)
{
    const auto offsets = readOffsets (file);
    if (! offsets.has_value())
        return std::unexpected (offsets.error());

    for (const auto& entry : *offsets)
        if (matches (entry, key))
            return entry.at ("offset_ms").get<double>();
    return 0.0;
}

core::Expected<void> writeCameraCalibration (const juce::File& file, const CameraCalibrationKey& key, double offsetMs)
{
    auto offsets = readOffsets (file);
    if (! offsets.has_value())
        return std::unexpected (offsets.error());

    auto kept = nlohmann::json::array();
    for (const auto& entry : *offsets)
        if (! matches (entry, key))
            kept.push_back (entry);
    kept.push_back ({ { "camera_id", key.cameraId }, { "output_device", key.outputDevice }, { "offset_ms", offsetMs } });

    const nlohmann::json document { { "version", calibrationVersion }, { "offsets", kept } };
    // Device names come from the OS; replace invalid UTF-8 rather than throw.
    const auto text = document.dump (1, ' ', false, nlohmann::json::error_handler_t::replace);
    // replaceWithText writes a temporary file and renames it over the target.
    const auto written = file.getParentDirectory().createDirectory().wasOk()
                      && file.replaceWithText (juce::String::fromUTF8 (text.data(), (int) text.size()));
    if (! written)
        return std::unexpected (core::Error { "capture.camera_calibration_write_failed",
                                              "the camera calibration file could not be written",
                                              { { "path", pathOf (file) } } });

    core::logEvent (core::Level::info, "capture.camera_calibration_saved", { { "path", pathOf (file) }, { "camera_id", key.cameraId }, { "output_device", key.outputDevice }, { "offset_ms", offsetMs } });
    return {};
}

song::SampleCount calibrationSamples (double offsetMs, int sampleRate)
{
    return (song::SampleCount) std::llround (offsetMs * (double) sampleRate / 1000.0);
}
} // namespace overdub::capture
