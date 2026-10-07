// PROTOTYPE — issue #6 spike.
#include "SpikeEngine.h"
#include "SpikeLog.h"
#include "HalDump.h"
#include <cstdlib>

using namespace juce;
using NodeID = AudioProcessorGraph::NodeID;

SpikeEngine::SpikeEngine()
{
    addDefaultFormatsToManager (formats);
    writerThread.startThread (Thread::Priority::high);
}

SpikeEngine::~SpikeEngine()
{
    deviceManager.removeAudioCallback (this);
    if (recorder != nullptr)
        recorder->stopAllTakes();
    deviceManager.closeAudioDevice();
    graph.clear();
    writerThread.stopThread (2000);
}

//==============================================================================
String SpikeEngine::openDefaultDevice()
{
    auto error = deviceManager.initialise (8, 2, nullptr, true);
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        spikelog::line ("device_open_failed", { { "error", error } });
        return error.isEmpty() ? "no device" : error;
    }

    const bool preprocessingOff = device->setAudioPreprocessingEnabled (false);
    numGraphInputs = device->getActiveInputChannels().countNumberOfSetBits();
    numGraphOutputs = device->getActiveOutputChannels().countNumberOfSetBits();

    spikelog::line ("device_open", { { "type", device->getTypeName() },
                                     { "name", device->getName() },
                                     { "inputs", device->getInputChannelNames().joinIntoString ("|") },
                                     { "active_in", String (numGraphInputs) },
                                     { "active_out", String (numGraphOutputs) },
                                     { "sr", String (device->getCurrentSampleRate()) },
                                     { "buffer", String (device->getCurrentBufferSizeSamples()) },
                                     { "available_buffers", device->getAvailableBufferSizes().size() > 0
                                           ? String (device->getAvailableBufferSizes().getFirst()) + ".."
                                               + String (device->getAvailableBufferSizes().getLast()) : "?" },
                                     { "preprocessing_disabled", preprocessingOff ? "yes" : "unsupported" } });

    graph.setPlayConfigDetails (numGraphInputs, numGraphOutputs,
                                device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    graph.prepareToPlay (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    buildGraph();
    deviceManager.addAudioCallback (this);
    return {};
}

String SpikeEngine::setBufferSize (int samples)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = samples;
    auto error = deviceManager.setAudioDeviceSetup (setup, true);
    auto* device = deviceManager.getCurrentAudioDevice();
    spikelog::line ("buffer_set", { { "requested", String (samples) },
                                    { "actual", device != nullptr ? String (device->getCurrentBufferSizeSamples()) : "-" },
                                    { "error", error } });
    return error;
}

void SpikeEngine::audioDeviceAboutToStart (AudioIODevice* device)
{
    const auto ins = device->getActiveInputChannels().countNumberOfSetBits();
    const auto outs = device->getActiveOutputChannels().countNumberOfSetBits();
    const auto blockSize = device->getCurrentBufferSizeSamples();

    scratch.setSize (jmax (ins, outs, 2), blockSize * 2);
    midi.ensureSize (256);
    graph.setPlayConfigDetails (numGraphInputs, numGraphOutputs, device->getCurrentSampleRate(), blockSize);
    graph.prepareToPlay (device->getCurrentSampleRate(), blockSize);
    maxCallbackTicks.store (0);

    if (ins != numGraphInputs || outs != numGraphOutputs)
        spikelog::line ("device_channels_changed_restart_spike", { { "ins", String (ins) }, { "outs", String (outs) } });
}

void SpikeEngine::audioDeviceStopped()
{
    graph.releaseResources();
}

void SpikeEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs, int numSamples,
                                                    const AudioIODeviceCallbackContext&) [[clang::nonblocking]]
{
    const auto startTicks = Time::getHighResolutionTicks();
    const int channels = jmax (numGraphInputs, numGraphOutputs);

    if (numSamples > scratch.getNumSamples() || channels > scratch.getNumChannels())
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            FloatVectorOperations::clear (outputs[ch], numSamples);
        return;
    }

    AudioBuffer<float> buffer (scratch.getArrayOfWritePointers(), channels, numSamples);

    for (int ch = 0; ch < channels; ++ch)
    {
        if (ch < numInputs && inputs[ch] != nullptr)
            FloatVectorOperations::copy (buffer.getWritePointer (ch), inputs[ch], numSamples);
        else
            buffer.clear (ch, 0, numSamples);
    }

    midi.clear();
    graph.processBlock (buffer, midi);

    for (int ch = 0; ch < numOutputs; ++ch)
    {
        if (ch < channels)
            FloatVectorOperations::copy (outputs[ch], buffer.getReadPointer (ch), numSamples);
        else
            FloatVectorOperations::clear (outputs[ch], numSamples);
    }

    if (transport.playing.load())
        transport.blockStart.fetch_add (numSamples);

    const auto elapsed = Time::getHighResolutionTicks() - startTicks;
    auto previous = maxCallbackTicks.load();
    while (elapsed > previous && ! maxCallbackTicks.compare_exchange_weak (previous, elapsed)) {}
}

//==============================================================================
void SpikeEngine::buildGraph()
{
    graph.clear();
    using IO = AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode = graph.addNode (std::make_unique<IO> (IO::audioInputNode))->nodeID;
    outputNode = graph.addNode (std::make_unique<IO> (IO::audioOutputNode))->nodeID;

    auto backingOwned = std::make_unique<BackingPlayer> (transport);
    backing = backingOwned.get();
    backingNode = graph.addNode (std::move (backingOwned))->nodeID;

    auto recorderOwned = std::make_unique<DryRecorder> (numGraphInputs, writerThread);
    recorderOwned->setTransport (transport);
    recorder = recorderOwned.get();
    recorderNode = graph.addNode (std::move (recorderOwned))->nodeID;

    for (int ch = 0; ch < jmin (2, numGraphOutputs); ++ch)
        graph.addConnection ({ { backingNode, ch }, { outputNode, ch } });

    for (int ch = 0; ch < numGraphInputs; ++ch)
        graph.addConnection ({ { inputNode, ch }, { recorderNode, ch } });

    for (int t = 0; t < numTracks; ++t)
    {
        tracks[t].gainNode = graph.addNode (std::make_unique<MonitorGain>())->nodeID;
        tracks[t].pluginNode = {};
        rewireTrack (t);
    }

    spikelog::line ("graph_built", { { "nodes", String (graph.getNumNodes()) },
                                     { "connections", String ((int) graph.getConnections().size()) } });
}

void SpikeEngine::rewireTrack (int t)
{
    auto& track = tracks[t];
    graph.disconnectNode (track.gainNode);
    if (track.pluginNode != NodeID{})
        graph.disconnectNode (track.pluginNode);

    if (numGraphInputs == 0)
        return;

    const int in = jlimit (0, numGraphInputs - 1, track.inputChannel);

    if (auto* node = graph.getNodeForId (track.pluginNode))
    {
        auto* plugin = node->getProcessor();
        const int pluginIns = plugin->getTotalNumInputChannels();
        const int pluginOuts = plugin->getTotalNumOutputChannels();

        for (int ch = 0; ch < jmin (2, pluginIns); ++ch)
            graph.addConnection ({ { inputNode, in }, { track.pluginNode, ch } });

        graph.addConnection ({ { track.pluginNode, 0 }, { track.gainNode, 0 } });
        graph.addConnection ({ { track.pluginNode, pluginOuts > 1 ? 1 : 0 }, { track.gainNode, 1 } });
    }
    else
    {
        graph.addConnection ({ { inputNode, in }, { track.gainNode, 0 } });
        graph.addConnection ({ { inputNode, in }, { track.gainNode, 1 } });
    }

    for (int ch = 0; ch < jmin (2, numGraphOutputs); ++ch)
        graph.addConnection ({ { track.gainNode, ch }, { outputNode, ch } });

    if (auto* gain = dynamic_cast<MonitorGain*> (graph.getNodeForId (track.gainNode)->getProcessor()))
        gain->gain.store (track.monitoring ? 1.0f : 0.0f);
}

//==============================================================================
StringArray SpikeEngine::listEffectNames()
{
    StringArray names;
    AudioUnitPluginFormat au;

    for (auto& id : au.searchPathsForPlugins ({}, true, false))
        if (id.contains ("Effect"))
            names.addIfNotAlreadyThere (au.getNameOfPluginFromIdentifier (id));

    names.sortNatural();
    return names;
}

String SpikeEngine::findIdentifier (const String& name)
{
    AudioUnitPluginFormat au;

    for (auto& id : au.searchPathsForPlugins ({}, true, false))
        if (au.getNameOfPluginFromIdentifier (id) == name)
            return id;

    return {};
}

String SpikeEngine::countHostingProcesses()
{
    ChildProcess pgrep;
    if (! pgrep.start ("pgrep -lf AUHosting"))
        return "pgrep_failed";

    auto lines = StringArray::fromLines (pgrep.readAllProcessOutput().trim());
    lines.removeEmptyStrings();
    return String (lines.size());
}

void SpikeEngine::loadPlugin (int t, const String& name, bool outOfProcess, std::function<void (String)> done)
{
    const auto id = findIdentifier (name);

    if (id.isEmpty())
    {
        spikelog::line ("plugin_not_found", { { "track", String (t) }, { "name", name } });
        done ("not found: " + name);
        return;
    }

    PluginDescription desc;
    desc.pluginFormatName = "AudioUnit";
    desc.fileOrIdentifier = id;
    desc.name = name;

    // Read by the patched JUCE createAudioUnit() at instantiation time.
    if (outOfProcess)
        setenv ("OVERDUB_SPIKE_AU_OOP", "1", 1);
    else
        unsetenv ("OVERDUB_SPIKE_AU_OOP");

    auto* device = deviceManager.getCurrentAudioDevice();
    const auto startMs = Time::getMillisecondCounterHiRes();
    spikelog::line ("plugin_load_begin", { { "track", String (t) }, { "name", name }, { "id", id },
                                           { "out_of_process", outOfProcess ? "yes" : "no" } });

    formats.createPluginInstanceAsync (desc, device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
        [this, t, name, outOfProcess, startMs, done] (std::unique_ptr<AudioPluginInstance> instance, const String& error)
        {
            unsetenv ("OVERDUB_SPIKE_AU_OOP");

            if (instance == nullptr)
            {
                spikelog::line ("plugin_load_failed", { { "track", String (t) }, { "name", name }, { "error", error } });
                done (error.isEmpty() ? "load failed" : error);
                return;
            }

            AudioProcessor::BusesLayout stereo;
            stereo.inputBuses.add (AudioChannelSet::stereo());
            stereo.outputBuses.add (AudioChannelSet::stereo());
            AudioProcessor::BusesLayout monoToStereo;
            monoToStereo.inputBuses.add (AudioChannelSet::mono());
            monoToStereo.outputBuses.add (AudioChannelSet::stereo());
            const bool layoutSet = instance->setBusesLayout (stereo) || instance->setBusesLayout (monoToStereo);

            unloadPlugin (t);
            auto& track = tracks[t];
            track.pluginName = name;
            track.outOfProcessRequested = outOfProcess;
            track.loadMs = Time::getMillisecondCounterHiRes() - startMs;
            track.pluginNode = graph.addNode (std::move (instance))->nodeID;
            rewireTrack (t);

            auto fields = describeTrack (t);
            fields.set ("layout_set", layoutSet ? "yes" : "no");
            fields.set ("hosting_processes", countHostingProcesses());
            spikelog::line ("plugin_loaded", fields);
            done ({});
        });
}

void SpikeEngine::unloadPlugin (int t)
{
    auto& track = tracks[t];

    if (track.pluginNode == NodeID{})
        return;

    graph.removeNode (track.pluginNode);
    track.pluginNode = {};
    spikelog::line ("plugin_unloaded", { { "track", String (t) }, { "name", track.pluginName } });
    track.pluginName = {};
    rewireTrack (t);
}

AudioPluginInstance* SpikeEngine::getPlugin (int t)
{
    if (auto* node = graph.getNodeForId (tracks[t].pluginNode))
        return dynamic_cast<AudioPluginInstance*> (node->getProcessor());
    return nullptr;
}

StringPairArray SpikeEngine::describeTrack (int t)
{
    StringPairArray fields;
    auto& track = tracks[t];
    fields.set ("track", String (t));
    fields.set ("input", String (track.inputChannel));
    fields.set ("monitoring", track.monitoring ? "on" : "off");

    if (auto* plugin = getPlugin (t))
    {
        fields.set ("plugin", plugin->getName());
        fields.set ("out_of_process", track.outOfProcessRequested ? "requested" : "no");
        fields.set ("ins", String (plugin->getTotalNumInputChannels()));
        fields.set ("outs", String (plugin->getTotalNumOutputChannels()));
        fields.set ("latency_samples", String (plugin->getLatencySamples()));
        fields.set ("tail_s", String (plugin->getTailLengthSeconds(), 3));
        fields.set ("has_editor", plugin->hasEditor() ? "yes" : "no");
        fields.set ("load_ms", String (track.loadMs, 1));
    }
    else
    {
        fields.set ("plugin", "none");
    }

    return fields;
}

void SpikeEngine::setMonitoring (int t, bool on)
{
    tracks[t].monitoring = on;
    if (auto* gain = dynamic_cast<MonitorGain*> (graph.getNodeForId (tracks[t].gainNode)->getProcessor()))
        gain->gain.store (on ? 1.0f : 0.0f);
}

void SpikeEngine::setInputChannel (int t, int channel)
{
    tracks[t].inputChannel = channel;
    rewireTrack (t);
}

//==============================================================================
String SpikeEngine::loadBackingFile (const File& file)
{
    AudioFormatManager manager;
    manager.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader (manager.createReaderFor (file));

    if (reader == nullptr)
        return "cannot read " + file.getFullPathName();

    AudioBuffer<float> raw ((int) jmin (2u, reader->numChannels), (int) reader->lengthInSamples);
    reader->read (&raw, 0, raw.getNumSamples(), 0, true, raw.getNumChannels() > 1);

    const auto deviceRate = deviceManager.getCurrentAudioDevice()->getCurrentSampleRate();
    const auto ratio = reader->sampleRate / deviceRate;
    AudioBuffer<float> resampled (raw.getNumChannels(), (int) std::ceil (raw.getNumSamples() / ratio));

    for (int ch = 0; ch < raw.getNumChannels(); ++ch)
    {
        LagrangeInterpolator interpolator;
        interpolator.process (ratio, raw.getReadPointer (ch), resampled.getWritePointer (ch), resampled.getNumSamples(),
                              raw.getNumSamples(), 0);
    }

    spikelog::line ("backing_loaded", { { "file", file.getFileName() }, { "file_sr", String (reader->sampleRate) },
                                        { "device_sr", String (deviceRate) }, { "seconds", String (raw.getNumSamples() / reader->sampleRate, 2) } });
    setBackingBuffer (std::move (resampled));
    return {};
}

void SpikeEngine::setBackingBuffer (AudioBuffer<float> buffer)
{
    jassert (! isPlaying());
    backingBuffers.push_back (std::make_unique<AudioBuffer<float>> (std::move (buffer)));
    backing->setBuffer (backingBuffers.back().get());
}

//==============================================================================
void SpikeEngine::start (const bool armed[numTracks])
{
    stop();
    transport.blockStart.store (0);
    latencyAtLastStart = getLatency();
    const auto stamp = Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    auto takesDir = spikelog::logDirectory().getChildFile ("takes");
    takesDir.createDirectory();

    for (int t = 0; t < numTracks; ++t)
        if (armed[t])
            recorder->startTake (t, tracks[t].inputChannel, takesDir.getChildFile ("take-" + stamp + "-track" + String (t) + ".wav"),
                                 latencyAtLastStart.sampleRate);

    transport.playing.store (true);
    logHalLatencies();
    spikelog::line ("transport_start", { { "armed0", armed[0] ? "yes" : "no" }, { "armed1", armed[1] ? "yes" : "no" },
                                         { "in_lat", String (latencyAtLastStart.input) },
                                         { "out_lat", String (latencyAtLastStart.output) },
                                         { "graph_lat", String (latencyAtLastStart.graph) } });
}

void SpikeEngine::stop()
{
    if (! transport.playing.load())
        return;

    transport.playing.store (false);
    recorder->stopAllTakes();

    for (int t = 0; t < numTracks; ++t)
    {
        auto stats = recorder->getStats (t);
        if (stats.samplesWritten == 0 && stats.samplesDropped == 0)
            continue;

        // A dry sample captured at timeline position Q was played against the backing heard
        // at Q - (input + output + graph latency); graph latency is the PDC delay on the backing.
        const auto placedStart = stats.firstBlockStart - latencyAtLastStart.total();
        spikelog::line ("take_recorded", { { "track", String (t) }, { "file", stats.file.getFileName() },
                                           { "first_block_start", String (stats.firstBlockStart) },
                                           { "compensation", String (latencyAtLastStart.total()) },
                                           { "placed_start", String (placedStart) },
                                           { "written", String (stats.samplesWritten) },
                                           { "dropped", String (stats.samplesDropped) } });
    }
}

SpikeEngine::Latency SpikeEngine::getLatency()
{
    Latency latency;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        latency.sampleRate = device->getCurrentSampleRate();
        latency.bufferSize = device->getCurrentBufferSizeSamples();
        latency.input = device->getInputLatencyInSamples();
        latency.output = device->getOutputLatencyInSamples();
    }
    latency.graph = graph.getLatencySamples();
    return latency;
}

SpikeEngine::Health SpikeEngine::getHealth()
{
    Health health;
    health.cpu = deviceManager.getCpuUsage();
    health.xruns = deviceManager.getXRunCount();
    health.maxCallbackMs = 1000.0 * Time::highResolutionTicksToSeconds (maxCallbackTicks.load());
    auto latency = getLatency();
    health.budgetMs = latency.sampleRate > 0 ? 1000.0 * latency.bufferSize / latency.sampleRate : 0;
    return health;
}
