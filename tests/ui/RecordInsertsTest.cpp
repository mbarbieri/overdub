#include "ui/RecordPage.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using overdub::plugin::AvailableAudioUnit;
using overdub::plugin::InsertStatus;
using overdub::plugin::ProcessorReady;
using overdub::song::InsertSlot;
using namespace overdub::ui;
using namespace overdub::ui::testing;

// "+ Insert" on Record rows (issue #33): the minimal way to add an AU before slice 13's editor.
namespace
{
const AvailableAudioUnit auDelay { .component = { "aufx", "dely", "appl" }, .name = "AUDelay", .manufacturer = "Apple", .version = "1.7.0" };
const AvailableAudioUnit auPeakLimiter { .component = { "aufx", "lmtr", "appl" }, .name = "AUPeakLimiter", .manufacturer = "Apple" };

InsertSlot slotNamed (const std::string& id, const std::string& name)
{
    return InsertSlot { .id = id, .component = { "aufx", "test", "ovdb" }, .name = name };
}

// Keeps the requests; the test completes them as the asynchronous AU loader would.
struct PendingPlugins
{
    std::vector<ProcessorReady> requests;

    overdub::plugin::Instantiator instantiator()
    {
        return [this] (const InsertSlot&, double, int, ProcessorReady ready) { requests.push_back (std::move (ready)); };
    }
};

class PassThrough final : public overdub::engine::PlainProcessor
{
public:
    PassThrough() : PlainProcessor (stereoInOut()) {}
    const juce::String getName() const override { return "Pass Through"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
};
} // namespace

TEST_CASE ("an Insert Chain reads as its plugins' names with their loading or missing state", "[ui][record_page][inserts]")
{
    const std::vector<InsertSlot> chain { slotNamed ("s1", "AUDelay"), slotNamed ("s2", "NAM"), slotNamed ("s3", "AUPeakLimiter") };
    const auto statusOf = [] (const overdub::song::Id& slotId) -> std::optional<InsertStatus> {
        if (slotId == "s1")
            return InsertStatus::ready;
        if (slotId == "s2")
            return InsertStatus::failed;
        return InsertStatus::loading;
    };

    CHECK (insertChainText (chain, statusOf) == "AUDelay \xC2\xB7 NAM (missing) \xC2\xB7 AUPeakLimiter (loading)");
    CHECK (insertChainText (chain, {}) == "AUDelay \xC2\xB7 NAM \xC2\xB7 AUPeakLimiter");
    CHECK (insertChainText ({}, statusOf) == "No Inserts");
}

TEST_CASE ("+ Insert on a Record row adds the picked AU to that Track's chain as one undo step", "[ui][record_page][inserts]")
{
    PendingPlugins plugins;
    MainWindowFixture f { overdub::song::testing::threeTrackFixture(), plugins.instantiator() };
    std::vector<AvailableAudioUnit> offered;
    f.window->listInsertPlugins = [] { return std::vector<AvailableAudioUnit> { auDelay, auPeakLimiter }; };
    f.window->chooseInsertPlugin = [&offered] (const std::vector<AvailableAudioUnit>& list, std::function<void (std::optional<std::size_t>)> done) {
        offered = list;
        done (1);
    };
    auto& row = f.rowNamed ("Guitar");
    CHECK (row.insertsLabel.getText() == "No Inserts");
    CHECK (isShown (row.insertButton));

    click (row.insertButton);

    CHECK (offered.size() == 2);
    const auto& chain = f.trackNamed ("Guitar").insertChain;
    REQUIRE (chain.size() == 1);
    CHECK (chain[0].name == "AUPeakLimiter");
    CHECK (chain[0].component == auPeakLimiter.component);
    CHECK (f.openSong->package().undoLabel() == "Add AUPeakLimiter to Guitar");
    CHECK (f.trackNamed ("Vocals").insertChain.empty());
    CHECK (f.rowNamed ("Guitar").insertsLabel.getText() == "AUPeakLimiter (loading)");

    // Once it has loaded, the row shows it plainly.
    REQUIRE (plugins.requests.size() == 1);
    plugins.requests[0](std::unique_ptr<juce::AudioProcessor> (std::make_unique<PassThrough>()));
    f.runBlocks (1);
    CHECK (f.rowNamed ("Guitar").insertsLabel.getText() == "AUPeakLimiter");
}

TEST_CASE ("dismissing the plugin menu adds nothing, and + Insert is disabled during a Pass", "[ui][record_page][inserts]")
{
    PendingPlugins plugins;
    MainWindowFixture f { overdub::song::testing::threeTrackFixture(), plugins.instantiator() };
    f.window->listInsertPlugins = [] { return std::vector<AvailableAudioUnit> { auDelay }; };
    f.window->chooseInsertPlugin = [] (const std::vector<AvailableAudioUnit>&, std::function<void (std::optional<std::size_t>)> done) { done (std::nullopt); };

    click (f.rowNamed ("Guitar").insertButton);
    CHECK (f.trackNamed ("Guitar").insertChain.empty());
    CHECK_FALSE (f.openSong->package().undoLabel().starts_with ("Add AUDelay"));

    REQUIRE (f.openSong->record().has_value());
    f.runBlocks (2);
    CHECK_FALSE (f.rowNamed ("Guitar").insertButton.isEnabled());
    f.openSong->stop();
    f.runBlocks (1);
}

TEST_CASE ("Record mode with Insert Chains renders to a snapshot", "[ui][record_page][inserts][snapshot]")
{
    PendingPlugins plugins;
    MainWindowFixture f { overdub::song::testing::threeTrackFixture(), plugins.instantiator() };
    f.window->listInsertPlugins = [] { return std::vector<AvailableAudioUnit> { auDelay, auPeakLimiter }; };
    std::size_t pick = 0;
    f.window->chooseInsertPlugin = [&pick] (const std::vector<AvailableAudioUnit>&, std::function<void (std::optional<std::size_t>)> done) { done (pick); };
    click (f.rowNamed ("Guitar").insertButton);
    pick = 1;
    click (f.rowNamed ("Guitar").insertButton);
    plugins.requests[0](std::unique_ptr<juce::AudioProcessor> (std::make_unique<PassThrough>()));
    f.runBlocks (1);

    const auto file = writeSnapshot (*f.window, "slice-12", "record-inserts");
    CHECK (file.existsAsFile());
}
