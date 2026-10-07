// PROTOTYPE — issue #6 spike.
#include "HalDump.h"
#include "SpikeLog.h"
#include <CoreAudio/CoreAudio.h>
#include <vector>

namespace
{
template <typename T>
T getProperty (AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T fallback)
{
    AudioObjectPropertyAddress address { selector, scope, kAudioObjectPropertyElementMain };
    T value = fallback;
    UInt32 size = sizeof (T);
    if (! AudioObjectHasProperty (object, &address)
        || AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, &value) != noErr)
        return fallback;
    return value;
}

template <typename T>
std::vector<T> getArray (AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address { selector, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (object, &address, 0, nullptr, &size) != noErr || size == 0)
        return {};
    std::vector<T> values (size / sizeof (T));
    AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, values.data());
    return values;
}

juce::String getName (AudioObjectID object)
{
    CFStringRef name = nullptr;
    AudioObjectPropertyAddress address { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof (name);
    if (AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, &name) != noErr || name == nullptr)
        return "?";
    auto result = juce::String::fromCFString (name);
    CFRelease (name);
    return result;
}

void logScope (AudioObjectID device, AudioObjectPropertyScope scope, const juce::String& scopeName)
{
    const auto streams = getArray<AudioStreamID> (device, kAudioDevicePropertyStreams, scope);
    if (streams.empty())
        return;

    juce::StringArray streamLatencies;
    for (auto stream : streams)
        streamLatencies.add (juce::String (getProperty<UInt32> (stream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, 0)));

    spikelog::line ("hal_latency", { { "device", getName (device) },
                                     { "id", juce::String ((int) device) },
                                     { "class", juce::String::toHexString ((int) getProperty<AudioClassID> (device, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, 0)) },
                                     { "scope", scopeName },
                                     { "device_latency", juce::String (getProperty<UInt32> (device, kAudioDevicePropertyLatency, scope, 0)) },
                                     { "safety_offset", juce::String (getProperty<UInt32> (device, kAudioDevicePropertySafetyOffset, scope, 0)) },
                                     { "buffer_frames", juce::String (getProperty<UInt32> (device, kAudioDevicePropertyBufferFrameSize, scope, 0)) },
                                     { "stream_latencies", streamLatencies.joinIntoString (",") },
                                     { "sr", juce::String (getProperty<Float64> (device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0)) } });
}
}

void logHalLatencies()
{
    for (auto device : getArray<AudioObjectID> (kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal))
    {
        logScope (device, kAudioObjectPropertyScopeInput, "input");
        logScope (device, kAudioObjectPropertyScopeOutput, "output");
    }
}
