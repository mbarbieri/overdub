#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// A thin bar filled to the fraction done (drawn at once, unlike juce::ProgressBar, which follows
// its value on a timer). The Export page and the Separation dialog show progress with it.
namespace overdub::ui
{
class ThinProgressBar final : public juce::Component
{
public:
    // Clamped to 0..1; repaints only when it changes.
    void setFraction (double fraction);
    double fraction() const { return value; }
    void paint (juce::Graphics& g) override;

private:
    double value = 0.0;
};
} // namespace overdub::ui
