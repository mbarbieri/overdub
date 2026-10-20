#include "ui/Activity.h"

#include "core/LogEvents.h"
#include "separation/OnnxTestModel.h"
#include "song/FakeSeparationHelper.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// OpenSong runs one activity at a time and refuses gestures by one table (issue #145): every
// gesture against every activity, through the real OpenSong over a fake device, a fake camera and
// a fake Separation helper (ADR 0007: no test runs the real model).
using overdub::core::ScopedLogCapture;
using overdub::core::testing::firstEvent;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using namespace overdub::song::testing;
namespace song = overdub::song;
namespace core = overdub::core;
namespace exporting = overdub::exporting;

namespace
{
enum class Running
{
    nothing,
    pass,
    separation,
    conversion,
    exporting,
};

const overdub::plugin::AvailableAudioUnit testPlugin { .component = { "aufx", "test", "ovdb" }, .name = "Test", .manufacturer = "Overdub", .version = "1.0.0" };

// Band Mix at the helper's rate, so it can be separated, and an Armed Guitar Track to record.
std::unique_ptr<FixtureSong> bandMixSong()
{
    return FixtureSongBuilder {}.sampleRate (helperSampleRate).backingTrack ("Band Mix", 2.0, 220.0).track ("Guitar", true, { 0 }).build();
}

constexpr int otherSampleRate = 48000;

song::Song withMasterDb (song::Song song, double volumeDb)
{
    song.master.volumeDb = volumeDb;
    return song;
}

struct ActivityFixture
{
    MainWindowFixture f { bandMixSong() };

    // A history with one step to undo and one to redo, then `running` started. Nothing polls, so
    // the activity runs until the test ends.
    explicit ActivityFixture (Running running)
    {
        f.openSong->setSeparationTools (song::SeparationTools {
            .model = writeStandInModel (f.fixture->folder),
            .startHelper = fakeSeparationHelper ({}, std::make_shared<FakeHelperLog>()),
            .workFolder = f.fixture->folder.getChildFile ("work"),
        });
        REQUIRE (f.openSong->edit (withMasterDb (f.song(), -6.0), "Master -6").has_value());
        REQUIRE (f.openSong->edit (withMasterDb (f.song(), -12.0), "Master -12").has_value());
        REQUIRE (f.openSong->undo().has_value());

        switch (running)
        {
            case Running::nothing: break;
            case Running::pass: REQUIRE (f.openSong->record().has_value()); break;
            case Running::separation: REQUIRE (f.openSong->startSeparation (bandMixId()).has_value()); break;
            case Running::conversion:
                REQUIRE (f.openSong->startSampleRateConversion (otherSampleRate).has_value());
                REQUIRE (f.openSong->isConverting());
                break;
            case Running::exporting:
                REQUIRE (f.openSong->startExport ({ exporting::ExportFormat::wav, f.fixture->folder.getChildFile ("Song.wav") }).has_value());
                REQUIRE (f.openSong->isExporting()); // waits for the audio thread, which no block runs
                break;
        }
    }

    song::Id bandMixId() const { return f.trackNamed ("Band Mix").id; }
    song::Id guitarId() const { return f.trackNamed ("Guitar").id; }
};

// "ok", or the Error's code.
template <typename T>
std::string outcomeOf (const core::Expected<T>& result)
{
    return result.has_value() ? "ok" : result.error().code;
}

struct GestureCase
{
    std::string name;
    std::function<std::string (ActivityFixture&)> run;
    // What it returns with nothing, a Pass, a Separation, a conversion and an export running.
    std::array<std::string, 5> expected;
};

const std::string pass = "ui.stop_recording_first";
const std::string separation = "ui.separation_running";
const std::string conversion = "ui.conversion_running";
const std::string exportRunning = "ui.export_running";

std::vector<GestureCase> gestureCases()
{
    return {
        { "record", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->record()); }, { "ok", "ok", separation, conversion, exportRunning } },
        { "playOrStop", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->playOrStop()); }, { "ok", "ok", separation, conversion, exportRunning } },
        { "seek", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->seek (1000)); }, { "ok", pass, "ok", "ok", "ok" } },
        { "edit", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->edit (withMasterDb (a.f.song(), -3.0), "Master -3")); }, { "ok", pass, separation, conversion, "ok" } },
        { "undo", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->undo()); }, { "ok", pass, separation, conversion, "ok" } },
        { "redo", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->redo()); }, { "ok", pass, separation, conversion, "ok" } },
        { "addInsert", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->addInsert (a.guitarId(), testPlugin)); }, { "ok", pass, separation, conversion, "ok" } },
        { "previewMix", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->previewMix (withMasterDb (a.f.song(), -3.0))); }, { "ok", pass, separation, conversion, "ok" } },
        { "setPreRoll", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->setPreRoll (song::PreRoll::twoSeconds)); }, { "ok", pass, separation, conversion, "ok" } },
        { "setArmed", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->setArmed (a.guitarId(), false)); }, { "ok", pass, separation, conversion, "ok" } },
        { "setInputChannels", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->setInputChannels (a.guitarId(), { 1 })); }, { "ok", pass, separation, conversion, "ok" } },
        { "setMonitoring", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->setMonitoring (a.guitarId(), true)); }, { "ok", pass, separation, conversion, "ok" } },
        { "setCameraArmed", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->setCameraArmed (true)); }, { "ok", pass, separation, conversion, "ok" } },
        { "setCameraDevice", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->setCameraDevice ("camera-usb")); }, { "ok", pass, separation, conversion, "ok" } },
        { "importFiles",
          [] (ActivityFixture& a) {
        const auto results = a.f.openSong->importFiles ({ a.f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0) });
        REQUIRE (results.size() == 1);
        return outcomeOf (results.front());
    },
          { "ok", pass, separation, conversion, exportRunning } },
        // An export was allowed to start beside a Separation before issue #145: the window's modal
        // dialog kept it from asking. One activity at a time now refuses it.
        { "startExport",
          [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->startExport ({ exporting::ExportFormat::wav, a.f.fixture->folder.getChildFile ("Other.wav") })); },
          { "ok", pass, separation, conversion, "ok" } },
        { "startSeparation", [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->startSeparation (a.bandMixId())); }, { "ok", pass, "ok", conversion, exportRunning } },
        { "startSampleRateConversion",
          [] (ActivityFixture& a) { return outcomeOf (a.f.openSong->startSampleRateConversion (otherSampleRate)); },
          { "ok", pass, separation, "ok", exportRunning } },
    };
}

bool isPresented (const nlohmann::json& event)
{
    return event.at ("event") == "ui.notice_shown" || event.at ("event") == "ui.alert_shown";
}

std::vector<nlohmann::json> presented (const ScopedLogCapture& capture)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (isPresented (event))
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("every gesture against every activity is refused with the activity's code, or allowed", "[ui][activity]")
{
    const auto cases = gestureCases();
    const auto index = GENERATE_COPY (Catch::Generators::range<std::size_t> (0, cases.size()));
    const auto running = GENERATE (Running::nothing, Running::pass, Running::separation, Running::conversion, Running::exporting);
    const auto& gesture = cases[index];
    INFO ("gesture " << gesture.name << ", running " << (int) running);

    ActivityFixture a { running };
    const auto before = a.f.song();
    const auto expected = gesture.expected[(std::size_t) running];
    CHECK (gesture.run (a) == expected);
    if (expected != "ok")
        CHECK (a.f.song() == before); // a refused gesture changes nothing
}

TEST_CASE ("a refusal names its gesture", "[ui][activity]")
{
    ActivityFixture a { Running::pass };
    const auto refused = a.f.openSong->undo();
    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == "ui.stop_recording_first");
    CHECK (refused.error().fields == core::Fields { { "gesture", "undo" } });
}

TEST_CASE ("an Error is a refusal only with a refusal code and the field gesture", "[ui][activity]")
{
    const auto refused = refusal (Activity { ExportActivity {} }, Gesture::record);
    REQUIRE (refused.has_value());
    CHECK (isRefusal (*refused));
    CHECK_FALSE (isRefusal (core::Error { "ui.export_running", "an export is running", {} }));
    CHECK_FALSE (isRefusal (core::Error { "song.track_not_found", "no such Track", { { "gesture", "edit" } } }));
}

TEST_CASE ("only one activity runs: starting another one is refused while it runs, and allowed once it ends", "[ui][activity]")
{
    ActivityFixture a { Running::exporting };
    CHECK (a.f.openSong->startSeparation (a.bandMixId()).error().code == "ui.export_running");
    CHECK (a.f.openSong->record().error().code == "ui.export_running");

    for (int poll = 0; poll < 200 && a.f.openSong->isExporting(); ++poll)
        a.f.runBlocks (1);
    REQUIRE_FALSE (a.f.openSong->isExporting());
    CHECK (a.f.openSong->record().has_value());
    CHECK (a.f.openSong->isRecording());
    CHECK_FALSE (a.f.openSong->isSeparating());
}

TEST_CASE ("the window presents no Notice or Alert for a gesture a Pass, a Separation or a conversion refuses", "[ui][activity]")
{
    const auto running = GENERATE (Running::pass, Running::separation, Running::conversion);
    const auto code = running == Running::pass ? pass : running == Running::separation ? separation
                                                                                       : conversion;
    ActivityFixture a { running };
    auto& window = *a.f.window;
    int pluginMenusShown = 0;
    int cameraAccessAsked = 0;
    window.listInsertPlugins = [] { return std::vector { testPlugin }; };
    window.chooseInsertPlugin = [&pluginMenusShown] (const std::vector<overdub::plugin::AvailableAudioUnit>&, std::function<void (std::optional<std::size_t>)> done) {
        ++pluginMenusShown;
        done (0);
    };
    window.askCameraAccess = [&cameraAccessAsked] (std::function<void (bool)> done) {
        ++cameraAccessAsked;
        done (true);
    };
    const auto before = a.f.song();
    const auto guitarId = a.guitarId();

    ScopedLogCapture capture;
    if (running == Running::pass)
    {
        window.run (SongIntent { intent::Undo {} });
        window.run (SongIntent { intent::Redo {} });
    }
    window.songView()->recordPage.actions().mute (guitarId, true);
    window.songView()->recordPage.actions().arm (guitarId, false);
    window.songView()->recordPage.onInput (guitarId, { 1 });
    window.songView()->recordPage.onMonitoring (guitarId, true);
    window.songView()->recordPage.onPreRoll (song::PreRoll::twoSeconds);
    window.songView()->recordPage.onCameraArm (true);
    window.songView()->recordPage.onCameraDevice ("camera-usb");
    window.songView()->recordPage.onAddTrack();
    window.songView()->recordPage.actions().moveTrack (guitarId, 0);
    window.songView()->recordPage.insertActions.add (guitarId);
    window.songView()->recordPage.insertActions.bypass ("no-such-slot", true); // an operation that fails, for an edit refused anyway
    window.songView()->recordPage.mix.onPreview (intent::PreviewMix { intent::SetMasterVolume { -3.0 } });

    CHECK (presented (capture).empty());
    CHECK (a.f.alertsShown.empty());
    CHECK (pluginMenusShown == 0); // no dialog opens for a gesture refused anyway
    CHECK (cameraAccessAsked == 0);
    CHECK (a.f.song() == before);
    const auto refusal = firstEvent (capture, code);
    CHECK (refusal.at ("lvl") == "debug");
}

TEST_CASE ("the window still presents a refused record as a Notice while an export runs", "[ui][activity]")
{
    ActivityFixture a { Running::exporting };
    ScopedLogCapture capture;
    a.f.window->run (SongIntent { intent::Record {} });

    const auto shown = presented (capture);
    REQUIRE (shown.size() == 1);
    CHECK (shown.front().at ("event") == "ui.notice_shown");
    CHECK (shown.front().at ("code") == "ui.export_running");
    CHECK_FALSE (a.f.openSong->isRecording());
}
