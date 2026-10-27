#include "ui/TopBar.h"

#include "song/Transpose.h"
#include "ui/Colours.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

// The top bar's Transpose field (issue #209): "Transpose [0] st", a spinner while renders run,
// highlighted whenever Transpose is not 0.
namespace
{
const std::string minusOne = "\xe2\x88\x92"
                             "1";

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

    void show (int transpose, bool rendering = false, bool canTranspose = true)
    {
        TopBarState state;
        state.songOpen = true;
        state.transpose = transpose;
        state.transposeRendering = rendering;
        state.canTranspose = canTranspose;
        bar.show (state);
    }

    std::optional<int> sentSemitones() const
    {
        if (sent.empty())
            return std::nullopt;
        const auto* song = std::get_if<SongIntent> (&sent.back());
        const auto* setTranspose = song != nullptr ? std::get_if<intent::SetTranspose> (song) : nullptr;
        return setTranspose != nullptr ? std::optional (setTranspose->semitones) : std::nullopt;
    }
};
} // namespace

TEST_CASE ("Transpose is typed as a whole number with an optional sign", "[ui][top_bar][transpose]")
{
    CHECK (parseTranspose ("3") == 3);
    CHECK (parseTranspose ("+3") == 3);
    CHECK (parseTranspose ("-1") == -1);
    CHECK (parseTranspose (minusOne) == -1);
    CHECK (parseTranspose (" 12 ") == 12);
    CHECK (parseTranspose ("0") == 0);
    CHECK (parseTranspose ("13") == 13); // out of range: the handler refuses it, with its Notice
    CHECK_FALSE (parseTranspose ("1.5").has_value());
    CHECK_FALSE (parseTranspose ("two").has_value());
    CHECK_FALSE (parseTranspose ("+").has_value());
    CHECK_FALSE (parseTranspose ("").has_value());
    CHECK_FALSE (parseTranspose ("99999999999").has_value());
    CHECK_FALSE (parseTranspose ("--3").has_value());
    CHECK_FALSE (parseTranspose ("+-3").has_value());
    CHECK_FALSE (parseTranspose ("- 3").has_value());

    CHECK (song::transposeText (0) == "0");
    CHECK (song::transposeText (5) == "+5");
    CHECK (song::transposeText (-1) == minusOne);
}

TEST_CASE ("the Transpose field shows the Song's value, highlighted unless 0, with a spinner while renders run", "[ui][top_bar][transpose]")
{
    TopBarHarness h;

    h.show (0);
    CHECK (h.bar.transposeCaption.getText() == "TRANSPOSE");
    CHECK (h.bar.transposeField.getText() == "0");
    CHECK (h.bar.transposeUnit.getText() == "st");
    CHECK_FALSE (h.bar.transposeField.findColour (juce::Label::backgroundColourId) == colours::accent);
    CHECK_FALSE (h.bar.transposeSpinner.isVisible());

    h.show (-1, true);
    CHECK (h.bar.transposeField.getText() == juce::String::fromUTF8 (minusOne.c_str()));
    CHECK (h.bar.transposeField.findColour (juce::Label::backgroundColourId) == colours::accent);
    CHECK (h.bar.transposeSpinner.isVisible());

    h.show (-1, false, false);
    CHECK_FALSE (h.bar.transposeSpinner.isVisible());
    CHECK_FALSE (h.bar.transposeField.isEnabled());
}

TEST_CASE ("a whole number typed into the Transpose field runs set_transpose, and anything else shows the value again", "[ui][top_bar][transpose]")
{
    TopBarHarness h;
    h.show (0);

    h.bar.transposeField.setText ("+2", juce::sendNotificationSync);
    CHECK (h.sentSemitones() == 2);

    h.sent.clear();
    h.bar.transposeField.setText ("two", juce::sendNotificationSync);
    CHECK (h.sent.empty());
    CHECK (h.bar.transposeField.getText() == "0");
}

TEST_CASE ("Transpose typed in the window's top bar is one undo step, and an out-of-range value a Notice", "[ui][top_bar][transpose]")
{
    MainWindowFixture f;
    auto& field = f.window->topBar.transposeField;

    field.setText ("-1", juce::sendNotificationSync);
    CHECK (f.song().transpose == -1);
    CHECK (f.openSong->package().undoLabel() == "Transpose " + minusOne);
    CHECK (field.getText() == juce::String::fromUTF8 (minusOne.c_str()));
    CHECK (f.window->topBar.transposeField.findColour (juce::Label::backgroundColourId) == colours::accent);

    core::ScopedLogCapture capture;
    field.setText ("13", juce::sendNotificationSync);
    CHECK (f.song().transpose == -1);
    CHECK (field.getText() == juce::String::fromUTF8 (minusOne.c_str()));
    bool noticeShown = false;
    for (const auto& event : capture.events())
        if (event.at ("event") == "ui.notice_shown" && event.at ("code") == "song.transpose_out_of_range")
            noticeShown = true;
    CHECK (noticeShown);
}
