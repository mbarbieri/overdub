#include "engine/RenderPlan.h"

#include "core/Log.h"
#include "engine/ClipSchedule.h"
#include "engine/TrackPlayer.h"
#include "song/BackingTrackImport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <map>
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
    // lacks the source).
class SourceLoader
{
public:
    SourceLoader (const song::Song& s, const juce::File& root) : song (s), packageRoot (root) {}

    SourceAudio load (const song::ClipSource& source)
    {
        const auto mediaFile = song::mediaFileFor (packageRoot, song, source);
        const auto path = mediaFile.has_value() ? mediaFile->getFullPathName().toStdString() : std::string {};
        if (const auto cached = loaded.find (path); cached != loaded.end())
            return cached->second;

        const auto file = mediaFile.value_or (packageRoot.getChildFile ("<unknown " + juce::String (std::string (song::toString (source.kind))) + " " + juce::String (source.id) + ">"));
        auto audio = loadSource (source, file, song.sampleRate);
        loaded.emplace (path, audio);
        return audio;
    }

private:
    const song::Song& song;
    juce::File packageRoot;
    std::map<std::string, SourceAudio> loaded;
};

std::unique_ptr<TrackPlayer> makeTrackPlayer (const song::Track& track, SourceLoader& loader, const LiveInput& liveInput)
{
    std::vector<LayeredClip> layered;
    std::vector<SourceAudio> sources;
    for (const auto& clip : track.clips)
    {
        auto audio = loader.load (clip.source);
        layered.push_back ({ .timelineStart = clip.timelineStart, .sourceOffset = clip.sourceOffset, .length = clip.length, .source = audio });
        if (audio != nullptr)
            sources.push_back (std::move (audio));
    }
    // Only an Armed Track hears its input; an unarmed Track's input channels stay silent.
    auto monitorChannels = track.armed ? track.inputChannels : std::vector<int> {};
    return std::make_unique<TrackPlayer> (juce::String (track.id), flattenClips (layered), std::move (sources), liveInput, std::move (monitorChannels));
}

void connectStereo (Graph& graph, Graph::NodeID from, Graph::NodeID to)
{
    for (int channel = 0; channel < 2; ++channel)
        graph.addConnection ({ { from, channel }, { to, channel } }, Graph::UpdateKind::none);
}
} // namespace

RenderPlan::RenderPlan (const song::Song& song, const juce::File& packageRoot, const EngineConfig& engineConfig, const TrackInsertFactory& makeInserts)
    : config (engineConfig), songSampleRate (song.sampleRate), trackCount ((int) song.tracks.size())
{
    graph.setPlayConfigDetails (0, config.numOutputChannels, songSampleRate, config.maxBlockSize);

    const auto output = graph.addNode (std::make_unique<Graph::AudioGraphIOProcessor> (Graph::AudioGraphIOProcessor::audioOutputNode), {}, Graph::UpdateKind::none);
    const auto master = graph.addNode (std::make_unique<MasterNode>(), {}, Graph::UpdateKind::none);

    SourceLoader loader { song, packageRoot };
    int clipCount = 0;
    for (const auto& track : song.tracks)
    {
        clipCount += (int) track.clips.size();
        auto previous = graph.addNode (makeTrackPlayer (track, loader, liveInput), {}, Graph::UpdateKind::none)->nodeID;

        if (makeInserts)
        {
            for (auto& insert : makeInserts (track))
            {
                const auto node = graph.addNode (std::move (insert), {}, Graph::UpdateKind::none)->nodeID;
                connectStereo (graph, previous, node);
                previous = node;
            }
        }

        connectStereo (graph, previous, master->nodeID);
    }

    for (int channel = 0; channel < std::min (2, config.numOutputChannels); ++channel)
        graph.addConnection ({ { master->nodeID, channel }, { output->nodeID, channel } }, Graph::UpdateKind::none);

    graph.setPlayHead (&playHead);
    // Synchronous rebuild on this thread: prepares every node and computes the PDC latency.
    graph.prepareToPlay (songSampleRate, config.maxBlockSize);
    graph.setNonRealtime (false);
    graphLatency = graph.getLatencySamples();

    // Warm-up: the first processBlock installs the render sequence, so the audio thread never
    // reaches the graph's install-or-rebuild path. Then clear any state the warm-up left.
    juce::AudioBuffer<float> scratch { config.numOutputChannels, config.maxBlockSize };
    graph.processBlock (scratch, midi);
    graph.reset();

    core::logEvent (core::Level::info,
                    "engine.plan_built",
                    { { "tracks", trackCount },
                      { "clips", clipCount },
                      { "latency_samples", graphLatency },
                      { "sample_rate", songSampleRate },
                      { "output_channels", config.numOutputChannels },
                      { "max_block_size", config.maxBlockSize } });
}

std::uint64_t RenderPlan::render (float* const* outputs, const LiveInput& input, int numSamples, std::int64_t renderSample, bool playing) noexcept
{
    liveInput = input;
    playHead.renderSample = renderSample;
    playHead.playing = playing;

    // The patched graph counts busy nodes process-wide (patches/juce-graph-realtime-safe.patch);
    // only the audio thread renders, so the difference across this call is this block's count.
    const auto busyBefore = juce::getAudioProcessorGraphBusyNodeBlockCount();

    // References the device's buffers (no allocation for fewer than 32 channels).
    juce::AudioBuffer<float> block { outputs, config.numOutputChannels, numSamples };
    graph.processBlock (block, midi);

    return juce::getAudioProcessorGraphBusyNodeBlockCount() - busyBefore;
}
} // namespace overdub::engine
