#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

// The UI test harness (ADR 0005 UI amendment, issue #27). Components are built for real under
// ScopedJuceInitialiser_GUI at a fixed size, with no window and so no on-screen peer.
//
// Events, as confirmed in issue #27:
// - Mouse: a synthesized juce::MouseEvent (from the desktop's main mouse source) is delivered by
//   calling the target's mouseDown / mouseUp handlers. JUCE's own routing goes through a peer
//   (ComponentPeer::handleMouseEvent), which an off-screen Component does not have; the handlers
//   themselves work without one, and a Button clicked this way fires onClick as it would live.
// - Keys: a juce::KeyPress delivered to the Component's keyPressed handler.
// - ComboBox: choosing an item calls setSelectedItemIndex with a synchronous notification,
//   because its popup menu needs a peer to open.
// Snapshots: createComponentSnapshot renders without a peer; PNGs go to
// build/ui-snapshots/<slice>/ for the slice's reviewer to look at, and nothing asserts on pixels.
namespace overdub::ui::testing
{
// Sizes the Component and makes it visible (its own flag; there is no window).
inline void layOut (juce::Component& component, int width, int height)
{
    component.setBounds (0, 0, width, height);
    component.setVisible (true);
}

inline juce::MouseEvent mouseEventAt (juce::Component& target, juce::Point<float> position, juce::ModifierKeys modifiers, std::optional<juce::Point<float>> mouseDownPosition = std::nullopt)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                             position,
                             modifiers,
                             juce::MouseInputSource::defaultPressure,
                             juce::MouseInputSource::defaultOrientation,
                             juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX,
                             juce::MouseInputSource::defaultTiltY,
                             &target,
                             &target,
                             now,
                             mouseDownPosition.value_or (position),
                             now,
                             1,
                             false);
}

// A left click at the Component's centre: mouseDown, then mouseUp.
inline void click (juce::Component& target)
{
    REQUIRE (target.isEnabled());
    const auto centre = target.getLocalBounds().getCentre().toFloat();
    target.mouseDown (mouseEventAt (target, centre, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
    target.mouseUp (mouseEventAt (target, centre, juce::ModifierKeys()));
}

// A left-button drag in the Component's coordinates: mouseDown at `from`, one mouseDrag and the
// mouseUp at `to`.
inline void drag (juce::Component& target, juce::Point<int> from, juce::Point<int> to)
{
    const auto down = from.toFloat();
    const auto up = to.toFloat();
    const juce::ModifierKeys leftButton (juce::ModifierKeys::leftButtonModifier);
    target.mouseDown (mouseEventAt (target, down, leftButton));
    target.mouseDrag (mouseEventAt (target, up, leftButton, down));
    target.mouseUp (mouseEventAt (target, up, juce::ModifierKeys(), down));
}

// A left click at a point in the Component's coordinates.
inline void clickAt (juce::Component& target, juce::Point<int> position)
{
    drag (target, position, position);
}

// A double-click at a point, as JUCE delivers it: down, up, down, up, then mouseDoubleClick.
inline void doubleClickAt (juce::Component& target, juce::Point<int> position)
{
    clickAt (target, position);
    clickAt (target, position);
    target.mouseDoubleClick (mouseEventAt (target, position.toFloat(), juce::ModifierKeys()));
}

// Delivers a key press; returns whether the Component used it.
inline bool pressKey (juce::Component& target, const juce::KeyPress& key)
{
    return target.keyPressed (key);
}

inline bool pressKey (juce::Component& target, juce::juce_wchar character, juce::ModifierKeys modifiers = {})
{
    return pressKey (target, juce::KeyPress ((int) character, modifiers, character));
}

// Picks a ComboBox item by index, notifying synchronously as a user's pick would.
inline void choose (juce::ComboBox& comboBox, int itemIndex)
{
    REQUIRE (comboBox.isEnabled());
    REQUIRE (itemIndex < comboBox.getNumItems());
    comboBox.setSelectedItemIndex (itemIndex, juce::sendNotificationSync);
}

// True when the Component and every parent up to the top are visible.
inline bool isShown (const juce::Component& component)
{
    for (auto* c = &component; c != nullptr; c = c->getParentComponent())
        if (! c->isVisible())
            return false;
    return true;
}

// Renders the Component and writes <OVERDUB_UI_SNAPSHOT_DIR>/<slice>/<name>.png. Returns the file.
inline juce::File writeSnapshot (juce::Component& component, const std::string& slice, const std::string& name)
{
    const auto folder = juce::File (OVERDUB_UI_SNAPSHOT_DIR).getChildFile (slice);
    REQUIRE (folder.createDirectory().wasOk());
    const auto file = folder.getChildFile (name + ".png");
    file.deleteFile();

    const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 2.0f);
    juce::FileOutputStream stream (file);
    REQUIRE (stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE (png.writeImageToStream (image, stream));
    return file;
}
} // namespace overdub::ui::testing
