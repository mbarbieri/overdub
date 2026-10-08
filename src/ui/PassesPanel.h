#pragma once

#include "song/Song.h"
#include "ui/TakeLanes.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// The Passes list (issue #30, issue #32): a heading, then one row per Pass, newest first
// (ui/TakeLanes.h passRows), each with its title, heard status and a "↺ Bring back" button. Edit
// shows it as a panel under "Passes ▾"; Record mode in its right column, where clicking a row
// (outside its button) jumps to the Pass's start. Thin: it reports gestures by Pass id.
namespace overdub::ui
{
class PassesPanel final : public juce::Component
{
public:
    // `heading` is shown above the rows when there are any.
    explicit PassesPanel (std::string heading = "Passes \xC2\xB7 newest first. \xE2\x86\xBA Bring back puts a whole Pass back on top, as recorded.");

    // Bring back is enabled only when `editable` and the row can be brought back.
    void show (const std::vector<PassRow>& rows, bool editable);
    void resized() override;
    void paint (juce::Graphics& g) override;

    int numRows() const { return (int) rowComponents.size(); }

    std::function<void (const song::Id& passId)> onBringBack;
    // A click on a row outside its button; rows ignore clicks while this is unset.
    std::function<void (const song::Id& passId)> onSelect;

    class Row final : public juce::Component
    {
    public:
        explicit Row (PassesPanel& owner);

        void resized() override;
        void mouseUp (const juce::MouseEvent& event) override;

        song::Id passId;
        juce::Label title;
        juce::Label status;
        juce::TextButton bringBackButton;

    private:
        PassesPanel& panel;
    };

    juce::Label heading;
    std::vector<std::unique_ptr<Row>> rowComponents;

private:
    std::string headingWithRows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PassesPanel)
};
} // namespace overdub::ui
