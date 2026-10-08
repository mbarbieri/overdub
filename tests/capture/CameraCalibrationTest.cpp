#include "capture/CameraCalibration.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using overdub::capture::CameraCalibrationKey;
using overdub::capture::readCameraCalibration;
using overdub::capture::writeCameraCalibration;

namespace
{
struct TempFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("overdub-camera-calibration-test-" + juce::Uuid().toString());

    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
};

const CameraCalibrationKey faceTimeOnSpeakers { "0x1420000005ac8600", "MacBook Pro Speakers" };
} // namespace

TEST_CASE ("a camera with no stored calibration uses an offset of 0 ms", "[capture][video][calibration]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("camera-calibration.json");

    CHECK (readCameraCalibration (file, faceTimeOnSpeakers).value_or (-999.0) == Catch::Approx (0.0));
    REQUIRE (writeCameraCalibration (file, CameraCalibrationKey { "usb-camera", "MacBook Pro Speakers" }, 40.0).has_value());
    CHECK (readCameraCalibration (file, faceTimeOnSpeakers).value_or (-999.0) == Catch::Approx (0.0));
}

TEST_CASE ("camera calibration is stored per camera and output device", "[capture][video][calibration]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("nested/camera-calibration.json");
    const CameraCalibrationKey faceTimeOnInterface { faceTimeOnSpeakers.cameraId, "Scarlett 2i2" };

    REQUIRE (writeCameraCalibration (file, faceTimeOnSpeakers, 12.5).has_value());
    REQUIRE (writeCameraCalibration (file, faceTimeOnInterface, -3.0).has_value());
    REQUIRE (writeCameraCalibration (file, faceTimeOnSpeakers, 8.0).has_value()); // replaces, keeps the other

    CHECK (readCameraCalibration (file, faceTimeOnSpeakers).value_or (-999.0) == Catch::Approx (8.0));
    CHECK (readCameraCalibration (file, faceTimeOnInterface).value_or (-999.0) == Catch::Approx (-3.0));
}

TEST_CASE ("camera calibration lives in Overdub's Application Support folder, not in the Song", "[capture][video][calibration]")
{
    const auto file = overdub::capture::defaultCameraCalibrationFile();
    CHECK (file.getFileName() == "camera-calibration.json");
    CHECK (file.getParentDirectory().getFileName() == "Overdub");
    CHECK (file.getParentDirectory().getParentDirectory().getFileName() == "Application Support");
}

TEST_CASE ("a camera calibration file that is not valid calibration JSON is an explicit error", "[capture][video][calibration]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("camera-calibration.json");
    for (const auto* text : { "not json", "{\"version\": 2, \"offsets\": []}", "{\"version\": 1, \"offsets\": [{\"camera_id\": 3}]}" })
    {
        REQUIRE (file.replaceWithText (text));
        const auto read = readCameraCalibration (file, faceTimeOnSpeakers);
        REQUIRE_FALSE (read.has_value());
        CHECK (read.error().code == "capture.camera_calibration_unreadable");
        CHECK (read.error().fields["path"] == file.getFullPathName().toStdString());
        CHECK_FALSE (writeCameraCalibration (file, faceTimeOnSpeakers, 1.0).has_value());
    }
}

TEST_CASE ("a camera calibration in ms becomes whole samples at the Song's rate", "[capture][video][calibration]")
{
    CHECK (overdub::capture::calibrationSamples (10.0, 48000) == 480);
    CHECK (overdub::capture::calibrationSamples (-2.5, 44100) == -110); // -110.25 rounds to -110
    CHECK (overdub::capture::calibrationSamples (0.0, 48000) == 0);
}
