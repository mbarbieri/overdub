#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

// The simple pages and the notice line (issue #27): the page shown when no Song is open and the
// Notice one-liner. (The Export sheet is ui/ExportSheet.h.)
namespace overdub::ui
{
// Shown with no Song open: New Song and Open.
class NoSongPage final : public juce::Component
{
public:
    NoSongPage();

    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void()> onNewSong;
    std::function<void()> onOpenSong;

    juce::Label message;
    juce::TextButton newSongButton { "New Song..." };
    juce::TextButton openSongButton { "Open..." };
};

// The Notice tier (ADR 0006): a non-modal one-liner that hides itself after `visibleFor`. A Notice
// may offer one action (issue #160), a button at its right end: it then stays until the action is
// clicked, another Notice replaces it or dismiss() is called.
class NoticeLine final : public juce::Component, private juce::Timer
{
public:
    static constexpr int visibleForMs = 5000;

    NoticeLine();

    void show (const juce::String& sentence, const juce::String& actionLabel = {}, std::function<void()> onAction = {});
    void dismiss();

    const juce::String& text() const { return sentence; }
    bool hasAction() const { return action != nullptr; }
    // When it hides on its own, in juce::Time::currentTimeMillis (Unix epoch ms), fixed when shown;
    // none while hidden or while it has an action, which it shows until clicked or replaced.
    std::optional<juce::int64> hidesAtMs() const { return isVisible() ? hidesAt : std::nullopt; }

    void resized() override;
    void paint (juce::Graphics& g) override;

    juce::TextButton actionButton; // public so tests can click it

private:
    void timerCallback() override { dismiss(); }

    juce::String sentence;
    std::function<void()> action;
    std::optional<juce::int64> hidesAt;
};
} // namespace overdub::ui
