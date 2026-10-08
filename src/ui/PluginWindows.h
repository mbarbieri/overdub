#pragma once

#include "song/Song.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Plugin editor windows (issue #34, ADR 0002): one floating, resizable DocumentWindow per Insert
// Chain slot, titled "Guitar — AUDelay". Under the title bar a strip shows where the plugin runs
// (plugin::hostingBadge, "AUv2 · out-of-process") and a bypass toggle; under it, the plugin's own
// editor, or JUCE's generic editor when it has none. Parameters moved in an editor are outside
// history: the InsertHost hears them and OpenSong schedules the song.json write.
//
// ADR 0002: AU editors arrive as remote views that open at a placeholder size and resize once
// attached, so the window follows its editor's size (logs ui.plugin_editor_resized), and the
// native title bar is set before the editor is attached (changing it later re-parents the
// plugin's NSView). A window keeps its processor alive (shared) until it closes, so an editor
// never outlives its plugin.
//
// Message thread only.
namespace overdub::ui
{
struct PluginWindowState
{
    song::Id slotId;
    std::string title;      // "Guitar — AUDelay"
    std::string badge;      // plugin::hostingBadge
    bool bypassed = false;
    bool editable = true;   // false while a Pass runs: the bypass toggle does nothing
};

// The window for a slot of the Song: titled after its chain's owner and plugin ("Guitar — AUDelay",
// "Master — AUPeakLimiter"), with its bypass; none when the Song has no such slot.
std::optional<PluginWindowState> pluginWindowStateFor (const song::Song& song, const song::Id& slotId, std::string badge, bool editable);

// The window's content: the badge strip over the editor.
class PluginEditorPanel final : public juce::Component, private juce::ComponentListener
{
public:
    static constexpr int headerHeight = 30;
    static constexpr int minimumWidth = 300;

    PluginEditorPanel (juce::AudioProcessor& processor, const PluginWindowState& state);
    ~PluginEditorPanel() override;

    void show (const PluginWindowState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void (bool bypassed)> onBypass;
    // The editor took a new size (logged by the window).
    std::function<void (int width, int height)> onEditorResized;

    juce::Label badgeLabel;
    juce::TextButton bypassButton { "Bypass" };
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    bool usesGenericEditor = false;

private:
    void componentMovedOrResized (juce::Component& component, bool wasMoved, bool wasResized) override;
    void fitToEditor();

    PluginWindowState current;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditorPanel)
};

class PluginWindow final : public juce::DocumentWindow
{
public:
    // onDesktop false keeps the window off-screen (tests): it is built and laid out, never shown.
    PluginWindow (std::shared_ptr<juce::AudioProcessor> processor, const PluginWindowState& state, bool onDesktop);
    ~PluginWindow() override;

    void closeButtonPressed() override;

    PluginEditorPanel& panel() { return *editorPanel; }
    const std::shared_ptr<juce::AudioProcessor>& processor() const { return plugin; }

    std::function<void()> onClose;

private:
    std::shared_ptr<juce::AudioProcessor> plugin;
    PluginEditorPanel* editorPanel = nullptr; // owned as the content component
    song::Id slotId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

// The open editor windows, at most one per slot.
class PluginWindows
{
public:
    explicit PluginWindows (bool onDesktop = true) : showOnDesktop (onDesktop) {}
    ~PluginWindows() { closeAll(); }

    PluginWindows (const PluginWindows&) = delete;
    PluginWindows& operator= (const PluginWindows&) = delete;

    // Opens the slot's window, or brings it to the front when it is open. Logs
    // ui.plugin_window_opened (info: slot_id, title, badge, editor ("own" or "generic"), width,
    // height, native_title_bar).
    void open (std::shared_ptr<juce::AudioProcessor> processor, const PluginWindowState& state);
    // Closes it (logs ui.plugin_window_closed, info: slot_id); nothing when it is not open.
    void close (const song::Id& slotId);
    void closeAll();

    // Updates every open window from its state (title, bypass, editable); closes a window whose
    // slot has no state (gone from the Song) or whose processor is no longer the slot's (null:
    // not ready). `stateOf` and `processorOf` answer by slot id.
    void sync (const std::function<std::optional<PluginWindowState> (const song::Id&)>& stateOf,
               const std::function<juce::AudioProcessor* (const song::Id&)>& processorOf);

    PluginWindow* find (const song::Id& slotId) const;
    std::size_t size() const { return windows.size(); }

    // The bypass toggle in a window.
    std::function<void (const song::Id& slotId, bool bypassed)> onBypass;
    // A window was closed by its close button.
    std::function<void (const song::Id& slotId)> onClosed;

    // False in tests: windows are built but never put on the desktop.
    bool showOnDesktop = true;

private:
    std::map<song::Id, std::unique_ptr<PluginWindow>> windows;
    // Expires with this object, so a close posted by a window's close button is dropped.
    std::shared_ptr<int> lifetime = std::make_shared<int> (0);
};
} // namespace overdub::ui
