#pragma once

#include "core/Error.h"
#include "engine/RenderPlan.h"
#include "plugin/AudioUnitLoader.h"
#include "plugin/InvalidationWatch.h"
#include "song/Song.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
//   takeGraphChanged() says when a new plan is needed: a slot became ready, or a ready plugin's
//   latency is not the one the last plan used.
// - captureStates() writes every ready plugin's state into its slot (ADR 0004 blobs); the package
//   calls it on each history step and debounced write. takeStateChanged() says a parameter moved.
// - Crash recovery (issue #40, ADR 0002): when a plugin's hosting process dies (an
//   InvalidationWatch hears kAudioComponentInstanceInvalidationNotification for its AU; tests call
//   pluginCrashed), its slot becomes crashed: every node that runs it passes audio through from
//   then on (engine::SharedInsertNode), its processor is released, the next plan leaves it out
//   (takeGraphChanged) and takeCrashes() reports it once. reloadCrashed() loads it again with the
//   state its Song slot holds then (the last capture: a dead plugin's own state cannot be read).
//   A slot reloads after at most maxCrashReloads crashes; after one more it stays crashed until
//   the Song is opened again (or the slot leaves the Song and comes back).
//
// Message thread only (the plugins' parameter callbacks may come from any thread).
namespace overdub::plugin
{
enum class InsertStatus
{
    loading,
    ready,
    failed,
    crashed // its plugin's hosting process died; out of the plan until it reloads
};

// A crashed slot is reloaded automatically after its first maxCrashReloads crashes, not after more.
inline constexpr int maxCrashReloads = 2;

// One crash, as takeCrashes() reports it.
struct InsertCrash
{
    song::Id slotId;
    std::string name;
    int crashes = 0;         // how many times this slot's plugin crashed since it was first loaded
    bool willReload = false; // false once it crashed more than maxCrashReloads times

    bool operator== (const InsertCrash&) const = default;
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
    // The same, shared: an editor window keeps it alive until it closes (issue #34).
    std::shared_ptr<juce::AudioProcessor> sharedProcessor (const song::Id& slotId) const;

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

    // The slot whose ready processor this is crashed (see the header comment). Ignored for a
    // processor that is not a ready slot's (a repeated notification for the same crash). Logs
    // plugin.invalidated (warn: slot_id, name, component, version, latency_samples, tail_seconds,
    // out_of_process, crashes, will_reload).
    void pluginCrashed (const juce::AudioProcessor& processor);
    // kAudioComponentInstanceInvalidationNotification named this AU instance (InvalidationWatch):
    // pluginCrashed for the ready slot whose AU it is; plugin.invalidation_unmatched (debug:
    // matched none) otherwise.
    void audioUnitInvalidated (const void* audioUnit);
    // Every crash since the last call, in the order they happened.
    std::vector<InsertCrash> takeCrashes();
    // Starts loading every crashed slot of the Song that may still reload, with the Song's slot
    // as it is now (its stateHash restored on arrival). Returns how many it started. Logs
    // plugin.insert_reloading (info: slot_id, crashes).
    int reloadCrashed (const song::Song& song);

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
        // Set when the plugin crashed: read by every plan node that runs this processor.
        std::shared_ptr<std::atomic<bool>> crashedFlag;
        int crashes = 0;
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
    std::vector<InsertCrash> crashesToReport;
    std::atomic<bool> stateChanged { false };
    // Expires with the host, so an instantiation that finishes later drops its processor.
    std::shared_ptr<int> lifetime = std::make_shared<int> (0);
    // Last: destroyed first, so no crash notification reaches a host being torn down.
    InvalidationWatch invalidations { [this] (const void* audioUnit) { audioUnitInvalidated (audioUnit); } };
};
} // namespace overdub::plugin
