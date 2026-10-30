#include "engine/RenderPlan.h"

#include "core/Error.h"
#include "core/Log.h"
#include "engine/ClipSchedule.h"
#include "engine/SharedInsertNode.h"
#include "engine/TrackPlayer.h"
#include "song/PracticeSpeed.h"
#include "song/Transpose.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>

namespace overdub::engine
{
using Graph = juce::AudioProcessorGraph;

juce::Optional<juce::AudioPlayHead::PositionInfo> PlanPlayHead::getPosition() const
{
    PositionInfo info;
    info.setTimeInSamples (renderSample);
    info.setIsPlaying (playing);
    return info;
}

std::int64_t songToPlanSample (std::int64_t songSample, int speedPercent)
{
    return speedPercent == song::normalPracticeSpeed ? songSample : (std::int64_t) std::llround ((double) songSample * 100.0 / speedPercent);
}

std::int64_t planToSongSample (std::int64_t planSample, int speedPercent)
{
    return speedPercent == song::normalPracticeSpeed ? planSample : (std::int64_t) std::llround ((double) planSample * speedPercent / 100.0);
}

namespace
{
    // Every Clip source that plays silence is logged here: `event` with the source's kind, id and
    // path, plus any event-specific fields.
void logSilentSource (const char* event, const song::ClipSource& source, const juce::File& file, const char* message, core::Fields extra = core::Fields::object())
{
    extra["source_kind"] = song::toString (source.kind);
    extra["source_id"] = source.id;
    extra["path"] = file.getFullPathName().toStdString();
    core::logEvent (core::Level::warn, event, extra, message);
}

void logUnusable (const song::ClipSource& source, const juce::File& file, const char* reason, core::Fields extra = core::Fields::object())
{
    extra["reason"] = reason;
    logSilentSource ("engine.clip_source_unusable", source, file, "Clip source plays silence", std::move (extra));
}

    // Decodes a 32-bit float WAV at the Song rate, mono or stereo. Anything else plays silence.
SourceAudio loadSource (const song::ClipSource& source, const juce::File& file, int songSampleRate)
{
    if (! file.existsAsFile())
    {
        logSilentSource ("engine.clip_source_missing", source, file, "Clip source file is missing; it plays silence");
        return nullptr;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader { wav.createReaderFor (new juce::FileInputStream (file), true) };
    if (reader == nullptr)
    {
        logUnusable (source, file, "not_wav");
        return nullptr;
    }
    if (! reader->usesFloatingPointData || reader->bitsPerSample != 32)
    {
        logUnusable (source, file, "not_float32", { { "bits_per_sample", reader->bitsPerSample } });
        return nullptr;
    }
    if ((int) reader->sampleRate != songSampleRate)
    {
        logUnusable (source, file, "sample_rate_mismatch", { { "file_rate", reader->sampleRate }, { "song_rate", songSampleRate } });
        return nullptr;
    }
    if (reader->numChannels < 1 || reader->numChannels > 2)
    {
        logUnusable (source, file, "channel_count", { { "channels", reader->numChannels } });
        return nullptr;
    }

    auto audio = std::make_shared<juce::AudioBuffer<float>> ((int) reader->numChannels, (int) reader->lengthInSamples);
    if (! reader->read (audio->getArrayOfWritePointers(), audio->getNumChannels(), 0, audio->getNumSamples()))
    {
        logUnusable (source, file, "read_failed");
        return nullptr;
    }
    return audio;
}

// Loads each distinct source file once per plan, keyed by its full path (empty when the Song
// lacks the source). A source with no render at a Practice Speed, or none on the octave-shifted
// Track, plays silence.
class SourceLoader
{
public:
    SourceLoader (const song::Song& s, const juce::File& root, const PracticePlayback& p) : song (s), packageRoot (root), practice (p) {}

    SourceAudio load (const song::ClipSource& source, const song::Id& trackId)
    {
        if (practice.octaveShifted.has_value() && practice.octaveShifted->trackId == trackId)
        {
            const auto& renders = practice.octaveShifted->renders;
            const auto render = renders.find (source);
            return render != renders.end() ? loadCached (source, render->second.getFullPathName().toStdString(), render->second) : nullptr;
        }
        const auto atPracticeSpeed = practice.speedPercent != song::normalPracticeSpeed;
        if (atPracticeSpeed && ! practice.renders.contains (source))
            return nullptr;
        const auto mediaFile = atPracticeSpeed ? std::optional (practice.renders.at (source)) : song::playedFileFor (packageRoot, song, source);
        if (mediaFile.has_value())
            return loadCached (source, mediaFile->getFullPathName().toStdString(), *mediaFile);
        return loadCached (source, {}, packageRoot.getChildFile ("<unknown " + juce::String (std::string (song::toString (source.kind))) + " " + juce::String (source.id) + ">"));
    }

private:
    const song::Song& song;
    juce::File packageRoot;
    const PracticePlayback& practice;
    std::map<std::string, SourceAudio> loaded;

    SourceAudio loadCached (const song::ClipSource& source, const std::string& path, const juce::File& file)
    {
        if (const auto cached = loaded.find (path); cached != loaded.end())
            return cached->second;
        auto audio = loadSource (source, file, song.sampleRate);
        loaded.emplace (path, audio);
        return audio;
    }
};

// The Clip in plan time at `speedPercent`: every Song time and length × 100 / speedPercent, so its
// render plays it where the Clip is and its fades span the same Song samples.
LayeredClip layeredAt (const song::Clip& clip, int speedPercent, SourceAudio audio)
{
    const auto toPlan = [speedPercent] (SampleCount songSample) { return songToPlanSample (songSample, speedPercent); };
    const auto start = toPlan (clip.timelineStart);
    const auto length = toPlan (clip.timelineStart + clip.length) - start;
    // Rounding must not let the fades overlap (Clip invariant: fade-in + fade-out <= length).
    const auto fadeIn = std::min (toPlan (clip.fadeInLength), length);
    return {
        .timelineStart = start,
        .sourceOffset = toPlan (clip.sourceOffset),
        .length = length,
        .gainDb = clip.gainDb,
        .fadeInLength = fadeIn,
        .fadeOutLength = std::min (toPlan (clip.fadeOutLength), length - fadeIn),
        .source = std::move (audio),
    };
}

std::unique_ptr<TrackPlayer> makeTrackPlayer (const song::Track& track, SourceLoader& loader, const LiveInput& liveInput, bool monitored, int speedPercent)
{
    std::vector<LayeredClip> layered;
    std::vector<SourceAudio> sources;
    for (const auto& clip : track.clips)
    {
        auto audio = loader.load (clip.source, track.id);
        layered.push_back (layeredAt (clip, speedPercent, audio));
        if (audio != nullptr)
            sources.push_back (std::move (audio));
    }
    // Only a monitored Armed Track hears its input; any other Track's input channels stay silent.
    auto monitorChannels = track.settings.armed && monitored ? track.settings.inputChannels : std::vector<int> {};
    return std::make_unique<TrackPlayer> (juce::String (track.id), flattenClips (layered), std::move (sources), liveInput, std::move (monitorChannels));
}

void connectStereo (Graph& graph, Graph::NodeID from, Graph::NodeID to)
{
    for (int channel = 0; channel < 2; ++channel)
        graph.addConnection ({ { from, channel }, { to, channel } }, Graph::UpdateKind::none);
}

// What an Insert Chain added to the graph: its first and last node (none when it is empty), how
// many nodes, and its summed tail in seconds.
struct ChainNodes
{
    std::optional<Graph::NodeID> first;
    std::optional<Graph::NodeID> last;
    int count = 0;
    double tailSeconds = 0.0;
};

ChainNodes addChain (Graph& graph, std::vector<std::unique_ptr<juce::AudioProcessor>> processors)
{
    ChainNodes chain;
    for (auto& processor : processors)
    {
        chain.tailSeconds += processor->getTailLengthSeconds();
        const auto node = graph.addNode (std::move (processor), {}, Graph::UpdateKind::none)->nodeID;
        if (chain.last.has_value())
            connectStereo (graph, *chain.last, node);
        else
            chain.first = node;
        chain.last = node;
        ++chain.count;
    }
    return chain;
}

std::vector<std::unique_ptr<juce::AudioProcessor>> makeChain (const InsertFactory& makeInserts, const song::Id& trackId, const std::vector<song::InsertSlot>& slots)
{
    return makeInserts ? makeInserts (trackId, slots) : std::vector<std::unique_ptr<juce::AudioProcessor>> {};
}
} // namespace

RenderPlan::RenderPlan (const song::Song& song, const juce::File& packageRoot, const EngineConfig& engineConfig, const InsertFactory& makeInserts, const MonitoredTracks& monitoredTracks, const PracticePlayback& practice)
    : config (engineConfig), songSampleRate (song.sampleRate), practiceSpeedPercent (practice.speedPercent), trackCount ((int) song.tracks.size())
{
    for (const auto& track : song.tracks)
    {
        trackIds.push_back (track.id);
        trackStrips.push_back (std::make_unique<StripControl>());
        if (track.settings.armed)
        {
            auto tap = std::make_unique<InputTap>();
            tap->trackId = track.id;
            tap->channels = track.settings.inputChannels;
            inputTaps.push_back (std::move (tap));
        }
    }
    // Before the strip nodes are made: each starts from its control's gains, without a ramp.
    applyMix (song);

    graph.setPlayConfigDetails (0, config.numOutputChannels, songSampleRate, config.maxBlockSize);

    const auto output = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode), {}, Graph::UpdateKind::none);
    const auto master = graph.addNode (std::make_unique<MixStripNode> ("Master", masterStrip), {}, Graph::UpdateKind::none)->nodeID;

    // Every Track strip sums into the Master's Insert Chain, or straight into the Master strip.
    const auto masterChain = addChain (graph, makeChain (makeInserts, {}, song.master.insertChain));
    const auto masterInput = masterChain.first.value_or (master);
    if (masterChain.last.has_value())
        connectStereo (graph, *masterChain.last, master);

    SourceLoader loader { song, packageRoot, practice };
    int clipCount = 0;
    int insertCount = masterChain.count;
    double longestTrackTailSeconds = 0.0;
    for (std::size_t trackIndex = 0; trackIndex < song.tracks.size(); ++trackIndex)
    {
        const auto& track = song.tracks[trackIndex];
        clipCount += (int) track.clips.size();
        const auto monitored = ! monitoredTracks.has_value() || monitoredTracks->contains (track.id);
        const auto player = graph.addNode (makeTrackPlayer (track, loader, liveInput, monitored, practiceSpeedPercent), {}, Graph::UpdateKind::none)->nodeID;

        const auto chain = addChain (graph, makeChain (makeInserts, track.id, track.insertChain));
        insertCount += chain.count;
        longestTrackTailSeconds = std::max (longestTrackTailSeconds, chain.tailSeconds);
        if (chain.first.has_value())
            connectStereo (graph, player, *chain.first);

        const auto strip = graph.addNode (std::make_unique<MixStripNode> ("Strip " + juce::String (track.id), *trackStrips[trackIndex]), {}, Graph::UpdateKind::none)->nodeID;
        connectStereo (graph, chain.last.value_or (player), strip);
        connectStereo (graph, strip, masterInput);
    }

    for (int channel = 0; channel < std::min (2, config.numOutputChannels); ++channel)
        graph.addConnection ({ { master, channel }, { output->nodeID, channel } }, Graph::UpdateKind::none);

    graph.setPlayHead (&playHead);
    // Synchronous rebuild on this thread: prepares every node and computes the PDC latency.
    graph.prepareToPlay (songSampleRate, config.maxBlockSize);
    graph.setNonRealtime (false);
    graphLatency = graph.getLatencySamples();
    graphTail = (int) std::lround ((longestTrackTailSeconds + masterChain.tailSeconds) * songSampleRate);

    // Warm-up: the first processBlock installs the render sequence, so the audio thread never
    // reaches the graph's install-or-rebuild path. Then clear any state the warm-up left.
    juce::AudioBuffer<float> scratch { config.numOutputChannels, config.maxBlockSize };
    graph.processBlock (scratch, midi);
    graph.reset();
    takeMeterPeaks();

    // Only now may shared plugins run: the warm-up passed audio through them untouched, because
    // the plan the audio thread plays may be running them (engine/SharedInsertNode.h).
    for (auto* node : graph.getNodes())
        if (auto* shared = dynamic_cast<SharedInsertNode*> (node->getProcessor()))
            shared->startForwarding();

    core::logEvent (core::Level::info,
                    "engine.plan_built",
                    { { "tracks", trackCount },
                      { "clips", clipCount },
                      { "inserts", insertCount },
                      { "latency_samples", graphLatency },
                      { "tail_samples", graphTail },
                      { "sample_rate", songSampleRate },
                      { "practice_speed_percent", practiceSpeedPercent },
                      { "output_channels", config.numOutputChannels },
                      { "max_block_size", config.maxBlockSize } });
}

bool RenderPlan::hasTracksOf (const song::Song& song) const
{
    return std::ranges::equal (song.tracks, trackIds, {}, &song::Track::id);
}

void RenderPlan::applyMix (const song::Song& song)
{
    if (! OVERDUB_CHECK (hasTracksOf (song), "engine.mix_tracks_mismatch", { "plan_tracks", trackCount }, { "song_tracks", song.tracks.size() }))
        return;
    const auto gains = mixGains (song);
    for (std::size_t i = 0; i < trackStrips.size(); ++i)
        trackStrips[i]->setGains (gains.tracks[i]);
    masterStrip.setGains ({ gains.master, gains.master });
}

void RenderPlan::applyBandFilter (const std::optional<HeardBandFilter>& filter)
{
    for (std::size_t i = 0; i < trackStrips.size(); ++i)
    {
        const auto heard = filter.has_value() && filter->trackId == trackIds[i];
        trackStrips[i]->bandFilter.set (heard ? BandFilterSetting { true, filter->lowHz, filter->highHz } : BandFilterSetting {});
    }
}

MeterPeaks RenderPlan::takeMeterPeaks()
{
    MeterPeaks peaks;
    for (std::size_t i = 0; i < trackStrips.size(); ++i)
        peaks.tracks[trackIds[i]] = trackStrips[i]->peak.take();
    for (auto& tap : inputTaps)
        peaks.inputs[tap->trackId] = tap->peak.take();
    peaks.master = masterStrip.peak.take();
    return peaks;
}

void RenderPlan::tapInputs (const LiveInput& input, int numSamples) noexcept
{
    for (auto& tap : inputTaps)
    {
        float blockPeak = 0.0f;
        for (const auto channel : tap->channels)
        {
            if (input.channels == nullptr || channel < 0 || channel >= input.numChannels || input.channels[channel] == nullptr)
                continue;
            const auto range = juce::FloatVectorOperations::findMinAndMax (input.channels[channel] + input.firstFrame, numSamples);
            blockPeak = std::max ({ blockPeak, range.getEnd(), -range.getStart() });
        }
        tap->peak.raise (blockPeak);
    }
}

std::uint64_t RenderPlan::render (float* const* outputs, const LiveInput& input, int startFrame, int numSamples, std::int64_t renderSample, bool playing) noexcept
{
    liveInput = input;
    tapInputs (input, numSamples);
    playHead.renderSample = renderSample;
    playHead.playing = playing;

    // The patched graph counts busy nodes process-wide (patches/juce-graph-realtime-safe.patch);
    // only the audio thread renders, so the difference across this call is this block's count.
    const auto busyBefore = juce::getAudioProcessorGraphBusyNodeBlockCount();

    // References the device's buffers (no allocation for fewer than 32 channels).
    juce::AudioBuffer<float> block { outputs, config.numOutputChannels, startFrame, numSamples };
    graph.processBlock (block, midi);

    return juce::getAudioProcessorGraphBusyNodeBlockCount() - busyBefore;
}
} // namespace overdub::engine
