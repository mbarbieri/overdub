#pragma once

#include "song/Song.h"
#include "ui/Intent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Loops on screen (issue #228, ADR 0010): a LoopBar per Loop on the Loop/Marker strip
// (ui/MarkerStrip.h places them), and the Loop cards of Transcribe mode's left column.
//
// A bar is the Loop's range across the strip's height, its name inside. A click runs select_loop
// (the strip wires it). While its strip is editable, dragging either edge resizes the Loop on
// release (resize_loop) and double-clicking opens the strip's name field (rename_loop). A card
// shows the Loop's name, Practice Speed, range, length and its option-key number; a click runs
// select_loop.
namespace overdub::ui
{
// The text of a Loop's range ("1.5 s – 3.0 s") and length ("1.5 s").
std::string loopRangeText (const song::Loop& loop, int sampleRate);
std::string loopLengthText (const song::Loop& loop, int sampleRate);
// The Practice Speed it is drilled at ("40%", issue #230).
std::string loopSpeedText (const song::Loop& loop);
// "⌥1" .. "⌥9" for the first nine Loops in time order (index from 0); "" for the rest.
std::string loopKeyText (std::size_t index);

enum class LoopEdge
{
    start,
    end
};

class LoopBar final : public juce::Component
{
public:
    explicit LoopBar (song::Id loopId);

    const song::Id& loopId() const { return id; }
    const juce::String& name() const { return shownName; }
    bool isActive() const { return active; }
    bool isSelected() const { return selected; }
    void show (const song::Loop& loop, bool isActive, bool isSelected, bool isEditable);
    // The edge a press at local x grabs while editable; none: the body (a click).
    std::optional<LoopEdge> edgeAt (int x) const;
    void paint (juce::Graphics& g) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;

    // A click without a drag; an edge dragged by deltaX pixels in progress, and released.
    std::function<void()> onClick;
    std::function<void (LoopEdge edge, int deltaX)> onEdgeDrag;
    std::function<void (LoopEdge edge, int deltaX)> onEdgeDragEnd;
    std::function<void()> onDoubleClick;

private:
    song::Id id;
    juce::String shownName;
    bool active = false;
    bool selected = false;
    bool editable = false;
    std::optional<LoopEdge> grabbed;
    bool dragging = false;
};

// One card of the Loops list: name, Practice Speed, range, length and its option-key number.
class LoopCard final : public juce::Component
{
public:
    explicit LoopCard (song::Id loopId);

    const song::Id& loopId() const { return id; }
    bool isActive() const { return active; }
    const juce::String& keyText() const { return key; }
    const juce::String& speedText() const { return speed; }
    void show (const song::Loop& loop, int sampleRate, std::size_t index, bool isActive);
    void paint (juce::Graphics& g) override;
    void mouseUp (const juce::MouseEvent& event) override;

    std::function<void()> onClick;

private:
    song::Id id;
    juce::String name;
    juce::String range;
    juce::String length;
    juce::String speed;
    bool atNormalSpeed = true;
    juce::String key;
    bool active = false;
};

// Transcribe mode's Loops (issue #228): "LOOPS" over one LoopCard per Loop in time order, above
// the Markers, or a hint while the Song has none.
class LoopList final : public juce::Component
{
public:
    LoopList();

    void show (const std::vector<song::Loop>& loops, int sampleRate, std::optional<song::Id> active);
    void resized() override;
    void paint (juce::Graphics& g) override;

    IntentSink onIntent;

    juce::Label heading { {}, "LOOPS" };
    juce::Label hint;
    std::vector<std::unique_ptr<LoopCard>> cards;

private:
    void layOutCards();
};
} // namespace overdub::ui
