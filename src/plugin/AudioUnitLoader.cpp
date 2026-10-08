#include "plugin/AudioUnitLoader.h"

#include "core/Log.h"
#include "plugin/AudioUnitIdentifier.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <optional>

namespace overdub::plugin
{
namespace
{
OSType osTypeOf (const std::string& code)
{
    OSType value = 0;
    for (const auto character : code)
        value = (value << 8) | (OSType) (unsigned char) character;
    return value;
}

// The installed component with these codes, its description (with flags) and "Manufacturer: Name".
struct InstalledComponent
{
    AudioComponentDescription description {};
    juce::String fullName;
    std::string version;
};

// Core Audio marks the found component nullable; it is checked before use.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullable-to-nonnull-conversion"
std::optional<InstalledComponent> findInstalled (const song::AudioUnitComponent& component)
{
    AudioComponentDescription wanted {};
    wanted.componentType = osTypeOf (component.type);
    wanted.componentSubType = osTypeOf (component.subtype);
    wanted.componentManufacturer = osTypeOf (component.manufacturer);

    const auto found = AudioComponentFindNext (nullptr, &wanted);
    if (found == nullptr)
        return std::nullopt;

    InstalledComponent installed;
    if (AudioComponentGetDescription (found, &installed.description) != noErr)
        return std::nullopt;
    CFStringRef name = nullptr;
    if (AudioComponentCopyName (found, &name) == noErr && name != nullptr)
    {
        installed.fullName = juce::String::fromCFString (name);
        CFRelease (name);
    }
    UInt32 version = 0;
    if (AudioComponentGetVersion (found, &version) == noErr)
        installed.version = std::to_string (version >> 16) + "." + std::to_string ((version >> 8) & 0xff) + "." + std::to_string (version & 0xff);
    return installed;
}
#pragma clang diagnostic pop

bool isAUv3 (const AudioComponentDescription& description)
{
    return (description.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;
}

core::Fields componentFields (const song::AudioUnitComponent& component)
{
    return { { "component", componentText (component) } };
}

// Core Audio names a component "Manufacturer: Name".
AvailableAudioUnit availableFrom (const song::AudioUnitComponent& component, const InstalledComponent& installed)
{
    const auto& fullName = installed.fullName;
    const auto separator = fullName.indexOf (": ");
    const auto name = separator >= 0 ? fullName.substring (separator + 2) : fullName;
    const auto manufacturer = separator >= 0 ? fullName.substring (0, separator) : juce::String (component.manufacturer);
    return { component, name.toStdString(), manufacturer.toStdString(), installed.version, isAUv3 (installed.description) };
}

// Stereo in and out, else mono in and stereo out: the shape of every node on a Track's path.
bool setInsertLayout (juce::AudioPluginInstance& instance)
{
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());
    juce::AudioProcessor::BusesLayout monoToStereo;
    monoToStereo.inputBuses.add (juce::AudioChannelSet::mono());
    monoToStereo.outputBuses.add (juce::AudioChannelSet::stereo());
    return instance.setBusesLayout (stereo) || instance.setBusesLayout (monoToStereo);
}
} // namespace

AudioUnitLoader::AudioUnitLoader()
{
    auto format = std::make_unique<juce::AudioUnitPluginFormat>();
    audioUnitFormat = format.get();
    formats.addFormat (std::move (format));
}

std::vector<AvailableAudioUnit> AudioUnitLoader::findEffects()
{
    std::vector<AvailableAudioUnit> effects;
    // true: include AUv3, which can only be created asynchronously (as every Insert is).
    for (const auto& identifier : audioUnitFormat->searchPathsForPlugins ({}, true, true))
    {
        if (! identifier.startsWith ("AudioUnit:Effects/"))
            continue;
        const auto component = componentFromIdentifier (identifier.toStdString());
        if (! component.has_value())
            continue;
        if (const auto installed = findInstalled (*component); installed.has_value())
            effects.push_back (availableFrom (*component, *installed));
    }
    std::ranges::sort (effects, [] (const auto& a, const auto& b) { return juce::String (a.name).compareNatural (juce::String (b.name)) < 0; });

    const auto auv3Count = std::ranges::count_if (effects, &AvailableAudioUnit::isAUv3);
    core::logEvent (core::Level::info, "plugin.discovered", { { "effects", effects.size() }, { "auv3", auv3Count } });
    return effects;
}

void AudioUnitLoader::instantiate (const song::InsertSlot& slot, double sampleRate, int blockSize, ProcessorReady ready)
{
    const auto installed = findInstalled (slot.component);
    if (! installed.has_value())
    {
        // Still asynchronous, so callers see one behaviour.
        juce::MessageManager::callAsync ([ready, component = slot.component] {
            ready (std::unexpected (core::Error { "plugin.not_found", "this plugin is not installed", componentFields (component) }));
        });
        return;
    }

    juce::PluginDescription pluginDescription;
    pluginDescription.pluginFormatName = audioUnitFormat->getName();
    pluginDescription.fileOrIdentifier = identifierFor (slot.component);

    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    // The patched JUCE counts every AU it creates out-of-process (ADR 0002): a rise since this
    // request shows that this one took that path.
    const auto outOfProcessBefore = juce::getAudioUnitOutOfProcessInstantiationCount();
    const auto auv3 = isAUv3 (installed->description);
    formats.createPluginInstanceAsync (
        pluginDescription,
        sampleRate,
        blockSize,
        [ready, slot, auv3, startMs, outOfProcessBefore] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
            // The AU completion may arrive on any thread; everything after it runs on the message thread.
        auto shared = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>> (std::move (instance));
        juce::MessageManager::callAsync ([ready, slot, auv3, startMs, outOfProcessBefore, shared, error] {
            auto& created = *shared;
            if (created == nullptr)
            {
                auto fields = componentFields (slot.component);
                fields["reason"] = error.toStdString();
                ready (std::unexpected (core::Error { "plugin.instantiate_failed", "the plugin could not be loaded", fields }));
                return;
            }
            if (! setInsertLayout (*created))
            {
                ready (std::unexpected (core::Error { "plugin.layout_unsupported", "the plugin cannot process stereo", componentFields (slot.component) }));
                return;
            }

            const auto outOfProcessNow = juce::getAudioUnitOutOfProcessInstantiationCount();
            core::logEvent (core::Level::info,
                            "plugin.instantiated",
                            { { "slot_id", slot.id },
                              { "name", created->getName().toStdString() },
                              { "component", componentText (slot.component) },
                              { "auv3", auv3 },
                              { "out_of_process", outOfProcessNow > outOfProcessBefore },
                              { "out_of_process_instantiations", outOfProcessNow },
                              { "load_ms", juce::Time::getMillisecondCounterHiRes() - startMs } });
            ready (std::unique_ptr<juce::AudioProcessor> (std::move (created)));
        });
    });
}

Instantiator AudioUnitLoader::instantiator()
{
    return [this] (const song::InsertSlot& slot, double sampleRate, int blockSize, ProcessorReady ready) {
        instantiate (slot, sampleRate, blockSize, std::move (ready));
    };
}
} // namespace overdub::plugin
