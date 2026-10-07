#include "app/LiveEngine.h"

#include "core/Error.h"
#include "core/Log.h"
#include "engine/DeviceSettings.h"
#include "engine/EngineEvents.h"

namespace overdub::app
{
namespace
{
    // Duplex from the start (ADR 0005): recording (slice 5) reads the inputs.
constexpr int inputChannelsNeeded = 2;
constexpr int outputChannelsNeeded = 2;
constexpr int housekeepingIntervalMs = 100;

    // Applies a saved choice to an initialised manager. Returns JUCE's error text ("" on success).
juce::String applySavedChoice (juce::AudioDeviceManager& deviceManager, const engine::DeviceSettings& settings)
{
    if (settings.outputDevice.empty())
        return "the saved choice names no output device";

    if (! settings.deviceType.empty())
        deviceManager.setCurrentAudioDeviceType (juce::String::fromUTF8 (settings.deviceType.c_str()), true);

    return deviceManager.setAudioDeviceSetup (engine::toAudioDeviceSetup (settings), true);
}
} // namespace

LiveEngine::LiveEngine()
{
    openDevice();

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        core::logEvent (core::Level::error, "app.device_unavailable", {}, "no audio output device could be opened; the app runs silent");
        return;
    }

    startEngine (*device);
}

LiveEngine::~LiveEngine()
{
    stopTimer();
    // The device stops calling the binding before any member is destroyed.
    if (binding != nullptr)
        deviceManager.removeAudioCallback (binding.get());
    deviceManager.closeAudioDevice();
}

void LiveEngine::openDevice()
{
    const auto settingsFile = engine::defaultDeviceSettingsFile();
    const auto saved = engine::readDeviceSettings (settingsFile);
    if (! saved.has_value())
        core::logError (saved.error(), core::Level::warn);

    const auto defaultError = deviceManager.initialise (inputChannelsNeeded, outputChannelsNeeded, nullptr, true);
    if (defaultError.isNotEmpty())
        core::logEvent (core::Level::warn, "app.device_open_failed", { { "choice", "default" }, { "reason", defaultError.toStdString() } });

    const auto savedChoice = saved.has_value() ? *saved : std::optional<engine::DeviceSettings> {};
    if (savedChoice.has_value())
    {
        const auto error = applySavedChoice (deviceManager, *savedChoice);
        if (error.isEmpty())
            return;

        auto fields = engine::deviceSettingsFields (*savedChoice);
        fields["choice"] = "saved";
        fields["reason"] = error.toStdString();
        core::logEvent (core::Level::warn, "app.device_open_failed", fields);
        deviceManager.initialiseWithDefaultDevices (inputChannelsNeeded, outputChannelsNeeded);
        return;
    }

    // First launch on this machine: remember what opened. An unreadable file is left untouched.
    if (saved.has_value() && deviceManager.getCurrentAudioDevice() != nullptr)
    {
        const auto opened = engine::deviceSettingsFrom (deviceManager.getCurrentAudioDeviceType(), deviceManager.getAudioDeviceSetup());
        if (const auto written = engine::writeDeviceSettings (settingsFile, opened); ! written.has_value())
            core::logError (written.error(), core::Level::warn);
    }
}

void LiveEngine::startEngine (juce::AudioIODevice& device)
{
    audioEngine = std::make_unique<engine::Engine> (engine::engineConfigFor (device));
    binding = std::make_unique<engine::DeviceBinding> (*audioEngine);
    drainThread.emplace (audioEngine->rtEvents(), engine::engineRtCatalog);

    // Calls binding->audioDeviceAboutToStart (logs engine.device_opened), then the callbacks begin.
    deviceManager.addAudioCallback (binding.get());
    startTimer (housekeepingIntervalMs);
}

void LiveEngine::timerCallback()
{
    if (audioEngine != nullptr)
        audioEngine->housekeeping (core::hostTimeNowNs());
}
} // namespace overdub::app
