#include "plugin/InsertHost.h"

#include "core/Log.h"
#include "engine/SharedInsertNode.h"
#include "plugin/AudioUnitIdentifier.h"
#include "plugin/PluginState.h"

#include <set>

namespace overdub::plugin
{
namespace
{
// Every Insert Chain slot of the Song: each Track's, then the Master's.
std::vector<const song::InsertSlot*> slotsOf (const song::Song& song)
{
    std::vector<const song::InsertSlot*> slots;
    for (const auto& track : song.tracks)
        for (const auto& slot : track.insertChain)
            slots.push_back (&slot);
    for (const auto& slot : song.master.insertChain)
        slots.push_back (&slot);
    return slots;
}

void logFailure (core::Error error, const song::InsertSlot& slot)
{
    error.fields["slot_id"] = slot.id;
    error.fields["name"] = slot.name;
    core::logError (error);
}
} // namespace

InsertHost::InsertHost (Instantiator instantiator, StateLoader stateLoader, double rate, int samplesPerBlock)
    : instantiate (std::move (instantiator)), loadState (std::move (stateLoader)), sampleRate (rate), blockSize (samplesPerBlock)
{
}

InsertHost::~InsertHost()
{
    for (auto& [slotId, entry] : entries)
        release (entry);
}

void InsertHost::sync (const song::Song& song)
{
    std::set<song::Id> wanted;
    for (const auto* slot : slotsOf (song))
    {
        wanted.insert (slot->id);
        if (! entries.contains (slot->id))
            request (*slot);
    }

    for (auto entry = entries.begin(); entry != entries.end();)
    {
        if (wanted.contains (entry->first))
        {
            ++entry;
            continue;
        }
        core::logEvent (core::Level::debug, "plugin.insert_released", { { "slot_id", entry->first } });
        release (entry->second);
        entry = entries.erase (entry);
    }
}

void InsertHost::request (const song::InsertSlot& slot)
{
    auto& entry = entries[slot.id];
    entry.slot = slot;
    entry.request = nextRequest++;
    core::logEvent (core::Level::debug, "plugin.insert_requested", { { "slot_id", slot.id }, { "component", componentText (slot.component) } });

    if (! instantiate)
    {
        entry.status = InsertStatus::failed;
        logFailure (core::Error { "plugin.host_unavailable", "plugins cannot be loaded here", {} }, slot);
        return;
    }

    std::weak_ptr<int> alive = lifetime;
    instantiate (slot, sampleRate, blockSize, [this, alive, slotId = slot.id, requestId = entry.request] (core::Expected<std::unique_ptr<juce::AudioProcessor>> created) {
        if (alive.expired())
            return;
        arrived (slotId, requestId, std::move (created));
    });
}

void InsertHost::arrived (const song::Id& slotId, std::uint64_t requestId, core::Expected<std::unique_ptr<juce::AudioProcessor>> created)
{
    const auto found = entries.find (slotId);
    // Released (or released and requested again) while it loaded: this one is not wanted.
    if (found == entries.end() || found->second.request != requestId)
        return;

    auto& entry = found->second;
    if (! created.has_value())
    {
        entry.status = InsertStatus::failed;
        logFailure (created.error(), entry.slot);
        return;
    }

    auto& processor = **created;
    processor.prepareToPlay (sampleRate, blockSize);
    bool stateRestored = false;
    if (! entry.slot.stateHash.empty())
    {
        auto restored = loadState ? loadState (entry.slot.stateHash) : core::Expected<juce::MemoryBlock> { std::unexpected (core::Error { "plugin.state_unavailable", "no plugin state store", {} }) };
        if (restored.has_value())
        {
            auto applied = restoreState (processor, *restored);
            stateRestored = applied.has_value();
            if (! stateRestored)
                logFailure (applied.error(), entry.slot);
        }
        else
        {
            logFailure (restored.error(), entry.slot);
        }
    }

    processor.addListener (this);
    entry.processor = std::shared_ptr<juce::AudioProcessor> (std::move (*created));
    entry.status = InsertStatus::ready;
    graphChanged = true;
    core::logEvent (core::Level::info,
                    "plugin.insert_ready",
                    { { "slot_id", slotId },
                      { "name", entry.processor->getName().toStdString() },
                      { "latency_samples", entry.processor->getLatencySamples() },
                      { "state_restored", stateRestored } });
}

void InsertHost::release (Entry& entry)
{
    if (entry.processor != nullptr)
        entry.processor->removeListener (this);
    entry.processor.reset();
}

engine::InsertFactory InsertHost::insertFactory()
{
    // The plan this factory builds has every ready plugin at its current latency.
    graphChanged = false;
    std::map<song::Id, std::shared_ptr<juce::AudioProcessor>> ready;
    for (auto& [slotId, entry] : entries)
    {
        if (entry.status != InsertStatus::ready)
            continue;
        entry.latencyInPlan = entry.processor->getLatencySamples();
        ready.emplace (slotId, entry.processor);
    }

    return [ready] (const song::Id&, const std::vector<song::InsertSlot>& chain) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> nodes;
        for (const auto& slot : chain)
            if (const auto found = ready.find (slot.id); ! slot.bypassed && found != ready.end())
                nodes.push_back (std::make_unique<engine::SharedInsertNode> (found->second));
        return nodes;
    };
}

std::optional<InsertStatus> InsertHost::status (const song::Id& slotId) const
{
    const auto found = entries.find (slotId);
    return found != entries.end() ? std::optional { found->second.status } : std::nullopt;
}

juce::AudioProcessor* InsertHost::processor (const song::Id& slotId) const
{
    const auto found = entries.find (slotId);
    return found != entries.end() ? found->second.processor.get() : nullptr;
}

std::shared_ptr<juce::AudioProcessor> InsertHost::sharedProcessor (const song::Id& slotId) const
{
    const auto found = entries.find (slotId);
    return found != entries.end() ? found->second.processor : nullptr;
}

bool InsertHost::takeGraphChanged()
{
    for (auto& [slotId, entry] : entries)
    {
        if (entry.status != InsertStatus::ready)
            continue;
        const auto latency = entry.processor->getLatencySamples();
        if (latency == entry.latencyInPlan)
            continue;
        core::logEvent (core::Level::info, "plugin.latency_changed", { { "slot_id", slotId }, { "from", entry.latencyInPlan }, { "to", latency } });
        entry.latencyInPlan = latency;
        graphChanged = true;
    }
    return std::exchange (graphChanged, false);
}

bool InsertHost::takeStateChanged()
{
    return stateChanged.exchange (false);
}

int InsertHost::captureStates (song::Song& song, const StateStore& store)
{
    int failures = 0;
    const auto logFailed = [&failures] (core::Error error, const song::InsertSlot& slot) {
        ++failures;
        error.fields["slot_id"] = slot.id;
        error.fields["name"] = slot.name;
        core::logError (error, core::Level::warn);
    };
    const auto captureInto = [&] (song::InsertSlot& slot) {
        const auto found = entries.find (slot.id);
        if (found == entries.end() || found->second.status != InsertStatus::ready)
            return;
        auto state = captureState (*found->second.processor);
        if (! state.has_value())
            return logFailed (state.error(), slot);
        if (state->isEmpty())
            return;
        auto hash = store (*state);
        if (! hash.has_value())
            return logFailed (hash.error(), slot);
        slot.stateHash = *hash;
    };

    for (auto& track : song.tracks)
        for (auto& slot : track.insertChain)
            captureInto (slot);
    for (auto& slot : song.master.insertChain)
        captureInto (slot);
    return failures;
}

void InsertHost::prepare (double rate, int samplesPerBlock)
{
    sampleRate = rate;
    blockSize = samplesPerBlock;
    for (auto& [slotId, entry] : entries)
    {
        if (entry.status != InsertStatus::ready)
            continue;
        entry.processor->releaseResources();
        entry.processor->prepareToPlay (sampleRate, blockSize);
    }
}

void InsertHost::audioProcessorParameterChanged (juce::AudioProcessor*, int, float)
{
    stateChanged.store (true);
}
} // namespace overdub::plugin
