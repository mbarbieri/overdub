#pragma once

#include "ui/Intent.h"
#include "ui/ThinProgressBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// The always-visible top bar (issue #9 resolution): Record · Edit · Transcribe (issue #225), the transport (⏮ ▶/■ ●) with
// the clock and its REC · PASS n line, Undo / Redo naming their steps, and Export..., which opens
// the Export sheet (issue #200); while an export runs, its status and progress stand in its place,
// with ✕, which cancels it (issue #202). Before it, Transpose (issue #209): "TRANSPOSE" over the
// field, "st" and a spinner while its renders run; the field is highlighted unless Transpose is 0,
// and a whole number typed into it runs set_transpose. Before that, Practice Speed (issue #212) the
// same way: "PRACTICE SPEED" over the field, "%" and a spinner; highlighted unless 100, and a whole
// number typed into it runs set_practice_speed.
// While a control client is connected it names the controlling process (ADR 0008 Security, issue
// #177). Thin: it shows a TopBarState and turns clicks into Intents.
namespace overdub::ui
{
struct TopBarState
{
    Mode mode = Mode::record;
    bool songOpen = false;
    bool playing = false;
    bool recording = false;
    std::string clock = "0.0 s";
    std::string status = "STOP";
    std::string undoText = "Undo";
    std::string redoText = "Redo";
    bool canUndo = false;
    bool canRedo = false;
    // The codes record and show_export are refused with now (ui/Availability.h), none when they
    // may run; the button is unavailable with unavailableReason as its tooltip.
    std::optional<std::string> recordRefusal;
    std::optional<std::string> exportRefusal;
    bool canCancelExport = false; // cancel_export may run
    // While an export runs: "Preparing…" or "Exporting… 42%" (exportStatusText), and its progress.
    std::optional<std::string> exportStatus;
    double exportProgress = 0.0;
    int transpose = 0;               // the Song's
    bool transposeRendering = false; // OpenSong::isTransposeRendering
    bool canTranspose = false;       // set_transpose may run
    int practiceSpeed = 100;            // OpenSong::practiceSpeed
    bool practiceRendering = false;     // OpenSong::isPracticeRendering
    bool canSetPracticeSpeed = false;   // set_practice_speed may run
    // The connected control clients' process names, oldest connection first.
    std::vector<std::string> controllingProcesses;
};

// The running export's status: "Preparing…" while it waits for the audio to let go of the
// plugins, then "Exporting… 42%" (the fraction written, rounded down).
std::string exportStatusText (bool waitingForAudio, double progress);

// A whole number typed into the Transpose field (what song::transposeText shows), with one optional sign (+, - or U+2212 minus) and
// spaces around it; none for anything else. The range is the handler's to check.
std::optional<int> parseTranspose (const std::string& text);

// A whole percent typed into the Practice Speed field: digits, an optional "%" after them, and
// spaces around; none for anything else. The range is the handler's to check.
std::optional<int> parsePracticeSpeed (const std::string& text);

// The indicator's name line: "<first>", or "<first> +N" with N other processes; empty with none.
std::string controlIndicatorName (const std::vector<std::string>& processes);

// The transport clock: the time over its status line.
class ClockDisplay final : public juce::Component
{
public:
    void show (const std::string& time, const std::string& status, bool recording);
    void paint (juce::Graphics& g) override;

    const juce::String& timeText() const { return time; }
    const juce::String& statusText() const { return status; }

private:
    juce::String time;
    juce::String status;
    bool isRecording = false;
};

// "CONTROLLED BY" over the controlling process's name, drawn only while there is a name. The name
// is never scaled down: one too wide for the slot ends in an ellipsis.
class ControlIndicator final : public juce::Component
{
public:
    static constexpr const char* caption = "CONTROLLED BY";

    void show (const std::string& name);
    void paint (juce::Graphics& g) override;

    const juce::String& nameText() const { return name; }

private:
    juce::String name;
};

// A small arc turning while it is visible: the Transpose or Practice Speed renders run.
class Spinner final : public juce::Component, private juce::Timer
{
public:
    void visibilityChanged() override;
    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    int step = 0;
};

class TopBar final : public juce::Component
{
public:
    TopBar();

    void show (const TopBarState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    IntentSink onIntent;

    // Children are public so tests can click them (ADR 0005 UI amendment).
    juce::TextButton recordModeButton { "Record" };
    juce::TextButton editModeButton { "Edit" };
    juce::TextButton transcribeModeButton { "Transcribe" };
    juce::TextButton rewindButton;
    juce::TextButton playStopButton;
    juce::TextButton recordButton;
    ClockDisplay clock;
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::Label transposeCaption { {}, "TRANSPOSE" };
    juce::Label transposeField;
    juce::Label transposeUnit { {}, "st" };
    Spinner transposeSpinner;
    juce::Label practiceSpeedCaption { {}, "PRACTICE SPEED" };
    juce::Label practiceSpeedField;
    juce::Label practiceSpeedUnit { {}, "%" };
    Spinner practiceSpeedSpinner;
    ControlIndicator controlIndicator;
    juce::TextButton exportButton { juce::String::fromUTF8 ("Export\xe2\x80\xa6") };
    juce::Label exportStatusLabel;
    ThinProgressBar exportProgressBar;
    juce::TextButton cancelExportButton;

private:
    void send (const Intent& intent);
    void transposeEntered();
    void practiceSpeedEntered();

    std::string shownTranspose = "0";
    int shownPracticeSpeed = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};
} // namespace overdub::ui
