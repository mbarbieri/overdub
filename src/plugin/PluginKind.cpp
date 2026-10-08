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

std::string hostingBadge (juce::AudioProcessor& processor)
{
    auto* instance = dynamic_cast<juce::AudioPluginInstance*> (&processor);
    auto* client = instance != nullptr ? instance->getAudioUnitClient() : nullptr;
    if (client == nullptr)
        return "in-process";
    const auto unit = static_cast<AudioUnit> (client->getAudioUnitHandle());
    return std::string (isAUv3 (unit) ? "AUv3" : "AUv2") + " \xC2\xB7 out-of-process";
}
} // namespace overdub::plugin
