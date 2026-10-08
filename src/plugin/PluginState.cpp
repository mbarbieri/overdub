#include "plugin/PluginState.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

namespace overdub::plugin
{
namespace
{
// The AU behind a JUCE AU instance; null for any other processor.
AudioUnit audioUnitOf (juce::AudioProcessor& processor)
{
    auto* instance = dynamic_cast<juce::AudioPluginInstance*> (&processor);
    if (instance == nullptr)
        return nullptr;
    const auto* client = instance->getAudioUnitClient();
    return client != nullptr ? client->getAudioUnitHandle() : nullptr;
}

// Owns one Core Foundation reference.
struct CFReleaser
{
    CFTypeRef object = nullptr;
    ~CFReleaser()
    {
        if (object != nullptr)
            CFRelease (object);
    }
};

core::Error captureFailed (OSStatus status)
{
    return core::Error { "plugin.state_capture_failed", "the plugin did not give its state", { { "os_status", (int) status } } };
}

core::Expected<juce::MemoryBlock> captureAudioUnitState (AudioUnit unit)
{
    CFPropertyListRef propertyList = nullptr;
    UInt32 size = sizeof (propertyList);
    auto status = AudioUnitGetProperty (unit, kAudioUnitProperty_ClassInfoFromDocument, kAudioUnitScope_Global, 0, &propertyList, &size);
    if (status != noErr || propertyList == nullptr)
    {
        propertyList = nullptr;
        size = sizeof (propertyList);
        status = AudioUnitGetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &propertyList, &size);
    }
    if (status != noErr || propertyList == nullptr)
        return std::unexpected (captureFailed (status));

    const CFReleaser ownedList { propertyList };
    const auto data = CFPropertyListCreateData (kCFAllocatorDefault, propertyList, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    if (data == nullptr)
        return std::unexpected (captureFailed (status));

    const CFReleaser ownedData { data };
    return juce::MemoryBlock (CFDataGetBytePtr (data), (std::size_t) CFDataGetLength (data));
}

core::Expected<void> restoreAudioUnitState (AudioUnit unit, const juce::MemoryBlock& state)
{
    const auto data = CFDataCreate (kCFAllocatorDefault, static_cast<const UInt8*> (state.getData()), (CFIndex) state.getSize());
    const CFReleaser ownedData { data };
    const auto propertyList = data != nullptr ? CFPropertyListCreateWithData (kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr) : nullptr;
    if (propertyList == nullptr)
        return std::unexpected (core::Error { "plugin.state_unreadable", "the saved plugin state is not a property list", { { "bytes", state.getSize() } } });

    const CFReleaser ownedList { propertyList };
    auto status = AudioUnitSetProperty (unit, kAudioUnitProperty_ClassInfoFromDocument, kAudioUnitScope_Global, 0, &propertyList, sizeof (propertyList));
    if (status != noErr)
        status = AudioUnitSetProperty (unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &propertyList, sizeof (propertyList));
    if (status != noErr)
        return std::unexpected (core::Error { "plugin.state_restore_failed", "the plugin refused its saved state", { { "os_status", (int) status } } });
    return {};
}
} // namespace

core::Expected<juce::MemoryBlock> captureState (juce::AudioProcessor& processor)
{
    if (auto* unit = audioUnitOf (processor))
        return captureAudioUnitState (unit);

    juce::MemoryBlock state;
    processor.getStateInformation (state);
    return state;
}

core::Expected<void> restoreState (juce::AudioProcessor& processor, const juce::MemoryBlock& state)
{
    if (auto* unit = audioUnitOf (processor))
        return restoreAudioUnitState (unit, state);

    processor.setStateInformation (state.getData(), (int) state.getSize());
    return {};
}
} // namespace overdub::plugin
