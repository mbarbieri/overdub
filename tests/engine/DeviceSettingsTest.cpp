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
