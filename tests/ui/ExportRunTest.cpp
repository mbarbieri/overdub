#include "ui/ExportRun.h"

#include "core/Log.h"
#include "engine/TestProcessors.h"
#include "song/FixtureSongBuilder.h"
#include "song/InsertEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using overdub::plugin::InsertHost;
using overdub::plugin::InsertStatus;
using overdub::plugin::ProcessorReady;
using overdub::ui::ExportEnd;
using overdub::ui::ExportPhase;
using overdub::ui::ExportRun;
using namespace overdub::song::testing;
using namespace std::chrono_literals;

// The export's custody of the plugins (issue #103), driven through ExportRun alone: "the audio has
// let go" is an input, and the plugins are an InsertHost whose loads the test completes by hand.
namespace
{
constexpr int blockSize = 256;

// Requests the test completes by hand, standing in for the asynchronous AU loader.
struct FakeInstantiator
{
    std::vector<ProcessorReady> requests;

    overdub::plugin::Instantiator instantiator()
    {
        return [this] (const overdub::song::InsertSlot&, double, int, ProcessorReady ready) { requests.push_back (std::move (ready)); };
    }

    juce::AudioProcessor* succeed (std::size_t index, std::unique_ptr<juce::AudioProcessor> processor = std::make_unique<overdub::engine::testing::GainProcessor> (1.0f))
    {
        auto* raw = processor.get();
        requests.at (index) (std::move (processor));
        return raw;
    }
};

// Holds the render in its first block until the export's worker is told to stop, so a test acts
// while the render is under way, its partial file open.
class HoldingProcessor final : public overdub::engine::PlainProcessor
{
public:
    HoldingProcessor() : PlainProcessor (stereoInOut()) {}

    const juce::String getName() const override { return "Test Hold"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        entered.store (true);
        while (! juce::Thread::currentThreadShouldExit())
            juce::Thread::sleep (1);
    }

    std::atomic<bool> entered { false };
};

std::vector<nlohmann::json> eventsNamed (const overdub::core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

struct RunFixture
{
    std::unique_ptr<FixtureSong> fixture = threeTrackFixture();
    FakeInstantiator fake;
    InsertHost inserts { fake.instantiator(),
                         [] (const std::string&) -> overdub::core::Expected<juce::MemoryBlock> { return juce::MemoryBlock {}; },
                         (double) fixture->song().sampleRate,
                         blockSize };
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-export-run-" + juce::Uuid().toString());
    juce::File destination = folder.getChildFile ("Song.wav");
    ExportRun::Clock::time_point requestedAt = ExportRun::Clock::now();
    std::optional<ExportRun> run;

    RunFixture() { REQUIRE (folder.createDirectory()); }
    ~RunFixture()
    {
        run.reset();
        folder.deleteRecursively();
    }

    overdub::song::SongPackage& package() { return *fixture->package; }
    const overdub::song::Song& song() { return package().song(); }

    // A slot at the end of Guitar's Insert Chain, its load requested (not completed).
    overdub::song::Id addSlot()
    {
        const overdub::song::InsertSlot slot { .id = overdub::song::makeId(), .component = { "aufx", "test", "ovdb" }, .name = "Test Gain", .version = "1.0.0" };
        auto changed = overdub::song::addInsert (song(), fixture->trackNamed ("Guitar").id, slot);
        REQUIRE (changed.has_value());
        REQUIRE (package().edit (std::move (*changed), "Add Test Gain").has_value());
        syncWithSong();
        return slot.id;
    }

    void setBypassed (const overdub::song::Id& slotId, bool bypassed)
    {
        auto changed = overdub::song::setInsertBypassed (song(), slotId, bypassed);
        REQUIRE (changed.has_value());
        REQUIRE (package().edit (std::move (*changed), "Bypass").has_value());
        syncWithSong();
    }

    void removeSlot (const overdub::song::Id& slotId)
    {
        auto changed = overdub::song::removeInsert (song(), slotId);
        REQUIRE (changed.has_value());
        REQUIRE (package().edit (std::move (*changed), "Remove").has_value());
        syncWithSong();
    }

    // As the owner does after an edit: the run follows the Song while it exists.
    void syncWithSong()
    {
        if (run.has_value())
            run->followSong();
        else
            inserts.sync (song());
    }

    void start() { run.emplace (overdub::exporting::ExportRequest { overdub::exporting::ExportFormat::wav, destination }, std::nullopt, package(), inserts, blockSize, requestedAt); }

    // Starts the run and lets the audio go at once, so it renders.
    void startRendering()
    {
        start();
        REQUIRE_FALSE (run->advance (requestedAt, true).has_value());
        REQUIRE (run->phase() == ExportPhase::rendering);
    }

    // The crashes the InsertHost heard, handed to the run as the owner does.
    std::optional<ExportEnd> crash (juce::AudioProcessor* processor)
    {
        inserts.pluginCrashed (*processor);
        return run->crashed (inserts.takeCrashes());
    }

    // Advances with the audio let go until the run ends.
    ExportEnd advanceUntilEnded()
    {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto end = run->advance (requestedAt, true))
                return *end;
            std::this_thread::sleep_for (5ms);
        }
        FAIL ("the export did not end");
        return {};
    }

    void waitUntilFileWritten()
    {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (! run->hasWrittenFile() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (5ms);
        REQUIRE (run->hasWrittenFile());
    }
};
} // namespace

TEST_CASE ("an export waits while the audio thread holds the plugins, then renders and ends with its file", "[ui][export_run]")
{
    RunFixture f;
    f.addSlot();
    f.fake.succeed (0);
    f.start();

    CHECK_FALSE (f.run->advance (f.requestedAt + 1s, false).has_value());
    CHECK (f.run->phase() == ExportPhase::waitingForAudio);
    CHECK (juce::exactlyEqual (f.run->progress(), 0.0));

    CHECK_FALSE (f.run->advance (f.requestedAt + 2s, true).has_value());
    CHECK (f.run->phase() == ExportPhase::rendering);

    const auto end = f.advanceUntilEnded();
    CHECK_FALSE (end.error.has_value());
    CHECK (end.file == f.destination);
    CHECK (f.destination.existsAsFile());
}

TEST_CASE ("an export the audio never lets go of is given up after audioTimeout, without taking the plugins over", "[ui][export_run]")
{
    RunFixture f;
    f.addSlot();
    f.fake.succeed (0);
    f.start();
    overdub::core::ScopedLogCapture capture;

    CHECK_FALSE (f.run->advance (f.requestedAt + ExportRun::audioTimeout - 1ms, false).has_value());
    CHECK (f.run->phase() == ExportPhase::waitingForAudio);

    const auto end = f.run->advance (f.requestedAt + ExportRun::audioTimeout, false);

    REQUIRE (end.has_value());
    REQUIRE (end->error.has_value());
    CHECK (end->error->code == "ui.export_audio_unresponsive");
    CHECK (end->error->fields.at ("path") == f.destination.getFullPathName().toStdString());
    CHECK (end->error->fields.at ("waited_ms") == 5000);
    CHECK_FALSE (end->file.has_value());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "audio_unresponsive");
    CHECK (stopped[0]["phase"] == "waiting_for_audio");
    CHECK_FALSE (f.destination.exists());
}

TEST_CASE ("a plugin that loads and crashes while the export waits for the audio discards it", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    f.start();
    overdub::core::ScopedLogCapture capture;

    auto* plugin = f.fake.succeed (0); // loads while the export waits
    const auto end = f.crash (plugin);

    REQUIRE (end.has_value());
    REQUIRE (end->error.has_value());
    CHECK (end->error->code == "ui.export_plugin_crashed");
    CHECK (end->error->fields.at ("path") == f.destination.getFullPathName().toStdString());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "plugin_crashed");
    CHECK (stopped[0]["phase"] == "waiting_for_audio");
    CHECK_FALSE (f.destination.exists());
}

TEST_CASE ("a crash in a bypassed plugin while the export waits leaves it running, and it writes its file", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    const auto bypassedId = f.addSlot();
    f.fake.succeed (0);
    auto* bypassed = f.fake.succeed (1);
    f.setBypassed (bypassedId, true);
    f.start();

    CHECK_FALSE (f.crash (bypassed).has_value());

    const auto end = f.advanceUntilEnded();
    CHECK_FALSE (end.error.has_value());
    CHECK (end.file == f.destination);
    CHECK (f.destination.existsAsFile());
}

TEST_CASE ("a plugin that crashes bypassed while the export waits and is then played discards it once the audio lets go", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    const auto bypassedId = f.addSlot();
    f.fake.succeed (0);
    auto* bypassed = f.fake.succeed (1);
    f.setBypassed (bypassedId, true);
    f.start();
    REQUIRE_FALSE (f.crash (bypassed).has_value());
    f.setBypassed (bypassedId, false); // played now, still crashed: the render would leave it dry

    const auto end = f.run->advance (f.requestedAt, true);

    REQUIRE (end.has_value());
    REQUIRE (end->error.has_value());
    CHECK (end->error->code == "ui.export_plugin_crashed");
    CHECK (end->error->fields.at ("path") == f.destination.getFullPathName().toStdString());
    CHECK_FALSE (f.destination.exists());
}

TEST_CASE ("a plugin that crashed for good before the export is left out of it, as playback leaves it out", "[ui][export_run][crash]")
{
    RunFixture f;
    const auto slotId = f.addSlot();
    auto* plugin = f.fake.succeed (0);
    for (int reload = 0; reload < overdub::plugin::maxCrashReloads; ++reload)
    {
        f.inserts.pluginCrashed (*plugin);
        f.inserts.reloadCrashed (f.song());
        plugin = f.fake.succeed (f.fake.requests.size() - 1);
    }
    f.inserts.pluginCrashed (*plugin);
    REQUIRE (f.inserts.crashedForGood (slotId));
    f.inserts.takeCrashes();

    f.startRendering();

    const auto end = f.advanceUntilEnded();
    CHECK_FALSE (end.error.has_value());
    CHECK (end.file == f.destination);
}

TEST_CASE ("a crash in a plugin the export renders discards it and leaves no file", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    auto* plugin = f.fake.succeed (0);
    f.startRendering();
    overdub::core::ScopedLogCapture capture;

    const auto end = f.crash (plugin);

    REQUIRE (end.has_value());
    REQUIRE (end->error.has_value());
    CHECK (end->error->code == "ui.export_plugin_crashed");
    CHECK_FALSE (end->file.has_value());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "plugin_crashed");
    CHECK ((stopped[0]["phase"] == "rendering" || stopped[0]["phase"] == "finished"));
    CHECK_FALSE (f.destination.exists());
}

TEST_CASE ("a crash in a plugin the export does not render leaves it running and its file intact", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    const auto bypassedId = f.addSlot();
    f.fake.succeed (0);
    auto* bypassed = f.fake.succeed (1);
    f.setBypassed (bypassedId, true);
    f.startRendering();

    CHECK_FALSE (f.crash (bypassed).has_value());

    const auto end = f.advanceUntilEnded();
    CHECK_FALSE (end.error.has_value());
    CHECK (end.file == f.destination);
    CHECK (f.destination.existsAsFile());
}

TEST_CASE ("a crash in a plugin the export rendered, after its file was written, deletes that file", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    auto* plugin = f.fake.succeed (0);
    f.startRendering();
    f.waitUntilFileWritten();
    REQUIRE (f.destination.existsAsFile());
    overdub::core::ScopedLogCapture capture;

    const auto end = f.crash (plugin);

    REQUIRE (end.has_value());
    REQUIRE (end->error.has_value());
    CHECK (end->error->code == "ui.export_plugin_crashed");
    CHECK_FALSE (end->file.has_value());
    CHECK_FALSE (f.destination.exists());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "plugin_crashed");
    CHECK (stopped[0]["phase"] == "finished");
}

TEST_CASE ("a crash in a plugin the export did not render, after its file was written, keeps that file", "[ui][export_run][crash]")
{
    RunFixture f;
    f.addSlot();
    const auto bypassedId = f.addSlot();
    f.fake.succeed (0);
    auto* bypassed = f.fake.succeed (1);
    f.setBypassed (bypassedId, true);
    f.startRendering();
    f.waitUntilFileWritten();

    CHECK_FALSE (f.crash (bypassed).has_value());

    const auto end = f.run->advance (f.requestedAt, true);
    REQUIRE (end.has_value());
    CHECK (end->file == f.destination);
    CHECK (f.destination.existsAsFile());
}

TEST_CASE ("an Insert removed during the export stays hosted: its crash still discards the export", "[ui][export_run][crash]")
{
    RunFixture f;
    const auto slotId = f.addSlot();
    auto* plugin = f.fake.succeed (0);
    f.startRendering();

    f.removeSlot (slotId);
    CHECK (f.song().tracks.at (1).insertChain.empty());
    CHECK (f.inserts.status (slotId) == InsertStatus::ready); // the export still renders it

    const auto end = f.crash (plugin);
    REQUIRE (end.has_value());
    REQUIRE (end->error.has_value());
    CHECK (end->error->code == "ui.export_plugin_crashed");
}

TEST_CASE ("an Insert added during the export loads at once, and its crash leaves the export running", "[ui][export_run][crash]")
{
    RunFixture f;
    f.startRendering();

    const auto slotId = f.addSlot();
    REQUIRE (f.fake.requests.size() == 1);
    auto* plugin = f.fake.succeed (0);
    CHECK (f.inserts.status (slotId) == InsertStatus::ready);

    CHECK_FALSE (f.crash (plugin).has_value());
    const auto end = f.advanceUntilEnded();
    CHECK_FALSE (end.error.has_value());
    CHECK (end.file == f.destination);
}

TEST_CASE ("stopping an export logs its reason and phase", "[ui][export_run]")
{
    RunFixture f;
    f.start();
    overdub::core::ScopedLogCapture capture;

    f.run->stop ("audio_detached");

    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "audio_detached");
    CHECK (stopped[0]["phase"] == "waiting_for_audio");
}

TEST_CASE ("cancelling a render stops it, removes its partial file and keeps the file that was at the destination", "[ui][export_run][cancel]")
{
    RunFixture f;
    REQUIRE (f.destination.replaceWithText ("before the export"));
    f.addSlot();
    auto* holding = dynamic_cast<HoldingProcessor*> (f.fake.succeed (0, std::make_unique<HoldingProcessor>()));
    f.startRendering();
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (! holding->entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (1ms);
    REQUIRE (holding->entered.load());
    REQUIRE (f.folder.getNumberOfChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, "*") == 1);
    REQUIRE (f.folder.getNumberOfChildFiles (juce::File::findFiles, "*") == 2); // the destination and the hidden partial file
    overdub::core::ScopedLogCapture capture;

    const auto end = f.run->cancel();

    REQUIRE (end.error.has_value());
    CHECK (end.error->code == "ui.export_cancelled");
    CHECK (end.error->fields.at ("path") == f.destination.getFullPathName().toStdString());
    CHECK_FALSE (end.file.has_value());
    CHECK (f.destination.loadFileAsString() == "before the export");
    CHECK (f.folder.getNumberOfChildFiles (juce::File::findFiles, "*") == 1);
    const auto cancelled = eventsNamed (capture, "ui.export_cancelled");
    REQUIRE (cancelled.size() == 1);
    CHECK (cancelled[0]["path"] == f.destination.getFullPathName().toStdString());
    CHECK (cancelled[0]["phase"] == "rendering");
    CHECK (eventsNamed (capture, "ui.export_stopped").empty());
}

TEST_CASE ("cancelling an export that waits for the audio ends it before it renders", "[ui][export_run][cancel]")
{
    RunFixture f;
    f.start();
    overdub::core::ScopedLogCapture capture;

    const auto end = f.run->cancel();

    REQUIRE (end.error.has_value());
    CHECK (end.error->code == "ui.export_cancelled");
    CHECK_FALSE (f.destination.exists());
    const auto cancelled = eventsNamed (capture, "ui.export_cancelled");
    REQUIRE (cancelled.size() == 1);
    CHECK (cancelled[0]["phase"] == "waiting_for_audio");
}

TEST_CASE ("a cancel that arrives after the file is written is too late: the export ends with its file", "[ui][export_run][cancel]")
{
    RunFixture f;
    f.startRendering();
    f.waitUntilFileWritten();
    overdub::core::ScopedLogCapture capture;

    const auto end = f.run->cancel();

    CHECK_FALSE (end.error.has_value());
    CHECK (end.file == f.destination);
    CHECK (f.destination.existsAsFile());
    CHECK (eventsNamed (capture, "ui.export_cancelled").empty());
}
