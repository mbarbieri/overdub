#pragma once

#include "ui/Colours.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// The M and S toggle buttons a Track shows in the Record rows, the Edit headers and the Mixer
// strips (issue #31): one look everywhere. Clicking one reports the opposite of what `isOn` says
// now; the owner sets the toggle state from the Song.
namespace overdub::ui
{
inline void setUpMuteSoloButtons (juce::TextButton& mute,
                                  juce::TextButton& solo,
                                  std::function<bool()> isMuted,
                                  std::function<bool()> isSoloed,
                                  std::function<void (bool)>& onMute,
                                  std::function<void (bool)>& onSolo)
{
    mute.setColour (juce::TextButton::buttonOnColourId, colours::mute);
    solo.setColour (juce::TextButton::buttonOnColourId, colours::solo);
    solo.setColour (juce::TextButton::textColourOnId, colours::background);
    mute.setWantsKeyboardFocus (false);
    solo.setWantsKeyboardFocus (false);
    // The callbacks are members of the buttons' owner, so the references outlive the buttons.
    mute.onClick = [muted = std::move (isMuted), &onMute] {
        if (onMute)
            onMute (! muted());
    };
    solo.onClick = [soloed = std::move (isSoloed), &onSolo] {
        if (onSolo)
            onSolo (! soloed());
    };
}
} // namespace overdub::ui
