#pragma once

#include "plugin/InsertHost.h"
#include "song/Song.h"
#include "ui/Intent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// An Insert Chain as the Record rows, the Mixer strips and the Master strip show it (issue #34,
// prototype/main-window): "+ Insert" and one chip per slot. A chip shows the plugin's name (and
// whether it is loading or missing), a bypass toggle and a remove button; clicking its name opens
// the plugin's editor, dragging it along the chain reorders it (ui/Layout.h insertDropIndex).
// Thin: every gesture is an Intent reported through onIntent: open_insert_editor, and
// set_insert_bypassed, remove_insert and move_insert, one undo step each. "+ Insert" is the one
// hook, onAddInsert: the window asks for an AU effect, then runs add_insert. While a Pass runs (not
// editable) only opening an editor acts.
namespace overdub::ui
{
using InsertStatusLookup = std::function<std::optional<plugin::InsertStatus> (const song::Id& slotId)>;

struct InsertChipState
{
    song::Id slotId;
    std::string text; // insertChipText
    bool bypassed = false;
    bool ready = true; // false while loading, missing or crashed: drawn dimmed

    bool operator== (const InsertChipState&) const = default;
};

// The chip's text: the plugin's name, with " (loading)", " (missing)" or " (crashed)" when it is
// not ready.
// Without a status lookup every slot reads as ready.
std::string insertChipText (const song::InsertSlot& slot, const InsertStatusLookup& statusOf);

std::vector<InsertChipState> insertChipsFor (const std::vector<song::InsertSlot>& chain, const InsertStatusLookup& statusOf);

// The Inserts that process the signal: ready and not bypassed. Without a status lookup, every
// slot that is not bypassed.
int activeInsertCount (const std::vector<song::InsertSlot>& chain, const InsertStatusLookup& statusOf);

class InsertChip final : public juce::Component
{
public:
    InsertChip();

    void show (const InsertChipState& state, bool editable);
    const song::Id& slotId() const { return current.slotId; }
    const InsertChipState& state() const { return current; }
    juce::String text() const { return juce::String::fromUTF8 (current.text.c_str()); }

    // A click on the name (moved less than a few points).
    std::function<void()> onOpen;
    // A drag that ended at this point, in the chip's coordinates.
    std::function<void (juce::Point<int> end)> onDragEnd;
    std::function<void (bool bypassed)> onBypass;
    std::function<void()> onRemove;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void resized() override;
    void paint (juce::Graphics& g) override;

    juce::TextButton bypassButton;
    juce::TextButton removeButton;

private:
    InsertChipState current;
    bool editable = true;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertChip)
};

class InsertChainView final : public juce::Component
{
public:
    explicit InsertChainView (bool vertical);

    // Shows one chip per state, in order. Chips are made as needed and kept (hidden) when the
    // chain shrinks, so none is deleted during its own gesture.
    void show (const song::Id& trackId, const std::vector<InsertChipState>& chips, bool editable);
    void resized() override;

    // Set by the owner (RecordPage, Mixer): where the chain's Intents go, and "+ Insert" with this
    // chain's Track id (empty for the Master's chain).
    IntentSink onIntent;
    std::function<void (const song::Id& trackId)> onAddInsert;

    // The first numShown() are the chain's, in order; any after them are hidden spares.
    std::vector<std::unique_ptr<InsertChip>> chips;
    std::size_t numShown() const { return shown; }
    juce::TextButton addButton { "+ Insert" };

private:
    void dropped (std::size_t index, juce::Point<int> pointInChain);
    void emit (Intent intent) const;

    bool vertical;
    song::Id trackId;
    bool editable = true;
    std::size_t shown = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertChainView)
};
} // namespace overdub::ui
