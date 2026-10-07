#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace overdub::app
{
// The one top-level window. Closing it quits the app.
class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow (const juce::String& title);

    void closeButtonPressed() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace overdub::app
