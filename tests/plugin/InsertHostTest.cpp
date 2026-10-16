#include "plugin/InsertHost.h"
#include "engine/SharedInsertNode.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

using overdub::core::Error;
using overdub::core::Expected;
using overdub::core::ScopedLogCapture;
using overdub::plugin::InsertHost;
using overdub::plugin::InsertStatus;
using overdub::plugin::ProcessorReady;
using overdub::song::InsertSlot;
using overdub::song::Song;

// The InsertHost with a fake Instantiator that the test completes by hand, standing in for the
// asynchronous AU loader (the real one is covered by the [au] tests).
namespace
{
constexpr double hostRate = 48000.0;
constexpr int hostBlock = 256;

// A plugin with one parameter, whose value is its whole state.
class StateProcessor final : public overdub::engine::PlainProcessor
{
public:
    StateProcessor() : PlainProcessor (stereoInOut())
    {
        auto parameter = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", 0.0f, 1.0f, 0.5f);
        mix = parameter.get();
        addParameter (parameter.release());
    }

    const juce::String getName() const override { return "Test State"; }
    void prepareToPlay (double rate, int block) override
    {
        preparedRate = rate;
        preparedBlock = block;
    }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override { ++blocksProcessed; }
    void getStateInformation (juce::MemoryBlock& state) override { state.replaceAll (juce::String (mix->get()).toRawUTF8(), juce::String (mix->get()).getNumBytesAsUTF8()); }
    void setStateInformation (const void* data, int size) override { *mix = juce::String::fromUTF8 (static_cast<const char*> (data), size).getFloatValue(); }

    juce::AudioParameterFloat* mix = nullptr;
    double preparedRate = 0.0;
    int preparedBlock = 0;
    int blocksProcessed = 0;
};

struct PendingRequest
{
    InsertSlot slot;
    ProcessorReady ready;
};

// Keeps every request; the test completes them in any order.
struct FakeInstantiator
{
    std::vector<PendingRequest> requests;

    overdub::plugin::Instantiator instantiator()
    {
        return [this] (const InsertSlot& slot, double, int, ProcessorReady ready) { requests.push_back ({ slot, std::move (ready) }); };
    }

    StateProcessor* succeed (std::size_t index, float mix = 0.5f)
    {
        auto processor = std::make_unique<StateProcessor>();
        *processor->mix = mix;
        auto* raw = processor.get();
        requests.at (index).ready (std::unique_ptr<juce::AudioProcessor> (std::move (processor)));
        return raw;
    }

    void fail (std::size_t index)
    {
        requests.at (index).ready (std::unexpected (Error { "plugin.not_found", "not installed", { { "component", "aufx,zzzz,none" } } }));
    }
};

InsertSlot slotNamed (const std::string& id)
{
    return InsertSlot { .id = id, .component = { "aufx", "test", "ovdb" }, .name = id };
}

// Track A with slots a1, a2; Track B with none; the Master with m1.
Song songWithChains()
{
    auto song = overdub::song::makeEmptySong (48000);
    overdub::song::Track a;
    a.id = "track-a";
    a.insertChain = { slotNamed ("a1"), slotNamed ("a2") };
    overdub::song::Track b;
    b.id = "track-b";
    song.tracks = { a, b };
    song.master.insertChain = { slotNamed ("m1") };
    return song;
}

// A state store over a map: the hash is "hash-" + the bytes.
struct FakeStateStore
{
    std::map<std::string, juce::MemoryBlock> blobs;

    overdub::plugin::StateLoader loader()
    {
        return [this] (const std::string& hash) -> Expected<juce::MemoryBlock> {
            const auto found = blobs.find (hash);
            if (found == blobs.end())
                return std::unexpected (Error { "song.plugin_state_missing", "missing", { { "hash", hash } } });
            return found->second;
        };
    }

    overdub::plugin::StateStore store()
    {
        return [this] (const juce::MemoryBlock& state) -> Expected<std::string> {
            const auto hash = "hash-" + state.toString().toStdString();
            blobs[hash] = state;
            return hash;
        };
    }
};

std::vector<std::string> namesOf (const std::vector<std::unique_ptr<juce::AudioProcessor>>& nodes)
{
    std::vector<std::string> names;
    for (const auto& node : nodes)
        names.push_back (node->getName().toStdString());
    return names;
}

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("sync requests every slot of the Tracks' and the Master's chains once", "[plugin][insert_host]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };

    host.sync (songWithChains());
    host.sync (songWithChains());

    REQUIRE (fake.requests.size() == 3);
    CHECK (fake.requests[0].slot.id == "a1");
    CHECK (fake.requests[1].slot.id == "a2");
    CHECK (fake.requests[2].slot.id == "m1");
    CHECK (host.status ("a1") == InsertStatus::loading);
    CHECK_FALSE (host.status ("unknown").has_value());
    CHECK_FALSE (host.takeGraphChanged());
}

TEST_CASE ("a plugin that arrives is prepared, ready, and played in chain order; loading slots are left out", "[plugin][insert_host]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());

    auto* a2 = fake.succeed (1);
    auto* m1 = fake.succeed (2);

    CHECK (host.status ("a2") == InsertStatus::ready);
    CHECK (host.processor ("a2") == a2);
    CHECK_THAT (a2->preparedRate, Catch::Matchers::WithinULP (hostRate, 0));
    CHECK (a2->preparedBlock == hostBlock);
    CHECK (host.takeGraphChanged());
    CHECK_FALSE (host.takeGraphChanged());
    fake.succeed (0);
    host.insertFactory(); // a plan with a1 built: nothing more to do
    CHECK_FALSE (host.takeGraphChanged());

    const auto song = songWithChains();
    auto factory = host.insertFactory();
    CHECK (namesOf (factory ("track-a", song.tracks[0].insertChain)) == std::vector<std::string> { "Test State", "Test State" });
    CHECK (factory ("track-b", song.tracks[1].insertChain).empty());
    const auto master = factory ({}, song.master.insertChain);
    REQUIRE (master.size() == 1);
    CHECK (dynamic_cast<overdub::engine::SharedInsertNode*> (master[0].get()) != nullptr);
    CHECK (m1 == host.processor ("m1"));
}

TEST_CASE ("a chain leaves out a slot still loading", "[plugin][insert_host]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    fake.succeed (1);

    const auto song = songWithChains();
    CHECK (host.insertFactory() ("track-a", song.tracks[0].insertChain).size() == 1);
}

TEST_CASE ("a plugin that fails to load stays an inert placeholder and is logged with its slot", "[plugin][insert_host]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    ScopedLogCapture capture;

    fake.fail (0);

    CHECK (host.status ("a1") == InsertStatus::failed);
    CHECK_FALSE (host.takeGraphChanged());
    const auto song = songWithChains();
    CHECK (host.insertFactory() ("track-a", song.tracks[0].insertChain).empty());
    const auto errors = eventsNamed (capture, "plugin.not_found");
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].at ("slot_id") == "a1");
    CHECK (errors[0].at ("component") == "aufx,zzzz,none");

    // It is not requested again while the Song keeps it.
    host.sync (song);
    CHECK (fake.requests.size() == 3);
}

TEST_CASE ("a slot's saved state is restored when its plugin arrives", "[plugin][insert_host][state]")
{
    FakeInstantiator fake;
    FakeStateStore states;
    states.blobs["hash-0.25"] = juce::MemoryBlock ("0.25", 4);
    InsertHost host { fake.instantiator(), states.loader(), hostRate, hostBlock };
    auto song = songWithChains();
    song.settings.pluginStateHashBySlot["a1"] = "hash-0.25";
    host.sync (song);

    auto* restored = fake.succeed (0, 0.9f);
    CHECK_THAT (restored->mix->get(), Catch::Matchers::WithinULP (0.25f, 0));
}

TEST_CASE ("a missing state blob is logged and the plugin still loads with its defaults", "[plugin][insert_host][state]")
{
    FakeInstantiator fake;
    FakeStateStore states;
    InsertHost host { fake.instantiator(), states.loader(), hostRate, hostBlock };
    auto song = songWithChains();
    song.settings.pluginStateHashBySlot["a1"] = "hash-gone";
    host.sync (song);
    ScopedLogCapture capture;

    auto* loaded = fake.succeed (0, 0.75f);

    CHECK (host.status ("a1") == InsertStatus::ready);
    CHECK_THAT (loaded->mix->get(), Catch::Matchers::WithinULP (0.75f, 0));
    const auto errors = eventsNamed (capture, "song.plugin_state_missing");
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].at ("slot_id") == "a1");
}

TEST_CASE ("captureStates stores every ready plugin's state into its slot's hash", "[plugin][insert_host][state]")
{
    FakeInstantiator fake;
    FakeStateStore states;
    InsertHost host { fake.instantiator(), states.loader(), hostRate, hostBlock };
    auto song = songWithChains();
    song.settings.pluginStateHashBySlot["a2"] = "hash-kept";
    host.sync (song);
    fake.succeed (0, 0.25f);
    fake.succeed (2, 1.0f);

    REQUIRE (host.captureStates (song, states.store()) == 0);

    CHECK (pluginStateHash (song, "a1") == "hash-0.25");
    CHECK (pluginStateHash (song, "a2") == "hash-kept"); // still loading: unchanged
    CHECK (pluginStateHash (song, "m1") == "hash-1");
    CHECK (states.blobs.at ("hash-0.25").toString() == "0.25");
}

TEST_CASE ("a parameter change is reported once as a state change", "[plugin][insert_host][state]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    auto* plugin = fake.succeed (0);
    CHECK_FALSE (host.takeStateChanged());

    plugin->mix->setValueNotifyingHost (0.8f);

    CHECK (host.takeStateChanged());
    CHECK_FALSE (host.takeStateChanged());
}

TEST_CASE ("a ready plugin whose latency changes asks for a new plan once", "[plugin][insert_host][latency]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    auto* plugin = fake.succeed (0);
    host.insertFactory();
    CHECK_FALSE (host.takeGraphChanged());
    ScopedLogCapture capture;

    plugin->setLatencySamples (64);

    CHECK (host.takeGraphChanged());
    CHECK_FALSE (host.takeGraphChanged());
    const auto changed = eventsNamed (capture, "plugin.latency_changed");
    REQUIRE (changed.size() == 1);
    CHECK (changed[0].at ("slot_id") == "a1");
    CHECK (changed[0].at ("from") == 0);
    CHECK (changed[0].at ("to") == 64);
}

TEST_CASE ("a slot the Song no longer has is released; a late arrival for it is dropped", "[plugin][insert_host]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    fake.succeed (0);

    auto without = songWithChains();
    without.tracks[0].insertChain.clear();
    host.sync (without);
    CHECK_FALSE (host.status ("a1").has_value());
    CHECK_FALSE (host.status ("a2").has_value());

    // a2 was still loading: brought back (an undo), it is requested again, and only the new
    // request may fill it.
    host.sync (songWithChains());
    REQUIRE (fake.requests.size() == 5);
    fake.succeed (1); // the old a2 request
    CHECK (host.status ("a2") == InsertStatus::loading);
    fake.succeed (4);
    CHECK (host.status ("a2") == InsertStatus::ready);
}

TEST_CASE ("a plugin that arrives after its host is gone is dropped", "[plugin][insert_host]")
{
    FakeInstantiator fake;
    {
        InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
        host.sync (songWithChains());
    }
    fake.succeed (0); // must not touch the destroyed host
    SUCCEED();
}

TEST_CASE ("without an Instantiator every slot fails with plugin.host_unavailable", "[plugin][insert_host]")
{
    InsertHost host { {}, {}, hostRate, hostBlock };
    ScopedLogCapture capture;
    host.sync (songWithChains());

    CHECK (host.status ("a1") == InsertStatus::failed);
    CHECK (eventsNamed (capture, "plugin.host_unavailable").size() == 3);
}

TEST_CASE ("a state that cannot be stored is logged per slot and the slot keeps its hash", "[plugin][insert_host][state]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    auto song = songWithChains();
    song.settings.pluginStateHashBySlot["a1"] = "hash-old";
    host.sync (song);
    fake.succeed (0);
    fake.succeed (2);
    ScopedLogCapture capture;

    const auto failures = host.captureStates (song, [] (const juce::MemoryBlock&) -> Expected<std::string> {
        return std::unexpected (Error { "song.write_failed", "disk full", {} });
    });

    CHECK (failures == 2);
    CHECK (pluginStateHash (song, "a1") == "hash-old");
    const auto logged = eventsNamed (capture, "song.write_failed");
    REQUIRE (logged.size() == 2);
    CHECK (logged[0].at ("slot_id") == "a1");
    CHECK (logged[1].at ("slot_id") == "m1");
}

TEST_CASE ("plugin.insert_ready says whether the saved state was really restored", "[plugin][insert_host][state]")
{
    FakeInstantiator fake;
    FakeStateStore states;
    states.blobs["hash-0.25"] = juce::MemoryBlock ("0.25", 4);
    InsertHost host { fake.instantiator(), states.loader(), hostRate, hostBlock };
    auto song = songWithChains();
    song.settings.pluginStateHashBySlot["a1"] = "hash-0.25";
    song.settings.pluginStateHashBySlot["a2"] = "hash-gone";
    host.sync (song);
    ScopedLogCapture capture;

    fake.succeed (0);
    fake.succeed (1);
    fake.succeed (2);

    const auto ready = eventsNamed (capture, "plugin.insert_ready");
    REQUIRE (ready.size() == 3);
    CHECK (ready[0].at ("state_restored") == true);
    CHECK (ready[1].at ("state_restored") == false);
    CHECK (ready[2].at ("state_restored") == false);
}

//==============================================================================
// Crash recovery (issue #40): the test stands in for kAudioComponentInstanceInvalidationNotification
// by calling pluginCrashed with the slot's processor.

TEST_CASE ("a plugin whose hosting process crashed is logged, released, and no longer run by any plan", "[plugin][insert_host][crash]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    const auto song = songWithChains();
    host.sync (song);
    auto* a1 = fake.succeed (0);
    a1->setLatencySamples (32);
    fake.succeed (1);
    // A plan that the audio thread still renders when the crash arrives.
    auto planned = host.insertFactory() ("track-a", song.tracks[0].insertChain);
    REQUIRE (planned.size() == 2);
    auto* node = dynamic_cast<overdub::engine::SharedInsertNode*> (planned[0].get());
    REQUIRE (node != nullptr);
    node->startForwarding();
    juce::AudioBuffer<float> buffer (2, hostBlock);
    juce::MidiBuffer midi;
    node->processBlock (buffer, midi);
    REQUIRE (a1->blocksProcessed == 1);
    ScopedLogCapture capture;

    host.pluginCrashed (*a1);

    CHECK (host.status ("a1") == InsertStatus::crashed);
    CHECK (host.processor ("a1") == nullptr);
    node->processBlock (buffer, midi);
    CHECK (a1->blocksProcessed == 1); // the old plan's node no longer calls the dead plugin
    CHECK (host.takeGraphChanged());
    CHECK (namesOf (host.insertFactory() ("track-a", song.tracks[0].insertChain)).size() == 1);
    const auto invalidated = eventsNamed (capture, "plugin.invalidated");
    REQUIRE (invalidated.size() == 1);
    CHECK (invalidated[0].at ("lvl") == "warn");
    CHECK (invalidated[0].at ("slot_id") == "a1");
    CHECK (invalidated[0].at ("name") == "a1");
    CHECK (invalidated[0].at ("component") == "aufx,test,ovdb");
    CHECK (invalidated[0].at ("latency_samples") == 32);
    CHECK (invalidated[0].at ("out_of_process") == false);
    CHECK (invalidated[0].at ("crashes") == 1);
    CHECK (invalidated[0].at ("will_reload") == true);
}

TEST_CASE ("takeCrashes hands each crash over once; a repeated notification for it is ignored", "[plugin][insert_host][crash]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    auto* a1 = fake.succeed (0);
    fake.succeed (1);
    auto* m1 = fake.succeed (2);

    // One hosting process may host several plugins: they all go at once.
    host.pluginCrashed (*a1);
    host.pluginCrashed (*m1);
    host.pluginCrashed (*a1);

    const auto crashes = host.takeCrashes();
    REQUIRE (crashes.size() == 2);
    CHECK (crashes[0].slotId == "a1");
    CHECK (crashes[0].name == "a1");
    CHECK (crashes[0].willReload);
    CHECK (crashes[1].slotId == "m1");
    CHECK (host.takeCrashes().empty());
    CHECK (host.status ("a2") == InsertStatus::ready);
}

TEST_CASE ("a crashed slot reloads with the state the Song holds for it now", "[plugin][insert_host][crash]")
{
    FakeInstantiator fake;
    FakeStateStore states;
    InsertHost host { fake.instantiator(), states.loader(), hostRate, hostBlock };
    auto song = songWithChains();
    host.sync (song);
    auto* a1 = fake.succeed (0, 0.25f);
    fake.succeed (1);
    fake.succeed (2);
    // The last capture before the crash stored 0.25 into the slot.
    REQUIRE (host.captureStates (song, states.store()) == 0);
    host.pluginCrashed (*a1);
    host.takeCrashes();
    host.takeGraphChanged();
    ScopedLogCapture capture;

    CHECK (host.reloadCrashed (song) == 1);

    REQUIRE (fake.requests.size() == 4);
    CHECK (fake.requests[3].slot.id == "a1");
    CHECK (host.status ("a1") == InsertStatus::loading);
    CHECK (host.reloadCrashed (song) == 0); // already loading
    const auto reloading = eventsNamed (capture, "plugin.insert_reloading");
    REQUIRE (reloading.size() == 1);
    CHECK (reloading[0].at ("slot_id") == "a1");
    CHECK (reloading[0].at ("crashes") == 1);

    auto* reloaded = fake.succeed (3, 0.9f);
    CHECK (host.status ("a1") == InsertStatus::ready);
    CHECK (host.processor ("a1") == reloaded);
    CHECK_THAT (reloaded->mix->get(), Catch::Matchers::WithinULP (0.25f, 0));
    CHECK (host.takeGraphChanged());
}

TEST_CASE ("a slot that crashes a third time stays crashed and is not reloaded again", "[plugin][insert_host][crash]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    const auto song = songWithChains();
    host.sync (song);
    auto* plugin = fake.succeed (0);

    for (int reload = 0; reload < overdub::plugin::maxCrashReloads; ++reload)
    {
        host.pluginCrashed (*plugin);
        const auto crashes = host.takeCrashes();
        REQUIRE (crashes.size() == 1);
        CHECK (crashes[0].willReload);
        REQUIRE (host.reloadCrashed (song) == 1);
        plugin = fake.succeed (fake.requests.size() - 1);
    }
    ScopedLogCapture capture;
    host.pluginCrashed (*plugin);

    const auto crashes = host.takeCrashes();
    REQUIRE (crashes.size() == 1);
    CHECK (crashes[0].crashes == 3);
    CHECK_FALSE (crashes[0].willReload);
    const auto requested = fake.requests.size();
    CHECK (host.reloadCrashed (song) == 0);
    host.sync (song);
    CHECK (fake.requests.size() == requested);
    CHECK (host.status ("a1") == InsertStatus::crashed);
    const auto invalidated = eventsNamed (capture, "plugin.invalidated");
    REQUIRE (invalidated.size() == 1);
    CHECK (invalidated[0].at ("will_reload") == false);
}

TEST_CASE ("a crashed slot the Song loses is forgotten; brought back by an undo it loads afresh", "[plugin][insert_host][crash]")
{
    FakeInstantiator fake;
    InsertHost host { fake.instantiator(), {}, hostRate, hostBlock };
    host.sync (songWithChains());
    host.pluginCrashed (*fake.succeed (0));

    auto without = songWithChains();
    without.tracks[0].insertChain.erase (without.tracks[0].insertChain.begin());
    host.sync (without);
    CHECK_FALSE (host.status ("a1").has_value());

    host.sync (songWithChains());
    CHECK (host.status ("a1") == InsertStatus::loading);
    REQUIRE (fake.requests.size() == 4);
    CHECK (fake.requests[3].slot.id == "a1");
}
