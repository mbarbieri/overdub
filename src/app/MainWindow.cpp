#include "app/MainWindow.h"

#include "core/Log.h"
#include "ui/Layout.h"
#include "ui/SongFiles.h"

namespace overdub::app
{
namespace
{
enum MenuItem
{
    newSongItem = 1,
    openSongItem,
    duplicateSongItem,
    settingsItem,
    undoItem,
    redoItem
};

juce::File defaultSongFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

// The settings window: the device picker, hidden (not destroyed) when closed.
class SettingsWindow final : public juce::DocumentWindow
{
public:
    explicit SettingsWindow (std::unique_ptr<ui::DeviceSettingsPanel> panel)
        : DocumentWindow ("Settings", juce::Colours::black, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel.release(), false);
        centreWithSize (460, 240);
    }

    void closeButtonPressed() override { setVisible (false); }
};

juce::PopupMenu::Item historyItem (int id, const juce::String& verb, bool enabled, const std::string& label, const char* shortcut)
{
    juce::PopupMenu::Item item { enabled ? verb + " " + juce::String::fromUTF8 (label.c_str()) : verb };
    item.setID (id).setEnabled (enabled);
    item.shortcutKeyDescription = juce::String (juce::CharPointer_UTF8 (shortcut));
    return item;
}
} // namespace

MainWindow::MainWindow (const juce::String& name, std::unique_ptr<LiveEngine> liveEngine)
    : DocumentWindow (name,
                      juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      juce::DocumentWindow::allButtons),
      appName (name),
      live (std::move (liveEngine))
{
    content.onNewSong = [this] { newSong(); };
    content.onOpenSong = [this] { chooseSongToOpen(); };
    // Discovery instantiates nothing and is quick; it runs on each "+ Insert" so new installs show.
    content.listInsertPlugins = [this] { return pluginLoader.findEffects(); };

    setUsingNativeTitleBar (true);
    setContentNonOwned (&content, false);
    setResizable (true, true);
    setResizeLimits (ui::minimumWindowWidth, ui::minimumWindowHeight, 10000, 10000);
    centreWithSize (1200, 800);

    juce::PopupMenu appleMenuExtras;
    appleMenuExtras.addItem (settingsItem, "Settings...");
    juce::MenuBarModel::setMacMainMenu (this, &appleMenuExtras);

    setVisible (true);
    content.grabKeyboardFocus();
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
}

MainWindow::~MainWindow()
{
    juce::MenuBarModel::setMacMainMenu (nullptr);
    settingsPanel = nullptr;
    settingsWindow.reset();
    closeSong();
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

bool MainWindow::passRunning() const
{
    return openSong != nullptr && openSong->isRecording();
}

bool MainWindow::idleOrNotice()
{
    if (! passRunning())
        return true;
    content.showNotice ("ui.stop_recording_first");
    return false;
}

//==============================================================================
// Menus

juce::StringArray MainWindow::getMenuBarNames()
{
    return { "File", "Edit" };
}

juce::PopupMenu MainWindow::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    const auto idle = ! passRunning();
    if (topLevelMenuIndex == 0)
    {
        menu.addItem (newSongItem, "New Song...", idle);
        menu.addItem (openSongItem, "Open...", idle);
        menu.addItem (duplicateSongItem, "Duplicate...", idle && openSong != nullptr);
        menu.addSeparator();
        menu.addItem (settingsItem, "Settings...", idle);
        return menu;
    }

    if (openSong == nullptr)
    {
        menu.addItem (historyItem (undoItem, "Undo", false, {}, "\xe2\x8c\x98Z"));
        menu.addItem (historyItem (redoItem, "Redo", false, {}, "\xe2\x87\xa7\xe2\x8c\x98Z"));
        return menu;
    }
    const auto& package = openSong->package();
    menu.addItem (historyItem (undoItem, "Undo", idle && package.canUndo(), package.undoLabel(), "\xe2\x8c\x98Z"));
    menu.addItem (historyItem (redoItem, "Redo", idle && package.canRedo(), package.redoLabel(), "\xe2\x87\xa7\xe2\x8c\x98Z"));
    return menu;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    switch (menuItemID)
    {
        case newSongItem: newSong(); break;
        case openSongItem: chooseSongToOpen(); break;
        case duplicateSongItem: duplicateSong(); break;
        case settingsItem: showSettings(); break;
        case undoItem: content.perform (ui::Command::undo); break;
        case redoItem: content.perform (ui::Command::redo); break;
        default: break;
    }
}

//==============================================================================
// Songs. Each dialog's callback checks again: a Pass may have started while it was open.

void MainWindow::newSong()
{
    if (! idleOrNotice())
        return;
    chooser = std::make_unique<juce::FileChooser> ("New Song", defaultSongFolder().getChildFile ("Untitled.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || ! idleOrNotice())
            return;
        auto created = ui::createSongAt (chosen, live->sampleRate());
        if (! created.has_value())
            return content.handleError (created.error());
        showSong (std::move (*created), {});
    });
}

void MainWindow::chooseSongToOpen()
{
    if (! idleOrNotice())
        return;
    chooser = std::make_unique<juce::FileChooser> ("Open Song", defaultSongFolder(), "*.overdub");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || ! idleOrNotice())
            return;
        openSongFrom (chosen);
    });
}

void MainWindow::duplicateSong()
{
    if (openSong == nullptr || ! idleOrNotice())
        return;
    const auto current = openSong->package().location();
    chooser = std::make_unique<juce::FileChooser> ("Duplicate Song", current.getSiblingFile (current.getFileNameWithoutExtension() + " copy.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || openSong == nullptr || ! idleOrNotice())
            return;
        auto duplicated = ui::duplicateSongTo (openSong->package(), chosen);
        if (! duplicated.has_value())
            return content.handleError (duplicated.error());
        // The duplicate becomes the open Song; the original stays as it was.
        openSongFrom (*duplicated);
    });
}

void MainWindow::openSongFrom (const juce::File& chosen)
{
    // A package is never open twice: opening the open Song again changes nothing.
    if (openSong != nullptr && openSong->package().location() == ui::packageLocationOpened (chosen))
        return;
    auto opened = ui::openSongAt (chosen);
    if (! opened.has_value())
        return content.handleError (opened.error());
    showSong (std::move (opened->package), opened->notices);
}

void MainWindow::showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices)
{
    closeSong();
    openSong = std::make_unique<ui::OpenSong> (std::move (package), live->audioBinding(), pluginLoader.instantiator());
    content.setOpenSong (openSong.get());
    for (const auto& notice : notices)
        content.showNotice (notice.code);
    updateTitle();
    content.grabKeyboardFocus();
}

void MainWindow::detachAudio()
{
    if (openSong == nullptr)
        return;
    for (const auto& error : openSong->detachAudio())
        content.handleError (error);
}

void MainWindow::closeSong()
{
    detachAudio();
    content.setOpenSong (nullptr);
    openSong.reset();
    updateTitle();
}

void MainWindow::updateTitle()
{
    if (openSong == nullptr)
        setName (appName);
    else
        setName (appName + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + openSong->package().location().getFileNameWithoutExtension());
}

//==============================================================================
// Settings

void MainWindow::showSettings()
{
    if (! idleOrNotice())
        return;
    if (settingsWindow == nullptr)
    {
        auto panel = std::make_unique<ui::DeviceSettingsPanel>();
        panel->onApply = [this] (const engine::DeviceSettings& settings) { applyDeviceSettings (settings); };
        settingsPanel = panel.get();
        settingsWindow = std::make_unique<SettingsWindow> (std::move (panel));
    }
    settingsPanel->show (live->deviceChoices());
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

void MainWindow::applyDeviceSettings (const engine::DeviceSettings& settings)
{
    if (! idleOrNotice())
        return;
    core::logEvent (core::Level::info, "app.device_choice_changed", engine::deviceSettingsFields (settings));

    // The engine's shape is fixed per device (engine/Engine.h), so a new choice builds a new
    // LiveEngine, which opens the saved choice (or falls back to the default and logs why).
    detachAudio();
    live.reset();
    if (auto written = engine::writeDeviceSettings (engine::defaultDeviceSettingsFile(), settings); ! written.has_value())
        content.handleError (written.error());
    live = std::make_unique<LiveEngine>();
    if (openSong != nullptr)
        openSong->attachAudio (live->audioBinding());
    content.refresh();

    if (settingsPanel != nullptr)
        settingsPanel->show (live->deviceChoices());
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
}
} // namespace overdub::app
