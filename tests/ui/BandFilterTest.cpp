#include "ui/TranscribePage.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "song/TranscribeMix.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using core::ScopedLogCapture;

// The Focus Track's band filter (issue #234, ADR 0010): each Track keeps its own (on/off, low cut,
// high cut; outside history, saved), and only the Focus Track's is heard, only in Transcribe mode,
// live in the engine. Driven through MainComponent::run and the right column on a fixture Song of
// two Backing Tracks, "Bass" and "Drums", each the same 2 s 100 Hz sine (0.25) on its own Track:
// with the band at 200..3200 Hz the sine is an octave below the low cut, so at least 12 dB down
// (the dB figures themselves are tests/engine/EngineBandFilterTest.cpp's).
namespace
{
constexpr double sineFrequency = 100.0;
const song::BandFilter band { .on = true, .lowHz = 200, .highHz = 3200 };

std::unique_ptr<song::testing::FixtureSong> bassAndDrums()
{
    return song::testing::FixtureSongBuilder {}.backingTrack ("Bass", 2.0, sineFrequency).backingTrack ("Drums", 2.0, sineFrequency).build();
}

IntentResult runControl (MainWindowFixture& f, const Intent& intent)
{
    return f.window->run (intent, Origin::control);
}

void run (MainWindowFixture& f, SongIntent intent)
{
    REQUIRE (runControl (f, std::move (intent)).has_value());
}

void showMode (MainWindowFixture& f, Mode mode)
{
    REQUIRE (runControl (f, WindowIntent { intent::ShowMode { mode } }).has_value());
}

intent::SetBandFilter setBandFilter (const song::Id& trackId, song::BandFilter filter)
{
    return { .trackId = trackId, .on = filter.on, .lowHz = filter.lowHz, .highHz = filter.highHz };
}

TranscribeColumn& column (MainWindowFixture& f)
{
    return f.window->songView()->transcribePage.column;
}

// Plays about 0.8 s from the start and returns the last 12000 output samples of channel 0.
std::vector<float> heardFromStart (MainWindowFixture& f)
{
    run (f, SongIntent { intent::Seek { 0 } });
    if (! f.openSong->isPlaying())
        run (f, SongIntent { intent::PlayOrStop {} });
    f.device.run (f.engine, 150);
    const auto& output = f.device.outputHistory[0];
    REQUIRE (output.size() >= 12000);
    std::vector<float> heard { output.end() - 12000, output.end() };
    f.openSong->stop();
    f.device.run (f.engine, 1);
    return heard;
}

double rmsOf (const float* samples, int count)
{
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
        sum += (double) samples[i] * (double) samples[i];
    return std::sqrt (sum / count);
}

double heardRms (MainWindowFixture& f)
{
    const auto heard = heardFromStart (f);
    return rmsOf (heard.data(), (int) heard.size());
}

double dbBetween (double level, double reference)
{
    return 20.0 * std::log10 (level / reference);
}

// Closes the window's Song, which writes song.json, and opens its package again.
void reopenSong (MainWindowFixture& f)
{
    const auto location = f.openSong->package().location();
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    auto reopened = song::SongPackage::open (location);
    REQUIRE (reopened.has_value());
    f.openSong = std::make_unique<OpenSong> (std::move (reopened->package), AudioBinding { &f.engine, {}, { "In 1", "In 2" } });
    f.window->setOpenSong (f.openSong.get());
}

struct ExportFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-band-filter-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};
} // namespace

TEST_CASE ("the right column shows the Focus Track's band filter, On and its cuts in Hz, all disabled without a Focus Track", "[ui][transcribe][band_filter]")
{
    MainWindowFixture f { bassAndDrums() };
    showMode (f, Mode::transcribe);
    auto& c = column (f);

    for (auto* control : std::initializer_list<juce::Component*> { &c.filterCaption, &c.filterToggle, &c.filterLowField, &c.filterLowUnit, &c.filterHighField, &c.filterHighUnit })
        CHECK (isShown (*control));
    CHECK (c.filterCaption.getText() == "FILTER");
    CHECK (c.filterLowUnit.getText() == "Hz");
    CHECK_FALSE (c.filterToggle.isEnabled());
    CHECK_FALSE (c.filterLowField.isEnabled());
    CHECK_FALSE (c.filterHighField.isEnabled());

    run (f, SongIntent { intent::SetFocusTrack { f.trackNamed ("Bass").id } });
    CHECK (c.filterToggle.isEnabled());
    CHECK_FALSE (c.filterToggle.getToggleState());
    CHECK (c.filterLowField.getText() == juce::String (song::BandFilter {}.lowHz));
    CHECK (c.filterHighField.getText() == juce::String (song::BandFilter {}.highHz));

    const auto undoLabel = f.openSong->package().undoLabel();
    const ScopedLogCapture capture;
    click (c.filterToggle);
    CHECK (f.trackNamed ("Bass").bandFilter.on);
    CHECK (c.filterToggle.getToggleState());

    c.filterLowField.setText ("250 Hz", juce::sendNotificationSync);
    c.filterHighField.setText ("2500", juce::sendNotificationSync);

    CHECK (f.trackNamed ("Bass").bandFilter == song::BandFilter { .on = true, .lowHz = 250, .highHz = 2500 });
    CHECK (f.trackNamed ("Drums").bandFilter == song::BandFilter {});
    CHECK (c.filterLowField.getText() == "250");
    CHECK (c.filterHighField.getText() == "2500");
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    CHECK (std::ranges::any_of (capture.events(), [] (const nlohmann::json& event) { return event.at ("event") == "ui.intent_run" && event.at ("op") == "set_band_filter"; }));
    CHECK (std::ranges::any_of (capture.events(), [&] (const nlohmann::json& event) {
        return event.at ("event") == "ui.band_filter_set" && event.at ("track_id") == f.trackNamed ("Bass").id && event.at ("low_hz") == 250 && event.at ("high_hz") == 2500 && event.at ("on") == true;
    }));

    SECTION ("a high cut typed below the low cut is refused and the field shows the Track's again")
    {
        c.filterHighField.setText ("200", juce::sendNotificationSync);
        CHECK (f.trackNamed ("Bass").bandFilter.highHz == 2500);
        CHECK (c.filterHighField.getText() == "2500");
    }
}

TEST_CASE ("set_band_filter keeps the low cut below the high cut and both within 20..20000 Hz, refusing anything else with an explicit Error", "[ui][transcribe][band_filter]")
{
    MainWindowFixture f { bassAndDrums() };
    const auto bassId = f.trackNamed ("Bass").id;

    for (const auto& [low, high] : { std::pair { 400, 400 }, std::pair { 3000, 200 } })
    {
        const auto refused = runControl (f, SongIntent { setBandFilter (bassId, { .on = true, .lowHz = low, .highHz = high }) });
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.band_filter_low_not_below_high");
        CHECK (refused.error().fields.at ("low_hz") == low);
        CHECK (refused.error().fields.at ("high_hz") == high);
    }
    for (const auto& [low, high] : { std::pair { 19, 400 }, std::pair { 200, 20001 }, std::pair { -5, 400 } })
    {
        const auto refused = runControl (f, SongIntent { setBandFilter (bassId, { .on = false, .lowHz = low, .highHz = high }) });
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.band_filter_out_of_range");
        CHECK (refused.error().fields.at ("min_hz") == 20);
        CHECK (refused.error().fields.at ("max_hz") == 20000);
    }
    const auto unknown = runControl (f, SongIntent { setBandFilter ("track-gone", band) });
    REQUIRE_FALSE (unknown.has_value());
    CHECK (unknown.error().code == "song.track_not_found");
    CHECK (f.trackNamed ("Bass").bandFilter == song::BandFilter {});

    run (f, SongIntent { setBandFilter (bassId, { .on = false, .lowHz = 20, .highHz = 20000 }) });
    CHECK (f.trackNamed ("Bass").bandFilter == song::BandFilter { .on = false, .lowHz = 20, .highHz = 20000 });
}

TEST_CASE ("only the Focus Track's band filter is heard, only in Transcribe mode, live with no new plan", "[ui][transcribe][band_filter][engine]")
{
    MainWindowFixture f { bassAndDrums() };
    const auto bassId = f.trackNamed ("Bass").id;
    const auto drumsId = f.trackNamed ("Drums").id;
    showMode (f, Mode::transcribe);
    run (f, SongIntent { intent::SetFocusTrack { bassId } }); // soloes Bass alone
    const auto unfiltered = heardRms (f);
    REQUIRE (unfiltered > 0.05);

    run (f, SongIntent { setBandFilter (bassId, band) });

    CHECK_FALSE (f.engine.hasPendingPlan()); // set on the playing plan, not rebuilt
    CHECK (f.openSong->heardBandFilter() == engine::HeardBandFilter { .trackId = bassId, .lowHz = 200, .highHz = 3200 });
    CHECK (dbBetween (heardRms (f), unfiltered) <= -12.0);

    SECTION ("turned off, the output is the unfiltered one again")
    {
        run (f, SongIntent { setBandFilter (bassId, { .on = false, .lowHz = 200, .highHz = 3200 }) });
        CHECK_FALSE (f.openSong->heardBandFilter().has_value());
        CHECK (std::abs (dbBetween (heardRms (f), unfiltered)) < 0.01);
    }

    SECTION ("another Track's filter is not heard while the Bass is the Focus Track")
    {
        run (f, SongIntent { setBandFilter (bassId, { .on = false, .lowHz = 200, .highHz = 3200 }) });
        run (f, SongIntent { setBandFilter (drumsId, band) });
        run (f, SongIntent { intent::SetTranscribeSolo { bassId, false } });
        run (f, SongIntent { intent::SetTranscribeSolo { drumsId, true } });
        CHECK (f.song().focusTrackId == bassId);
        CHECK_FALSE (f.openSong->heardBandFilter().has_value());
        CHECK (std::abs (dbBetween (heardRms (f), unfiltered)) < 0.01);
    }

    SECTION ("Edit and Record never hear it")
    {
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { drumsId, true } } });
        showMode (f, Mode::edit);
        CHECK_FALSE (f.openSong->heardBandFilter().has_value());
        CHECK (std::abs (dbBetween (heardRms (f), unfiltered)) < 0.01);
        showMode (f, Mode::record);
        CHECK (std::abs (dbBetween (heardRms (f), unfiltered)) < 0.01);
        showMode (f, Mode::transcribe);
        CHECK (dbBetween (heardRms (f), unfiltered) <= -12.0);
    }

    SECTION ("an export from Transcribe mode never hears it")
    {
        ExportFolder folder;
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { drumsId, true } } });
        showMode (f, Mode::transcribe);
        const auto destination = folder.root.getChildFile ("Song.wav");
        setExportDestination (*f.window, destination);
        run (f, SongIntent { intent::Export {} });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
        {
            f.runBlocks (1);
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        REQUIRE_FALSE (f.openSong->isExporting());
        const auto decoded = song::decodeAudioFile (destination, f.song().sampleRate);
        REQUIRE (decoded.has_value());
        // The Bass alone, as the Song's own mix plays it: the same level the Transcribe Mix heard
        // unfiltered (Bass soloed there, Drums muted here).
        CHECK (std::abs (dbBetween (rmsOf (decoded->audio.getReadPointer (0) + 24000, 48000), unfiltered)) < 0.1);
    }
}

TEST_CASE ("each Track keeps its own band filter: moving the Focus brings back that Track's, undo keeps them and reopening the Song restores them", "[ui][transcribe][band_filter][persistence]")
{
    MainWindowFixture f { bassAndDrums() };
    const auto bassId = f.trackNamed ("Bass").id;
    const auto drumsId = f.trackNamed ("Drums").id;
    showMode (f, Mode::transcribe);
    run (f, SongIntent { setBandFilter (bassId, band) });
    const song::BandFilter drumsFilter { .on = false, .lowHz = 500, .highHz = 9000 };
    run (f, SongIntent { setBandFilter (drumsId, drumsFilter) });

    run (f, SongIntent { intent::SetFocusTrack { drumsId } });
    CHECK (column (f).filterLowField.getText() == "500");
    CHECK_FALSE (column (f).filterToggle.getToggleState());
    CHECK_FALSE (f.openSong->heardBandFilter().has_value());
    run (f, SongIntent { intent::SetFocusTrack { bassId } });
    CHECK (column (f).filterLowField.getText() == "200");
    CHECK (column (f).filterToggle.getToggleState());
    CHECK (f.openSong->heardBandFilter().has_value());

    SECTION ("an undo keeps the filters as they are")
    {
        run (f, SongIntent { intent::Edit { intent::SetTrackMute { drumsId, true } } });
        run (f, SongIntent { intent::Undo {} });
        CHECK (f.trackNamed ("Bass").bandFilter == band);
        CHECK (f.trackNamed ("Drums").bandFilter == drumsFilter);
    }

    SECTION ("reopening the Song restores every Track's filter, heard again in Transcribe mode")
    {
        reopenSong (f);
        showMode (f, Mode::edit);
        showMode (f, Mode::transcribe);
        CHECK (f.trackNamed ("Bass").bandFilter == band);
        CHECK (f.trackNamed ("Drums").bandFilter == drumsFilter);
        CHECK (column (f).filterToggle.getToggleState());
        CHECK (f.openSong->heardBandFilter() == engine::HeardBandFilter { .trackId = bassId, .lowHz = 200, .highHz = 3200 });
    }
}
