#include "ui/OpenSong.h"

#include "engine/EngineFixture.h"
#include "engine/TestProcessors.h"
#include "plugin/PluginTestSupport.h"
#include "song/FixtureSongBuilder.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "song/SongJson.h"

#include <AudioToolbox/AudioToolbox.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::plugin::AvailableAudioUnit;
using overdub::plugin::InsertStatus;
using overdub::plugin::ProcessorReady;
using overdub::song::SongPackage;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::song::testing;
using namespace overdub::engine::testing;
using namespace std::chrono_literals;

// Insert Chains in the open Song (issue #33): plugins load asynchronously and rebuild the plan,
// their state is captured into the package and restored on undo and on open.
namespace
{
constexpr int blockSize = 256;

// A plugin with latency, one parameter and that parameter as its whole state.
class LatentStateProcessor final : public overdub::engine::PlainProcessor
{
public:
    explicit LatentStateProcessor (int latency) : PlainProcessor (stereoInOut())
    {
        auto parameter = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", 0.0f, 1.0f, 0.5f);
        mix = parameter.get();
        addParameter (parameter.release());
        setLatencySamples (latency);
    }

    const juce::String getName() const override { return "Test Latent"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    void getStateInformation (juce::MemoryBlock& state) override
    {
        const auto text = juce::String (mix->get());
        state.replaceAll (text.toRawUTF8(), text.getNumBytesAsUTF8());
    }
    void setStateInformation (const void* data, int size) override { *mix = juce::String::fromUTF8 (static_cast<const char*> (data), size).getFloatValue(); }

    juce::AudioParameterFloat* mix = nullptr;
};

// Requests the test completes by hand, standing in for the asynchronous AU loader.
struct FakeInstantiator
{
    std::vector<ProcessorReady> requests;

    overdub::plugin::Instantiator instantiator()
    {
        return [this] (const overdub::song::InsertSlot&, double, int, ProcessorReady ready) { requests.push_back (std::move (ready)); };
    }

    LatentStateProcessor* succeed (std::size_t index, int latency)
    {
        auto processor = std::make_unique<LatentStateProcessor> (latency);
        auto* raw = processor.get();
        requests.at (index) (std::unique_ptr<juce::AudioProcessor> (std::move (processor)));
        return raw;
    }
};

const AvailableAudioUnit testPlugin { .component = { "aufx", "test", "ovdb" }, .name = "Test Latent", .manufacturer = "Overdub", .version = "1.0.0" };
const AvailableAudioUnit auDelay { .component = { "aufx", "dely", "appl" }, .name = "AUDelay", .manufacturer = "Apple" };

struct InsertsFixture
{
    std::unique_ptr<FixtureSong> fixture = threeTrackFixture();
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 512 };
    std::unique_ptr<OpenSong> openSong;

    explicit InsertsFixture (overdub::plugin::Instantiator instantiate)
    {
        openSong = std::make_unique<OpenSong> (std::move (fixture->package), AudioBinding { &engine, {}, { "In 1", "In 2" } }, std::move (instantiate));
    }

    ~InsertsFixture() { openSong.reset(); }

    const overdub::song::Track& trackNamed (const std::string& name) const
    {
        const auto& tracks = openSong->song().tracks;
        const auto found = std::ranges::find (tracks, name, &overdub::song::Track::name);
        REQUIRE (found != tracks.end());
        return *found;
    }

    const overdub::song::InsertSlot& guitarSlot() const
    {
        const auto& chain = trackNamed ("Guitar").insertChain;
        REQUIRE (chain.size() == 1);
        return chain[0];
    }
};

overdub::song::Song songOnDisk (const juce::File& location)
{
    const auto parsed = overdub::song::parseJsonText (location.getChildFile ("song.json").loadFileAsString().toStdString());
    REQUIRE (parsed.has_value());
    const auto song = overdub::song::songFromJson (*parsed);
    REQUIRE (song.has_value());
    return *song;
}

float auParameter (juce::AudioProcessor* processor, AudioUnitParameterID parameter)
{
    auto* instance = dynamic_cast<juce::AudioPluginInstance*> (processor);
    REQUIRE (instance != nullptr);
    REQUIRE (instance->getAudioUnitClient() != nullptr);
    AudioUnitParameterValue value = 0.0f;
    REQUIRE (AudioUnitGetParameter (instance->getAudioUnitClient()->getAudioUnitHandle(), parameter, kAudioUnitScope_Global, 0, &value) == noErr);
    return value;
}

void setAuParameter (juce::AudioProcessor* processor, AudioUnitParameterID parameter, float value)
{
    auto* instance = dynamic_cast<juce::AudioPluginInstance*> (processor);
    REQUIRE (instance != nullptr);
    REQUIRE (AudioUnitSetParameter (instance->getAudioUnitClient()->getAudioUnitHandle(), parameter, kAudioUnitScope_Global, 0, value, 0) == noErr);
}
} // namespace

TEST_CASE ("adding an Insert is one undo step, and the plugin is heard with its latency once it has loaded", "[ui][open_song][inserts]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    const auto guitarId = f.trackNamed ("Guitar").id;

    REQUIRE (f.openSong->addInsert (guitarId, testPlugin).has_value());

    CHECK (f.openSong->package().undoLabel() == "Add Test Latent to Guitar");
    const auto& slot = f.guitarSlot();
    CHECK (slot.component == testPlugin.component);
    CHECK (slot.version == "1.0.0");
    CHECK (f.openSong->insertStatus (slot.id) == InsertStatus::loading);
    REQUIRE (fake.requests.size() == 1);

    // Still loading: the plan plays without it.
    f.openSong->poll();
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 0);

    fake.succeed (0, 96);
    CHECK (f.openSong->insertStatus (slot.id) == InsertStatus::ready);
    f.openSong->poll(); // rebuilds the plan with the plugin
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 96);
}

TEST_CASE ("a plugin's latency change rebuilds the plan at the next poll", "[ui][open_song][inserts]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    REQUIRE (f.openSong->addInsert (f.trackNamed ("Guitar").id, testPlugin).has_value());
    auto* plugin = fake.succeed (0, 32);
    f.openSong->poll();
    f.device.run (f.engine, 1);
    REQUIRE (f.engine.latencySamples() == 32);

    plugin->setLatencySamples (200);
    f.openSong->poll();
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 200);
}

TEST_CASE ("a plugin that loads during a Pass joins the plan only after the Pass", "[ui][open_song][inserts][record]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    REQUIRE (f.openSong->addInsert (f.trackNamed ("Guitar").id, testPlugin).has_value());

    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 4);
    fake.succeed (0, 64);
    f.openSong->poll();
    f.device.run (f.engine, 4);
    CHECK (f.engine.latencySamples() == 0); // the Pass keeps the plan it started with

    f.openSong->stop();
    f.device.run (f.engine, 1);
    f.openSong->poll(); // finishes the Pass, then plays the plugin
    CHECK_FALSE (f.openSong->isRecording());
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 64);
}

TEST_CASE ("plugin state is captured into the next history step and into song.json after a parameter change", "[ui][open_song][inserts][state]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    const auto guitarId = f.trackNamed ("Guitar").id;
    REQUIRE (f.openSong->addInsert (guitarId, testPlugin).has_value());
    auto* plugin = fake.succeed (0, 0);
    CHECK (f.guitarSlot().stateHash.empty()); // the step that added it ran before it loaded

    // A parameter moved in the plugin: no undo step, but a debounced write captures it.
    plugin->mix->setValueNotifyingHost (0.25f);
    const auto start = SongPackage::Clock::now();
    f.openSong->poll (start);
    CHECK (f.openSong->package().hasPendingWrite());
    f.openSong->poll (start + 1s);
    const auto& location = f.openSong->package().location();
    const auto savedHash = songOnDisk (location).tracks.at (1).insertChain.at (0).stateHash;
    REQUIRE_FALSE (savedHash.empty());
    CHECK (f.openSong->package().loadPluginState (savedHash)->toString() == "0.25");

    // The next history step holds the state as it is then.
    *plugin->mix = 0.75f;
    REQUIRE (f.openSong->edit (*overdub::song::setTrackMute (f.openSong->song(), guitarId, true), "Mute Guitar").has_value());
    CHECK (f.openSong->package().loadPluginState (f.guitarSlot().stateHash)->toString() == "0.75");
}

TEST_CASE ("AUDelay's state round-trips through a blob and an undo, and through reopening the Song", "[ui][open_song][inserts][au][state]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    overdub::plugin::AudioUnitLoader loader;
    InsertsFixture f { loader.instantiator() };
    const auto guitarId = f.trackNamed ("Guitar").id;

    REQUIRE (f.openSong->addInsert (guitarId, auDelay).has_value());
    const auto slotId = f.guitarSlot().id;
    REQUIRE (overdub::plugin::testing::runMessagesUntil ([&] { return f.openSong->insertStatus (slotId) == InsertStatus::ready; }));
    f.openSong->poll();

    // Set AUDelay's delay time in the plugin, then make a history step: it holds that state.
    auto* first = f.openSong->insertHost().processor (slotId);
    REQUIRE (! juce::exactlyEqual (auParameter (first, kDelayParam_DelayTime), 1.5f));
    setAuParameter (first, kDelayParam_DelayTime, 1.5f);
    // An out-of-process AU applies a parameter set asynchronously; reading it back waits for it.
    REQUIRE_THAT (auParameter (first, kDelayParam_DelayTime), Catch::Matchers::WithinAbs (1.5, 1e-6));
    REQUIRE (f.openSong->edit (*overdub::song::setTrackMute (f.openSong->song(), guitarId, true), "Mute Guitar").has_value());
    const auto hash = f.guitarSlot().stateHash;
    REQUIRE_FALSE (hash.empty());
    CHECK (f.openSong->package().location().getChildFile ("plugins/" + hash + ".state").existsAsFile());

    // Remove it, then undo: the slot comes back with that blob, in a new plugin.
    REQUIRE (f.openSong->edit (*overdub::song::removeInsert (f.openSong->song(), slotId), overdub::song::removeInsertLabel (f.openSong->song(), slotId)).has_value());
    CHECK_FALSE (f.openSong->insertStatus (slotId).has_value());
    REQUIRE (f.openSong->undo().has_value());
    CHECK (f.guitarSlot().stateHash == hash);
    REQUIRE (overdub::plugin::testing::runMessagesUntil ([&] { return f.openSong->insertStatus (slotId) == InsertStatus::ready; }));
    auto* restored = f.openSong->insertHost().processor (slotId);
    CHECK_THAT (auParameter (restored, kDelayParam_DelayTime), Catch::Matchers::WithinAbs (1.5, 1e-6));

    // Closing captures it into the package; opening it again loads AUDelay with the same state.
    const auto location = f.openSong->package().location();
    f.openSong.reset();
    auto reopened = SongPackage::open (location);
    REQUIRE (reopened.has_value());
    f.openSong = std::make_unique<OpenSong> (std::move (reopened->package), AudioBinding { &f.engine, {}, { "In 1", "In 2" } }, loader.instantiator());
    REQUIRE (overdub::plugin::testing::runMessagesUntil ([&] { return f.openSong->insertStatus (slotId) == InsertStatus::ready; }));
    CHECK_THAT (auParameter (f.openSong->insertHost().processor (slotId), kDelayParam_DelayTime), Catch::Matchers::WithinAbs (1.5, 1e-6));
}

//==============================================================================
// Crash recovery (issue #40). The test calls InsertHost::pluginCrashed where the app hears
// kAudioComponentInstanceInvalidationNotification.

namespace
{
std::vector<std::string> codesOf (const std::vector<overdub::core::Error>& errors)
{
    std::vector<std::string> codes;
    for (const auto& error : errors)
        codes.push_back (error.code);
    return codes;
}
} // namespace

TEST_CASE ("a plugin that crashes while playing is reported once, left out of the plan, and reloaded with its saved state", "[ui][open_song][inserts][crash]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    const auto guitarId = f.trackNamed ("Guitar").id;
    REQUIRE (f.openSong->addInsert (guitarId, testPlugin).has_value());
    auto* plugin = fake.succeed (0, 64);
    *plugin->mix = 0.25f;
    // A history step captures the state the reload restores.
    REQUIRE (f.openSong->edit (*overdub::song::setTrackMute (f.openSong->song(), guitarId, true), "Mute Guitar").has_value());
    f.openSong->poll();
    f.openSong->playOrStop();
    f.device.run (f.engine, 2);
    REQUIRE (f.engine.latencySamples() == 64);

    f.openSong->insertHost().pluginCrashed (*plugin);
    const auto errors = f.openSong->poll();

    REQUIRE (codesOf (errors) == std::vector<std::string> { "ui.plugin_crashed" });
    CHECK (errors[0].fields.at ("slot_ids") == nlohmann::json::array ({ f.guitarSlot().id }));
    CHECK (errors[0].fields.at ("names") == nlohmann::json::array ({ "Test Latent" }));
    CHECK (f.openSong->isPlaying());
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 0); // left out, as a bypass would
    CHECK (f.openSong->insertStatus (f.guitarSlot().id) == InsertStatus::loading);
    REQUIRE (fake.requests.size() == 2);
    CHECK (f.openSong->poll().empty()); // reported once

    auto* reloaded = fake.succeed (1, 64);
    CHECK_THAT (reloaded->mix->get(), Catch::Matchers::WithinULP (0.25f, 0));
    f.openSong->poll();
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 64);
}

TEST_CASE ("a plugin that crashes during a Pass does not stop it: the Pass keeps its plan and Takes, and the slot reloads after it", "[ui][open_song][inserts][crash][record]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    const auto guitarId = f.trackNamed ("Guitar").id;
    REQUIRE (f.openSong->addInsert (guitarId, testPlugin).has_value());
    auto* plugin = fake.succeed (0, 64);
    f.openSong->setArmed (guitarId, true);
    f.openSong->setInputChannels (guitarId, { 0 });
    f.openSong->poll();
    const auto takesBefore = f.openSong->song().takes.size();

    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 4);
    f.openSong->insertHost().pluginCrashed (*plugin);
    const auto errors = f.openSong->poll();
    REQUIRE (codesOf (errors) == std::vector<std::string> { "ui.plugin_crashed" });
    f.device.run (f.engine, 4);

    CHECK (f.openSong->isRecording());
    CHECK (f.engine.latencySamples() == 64); // the Pass keeps the plan it started with
    CHECK (fake.requests.size() == 1);       // no reload while recording
    CHECK (f.openSong->insertStatus (f.guitarSlot().id) == InsertStatus::crashed);

    f.openSong->stop();
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());
    CHECK_FALSE (f.openSong->isRecording());
    CHECK (f.openSong->song().takes.size() > takesBefore); // the Pass was added
    CHECK (fake.requests.size() == 2);
    f.device.run (f.engine, 1);
    CHECK (f.engine.latencySamples() == 0); // until the reloaded plugin arrives
}

TEST_CASE ("a plugin that keeps crashing is reported as ui.plugin_crashed_repeatedly and stays left out", "[ui][open_song][inserts][crash]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    REQUIRE (f.openSong->addInsert (f.trackNamed ("Guitar").id, testPlugin).has_value());
    auto* plugin = fake.succeed (0, 0);
    for (int reload = 0; reload < overdub::plugin::maxCrashReloads; ++reload)
    {
        f.openSong->insertHost().pluginCrashed (*plugin);
        REQUIRE (codesOf (f.openSong->poll()) == std::vector<std::string> { "ui.plugin_crashed" });
        plugin = fake.succeed (fake.requests.size() - 1, 0);
    }

    f.openSong->insertHost().pluginCrashed (*plugin);
    const auto errors = f.openSong->poll();

    REQUIRE (codesOf (errors) == std::vector<std::string> { "ui.plugin_crashed_repeatedly" });
    CHECK (errors[0].fields.at ("names") == nlohmann::json::array ({ "Test Latent" }));
    f.openSong->poll();
    CHECK (fake.requests.size() == 1 + overdub::plugin::maxCrashReloads);
    CHECK (f.openSong->insertStatus (f.guitarSlot().id) == InsertStatus::crashed);
    CHECK (f.guitarSlot().component == testPlugin.component); // kept in the Song
}

TEST_CASE ("a plugin crash during an export stops it, leaves no file and reports ui.export_plugin_crashed", "[ui][open_song][inserts][crash][export]")
{
    FakeInstantiator fake;
    InsertsFixture f { fake.instantiator() };
    REQUIRE (f.openSong->addInsert (f.trackNamed ("Guitar").id, testPlugin).has_value());
    auto* plugin = fake.succeed (0, 0);
    f.openSong->poll();
    juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory());
    const auto destination = folder.getFile().getChildFile ("Song.wav");

    REQUIRE (f.openSong->startExport ({ overdub::exporting::ExportFormat::wav, destination }).has_value());
    f.device.run (f.engine, 1); // the audio thread lets go of the plugins
    f.openSong->poll();         // the export renders them now
    REQUIRE (f.openSong->isExporting());
    f.openSong->insertHost().pluginCrashed (*plugin);
    const auto errors = f.openSong->poll();

    CHECK (codesOf (errors) == std::vector<std::string> { "ui.export_plugin_crashed", "ui.plugin_crashed" });
    CHECK_FALSE (f.openSong->isExporting());
    CHECK_FALSE (destination.exists());
    CHECK_FALSE (f.openSong->lastExportedFile().has_value());
    folder.getFile().deleteRecursively();
}
