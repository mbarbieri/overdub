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
                          .getChildFile ("overdub-device-settings-test-" + juce::Uuid().toString());

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

    CHECK (settings == DeviceSettings { .deviceType = "CoreAudio", .inputDevice = "Scarlett 2i2 USB", .outputDevice = "MacBook Pro Speakers", .sampleRate = 48000, .bufferSize = 256 });
}

TEST_CASE ("device settings log as the same fields the settings file stores", "[engine][device][settings]")
{
    TempFolder folder;
    const auto file = folder.root.getChildFile ("audio-device.json");
    const DeviceSettings settings {
        .deviceType = "CoreAudio",
        .inputDevice = "Scarlett 2i2 USB",
        .outputDevice = "MacBook Pro Speakers",
        .sampleRate = 44100,
        .bufferSize = 128,
    };
    REQUIRE (writeDeviceSettings (file, settings).has_value());

    const auto fields = overdub::engine::deviceSettingsFields (settings);

    CHECK (fields == nlohmann::json {
               { "device_type", "CoreAudio" },
               { "input_device", "Scarlett 2i2 USB" },
               { "output_device", "MacBook Pro Speakers" },
               { "sample_rate", 44100 },
               { "buffer_size", 128 },
           });
    auto stored = nlohmann::json::parse (file.loadFileAsString().toStdString());
    stored.erase ("version");
    CHECK (stored == fields);
}

namespace
{
using overdub::engine::AvailableDevices;
using overdub::engine::classifyDeviceChange;
using overdub::engine::DeviceChange;

const DeviceSettings scarlett { "CoreAudio", "Scarlett 2i2 USB", "Scarlett 2i2 USB", 48000, 256 };
const DeviceSettings builtIn { "CoreAudio", "MacBook Pro Microphone", "MacBook Pro Speakers", 48000, 512 };
const AvailableDevices bothPresent { { "MacBook Pro Microphone", "Scarlett 2i2 USB" }, { "MacBook Pro Speakers", "Scarlett 2i2 USB" } };
const AvailableDevices builtInOnly { { "MacBook Pro Microphone" }, { "MacBook Pro Speakers" } };
} // namespace

TEST_CASE ("a device change is classified from the engine's device, the running one, the saved choice and what is plugged in", "[engine][device][settings][device_change]")
{
    SECTION ("nothing changed for the engine's device")
    {
        CHECK (classifyDeviceChange (scarlett, scarlett, scarlett, bothPresent) == DeviceChange::none);
    }
    SECTION ("the engine's device closed, or the device manager replaced it with another: lost")
    {
        CHECK (classifyDeviceChange (scarlett, std::nullopt, scarlett, builtInOnly) == DeviceChange::lost);
        CHECK (classifyDeviceChange (scarlett, builtIn, scarlett, builtInOnly) == DeviceChange::lost);
    }
    SECTION ("the same device now runs at another rate or buffer size: reconfigured")
    {
        auto other = scarlett;
        other.sampleRate = 44100;
        CHECK (classifyDeviceChange (scarlett, other, scarlett, bothPresent) == DeviceChange::reconfigured);
        other = scarlett;
        other.bufferSize = 128;
        CHECK (classifyDeviceChange (scarlett, other, scarlett, bothPresent) == DeviceChange::reconfigured);
    }
    SECTION ("running on a fallback, the saved choice plugged back in is available")
    {
        CHECK (classifyDeviceChange (builtIn, builtIn, scarlett, bothPresent) == DeviceChange::savedChoiceAvailable);
        CHECK (classifyDeviceChange (builtIn, builtIn, scarlett, builtInOnly) == DeviceChange::none);
        // On the saved choice (at whatever rate), or without one, there is nothing to return to.
        auto atSongRate = scarlett;
        atSongRate.sampleRate = 44100;
        CHECK (classifyDeviceChange (atSongRate, atSongRate, scarlett, bothPresent) == DeviceChange::none);
        CHECK (classifyDeviceChange (builtIn, builtIn, std::nullopt, bothPresent) == DeviceChange::none);
    }
    SECTION ("a saved choice that names only an output device needs only that output")
    {
        const DeviceSettings outputOnly { "CoreAudio", "", "Scarlett 2i2 USB", 0, 0 };
        CHECK (classifyDeviceChange (builtIn, builtIn, outputOnly, AvailableDevices { {}, { "Scarlett 2i2 USB" } }) == DeviceChange::savedChoiceAvailable);
    }
    SECTION ("without a device, any output device plugged in has appeared")
    {
        CHECK (classifyDeviceChange (std::nullopt, std::nullopt, scarlett, builtInOnly) == DeviceChange::appeared);
        CHECK (classifyDeviceChange (std::nullopt, std::nullopt, scarlett, AvailableDevices {}) == DeviceChange::none);
    }
}

TEST_CASE ("a device change logs under a stable name", "[engine][device][settings][device_change]")
{
    CHECK (std::string (overdub::engine::deviceChangeName (DeviceChange::none)) == "none");
    CHECK (std::string (overdub::engine::deviceChangeName (DeviceChange::lost)) == "lost");
    CHECK (std::string (overdub::engine::deviceChangeName (DeviceChange::reconfigured)) == "reconfigured");
    CHECK (std::string (overdub::engine::deviceChangeName (DeviceChange::savedChoiceAvailable)) == "saved_choice_available");
    CHECK (std::string (overdub::engine::deviceChangeName (DeviceChange::appeared)) == "appeared");
}

TEST_CASE ("the device is switched to the open Song's rate only when it runs at another one and offers it", "[engine][device][settings][sample_rate]")
{
    using overdub::engine::sampleRateForSong;
    CHECK (sampleRateForSong (48000, 44100, { 44100, 48000, 96000 }) == 44100);
    CHECK (sampleRateForSong (48000, 48000, { 44100, 48000 }) == std::nullopt);
    CHECK (sampleRateForSong (48000, 44100, { 48000, 96000 }) == std::nullopt);
    CHECK (sampleRateForSong (48000, 0, { 44100, 48000 }) == std::nullopt);
}
