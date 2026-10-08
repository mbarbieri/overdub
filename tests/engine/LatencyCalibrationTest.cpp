#include "engine/LatencyCalibration.h"

#include <catch2/catch_test_macros.hpp>

using overdub::engine::CalibrationKey;
using overdub::engine::readCalibrationOffset;
using overdub::engine::writeCalibrationOffset;

namespace
{
// A fresh folder under the system temp folder, deleted at the end of the test.
struct TempFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("overdub-calibration-test-" + juce::Uuid().toString());

    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
};

const CalibrationKey builtIn48k { "MacBook Pro Microphone", "MacBook Pro Speakers", 48000 };
} // namespace

TEST_CASE ("a device with no stored calibration uses an offset of 0", "[engine][calibration]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("latency-calibration.json");

    const auto missingFile = readCalibrationOffset (file, builtIn48k);
    REQUIRE (missingFile.has_value());
    CHECK (*missingFile == 0);

    REQUIRE (writeCalibrationOffset (file, CalibrationKey { "Scarlett 2i2", "Scarlett 2i2", 48000 }, 12).has_value());
    const auto otherDevice = readCalibrationOffset (file, builtIn48k);
    REQUIRE (otherDevice.has_value());
    CHECK (*otherDevice == 0);
}

TEST_CASE ("calibration offsets are stored per input device, output device and sample rate", "[engine][calibration]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("nested/latency-calibration.json");
    auto builtIn44k = builtIn48k;
    builtIn44k.sampleRate = 44100;

    REQUIRE (writeCalibrationOffset (file, builtIn48k, -821).has_value());
    REQUIRE (writeCalibrationOffset (file, builtIn44k, -754).has_value());
    // Replacing one key keeps the others.
    REQUIRE (writeCalibrationOffset (file, builtIn48k, -819).has_value());

    CHECK (readCalibrationOffset (file, builtIn48k) == -819);
    CHECK (readCalibrationOffset (file, builtIn44k) == -754);
}

TEST_CASE ("calibration lives in Overdub's Application Support folder, not in the Song", "[engine][calibration]")
{
    const auto file = overdub::engine::defaultLatencyCalibrationFile();
    CHECK (file.getFileName() == "latency-calibration.json");
    CHECK (file.getParentDirectory().getFileName() == "Overdub");
    CHECK (file.getParentDirectory().getParentDirectory().getFileName() == "Application Support");
}

TEST_CASE ("a calibration file that is not valid calibration JSON is an explicit error", "[engine][calibration]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("latency-calibration.json");
    for (const auto* text : { "not json", "{\"version\": 2, \"offsets\": []}", "{\"version\": 1, \"offsets\": [{\"input_device\": 3}]}" })
    {
        REQUIRE (file.replaceWithText (text));
        const auto read = readCalibrationOffset (file, builtIn48k);
        REQUIRE_FALSE (read.has_value());
        CHECK (read.error().code == "engine.calibration_unreadable");
        CHECK (read.error().fields["path"] == file.getFullPathName().toStdString());

        const auto written = writeCalibrationOffset (file, builtIn48k, 5);
        REQUIRE_FALSE (written.has_value());
        CHECK (written.error().code == "engine.calibration_unreadable");
    }
}
