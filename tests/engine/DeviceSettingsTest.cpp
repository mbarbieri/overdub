#include "engine/DeviceSettings.h"

#include <catch2/catch_test_macros.hpp>

using overdub::engine::DeviceSettings;
using overdub::engine::readDeviceSettings;
using overdub::engine::writeDeviceSettings;

namespace
{
// A fresh folder under the system temp folder, deleted at the end of the test.
struct TempFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("overdub-device-settings-test", "", false);

    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
};
} // namespace

TEST_CASE ("device settings written to a file read back unchanged", "[engine][device][settings]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("nested/audio-device.json");
    const DeviceSettings written {
        .deviceType = "CoreAudio",
        .inputDevice = "Scarlett 2i2 USB",
        .outputDevice = "MacBook Pro Speakers",
        .sampleRate = 48000,
        .bufferSize = 256,
    };

    REQUIRE (writeDeviceSettings (file, written).has_value());
    const auto read = readDeviceSettings (file);

    REQUIRE (read.has_value());
    REQUIRE (read->has_value());
    CHECK (**read == written);
}

TEST_CASE ("a machine with no device settings file has no saved choice", "[engine][device][settings]")
{
    TempFolder folder;

    const auto read = readDeviceSettings (folder.root.getChildFile ("audio-device.json"));

    REQUIRE (read.has_value());
    CHECK_FALSE (read->has_value());
}

TEST_CASE ("a device settings file that is not valid settings JSON is an explicit error", "[engine][device][settings]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("audio-device.json");

    for (const auto* contents : { "not json",
                                  "[1, 2]",
                                  R"({"version": 1})",
                                  R"({"version": 99, "device_type": "", "input_device": "", "output_device": "", "sample_rate": 0, "buffer_size": 0})" })
    {
        REQUIRE (file.replaceWithText (contents));

        const auto read = readDeviceSettings (file);

        REQUIRE_FALSE (read.has_value());
        CHECK (read.error().code == "engine.device_settings_unreadable");
        CHECK (read.error().fields.at ("path") == file.getFullPathName().toStdString());
    }
}

TEST_CASE ("device settings live in Overdub's Application Support folder, not in the Song", "[engine][device][settings]")
{
    const auto file = overdub::engine::defaultDeviceSettingsFile();

    CHECK (file.getFileName() == "audio-device.json");
    CHECK (file.getParentDirectory().getFileName() == "Overdub");
    CHECK (file.getParentDirectory().getParentDirectory().getFileName() == "Application Support");
}

TEST_CASE ("writing device settings to an unwritable place is an explicit error", "[engine][device][settings]")
{
    TempFolder folder;
    const auto blocker = folder.root.getChildFile ("blocker");
    REQUIRE (blocker.replaceWithText ("a file where a folder should be"));

    const auto written = writeDeviceSettings (blocker.getChildFile ("audio-device.json"), DeviceSettings {});

    REQUIRE_FALSE (written.has_value());
    CHECK (written.error().code == "engine.device_settings_write_failed");
}

TEST_CASE ("device settings become the JUCE device setup the device manager opens", "[engine][device][settings]")
{
    const DeviceSettings settings {
        .deviceType = "CoreAudio",
        .inputDevice = "Scarlett 2i2 USB",
        .outputDevice = "MacBook Pro Speakers",
        .sampleRate = 44100,
        .bufferSize = 128,
    };

    const auto setup = overdub::engine::toAudioDeviceSetup (settings);

    CHECK (setup.inputDeviceName == "Scarlett 2i2 USB");
    CHECK (setup.outputDeviceName == "MacBook Pro Speakers");
    CHECK (juce::exactlyEqual (setup.sampleRate, 44100.0));
    CHECK (setup.bufferSize == 128);
    CHECK (setup.useDefaultInputChannels);
    CHECK (setup.useDefaultOutputChannels);
}

TEST_CASE ("an open device's setup becomes the device settings to save", "[engine][device][settings]")
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = "Scarlett 2i2 USB";
    setup.outputDeviceName = "MacBook Pro Speakers";
    setup.sampleRate = 48000.0;
    setup.bufferSize = 256;

    const auto settings = overdub::engine::deviceSettingsFrom ("CoreAudio", setup);

    CHECK (settings == DeviceSettings { .deviceType = "CoreAudio",
                                        .inputDevice = "Scarlett 2i2 USB",
                                        .outputDevice = "MacBook Pro Speakers",
                                        .sampleRate = 48000,
                                        .bufferSize = 256 });
}
