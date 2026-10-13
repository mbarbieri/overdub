#pragma once

#include "capture/Camera.h"

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A Camera for tests (ADR 0005): no AVFoundation. beginMovie writes a small placeholder file at the
// path so the recorder's rename works; endMovie completes with `movie` (or `endError`) as if the
// frames had been written: at once, or, with a finishGate, on its own thread once the test
// releases the gate (a slow AVFoundation, issue #70).
namespace overdub::capture::testing
{
// Holds a movie's completion until the test releases it.
struct MovieGate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;

    void release()
    {
        {
            const std::scoped_lock lock { mutex };
            released = true;
        }
        changed.notify_all();
    }

    void wait()
    {
        std::unique_lock lock { mutex };
        changed.wait (lock, [this] { return released; });
    }
};

struct FakeCamera final : public Camera
{
    std::vector<CameraDevice> available { { "camera-facetime", "FaceTime HD Camera" }, { "camera-usb", "USB Camera" } };
    std::optional<CameraDevice> opened;
    std::optional<core::Error> openError;
    std::optional<core::Error> beginError;
    std::optional<core::Error> endError;
    CameraMovie movie;
    // Set by a test after changing `available`, as a camera plugged in or out would.
    bool devicesChanged = false;
    // When set, an ended movie completes only once this gate is released.
    std::shared_ptr<MovieGate> finishGate;

    std::vector<std::string> openRequests;
    std::optional<juce::File> writing;
    std::vector<juce::File> moviesBegun;
    int moviesEnded = 0;
    int moviesCancelled = 0;

    std::vector<CameraDevice> devices() override { return available; }

    bool takeDevicesChanged() override { return std::exchange (devicesChanged, false); }

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

    MovieCompletion endMovie() override
    {
        ++moviesEnded;
        writing.reset();
        auto finished = movie;
        finished.cameraId = opened.has_value() ? opened->id : "";
        const auto completed = endError.has_value() ? core::Expected<CameraMovie> (std::unexpected (*endError)) : core::Expected<CameraMovie> (finished);
        if (finishGate == nullptr)
            return completedMovie (completed);
        return std::async (std::launch::async, [gate = finishGate, completed] {
            gate->wait();
            return completed;
        });
    }

    void cancelMovie() override
    {
        if (! writing.has_value())
            return;
        ++moviesCancelled;
        writing->deleteFile();
        writing.reset();
    }

    void* previewLayer() const override { return nullptr; }
};
} // namespace overdub::capture::testing
