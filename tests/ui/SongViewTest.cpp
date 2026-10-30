#include "ui/SongView.h"

#include "engine/PlainProcessor.h"
#include "ui/MainWindowFixture.h"
#include "ui/MixerGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The window's Song-bound content (ADR 0005 UI amendment) through the real window: the Track
// gestures the Record page and the Edit page share driven from each page, then the
// Song, the playhead or the engine's output; and a Song closed while a chooser, a confirmation or
// a plugin editor window was still open, then another Song opened.
using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
const juce::ModifierKeys leftButton { juce::ModifierKeys::leftButtonModifier };
const plugin::AvailableAudioUnit auDelay { .component = { "aufx", "dely", "appl" }, .name = "AUDelay", .manufacturer = "Apple" };

// An Insert that passes nothing through and has no editor of its own.
class SilentInsert final : public engine::PlainProcessor
{
public:
    SilentInsert() : PlainProcessor (stereoInOut()) {}
    const juce::String getName() const override { return "Silent"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
};

std::vector<nlohmann::json> eventsNamed (const core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

std::vector<std::string> trackNames (const song::Song& song)
{
    std::vector<std::string> names;
    for (const auto& track : song.tracks)
        names.push_back (track.name);
    return names;
}

MixerStrip& stripNamed (MainWindowFixture& f, const std::string& name)
{
    const auto& trackId = f.trackNamed (name).id;
    for (auto& strip : f.window->songView()->editPage.mixer.strips)
        if (strip->trackId() == trackId)
            return *strip;
    FAIL ("no strip for Track " << name);
    return *f.window->songView()->editPage.mixer.strips.front();
}

// The peak of output channel 0 over the last `samples` samples the device rendered.
float recentPeak (const engine::testing::LoopbackDevice& device, std::size_t samples)
{
    const auto& output = device.outputHistory[0];
    float peak = 0.0f;
    for (auto i = output.size() - samples; i < output.size(); ++i)
        peak = std::max (peak, std::abs (output[i]));
    return peak;
}

// Grabs the fader and drags it all the way down (left for a horizontal one), without releasing:
// the Mixer preview of the quietest volume. Returns where the drag ended, for the release.
juce::Point<float> dragFaderToBottom (MixerFader& fader)
{
    const auto grab = fader.thumbCentre().toFloat();
    const auto end = fader.getHeight() > fader.getWidth() ? grab.translated (0.0f, 2000.0f) : grab.translated (-2000.0f, 0.0f);
    fader.mouseDown (mouseEventAt (fader, grab, leftButton));
    fader.mouseDrag (mouseEventAt (fader, end, leftButton, grab));
    return end;
}

void release (MixerFader& fader, juce::Point<float> at)
{
    fader.mouseUp (mouseEventAt (fader, at, {}, fader.thumbCentre().toFloat()));
}

// Drags the grip from its centre to the page's y and releases it there.
void dragGripTo (TrackGrip& grip, juce::Component& page, int pageY)
{
    const auto down = grip.getLocalBounds().getCentre().toFloat();
    const auto centre = page.getLocalPoint (&grip, grip.getLocalBounds().getCentre());
    const auto to = grip.getLocalPoint (&page, juce::Point<int> { centre.x, pageY }).toFloat();
    grip.mouseDown (mouseEventAt (grip, down, leftButton));
    grip.mouseDrag (mouseEventAt (grip, to, leftButton, down));
    grip.mouseUp (mouseEventAt (grip, to, {}, down));
}

// Pixels of the Clip's waveform area (under its 14 px name strip) not in the Clip's body colour.
int waveformPixels (Timeline& timeline, const song::Id& clipId)
{
    const auto clip = timeline.clipBounds (clipId);
    const auto area = clip.withTrimmedTop (14).reduced (4, 4);
    REQUIRE (! area.isEmpty());
    const auto image = timeline.createComponentSnapshot (area, true, 1.0f);
    const auto body = image.getPixelAt (0, 0);
    int differing = 0;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (image.getPixelAt (x, y) != body)
                ++differing;
    return differing;
}

// Closes the fixture's Song and opens a fresh three-Track fixture Song in the same window, as
// the Workspace does (the old OpenSong is gone before the new one opens).
void openAnotherSong (MainWindowFixture& f)
{
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    f.fixture = song::testing::threeTrackFixture();
    f.openSong = std::make_unique<OpenSong> (std::move (f.fixture->package), AudioBinding { &f.engine, {}, { "In 1", "In 2", "In 3", "In 4" } });
    f.window->setOpenSong (f.openSong.get());
}
} // namespace

TEST_CASE ("A from a Record row and from an Edit header arms the Track the same way", "[ui][song_view]")
{
    MainWindowFixture f;
    const auto before = f.song();
    REQUIRE (f.trackNamed ("Guitar").settings.armed);

    click (f.rowNamed ("Guitar").armButton);
    const auto viaRecord = f.song();
    CHECK (! f.trackNamed ("Guitar").settings.armed);
    click (f.rowNamed ("Guitar").armButton);
    REQUIRE (f.song() == before);

    REQUIRE (pressKey (*f.window, '2'));
    click (f.editHeaderNamed ("Guitar").armButton);
    CHECK (f.song() == viaRecord);
}

TEST_CASE ("M and S from a Record row and from an Edit header change the Song the same way", "[ui][song_view]")
{
    const auto button = GENERATE (std::string ("mute"), std::string ("solo"));
    MainWindowFixture f;
    const auto before = f.song();
    const auto pick = [&button] (auto& owner) -> juce::Button& { return button == "mute" ? (juce::Button&) owner.muteButton : (juce::Button&) owner.soloButton; };

    click (pick (f.rowNamed ("Vocals")));
    const auto viaRecord = f.song();
    const auto labelViaRecord = f.openSong->package().undoLabel();
    CHECK (viaRecord != before);
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    REQUIRE (f.song() == before);

    REQUIRE (pressKey (*f.window, '2'));
    click (pick (f.editHeaderNamed ("Vocals")));
    CHECK (f.song() == viaRecord);
    CHECK (f.openSong->package().undoLabel() == labelViaRecord);
}

TEST_CASE ("a click on the Record overview and on the Edit ruler moves the playhead to the sample there", "[ui][song_view]")
{
    MainWindowFixture f;
    auto& overview = f.window->songView()->recordPage.overview;
    const auto overviewX = overview.getWidth() / 3;
    const auto overviewSample = overviewSampleAt (overviewX, overview.getWidth(), overview.length());
    REQUIRE (overviewSample > 0);
    clickAt (overview, { overviewX, overview.getHeight() / 2 });
    CHECK (f.openSong->position() == overviewSample);

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto ruler = timeline.rulerBounds();
    const auto rulerSample = sampleForX (timeline.view(), ruler.getX() + 100);
    REQUIRE (rulerSample != overviewSample);
    clickAt (timeline, { ruler.getX() + 100, ruler.getCentreY() });
    CHECK (f.openSong->position() == rulerSample);
}

TEST_CASE ("a fader drag on a Record row and on a Mixer strip previews the same mix in the engine", "[ui][song_view]")
{
    MainWindowFixture f;
    const auto blockSamples = (std::size_t) deviceBlockSize;
    const auto before = f.song();
    f.openSong->playOrStop();
    f.device.run (f.engine, 4);
    const auto loud = recentPeak (f.device, blockSamples);
    REQUIRE (loud > 0.0f);

    auto& rowFader = f.rowNamed ("Band Mix").fader;
    const auto rowEnd = dragFaderToBottom (rowFader);
    f.device.run (f.engine, 4);
    const auto quietViaRecord = recentPeak (f.device, blockSamples);
    CHECK (f.song() == before);
    release (rowFader, rowEnd);
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    REQUIRE (f.song() == before);
    f.device.run (f.engine, 4);
    REQUIRE (recentPeak (f.device, blockSamples) > loud / 2.0f);

    REQUIRE (pressKey (*f.window, '2'));
    auto& stripFader = stripNamed (f, "Band Mix").fader;
    const auto stripEnd = dragFaderToBottom (stripFader);
    f.device.run (f.engine, 4);
    const auto quietViaEdit = recentPeak (f.device, blockSamples);
    CHECK (f.song() == before);
    release (stripFader, stripEnd);

    CHECK (quietViaRecord < loud / 100.0f);
    CHECK_THAT (quietViaEdit, Catch::Matchers::WithinAbs (quietViaRecord, 1.0e-6));
}

TEST_CASE ("the Record look-ahead strip and the Edit Timeline draw the Backing Track from its peaks", "[ui][song_view]")
{
    MainWindowFixture f;
    const auto& columns = f.window->songView()->recordPage.lookAheadStrip.currentView().columns;
    CHECK (std::ranges::any_of (columns, [] (float level) { return level > 0.0f; }));

    REQUIRE (pressKey (*f.window, '2'));
    const auto& bandMixClip = f.trackNamed ("Band Mix").clips.front();
    CHECK (waveformPixels (f.window->songView()->editPage.timeline, bandMixClip.id) > 1000); // 132 with no peaks, 2638 with them
}

TEST_CASE ("a grip drop on the Record page and on the Edit page moves the Track the same way", "[ui][song_view]")
{
    MainWindowFixture f;
    const auto before = f.song();
    auto& recordPage = f.window->songView()->recordPage;
    const auto rows = recordPage.trackRowSpans();
    REQUIRE (rows.size() == 3);

    // Into the first slot: above Band Mix's centre.
    dragGripTo (f.rowNamed ("Vocals").grip, recordPage, rows[0].getStart() + 2);
    const auto viaRecord = f.song();
    CHECK (trackNames (viaRecord) == std::vector<std::string> { "Vocals", "Band Mix", "Guitar" });
    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    REQUIRE (f.song() == before);

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->songView()->editPage.timeline;
    const auto lanes = timeline.trackRowSpans();
    REQUIRE (lanes.size() == 3);
    dragGripTo (f.editHeaderNamed ("Vocals").grip, timeline, lanes[0].getStart() + 2);
    CHECK (f.song() == viaRecord);
    CHECK (f.openSong->package().undoLabel() == "Move Track");
}

TEST_CASE ("answers that arrive after the Song closed do nothing to the Song opened next", "[ui][song_view]")
{
    MainWindowFixture f;
    std::function<void (std::vector<juce::File>)> pendingImport;
    std::function<void (std::optional<std::size_t>)> pendingInsert;
    std::function<void (bool)> pendingCameraAccess;
    f.window->chooseFilesToImport = [&] (auto done) { pendingImport = std::move (done); };
    f.window->listInsertPlugins = [] { return std::vector<plugin::AvailableAudioUnit> { auDelay }; };
    f.window->chooseInsertPlugin = [&] (const auto&, auto done) { pendingInsert = std::move (done); };
    f.window->askCameraAccess = [&] (auto done) { pendingCameraAccess = std::move (done); };

    click (f.window->songView()->recordPage.importButton);
    click (f.rowNamed ("Guitar").insertChain.addButton);
    click (f.window->songView()->recordPage.cameraArmButton);
    REQUIRE (pressKey (*f.window, '2'));
    click (f.editHeaderNamed ("Guitar").deleteButton);
    REQUIRE (pendingImport != nullptr);
    REQUIRE (pendingInsert != nullptr);
    REQUIRE (pendingCameraAccess != nullptr);
    REQUIRE (f.pendingDeleteAnswer != nullptr);

    openAnotherSong (f);
    const auto opened = f.song();
    const auto undoLabel = f.openSong->package().undoLabel();
    core::ScopedLogCapture capture;
    pendingImport ({ juce::File ("/nonexistent/Drums.wav") });
    pendingInsert (0);
    pendingCameraAccess (true);
    f.answerDeleteQuestion (true);
    f.window->tick();

    CHECK (f.song() == opened);
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    CHECK (eventsNamed (capture, "ui.notice_shown").empty());
    CHECK (eventsNamed (capture, "ui.alert_shown").empty());
    CHECK (f.alertsShown.empty());

    // The new Song's own gestures reach it.
    click (f.rowNamed ("Guitar").armButton);
    CHECK (! f.trackNamed ("Guitar").settings.armed);
}

TEST_CASE ("closing a Song closes its plugin editor windows, and the next Song opens with none", "[ui][song_view]")
{
    std::vector<plugin::ProcessorReady> requests;
    MainWindowFixture f (song::testing::threeTrackFixture(), [&requests] (const song::InsertSlot&, double, int, plugin::ProcessorReady ready) { requests.push_back (std::move (ready)); });
    f.window->listInsertPlugins = [] { return std::vector<plugin::AvailableAudioUnit> { auDelay }; };
    f.window->chooseInsertPlugin = [] (const auto&, auto done) { done (0); };

    click (f.rowNamed ("Guitar").insertChain.addButton);
    REQUIRE (requests.size() == 1);
    requests[0](std::unique_ptr<juce::AudioProcessor> (std::make_unique<SilentInsert>()));
    f.runBlocks (1);
    const auto slotId = f.trackNamed ("Guitar").insertChain.at (0).id;
    REQUIRE (f.window->songView()->pluginWindows.find (slotId) != nullptr);

    core::ScopedLogCapture capture;
    openAnotherSong (f);

    const auto closed = eventsNamed (capture, "ui.plugin_window_closed");
    REQUIRE (closed.size() == 1);
    CHECK (closed[0].at ("slot_id") == slotId);
    CHECK (f.window->songView()->pluginWindows.size() == 0);
    f.runBlocks (1);
    CHECK (f.window->songView()->pluginWindows.size() == 0);
}
