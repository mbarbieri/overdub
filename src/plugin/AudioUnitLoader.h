#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// AU effects for Insert Chains (issue #33, ADR 0002): discovery (AUv2 and AUv3) and asynchronous,
// out-of-process instantiation through juce::AudioPluginFormatManager. With the JUCE patch
// (patches/juce-force-auv2-oop.patch) every AU, AUv2 included, is created by
// AudioComponentInstantiate with kAudioComponentInstantiation_LoadOutOfProcess, in Apple's
// AUHostingService, so a crashing plugin cannot take the Song down with it.
//
// Message thread. instantiate() never blocks it: it returns at once and calls back later.
namespace overdub::plugin
{
// One AU effect installed on this Mac.
struct AvailableAudioUnit
{
    song::AudioUnitComponent component;
    std::string name;         // "AUDelay"
    std::string manufacturer; // "Apple"
    std::string version;      // "1.6.0", as JUCE's PluginDescription::version spells it
    bool isAUv3 = false;

    bool operator== (const AvailableAudioUnit&) const = default;
};

// A slot's processor, or why there is none. Called on the message thread.
using ProcessorReady = std::function<void (core::Expected<std::unique_ptr<juce::AudioProcessor>>)>;

// Creates the processor for an Insert Chain slot, asynchronously, prepared for nothing yet: the
// caller (plugin::InsertHost) prepares it. The production one is AudioUnitLoader::instantiate;
// tests pass their own.
using Instantiator = std::function<void (const song::InsertSlot& slot, double sampleRate, int blockSize, ProcessorReady ready)>;

class AudioUnitLoader
{
public:
    AudioUnitLoader();

    AudioUnitLoader (const AudioUnitLoader&) = delete;
    AudioUnitLoader& operator= (const AudioUnitLoader&) = delete;

    // Every installed AU effect (type aufx or aumf), AUv2 and AUv3, sorted by name. Instantiates
    // nothing. Logs plugin.discovered (info: effects, auv3).
    std::vector<AvailableAudioUnit> findEffects();

    // Creates the slot's AU out-of-process, initialised at this rate and block size, with stereo
    // in and out (or mono in, stereo out). Calls `ready` on the message thread, later. Logs
    // plugin.instantiated (info: slot_id, name, component, auv3, out_of_process,
    // out_of_process_instantiations, load_ms). Errors (not logged):
    // plugin.not_found (component), plugin.instantiate_failed (component, reason),
    // plugin.layout_unsupported (component).
    void instantiate (const song::InsertSlot& slot, double sampleRate, int blockSize, ProcessorReady ready);

    // instantiate, as an Instantiator. The loader must outlive every call it starts.
    Instantiator instantiator();

private:
    juce::AudioPluginFormatManager formats;
    juce::AudioPluginFormat* audioUnitFormat = nullptr; // owned by `formats`
};
} // namespace overdub::plugin
