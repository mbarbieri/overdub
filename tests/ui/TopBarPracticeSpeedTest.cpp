#include "ui/TopBar.h"

#include "ui/Colours.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

// The top bar's Practice Speed field (issue #212): "Practice Speed [100] %", a spinner while its
// renders run, highlighted whenever the value is not 100.
namespace
{
struct TopBarHarness
{
    juce::ScopedJuceInitialiser_GUI gui;
    TopBar bar;
    std::vector<Intent> sent;

    TopBarHarness()
    {
        bar.setBounds (0, 0, 1200, topBarHeight);
        bar.onIntent = [this] (const Intent& intent) { sent.push_back (intent); };
    }

    void show (int practiceSpeed, bool rendering = false, bool canSet = true)
    {
        TopBarState state;
        state.songOpen = true;
        state.practiceSpeed = practiceSpeed;
        state.practiceRendering = rendering;
        state.canSetPracticeSpeed = canSet;
        bar.show (state);
    }

    std::optional<int> sentPercent() const
    {
        if (sent.empty())
            return std::nullopt;
        const auto* song = std::get_if<SongIntent> (&sent.back());
        const auto* set = song != nullptr ? std::get_if<intent::SetPracticeSpeed> (song) : nullptr;
        return set != nullptr ? std::optional (set->speedPercent) : std::nullopt;
    }
};
} // namespace

TEST_CASE ("Practice Speed is typed as a whole number, with an optional %", "[ui][top_bar][practice_speed]")
{
    CHECK (parsePracticeSpeed ("80") == 80);
    CHECK (parsePracticeSpeed (" 125 ") == 125);
    CHECK (parsePracticeSpeed ("90%") == 90);
    CHECK (parsePracticeSpeed ("90 %") == 90);
    CHECK (parsePracticeSpeed ("200") == 200); // out of range: the handler refuses it, with its Notice
    CHECK_FALSE (parsePracticeSpeed ("80.5").has_value());
    CHECK_FALSE (parsePracticeSpeed ("-80").has_value());
    CHECK_FALSE (parsePracticeSpeed ("+80").has_value());
    CHECK_FALSE (parsePracticeSpeed ("fast").has_value());
    CHECK_FALSE (parsePracticeSpeed ("%").has_value());
    CHECK_FALSE (parsePracticeSpeed ("").has_value());
    CHECK_FALSE (parsePracticeSpeed ("99999999999").has_value());
}

TEST_CASE ("the Practice Speed field shows its value, highlighted unless 100, with a spinner while renders run", "[ui][top_bar][practice_speed]")
{
    TopBarHarness h;

    h.show (100);
    CHECK (h.bar.practiceSpeedCaption.getText() == "PRACTICE SPEED");
    CHECK (h.bar.practiceSpeedField.getText() == "100");
    CHECK (h.bar.practiceSpeedUnit.getText() == "%");
    CHECK_FALSE (h.bar.practiceSpeedField.findColour (juce::Label::backgroundColourId) == colours::accent);
    CHECK_FALSE (h.bar.practiceSpeedSpinner.isVisible());

    h.show (80, true);
    CHECK (h.bar.practiceSpeedField.getText() == "80");
    CHECK (h.bar.practiceSpeedField.findColour (juce::Label::backgroundColourId) == colours::accent);
    CHECK (h.bar.practiceSpeedSpinner.isVisible());

    h.show (80, false, false);
    CHECK_FALSE (h.bar.practiceSpeedSpinner.isVisible());
    CHECK_FALSE (h.bar.practiceSpeedField.isEnabled());
}

TEST_CASE ("a whole number typed into the Practice Speed field runs set_practice_speed, and anything else shows the value again", "[ui][top_bar][practice_speed]")
{
    TopBarHarness h;
    h.show (100);

    h.bar.practiceSpeedField.setText ("80", juce::sendNotificationSync);
    CHECK (h.sentPercent() == 80);

    h.sent.clear();
    h.bar.practiceSpeedField.setText ("slow", juce::sendNotificationSync);
    CHECK (h.sent.empty());
    CHECK (h.bar.practiceSpeedField.getText() == "100");
}

TEST_CASE ("Practice Speed typed in the window's top bar is no undo step, and an out-of-range value a Notice", "[ui][top_bar][practice_speed]")
{
    MainWindowFixture f;
    auto& field = f.window->topBar.practiceSpeedField;
    const auto undoLabel = f.openSong->package().undoLabel();

    field.setText ("80", juce::sendNotificationSync);
    CHECK (f.openSong->practiceSpeed() == 80);
    CHECK (f.openSong->package().undoLabel() == undoLabel);
    CHECK (field.getText() == "80");
    CHECK (field.findColour (juce::Label::backgroundColourId) == colours::accent);
    CHECK (f.window->topBar.practiceSpeedSpinner.isVisible());

    core::ScopedLogCapture capture;
    field.setText ("24", juce::sendNotificationSync);
    CHECK (f.openSong->practiceSpeed() == 80);
    CHECK (field.getText() == "80");
    bool noticeShown = false;
    for (const auto& event : capture.events())
        if (event.at ("event") == "ui.notice_shown" && event.at ("code") == "song.practice_speed_out_of_range")
            noticeShown = true;
    CHECK (noticeShown);
}

TEST_CASE ("in every mode H toggles half speed, and - and = step the Practice Speed by 5%, clamped to 25..125, never an undo step", "[ui][keys][practice_speed]")
{
    MainWindowFixture f;
    const auto undoLabel = f.openSong->package().undoLabel();
    const auto press = [&f] (char character) { f.window->keyPressed (juce::KeyPress ((int) character, {}, (juce::juce_wchar) character)); };

    for (const auto mode : { Mode::record, Mode::edit, Mode::transcribe })
    {
        INFO (modeName (mode));
        f.window->run (WindowIntent { intent::ShowMode { mode } });
        press ('h');
        CHECK (f.openSong->practiceSpeed() == 50);
        press ('h');
        CHECK (f.openSong->practiceSpeed() == 100);
        press ('-');
        CHECK (f.openSong->practiceSpeed() == 95);
        press ('=');
        CHECK (f.openSong->practiceSpeed() == 100);
    }

    for (int i = 0; i < 20; ++i)
        press ('-');
    CHECK (f.openSong->practiceSpeed() == 25);
    for (int i = 0; i < 30; ++i)
        press ('=');
    CHECK (f.openSong->practiceSpeed() == 125);
    CHECK (f.window->topBar.practiceSpeedField.getText() == "125");
    CHECK (f.openSong->package().undoLabel() == undoLabel);
}
