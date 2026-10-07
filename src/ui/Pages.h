#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// The simple pages and the notice line (issue #27): Edit and Export placeholders (their real
// pages are slices 7 and 16), the page shown when no Song is open, and the Notice one-liner.
namespace overdub::ui
{
// A page with one centred line of text.
class PlaceholderPage final : public juce::Component
{
public:
    explicit PlaceholderPage (const juce::String& text);

    void resized() override;
    void paint (juce::Graphics& g) override;

    juce::Label message;
};

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

// The Notice tier (ADR 0006): a non-modal one-liner that hides itself after `visibleFor`.
class NoticeLine final : public juce::Component, private juce::Timer
{
public:
    static constexpr int visibleForMs = 5000;

    NoticeLine();

    void show (const juce::String& sentence);
    void dismiss();

    const juce::String& text() const { return sentence; }

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override { dismiss(); }

    juce::String sentence;
};
} // namespace overdub::ui
