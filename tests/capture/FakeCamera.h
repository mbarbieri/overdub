#pragma once

#include "capture/Camera.h"

#include <optional>
#include <string>
#include <vector>

// A Camera for tests (ADR 0005): no AVFoundation. beginMovie writes a small placeholder file at the
// path so the recorder's rename works; endMovie returns `movie` (or `endError`) as if the frames
// had been written.
namespace overdub::capture::testing
{
struct FakeCamera final : public Camera
{
    std::vector<CameraDevice> available { { "camera-facetime", "FaceTime HD Camera" }, { "camera-usb", "USB Camera" } };
    std::optional<CameraDevice> opened;
    std::optional<core::Error> openError;
    std::optional<core::Error> beginError;
    std::optional<core::Error> endError;
    CameraMovie movie;

    std::vector<std::string> openRequests;
    std::optional<juce::File> writing;
    std::vector<juce::File> moviesBegun;
    int moviesEnded = 0;

    std::vector<CameraDevice> devices() override { return available; }

    core::Expected<CameraDevice> open (const std::string& cameraId) override
    {
        openRequests.push_back (cameraId);
        if (openError.has_value())
            return std::unexpected (*openError);
        auto chosen = available.front();
        for (const auto& device : available)
            if (device.id == cameraId)
                chosen = device;
        opened = chosen;
        return chosen;
    }

    void close() override
    {
        opened.reset();
        writing.reset();
    }

    std::optional<CameraDevice> openCamera() const override { return opened; }

    core::Expected<void> beginMovie (const juce::File& movieFile) override
    {
        if (! opened.has_value())
            return std::unexpected (core::Error { "capture.camera_not_open", "no camera is open", {} });
        if (beginError.has_value())
            return std::unexpected (*beginError);
        movieFile.replaceWithText ("fake movie");
        writing = movieFile;
        moviesBegun.push_back (movieFile);
        return {};
    }

    core::Expected<CameraMovie> endMovie() override
    {
        ++moviesEnded;
        writing.reset();
        if (endError.has_value())
            return std::unexpected (*endError);
        auto finished = movie;
        finished.cameraId = opened.has_value() ? opened->id : "";
        return finished;
    }

    void* previewLayer() const override { return nullptr; }
};
} // namespace overdub::capture::testing
