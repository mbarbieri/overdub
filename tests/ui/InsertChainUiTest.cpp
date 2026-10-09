#include "ui/InsertChain.h"

#include "plugin/PluginTestSupport.h"
#include "song/SongJson.h"
#include "ui/MainWindowFixture.h"
#include "ui/Mixer.h"
#include "ui/PluginWindows.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// The Insert Chain UI and plugin editor windows through the real window (issue #34, ADR 0005 UI
// amendment): synthesized clicks and drags on Mixer strips, the Master strip and Record rows,
// then the Song, the undo step's label and the editor windows. Plugins are test processors with
// one parameter and no editor of their own, so their windows show JUCE's generic editor, built
// off the desktop.
using overdub::core::ScopedLogCapture;
using overdub::plugin::AvailableAudioUnit;
using overdub::plugin::InsertStatus;
using overdub::plugin::ProcessorReady;
using overdub::song::InsertSlot;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using namespace std::chrono_literals;

namespace
{
const AvailableAudioUnit auDelay { .component = { "aufx", "dely", "appl" }, .name = "AUDelay", .manufacturer = "Apple" };
const AvailableAudioUnit auPeakLimiter { .component = { "aufx", "lmtr", "appl" }, .name = "AUPeakLimiter", .manufacturer = "Apple" };
const AvailableAudioUnit auReverb { .component = { "aufx", "rvb2", "appl" }, .name = "AUReverb", .manufacturer = "Apple" };

// A plugin with one parameter ("Mix") as its whole state, and no editor of its own.
class MixProcessor final : public overdub::engine::PlainProcessor
{
public:
    MixProcessor() : PlainProcessor (stereoInOut())
    {
        auto parameter = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "mix", 1 }, "Mix", 0.0f, 1.0f, 0.5f);
        mix = parameter.get();
        addParameter (parameter.release());
    }

    const juce::String getName() const override { return "Test Mix"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    void getStateInformation (juce::MemoryBlock& state) override
    {
        const auto text = juce::String (mix->get());
        state.replaceAll (text.toRawUTF8(), text.getNumBytesAsUTF8());
    }
    void setStateInformation (const void* data, int size) override { *mix = juce::String::fromUTF8 (static_cast<const char*> (data), size).getFloatValue(); }

    juce::AudioParameterFloat* mix = nullptr;
};

// Keeps the requests; the test completes them as the asynchronous AU loader would.
struct PendingPlugins
{
    std::vector<ProcessorReady> requests;
    std::size_t completed = 0;

    overdub::plugin::Instantiator instantiator()
    {
        return [this] (const InsertSlot&, double, int, ProcessorReady ready) { requests.push_back (std::move (ready)); };
    }

    void completeAll()
    {
        for (; completed < requests.size(); ++completed)
            requests[completed](std::unique_ptr<juce::AudioProcessor> (std::make_unique<MixProcessor>()));
    }

    void failNext()
    {
        requests.at (completed++) (std::unexpected (overdub::core::Error { "plugin.not_found", "not installed", {} }));
    }
};

struct InsertUiFixture : MainWindowFixture
{
    PendingPlugins* plugins;
    std::vector<AvailableAudioUnit> offered { auDelay, auPeakLimiter, auReverb };
    std::size_t pick = 0;

    explicit InsertUiFixture (PendingPlugins& pending) : MainWindowFixture (overdub::song::testing::threeTrackFixture(), pending.instantiator()), plugins (&pending)
    {
        window->listInsertPlugins = [this] { return offered; };
        window->chooseInsertPlugin = [this] (const std::vector<AvailableAudioUnit>&, std::function<void (std::optional<std::size_t>)> done) { done (pick); };
    }

    // Adds this offered plugin through the chain's "+ Insert", then lets it load.
    void addThrough (InsertChainView& chain, std::size_t offeredIndex)
    {
        pick = offeredIndex;
        click (chain.addButton);
        plugins->completeAll();
        runBlocks (1);
    }

    Mixer& mixer() { return window->editPage.mixer; }

    MixerStrip& stripNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& strip : mixer().strips)
            if (strip->trackId() == trackId)
                return *strip;
        FAIL ("no strip for Track " << name);
        return *mixer().strips.front();
    }

    std::vector<std::string> chainNames (const std::vector<InsertSlot>& chain) const
    {
        std::vector<std::string> names;
        for (const auto& slot : chain)
            names.push_back (slot.name);
        return names;
    }

    std::string undoLabel() const { return openSong->package().undoLabel(); }
    void undo() { REQUIRE (pressKey (*window, 'z', juce::ModifierKeys::commandModifier)); }
};

// Drags `chip` so that it is released at `target`'s centre moved by `offset`, all in the chain.
void dragChipTo (InsertChip& chip, const InsertChip& target, juce::Point<int> offset)
{
    const auto from = chip.getLocalBounds().getCentre();
    const auto to = target.getBounds().getCentre() + offset - chip.getPosition();
    drag (chip, from, to);
}

std::vector<std::string> chipTexts (const InsertChainView& chain)
{
    std::vector<std::string> texts;
    for (std::size_t i = 0; i < chain.numShown(); ++i)
        texts.push_back (chain.chips[i]->text().toStdString());
    return texts;
}

template <typename ComponentType>
ComponentType* findChild (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* found = dynamic_cast<ComponentType*> (child))
            return found;
        if (auto* found = findChild<ComponentType> (*child))
            return found;
    }
    return nullptr;
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

TEST_CASE ("in a Mixer strip, add, reorder, bypass and remove are one undo step each, with their labels", "[ui][mixer][inserts]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    REQUIRE (pressKey (*f.window, '2'));
    auto& chain = f.stripNamed ("Guitar").insertChain;
    REQUIRE (isShown (chain.addButton));

    f.addThrough (chain, 0);
    CHECK (f.undoLabel() == "Add AUDelay to Guitar");
    f.addThrough (chain, 1);
    f.addThrough (chain, 2);
    CHECK (f.chainNames (f.trackNamed ("Guitar").insertChain) == std::vector<std::string> { "AUDelay", "AUPeakLimiter", "AUReverb" });
    CHECK (chipTexts (chain) == std::vector<std::string> { "AUDelay", "AUPeakLimiter", "AUReverb" });
    CHECK (f.trackNamed ("Vocals").insertChain.empty());

    // Drag AUReverb above AUDelay's centre: it goes first.
    dragChipTo (*chain.chips[2], *chain.chips[0], { 0, -2 });
    CHECK (f.chainNames (f.trackNamed ("Guitar").insertChain) == std::vector<std::string> { "AUReverb", "AUDelay", "AUPeakLimiter" });
    CHECK (f.undoLabel() == "Move AUReverb in Guitar");
    CHECK (chipTexts (chain) == std::vector<std::string> { "AUReverb", "AUDelay", "AUPeakLimiter" });

    // A drag that ends where it started moves nothing.
    const auto stepsBefore = f.undoLabel();
    dragChipTo (*chain.chips[1], *chain.chips[1], { 0, 0 });
    CHECK (f.undoLabel() == stepsBefore);

    click (chain.chips[1]->bypassButton);
    CHECK (f.trackNamed ("Guitar").insertChain[1].bypassed);
    CHECK (f.undoLabel() == "Bypass AUDelay on Guitar");
    CHECK (chain.chips[1]->bypassButton.getToggleState());
    CHECK (chain.chips[1]->state().bypassed);

    click (chain.chips[2]->removeButton);
    CHECK (f.chainNames (f.trackNamed ("Guitar").insertChain) == std::vector<std::string> { "AUReverb", "AUDelay" });
    CHECK (f.undoLabel() == "Remove AUPeakLimiter from Guitar");
    CHECK (chain.numShown() == 2);

    // Undo walks back through each step.
    f.undo();
    CHECK (f.chainNames (f.trackNamed ("Guitar").insertChain) == std::vector<std::string> { "AUReverb", "AUDelay", "AUPeakLimiter" });
    f.undo();
    CHECK_FALSE (f.trackNamed ("Guitar").insertChain[1].bypassed);
    f.undo();
    CHECK (f.chainNames (f.trackNamed ("Guitar").insertChain) == std::vector<std::string> { "AUDelay", "AUPeakLimiter", "AUReverb" });
}

TEST_CASE ("the Master strip's chain adds and bypasses its own Inserts", "[ui][mixer][inserts][master]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    REQUIRE (pressKey (*f.window, '2'));
    auto& chain = f.mixer().masterStrip.insertChain;

    f.addThrough (chain, 1);
    REQUIRE (f.song().master.insertChain.size() == 1);
    CHECK (f.song().master.insertChain[0].name == "AUPeakLimiter");
    CHECK (f.undoLabel() == "Add AUPeakLimiter to Master");
    CHECK (f.trackNamed ("Guitar").insertChain.empty());

    click (chain.chips[0]->bypassButton);
    CHECK (f.song().master.insertChain[0].bypassed);
    CHECK (f.undoLabel() == "Bypass AUPeakLimiter on Master");
    click (chain.chips[0]->bypassButton);
    CHECK_FALSE (f.song().master.insertChain[0].bypassed);
    CHECK (f.undoLabel() == "Unbypass AUPeakLimiter on Master");
}

TEST_CASE ("on a Record row, chips reorder left to right, bypass and remove, and Monitoring counts the active ones", "[ui][record_page][inserts]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    auto& chain = f.rowNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    f.addThrough (chain, 1);
    CHECK (f.rowNamed ("Guitar").monitoringButton.getButtonText() == juce::String::fromUTF8 ("Monitoring \xC2\xB7 2 fx"));

    // Drag AUDelay right of AUPeakLimiter's centre: it goes last.
    dragChipTo (*chain.chips[0], *chain.chips[1], { 2, 0 });
    CHECK (f.chainNames (f.trackNamed ("Guitar").insertChain) == std::vector<std::string> { "AUPeakLimiter", "AUDelay" });
    CHECK (f.undoLabel() == "Move AUDelay in Guitar");

    click (chain.chips[0]->bypassButton);
    CHECK (f.undoLabel() == "Bypass AUPeakLimiter on Guitar");
    CHECK (f.rowNamed ("Guitar").monitoringButton.getButtonText() == juce::String::fromUTF8 ("Monitoring \xC2\xB7 1 fx"));

    click (chain.chips[1]->removeButton);
    CHECK (f.undoLabel() == "Remove AUDelay from Guitar");
    CHECK (f.rowNamed ("Guitar").monitoringButton.getButtonText() == "Monitoring");
}

TEST_CASE ("while a Pass runs the chain's edits are disabled, but an editor still opens", "[ui][inserts][record]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    auto& chain = f.rowNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    f.window->pluginWindows.closeAll();

    REQUIRE (f.openSong->record().has_value());
    f.runBlocks (2);
    CHECK_FALSE (chain.addButton.isEnabled());
    CHECK_FALSE (chain.chips[0]->bypassButton.isEnabled());
    CHECK_FALSE (chain.chips[0]->removeButton.isEnabled());
    dragChipTo (*chain.chips[0], *chain.chips[0], { 200, 0 });
    CHECK (f.trackNamed ("Guitar").insertChain.size() == 1);

    click (*chain.chips[0]);
    CHECK (f.window->pluginWindows.size() == 1);
    f.openSong->stop();
    f.runBlocks (1);
}

TEST_CASE ("clicking an Insert opens one editor window with the plugin's generic editor, a badge and bypass", "[ui][inserts][plugin_window]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    auto& chain = f.rowNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    f.window->pluginWindows.closeAll();
    REQUIRE (f.window->pluginWindows.size() == 0);
    ScopedLogCapture capture;

    click (*chain.chips[0]);
    click (*chain.chips[0]); // again: the same window comes forward

    REQUIRE (f.window->pluginWindows.size() == 1);
    const auto& slot = f.trackNamed ("Guitar").insertChain[0];
    auto* window = f.window->pluginWindows.find (slot.id);
    REQUIRE (window != nullptr);
    CHECK (window->getName() == juce::String::fromUTF8 ("Guitar \xE2\x80\x94 AUDelay"));
    CHECK (window->isUsingNativeTitleBar());
    CHECK (window->isResizable());
    CHECK_FALSE (window->isOnDesktop());
    auto& panel = window->panel();
    CHECK (panel.usesGenericEditor);
    CHECK (dynamic_cast<juce::GenericAudioProcessorEditor*> (panel.editor.get()) != nullptr);
    CHECK (panel.badgeLabel.getText() == "in-process");
    CHECK (window->processor().get() == f.openSong->insertHost().processor (slot.id));
    // The window fits the badge strip over the editor.
    CHECK (panel.getHeight() == PluginEditorPanel::headerHeight + panel.editor->getHeight());

    const auto opened = eventsNamed (capture, "ui.plugin_window_opened");
    REQUIRE (opened.size() == 1);
    CHECK (opened[0].at ("slot_id") == slot.id);
    CHECK (opened[0].at ("editor") == "generic");
    CHECK (opened[0].at ("native_title_bar") == true);

    // Its bypass toggle is the chain's bypass: one undo step, shown in both places.
    click (panel.bypassButton);
    CHECK (f.trackNamed ("Guitar").insertChain[0].bypassed);
    CHECK (f.undoLabel() == "Bypass AUDelay on Guitar");
    CHECK (panel.bypassButton.getToggleState());
    CHECK (chain.chips[0]->bypassButton.getToggleState());
}

TEST_CASE ("an editor window follows its editor's size and logs it", "[ui][inserts][plugin_window]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    f.addThrough (f.rowNamed ("Guitar").insertChain, 0);
    const auto slotId = f.trackNamed ("Guitar").insertChain[0].id;
    auto* window = f.window->pluginWindows.find (slotId);
    REQUIRE (window != nullptr);
    ScopedLogCapture capture;

    // As a remote AU view does once it attaches.
    window->panel().editor->setSize (640, 420);

    CHECK (window->panel().getWidth() == 640);
    CHECK (window->panel().getHeight() == PluginEditorPanel::headerHeight + 420);
    CHECK (window->getContentComponent()->getHeight() == PluginEditorPanel::headerHeight + 420);
    const auto resized = eventsNamed (capture, "ui.plugin_editor_resized");
    REQUIRE_FALSE (resized.empty());
    CHECK (resized.back().at ("width") == 640);
    CHECK (resized.back().at ("height") == 420);
    CHECK (resized.back().at ("native_title_bar") == true);
}

TEST_CASE ("adding an Insert opens its editor window once the plugin has loaded", "[ui][inserts][plugin_window]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    f.pick = 2;

    click (f.rowNamed ("Guitar").insertChain.addButton);
    CHECK (f.window->pluginWindows.size() == 0); // still loading
    f.runBlocks (1);
    CHECK (f.window->pluginWindows.size() == 0);

    plugins.completeAll();
    f.runBlocks (1);
    REQUIRE (f.window->pluginWindows.size() == 1);
    CHECK (f.window->pluginWindows.find (f.trackNamed ("Guitar").insertChain[0].id) != nullptr);
}

TEST_CASE ("a missing plugin's Insert opens no window and says so in a Notice", "[ui][inserts][plugin_window]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    click (f.rowNamed ("Guitar").insertChain.addButton);
    plugins.failNext();
    f.runBlocks (1);
    auto& chip = *f.rowNamed ("Guitar").insertChain.chips[0];
    CHECK (chip.text() == "AUDelay (missing)");

    click (chip);
    CHECK (f.window->pluginWindows.size() == 0);
    CHECK (isShown (f.window->noticeLine));
}

TEST_CASE ("removing an Insert, or undoing its addition, closes its editor window", "[ui][inserts][plugin_window]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    auto& chain = f.rowNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    f.addThrough (chain, 1);
    REQUIRE (f.window->pluginWindows.size() == 2);

    click (chain.chips[0]->removeButton);
    CHECK (f.window->pluginWindows.size() == 1);

    f.undo(); // the removal: the slot is back in a new plugin, with no window
    f.undo(); // the second addition
    CHECK (f.window->pluginWindows.size() == 0);
}

TEST_CASE ("a parameter moved in an editor is saved to song.json without an undo step", "[ui][inserts][plugin_window][state]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    f.addThrough (f.rowNamed ("Guitar").insertChain, 0);
    const auto slotId = f.trackNamed ("Guitar").insertChain[0].id;
    auto* window = f.window->pluginWindows.find (slotId);
    REQUIRE (window != nullptr);
    const auto labelBefore = f.undoLabel();

    auto* slider = findChild<juce::Slider> (*window->panel().editor);
    REQUIRE (slider != nullptr);
    slider->setValue (0.25, juce::sendNotificationSync);

    auto* processor = dynamic_cast<MixProcessor*> (f.openSong->insertHost().processor (slotId));
    REQUIRE (processor != nullptr);
    CHECK_THAT (processor->mix->get(), Catch::Matchers::WithinAbs (0.25, 1e-6));
    const auto start = overdub::song::SongPackage::Clock::now();
    f.openSong->poll (start);
    CHECK (f.openSong->package().hasPendingWrite());
    f.openSong->poll (start + 1s);
    CHECK (f.undoLabel() == labelBefore);

    const auto json = overdub::song::parseJsonText (f.openSong->package().location().getChildFile ("song.json").loadFileAsString().toStdString());
    REQUIRE (json.has_value());
    const auto onDisk = overdub::song::songFromJson (*json);
    REQUIRE (onDisk.has_value());
    const auto& hash = onDisk->tracks.at (1).insertChain.at (0).stateHash;
    REQUIRE_FALSE (hash.empty());
    CHECK (f.openSong->package().loadPluginState (hash)->toString() == "0.25");
}

TEST_CASE ("closing an editor window schedules a song.json write without an undo step", "[ui][inserts][plugin_window][state]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    f.addThrough (f.rowNamed ("Guitar").insertChain, 0);
    const auto slotId = f.trackNamed ("Guitar").insertChain[0].id;
    f.openSong->poll (overdub::song::SongPackage::Clock::now() + 10s); // nothing pending
    REQUIRE_FALSE (f.openSong->package().hasPendingWrite());
    const auto labelBefore = f.undoLabel();

    f.window->pluginWindows.find (slotId)->closeButtonPressed();
    REQUIRE (overdub::plugin::testing::runMessagesUntil ([&] { return f.window->pluginWindows.size() == 0; }, 5s));

    CHECK (f.openSong->package().hasPendingWrite());
    CHECK (f.undoLabel() == labelBefore);
}

TEST_CASE ("a Mixer strip with three Inserts, one bypassed, renders to a snapshot", "[ui][mixer][inserts][snapshot]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    REQUIRE (pressKey (*f.window, '2'));
    auto& chain = f.stripNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    f.addThrough (chain, 1);
    f.addThrough (chain, 2);
    f.addThrough (f.mixer().masterStrip.insertChain, 1);
    click (chain.chips[1]->bypassButton);
    f.window->pluginWindows.closeAll();
    f.runBlocks (1);

    CHECK (writeSnapshot (f.stripNamed ("Guitar"), "slice-13", "mixer-strip-three-inserts-one-bypassed").existsAsFile());
    CHECK (writeSnapshot (*f.window, "slice-13", "edit-mixer-inserts").existsAsFile());

    // Record mode shows the same chain on Guitar's row.
    REQUIRE (pressKey (*f.window, '1'));
    f.runBlocks (1);
    CHECK (writeSnapshot (*f.window, "slice-13", "record-inserts").existsAsFile());
}

TEST_CASE ("a plugin editor window renders to a snapshot", "[ui][inserts][plugin_window][snapshot]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    f.addThrough (f.rowNamed ("Guitar").insertChain, 0);
    auto* window = f.window->pluginWindows.find (f.trackNamed ("Guitar").insertChain[0].id);
    REQUIRE (window != nullptr);

    CHECK (writeSnapshot (window->panel(), "slice-13", "plugin-window").existsAsFile());
}

TEST_CASE ("a crashed plugin closes its editor window, reads (crashed), shows a Notice, and comes back once reloaded", "[ui][inserts][plugin_window][crash]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    auto& chain = f.rowNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    const auto slotId = f.trackNamed ("Guitar").insertChain[0].id;
    REQUIRE (f.window->pluginWindows.size() == 1);
    ScopedLogCapture capture;

    f.openSong->insertHost().pluginCrashed (*f.openSong->insertHost().processor (slotId));
    f.runBlocks (1);

    CHECK (f.window->pluginWindows.size() == 0);
    CHECK (chain.chips[0]->text() == "AUDelay (loading)"); // already reloading: nothing is recording
    CHECK (isShown (f.window->noticeLine));
    CHECK (f.window->noticeLine.text() == juce::String (sentenceFor ("ui.plugin_crashed")));
    const auto crashed = eventsNamed (capture, "ui.plugin_crashed");
    REQUIRE (crashed.size() == 1);
    CHECK (crashed[0].at ("lvl") == "warn");
    CHECK (crashed[0].at ("names") == nlohmann::json::array ({ "AUDelay" }));
    const auto shown = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (shown.size() == 1);
    CHECK (shown[0].at ("code") == "ui.plugin_crashed");
    CHECK (shown[0].at ("tier") == "notice");

    plugins.completeAll();
    f.runBlocks (1);
    CHECK (chain.chips[0]->text() == "AUDelay");
}

TEST_CASE ("a plugin crashed for good reads (crashed), and clicking it shows a Notice instead of a window", "[ui][inserts][plugin_window][crash]")
{
    PendingPlugins plugins;
    InsertUiFixture f { plugins };
    auto& chain = f.rowNamed ("Guitar").insertChain;
    f.addThrough (chain, 0);
    const auto slotId = f.trackNamed ("Guitar").insertChain[0].id;
    f.window->pluginWindows.closeAll();
    for (int crash = 0; crash <= overdub::plugin::maxCrashReloads; ++crash)
    {
        f.openSong->insertHost().pluginCrashed (*f.openSong->insertHost().processor (slotId));
        f.runBlocks (1);
        plugins.completeAll();
        f.runBlocks (1);
    }
    REQUIRE (chain.chips[0]->text() == "AUDelay (crashed)");
    ScopedLogCapture capture;

    click (*chain.chips[0]);

    CHECK (f.window->pluginWindows.size() == 0);
    const auto shown = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (shown.size() == 1);
    CHECK (shown[0].at ("code") == "ui.plugin_crashed_repeatedly");
}
