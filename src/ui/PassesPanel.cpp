#include "ui/PassesPanel.h"

#include "song/TakeEdits.h"
#include "ui/Colours.h"
#include "ui/Layout.h"

namespace overdub::ui
{
PassesPanel::Row::Row (PassesPanel& owner) : panel (owner)
{
    for (auto* label : { &title, &status })
    {
        label->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
    }
    title.setColour (juce::Label::textColourId, colours::text);
    title.setFont (juce::FontOptions (12.0f));
    status.setFont (juce::FontOptions (11.5f));
    status.setJustificationType (juce::Justification::centredRight);
    bringBackButton.setButtonText (juce::String::fromUTF8 ("\xE2\x86\xBA Bring back"));
    bringBackButton.setWantsKeyboardFocus (false);
    bringBackButton.onClick = [this] {
        if (panel.onBringBack)
            panel.onBringBack (passId);
    };
    addAndMakeVisible (bringBackButton);
}

void PassesPanel::Row::resized()
{
    const auto layout = passRowLayout (getLocalBounds());
    title.setBounds (layout.title);
    status.setBounds (layout.status);
    bringBackButton.setBounds (layout.bringBack);
}

void PassesPanel::Row::mouseUp (const juce::MouseEvent&)
{
    if (panel.onSelect)
        panel.onSelect (passId);
}

//==============================================================================

PassesPanel::PassesPanel (std::string heading_) : headingWithRows (std::move (heading_))
{
    heading.setColour (juce::Label::textColourId, colours::dimText);
    heading.setFont (juce::FontOptions (11.5f));
    heading.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (heading);
}

void PassesPanel::show (const std::vector<PassRow>& rows, bool editable)
{
    if (rowComponents.size() != rows.size())
    {
        rowComponents.clear();
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            auto row = std::make_unique<Row> (*this);
            addAndMakeVisible (*row);
            rowComponents.push_back (std::move (row));
        }
        resized();
    }

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = *rowComponents[i];
        row.passId = rows[i].passId;
        row.title.setText (juce::String::fromUTF8 (rows[i].title.c_str()), juce::dontSendNotification);
        row.status.setText (juce::String (std::string (song::toString (rows[i].status))), juce::dontSendNotification);
        row.status.setColour (juce::Label::textColourId, rows[i].status == song::HeardStatus::notHeard ? colours::dimText : colours::text);
        row.bringBackButton.setEnabled (editable && rows[i].canBringBack);
        row.setMouseCursor (onSelect ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    heading.setText (juce::String::fromUTF8 (rows.empty() ? "No Passes yet: press R to record one." : headingWithRows.c_str()), juce::dontSendNotification);
    repaint();
}

void PassesPanel::resized()
{
    const auto layout = passesPanelLayout (getLocalBounds(), numRows());
    heading.setBounds (layout.heading);
    for (std::size_t i = 0; i < rowComponents.size() && i < layout.rows.size(); ++i)
        rowComponents[i]->setBounds (layout.rows[i]);
}

void PassesPanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel.brighter (0.05f));
    g.setColour (colours::line.brighter (0.3f));
    g.drawRect (getLocalBounds());
}
} // namespace overdub::ui
