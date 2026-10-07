#pragma once

#include "ui/Commands.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

// The always-visible top bar (issue #9 resolution): Record · Edit · Export, the transport
// (⏮ ▶/■ ●) with the clock and its REC · PASS n line, and Undo / Redo naming their steps.
// Thin: it shows a TopBarState and turns clicks into Commands.
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
};

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

class TopBar final : public juce::Component
{
public:
    TopBar();

    void show (const TopBarState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void (Command)> onCommand;

    // Children are public so tests can click them (ADR 0005 UI amendment).
    juce::TextButton recordModeButton { "Record" };
    juce::TextButton editModeButton { "Edit" };
    juce::TextButton exportModeButton { "Export" };
    juce::TextButton rewindButton;
    juce::TextButton playStopButton;
    juce::TextButton recordButton;
    ClockDisplay clock;
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };

private:
    void send (Command command);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};
} // namespace overdub::ui
