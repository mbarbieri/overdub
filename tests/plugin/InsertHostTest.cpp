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
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    void getStateInformation (juce::MemoryBlock& state) override { state.replaceAll (juce::String (mix->get()).toRawUTF8(), juce::String (mix->get()).getNumBytesAsUTF8()); }
    void setStateInformation (const void* data, int size) override { *mix = juce::String::fromUTF8 (static_cast<const char*> (data), size).getFloatValue(); }

    juce::AudioParameterFloat* mix = nullptr;
    double preparedRate = 0.0;
    int preparedBlock = 0;
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

InsertSlot slotNamed (const std::string& id, std::string stateHash = {})
{
    return InsertSlot { .id = id, .component = { "aufx", "test", "ovdb" }, .name = id, .stateHash = std::move (stateHash) };
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
    song.tracks[0].insertChain[0].stateHash = "hash-0.25";
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
    song.tracks[0].insertChain[0].stateHash = "hash-gone";
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
    song.tracks[0].insertChain[1].stateHash = "hash-kept";
    host.sync (song);
    fake.succeed (0, 0.25f);
    fake.succeed (2, 1.0f);

    REQUIRE (host.captureStates (song, states.store()).has_value());

    CHECK (song.tracks[0].insertChain[0].stateHash == "hash-0.25");
    CHECK (song.tracks[0].insertChain[1].stateHash == "hash-kept"); // still loading: unchanged
    CHECK (song.master.insertChain[0].stateHash == "hash-1");
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
