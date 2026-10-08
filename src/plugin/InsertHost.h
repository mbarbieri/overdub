#pragma once

#include "core/Error.h"
#include "engine/RenderPlan.h"
#include "plugin/AudioUnitLoader.h"
#include "song/Song.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

// The plugins of an open Song's Insert Chains (issue #33). One processor per Insert Chain slot (by
// slot id, the Tracks' chains and the Master's), created asynchronously through an Instantiator
// (AudioUnitLoader in the app: out-of-process AUs, ADR 0002) and kept alive across render plans:
// each plan runs it through an engine::SharedInsertNode, so editing the Song never re-creates a
// plugin or loses its state.
//
// - sync() follows the Song: a slot with no processor starts loading (its saved state, stateHash,
//   is restored when it arrives), a slot the Song lost is released (plans that still run it keep
//   it alive until the engine frees them, off the audio thread). A slot that failed stays failed:
//   an inert placeholder, its Song entry and state kept.
// - The engine plays only ready slots that are not bypassed (insertFactory(); issue #34: a
//   bypassed slot is left out of the plan, latency included, and keeps its plugin and state).
//   takeGraphChanged() says when a new plan is
//   needed: a slot became ready, or a ready plugin's latency is not the one the last plan used.
// - captureStates() writes every ready plugin's state into its slot (ADR 0004 blobs); the package
//   calls it on each history step and debounced write. takeStateChanged() says a parameter moved.
//
// Message thread only (the plugins' parameter callbacks may come from any thread).
namespace overdub::plugin
{
enum class InsertStatus
{
    loading,
    ready,
    failed
};

// Reads a slot's saved state by its hash (SongPackage::loadPluginState).
using StateLoader = std::function<core::Expected<juce::MemoryBlock> (const std::string& hash)>;
// Stores state bytes and returns their hash (SongPackage::storePluginState).
using StateStore = std::function<core::Expected<std::string> (const juce::MemoryBlock& state)>;

class InsertHost final : private juce::AudioProcessorListener
{
public:
    // An empty instantiator loads nothing: every slot fails with plugin.host_unavailable.
    InsertHost (Instantiator instantiate, StateLoader loadState, double sampleRate, int blockSize);
    ~InsertHost() override;

    InsertHost (const InsertHost&) = delete;
    InsertHost& operator= (const InsertHost&) = delete;

    // Starts loading every slot of the Song that has no processor, releases those of slots it no
    // longer has. Logs plugin.insert_requested (debug: slot_id, component) and
    // plugin.insert_released (debug: slot_id).
    void sync (const song::Song& song);

    // The engine's InsertFactory for the next plan: per chain, a SharedInsertNode for each ready
    // slot, in chain order (loading, failed and bypassed slots are left out). Remembers each latency it saw,
    // so the plan it builds is up to date: takeGraphChanged() is false until something changes.
    engine::InsertFactory insertFactory();

    // The slot's status; none for a slot sync() has not seen.
    std::optional<InsertStatus> status (const song::Id& slotId) const;
    // The slot's ready processor, or null.
    juce::AudioProcessor* processor (const song::Id& slotId) const;

    // True once since a slot became ready or a ready plugin's latency changed from what the last
    // insertFactory() saw (logs plugin.latency_changed, info: slot_id, from, to).
    bool takeGraphChanged();
    // True once since any ready plugin reported a parameter change.
    bool takeStateChanged();

    // Stores the state of every ready slot of the Song and sets its stateHash. A slot whose state
    // cannot be captured or stored keeps its hash; each such failure is logged (warn, with
    // slot_id and name: plugin.state_capture_failed or the store's code) and counted. Returns the
    // number of failures.
    int captureStates (song::Song& song, const StateStore& store);

    // Prepares every ready plugin for a new device shape. Invariant (the caller's): no plan may be
    // rendering them, i.e. no engine that ever played them still has a device calling process().
    void prepare (double sampleRate, int blockSize);

private:
    struct Entry
    {
        song::InsertSlot slot; // as requested
        InsertStatus status = InsertStatus::loading;
        std::uint64_t request = 0; // which request may fill it
        std::shared_ptr<juce::AudioProcessor> processor;
        int latencyInPlan = 0; // the latency the last insertFactory() saw
    };

    void request (const song::InsertSlot& slot);
    void arrived (const song::Id& slotId, std::uint64_t request, core::Expected<std::unique_ptr<juce::AudioProcessor>> created);
    void release (Entry& entry);

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}

    Instantiator instantiate;
    StateLoader loadState;
    double sampleRate;
    int blockSize;
    std::map<song::Id, Entry> entries;
    std::uint64_t nextRequest = 1;
    bool graphChanged = false;
    std::atomic<bool> stateChanged { false };
    // Expires with the host, so an instantiation that finishes later drops its processor.
    std::shared_ptr<int> lifetime = std::make_shared<int> (0);
};
} // namespace overdub::plugin
