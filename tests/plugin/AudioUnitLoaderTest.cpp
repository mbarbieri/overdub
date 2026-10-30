#include "plugin/AudioUnitLoader.h"
#include "capture/PassRecorder.h"
#include "engine/Engine.h"
#include "engine/SharedInsertNode.h"
#include "song/AudioFileDecode.h"
#include "song/SongPackage.h"

#include "PluginTestSupport.h"
#include "engine/EngineFixture.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::engine::InsertFactory;
using overdub::engine::SharedInsertNode;
using overdub::plugin::AudioUnitLoader;
using overdub::song::SampleCount;
using namespace overdub::engine::testing;
using namespace overdub::plugin::testing;

// [au] tests (ADR 0005): Apple's AUPeakLimiter and AUDelay through the real loader, out-of-process
// (ADR 0002), so they also prove the JUCE patch. They run in plain ctest: every Mac has them.
namespace
{
constexpr int blockSize = 256;

// The loaded AU as a prepared, shared processor, the way plugin::InsertHost keeps it.
std::shared_ptr<juce::AudioProcessor> loadPrepared (AudioUnitLoader& loader, const overdub::song::InsertSlot& slot)
{
    auto created = instantiateAndWait (loader, slot, testSampleRate, blockSize);
    if (! created.has_value())
        FAIL ("instantiate failed: " << created.error().code << " " << created.error().fields.dump());
    std::shared_ptr<juce::AudioProcessor> processor = std::move (*created);
    processor->prepareToPlay (testSampleRate, blockSize);
    return processor;
}

InsertFactory sharedOnTrack (std::string trackId, std::shared_ptr<juce::AudioProcessor> processor)
{
    return [trackId, processor] (const overdub::song::Id& chainTrackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (chainTrackId == trackId)
            inserts.push_back (std::make_unique<SharedInsertNode> (processor));
        return inserts;
    };
}

// The index of the loudest sample in [from, to).
std::int64_t peakIndex (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    const auto begin = samples.begin() + from;
    return from + std::distance (begin, std::max_element (begin, samples.begin() + to, [] (float a, float b) { return std::abs (a) < std::abs (b); }));
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

TEST_CASE ("AUPeakLimiter loads out-of-process, asynchronously, and reports its latency", "[plugin][au][loader]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    ScopedLogCapture capture;
    const auto outOfProcessBefore = juce::getAudioUnitOutOfProcessInstantiationCount();

    bool arrived = false;
    std::unique_ptr<juce::AudioProcessor> limiter;
    loader.instantiate (peakLimiterSlot(), testSampleRate, blockSize, [&] (overdub::core::Expected<std::unique_ptr<juce::AudioProcessor>> created) {
        arrived = true;
        REQUIRE (created.has_value());
        limiter = std::move (*created);
    });
    // instantiate() returned without waiting for the plugin: it never blocks the message thread.
    CHECK_FALSE (arrived);

    REQUIRE (runMessagesUntil ([&] { return arrived; }));
    REQUIRE (limiter != nullptr);
    // The out-of-process path ran (AudioComponentInstantiate with LoadOutOfProcess, ADR 0002).
    CHECK (juce::getAudioUnitOutOfProcessInstantiationCount() == outOfProcessBefore + 1);
    CHECK (limiter->getTotalNumInputChannels() == 2);
    CHECK (limiter->getTotalNumOutputChannels() == 2);

    const auto instantiated = eventsNamed (capture, "plugin.instantiated");
    REQUIRE (instantiated.size() == 1);
    CHECK (instantiated[0].at ("slot_id") == "slot-limiter");
    CHECK (instantiated[0].at ("component") == "aufx,lmtr,appl");
    CHECK (instantiated[0].at ("out_of_process") == true);
    CHECK (instantiated[0].at ("auv3") == false);

    // Its latency (seconds in the AU) is known in samples once it is prepared at the Song's rate.
    limiter->prepareToPlay (testSampleRate, blockSize);
    CHECK (limiter->getLatencySamples() > 0);
}

TEST_CASE ("an AU that is not installed fails asynchronously with plugin.not_found", "[plugin][au][loader]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    auto missing = delaySlot();
    missing.component = { "aufx", "zzzz", "none" };

    auto created = instantiateAndWait (loader, missing, testSampleRate, blockSize);
    REQUIRE_FALSE (created.has_value());
    CHECK (created.error().code == "plugin.not_found");
    CHECK (created.error().fields.at ("component") == "aufx,zzzz,none");
}

TEST_CASE ("discovery lists the installed AU effects, AUv2 and AUv3, by name", "[plugin][au][discovery]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    ScopedLogCapture capture;

    const auto effects = loader.findEffects();

    const auto delay = std::ranges::find (effects, overdub::song::AudioUnitComponent { "aufx", "dely", "appl" }, &overdub::plugin::AvailableAudioUnit::component);
    REQUIRE (delay != effects.end());
    CHECK (delay->name == "AUDelay");
    CHECK (delay->manufacturer == "Apple");
    CHECK_FALSE (delay->isAUv3);
    CHECK (std::ranges::any_of (effects, [] (const auto& effect) { return effect.name == "AUPeakLimiter"; }));
    CHECK (std::ranges::all_of (effects, [] (const auto& effect) { return effect.component.type == "aufx" || effect.component.type == "aumf"; }));
    CHECK (std::ranges::is_sorted (effects, [] (const auto& a, const auto& b) { return juce::String (a.name).compareNatural (juce::String (b.name)) < 0; }));

    const auto discovered = eventsNamed (capture, "plugin.discovered");
    REQUIRE (discovered.size() == 1);
    CHECK (discovered[0].at ("effects") == effects.size());
}

TEST_CASE ("AUPeakLimiter's reported latency is compensated sample-exact in playback", "[plugin][au][pdc]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    const auto limiter = loadPrepared (loader, peakLimiterSlot());
    const auto latency = limiter->getLatencySamples();
    REQUIRE (latency > 0);

    // Low impulses: the limiter passes them at unity gain, only delayed by its look-ahead.
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto takeDry = addTake (song, package, "take-dry", impulse (12000, 3000, 0.1f));
    const auto takeLimited = addTake (song, package, "take-limited", impulse (12000, 7000, 0.1f));
    song.tracks.push_back (trackWith ("track-dry", { clipOf ("clip-dry", takeDry, 0, 0, 12000) }));
    song.tracks.push_back (trackWith ("track-limited", { clipOf ("clip-limited", takeLimited, 0, 0, 12000) }));

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (song, package.root, sharedOnTrack ("track-limited", limiter));
    engine.play();
    DeviceDriver device { 2, blockSize };
    const auto rendered = device.render (engine, 48).channels[0];

    CHECK (engine.latencySamples() == latency);
    // Each impulse at its Song position: PDC delays the dry Track by the limiter's latency.
    CHECK (peakIndex (rendered, 0, 5000) == 3000);
    CHECK (peakIndex (rendered, 5000, 12000) == 7000);
    CHECK (std::abs (rendered[7000]) > 0.05f);
}

TEST_CASE ("AUPeakLimiter's latency is compensated sample-exact in Take placement through the fake loopback", "[plugin][au][pdc][record]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    const auto limiter = loadPrepared (loader, peakLimiterSlot());
    const auto latency = limiter->getLatencySamples();

    TempPackage folder;
    auto created = overdub::song::SongPackage::create (folder.root.getChildFile ("Song.overdub"), overdub::song::makeEmptySong (testSampleRate));
    REQUIRE (created.has_value());
    auto& package = **created;

    // The Backing Track plays through the limiter; the loop echoes the output into input 1.
    constexpr SampleCount played = 6000;
    auto song = package.song();
    overdub::song::Take backing;
    backing.id = "take-backing";
    backing.file = "audio/take-backing.wav";
    backing.lengthSamples = 16000;
    writeFloatWav (package.location().getChildFile (backing.file), { impulse (16000, played, 0.1f) });
    song.takes.push_back (backing);
    song.tracks.push_back (trackWith ("track-backing", { clipOf ("clip-backing", backing, 0, 0, 16000) }));
    auto armed = trackWith ("track-guitar", {});
    armed.settings.armed = true;
    armed.settings.inputChannels = { 0 };
    song.tracks.push_back (armed);
    REQUIRE (package.edit (song, "Set up").has_value());

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (package.song(), package.location(), sharedOnTrack ("track-backing", limiter));
    LoopbackDevice device { 2, blockSize, 700 };
    engine.seek (1000);
    device.run (engine, 1); // takes the plan, so the Pass begins under the limiter's PDC

    auto started = overdub::capture::PassRecorder::start (package, engine, overdub::capture::RecordLatency { 300, 400, 0 });
    REQUIRE (started.has_value());
    auto& recorder = **started;
    engine.play();
    device.run (engine, 40);
    recorder.requestStop();
    device.run (engine, 1);
    REQUIRE (recorder.phase() == overdub::capture::PassPhase::saving);
    auto finished = recorder.finish().pass;
    REQUIRE (finished.has_value());

    const auto& recorded = package.song();
    const auto take = std::ranges::find (recorded.takes, std::string ("track-guitar"), &overdub::song::Take::trackId);
    REQUIRE (take != recorded.takes.end());
    CHECK (take->compensation.reportedLatencySamples == 300 + 400 + latency);

    // The looped-back impulse lands on the timeline where it was played.
    auto audio = overdub::song::readWavFile (package.location().getChildFile (take->file));
    REQUIRE (audio.has_value());
    const std::vector<float> samples (audio->getReadPointer (0), audio->getReadPointer (0) + audio->getNumSamples());
    const auto peak = peakIndex (samples, 0, (std::int64_t) samples.size());
    CHECK (take->origin + peak == played);
}

TEST_CASE ("process() neither allocates nor locks with out-of-process AUs in the graph", "[plugin][au][rt]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    const auto limiter = loadPrepared (loader, peakLimiterSlot());
    const auto delay = loadPrepared (loader, delaySlot());

    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", sine (12000, 440.0, 0.2f));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, 12000) }));
    // AUDelay on the Track, AUPeakLimiter on the Master.
    const InsertFactory inserts = [limiter, delay] (const overdub::song::Id& trackId, const std::vector<overdub::song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> nodes;
        nodes.push_back (std::make_unique<SharedInsertNode> (trackId.empty() ? limiter : delay));
        return nodes;
    };

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (song, package.root, inserts);
    DeviceDriver device { 2, blockSize };
    DeviceDriver smallBlocks { 2, 100 };
    // The audio thread role keeps JUCE's logger bridge off this thread (core/Log.h).
    const overdub::core::ScopedThreadRole audioThread { overdub::core::ThreadRole::audio };

    // Known exception, outside our code: the first out-of-process render on a thread makes
    // AudioToolbox create its thread-local render context (audioipc::current_render_context, a
    // 16-byte dyld TLV calloc), once per thread for the thread's lifetime. So the guard starts at
    // the audio thread's second block with AUs.
    device.processOneBlock (engine);

    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
    engine.play();
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 16; ++block) device.processOneBlock (engine));
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) smallBlocks.processOneBlock (engine));

    // A rebuilt plan (a Song edit) with the same AUs swaps in without allocating or locking.
    engine.loadSong (song, package.root, inserts);
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));
    engine.stop();
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 4; ++block) device.processOneBlock (engine));
}
