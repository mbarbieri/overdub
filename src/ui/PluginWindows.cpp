#include "ui/PluginWindows.h"

#include "core/Log.h"
#include "ui/Colours.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
juce::String fromUtf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}
} // namespace

std::optional<PluginWindowState> pluginWindowStateFor (const song::Song& song, const song::Id& slotId, std::string badge, bool editable)
{
    const auto stateIn = [&] (const std::vector<song::InsertSlot>& chain, const std::string& owner) -> std::optional<PluginWindowState> {
        const auto slot = std::ranges::find (chain, slotId, &song::InsertSlot::id);
        if (slot == chain.end())
            return std::nullopt;
        return PluginWindowState { .slotId = slotId, .title = owner + " \xE2\x80\x94 " + slot->name, .badge = std::move (badge), .bypassed = slot->bypassed, .editable = editable };
    };
    for (const auto& track : song.tracks)
        if (auto state = stateIn (track.insertChain, track.name))
            return state;
    return stateIn (song.master.insertChain, "Master");
}

PluginEditorPanel::PluginEditorPanel (juce::AudioProcessor& processor, const PluginWindowState& state)
{
    badgeLabel.setColour (juce::Label::textColourId, colours::dimText);
    badgeLabel.setFont (juce::FontOptions (12.0f));
    bypassButton.setClickingTogglesState (false);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, colours::mute);
    bypassButton.setWantsKeyboardFocus (false);
    bypassButton.onClick = [this] {
        if (current.editable && onBypass)
            onBypass (! current.bypassed);
    };
    // Where the plugin runs does not change while the window is open.
    badgeLabel.setText (fromUtf8 (state.badge), juce::dontSendNotification);
    addAndMakeVisible (badgeLabel);
    addAndMakeVisible (bypassButton);

    editor.reset (processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr);
    if (editor == nullptr)
    {
        editor = std::make_unique<juce::GenericAudioProcessorEditor> (processor);
        usesGenericEditor = true;
    }
    addAndMakeVisible (*editor);
    editor->addComponentListener (this);

    show (state);
    fitToEditor();
}

PluginEditorPanel::~PluginEditorPanel()
{
    if (editor != nullptr)
        editor->removeComponentListener (this);
    // The editor goes before the processor it edits (the window holds the processor).
    editor.reset();
}

void PluginEditorPanel::show (const PluginWindowState& state)
{
    current = state;
    bypassButton.setToggleState (state.bypassed, juce::dontSendNotification);
    bypassButton.setEnabled (state.editable);
}

void PluginEditorPanel::fitToEditor()
{
    setSize (std::max (minimumWidth, editor->getWidth()), headerHeight + editor->getHeight());
}

void PluginEditorPanel::componentMovedOrResized (juce::Component& component, bool, bool wasResized)
{
    if (&component != editor.get() || ! wasResized)
        return;
    fitToEditor();
    if (onEditorResized)
        onEditorResized (editor->getWidth(), editor->getHeight());
}

void PluginEditorPanel::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (8, 4);
    bypassButton.setBounds (header.removeFromRight (70));
    header.removeFromRight (6);
    badgeLabel.setBounds (header);
    // A resizable editor follows the window; a fixed one keeps its size at the top left.
    if (editor->isResizable())
        editor->setBounds (area);
    else
        editor->setTopLeftPosition (area.getTopLeft());
}

void PluginEditorPanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::panel);
    g.fillRect (getLocalBounds().removeFromTop (headerHeight));
}

//==============================================================================

PluginWindow::PluginWindow (std::shared_ptr<juce::AudioProcessor> processor, const PluginWindowState& state, bool onDesktop)
    : DocumentWindow (fromUtf8 (state.title), colours::background, juce::DocumentWindow::closeButton, false),
      plugin (std::move (processor)),
      slotId (state.slotId)
{
    // ADR 0002: the native title bar first, then the editor; it is never changed after.
    setUsingNativeTitleBar (true);
    auto panel = std::make_unique<PluginEditorPanel> (*plugin, state);
    editorPanel = panel.get();
    editorPanel->onEditorResized = [this] (int width, int height) {
        core::logEvent (core::Level::info,
                        "ui.plugin_editor_resized",
                        { { "slot_id", slotId }, { "width", width }, { "height", height }, { "native_title_bar", isUsingNativeTitleBar() } });
    };
    // Resizing to fit: the window follows the panel, which follows the editor.
    setContentOwned (panel.release(), true);
    setResizable (true, false);
    if (onDesktop)
    {
        centreWithSize (getWidth(), getHeight());
        addToDesktop (getDesktopWindowStyleFlags());
        setVisible (true);
        toFront (true);
    }
}

PluginWindow::~PluginWindow()
{
    editorPanel = nullptr;
    clearContentComponent(); // the editor, before `plugin` is released
}

void PluginWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

//==============================================================================

void PluginWindows::open (std::shared_ptr<juce::AudioProcessor> processor, const PluginWindowState& state)
{
    if (auto* window = find (state.slotId))
    {
        if (window->isOnDesktop())
            window->toFront (true);
        return;
    }
    if (processor == nullptr)
        return;

    auto window = std::make_unique<PluginWindow> (std::move (processor), state, showOnDesktop);
    window->panel().onBypass = [this, slotId = state.slotId] (bool bypassed) {
        if (onBypass)
            onBypass (slotId, bypassed);
    };
    // Closed from its own close button: deleted after that callback has returned.
    window->onClose = [this, slotId = state.slotId, alive = std::weak_ptr<int> (lifetime)] {
        juce::MessageManager::callAsync ([this, slotId, alive] {
            if (alive.expired())
                return;
            close (slotId);
            if (onClosed)
                onClosed (slotId);
        });
    };
    auto& panel = window->panel();
    core::logEvent (core::Level::info,
                    "ui.plugin_window_opened",
                    { { "slot_id", state.slotId },
                      { "title", state.title },
                      { "badge", state.badge },
                      { "editor", panel.usesGenericEditor ? "generic" : "own" },
                      { "width", panel.editor->getWidth() },
                      { "height", panel.editor->getHeight() },
                      { "native_title_bar", window->isUsingNativeTitleBar() } });
    windows[state.slotId] = std::move (window);
}

void PluginWindows::close (const song::Id& slotId)
{
    const auto found = windows.find (slotId);
    if (found == windows.end())
        return;
    windows.erase (found);
    core::logEvent (core::Level::info, "ui.plugin_window_closed", { { "slot_id", slotId } });
}

void PluginWindows::closeAll()
{
    while (! windows.empty())
    {
        const auto slotId = windows.begin()->first; // a copy: close erases the key it refers to
        close (slotId);
    }
}

void PluginWindows::sync (const std::function<std::optional<PluginWindowState> (const song::Id&)>& stateOf,
                          const std::function<juce::AudioProcessor*(const song::Id&)>& processorOf)
{
    std::vector<song::Id> gone;
    for (auto& [slotId, window] : windows)
    {
        const auto state = stateOf (slotId);
        if (! state.has_value() || processorOf (slotId) != window->processor().get())
        {
            gone.push_back (slotId);
            continue;
        }
        window->setName (fromUtf8 (state->title));
        window->panel().show (*state);
    }
    for (const auto& slotId : gone)
        close (slotId);
}

PluginWindow* PluginWindows::find (const song::Id& slotId) const
{
    const auto found = windows.find (slotId);
    return found != windows.end() ? found->second.get() : nullptr;
}
} // namespace overdub::ui
