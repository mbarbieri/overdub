#include "plugin/PluginKind.h"

#include <AudioToolbox/AudioToolbox.h>

namespace overdub::plugin
{
namespace
{
bool isAUv3 (AudioUnit unit)
{
    const auto component = AudioComponentInstanceGetComponent (unit);
    AudioComponentDescription description {};
    if (component == nullptr || AudioComponentGetDescription (component, &description) != noErr)
        return false;
    return (description.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;
}
} // namespace

const void* audioUnitHandle (const juce::AudioProcessor& processor)
{
    const auto* instance = dynamic_cast<const juce::AudioPluginInstance*> (&processor);
    const auto* client = instance != nullptr ? instance->getAudioUnitClient() : nullptr;
    return client != nullptr ? client->getAudioUnitHandle() : nullptr;
}

std::string hostingBadge (juce::AudioProcessor& processor)
{
    const auto handle = audioUnitHandle (processor);
    if (handle == nullptr)
        return "in-process";
    const auto unit = static_cast<AudioUnit> (const_cast<void*> (handle));
    return std::string (isAUv3 (unit) ? "AUv3" : "AUv2") + " \xC2\xB7 out-of-process";
}
} // namespace overdub::plugin
