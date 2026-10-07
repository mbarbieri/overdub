#include "app/MainWindow.h"

#include "core/Log.h"
#include "ui/Layout.h"

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

constexpr int fallbackSampleRate = 48000;

juce::File defaultSongFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

// A chosen package path always ends in .overdub.
juce::File asPackage (const juce::File& chosen)
{
    return chosen.hasFileExtension ("overdub") ? chosen : chosen.withFileExtension ("overdub");
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
} // namespace

MainWindow::MainWindow (const juce::String& name, std::unique_ptr<LiveEngine> liveEngine)
    : DocumentWindow (name,
                      juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      juce::DocumentWindow::allButtons),
      appName (name),
      live (std::move (liveEngine))
{
    content.onNewSong = [this] { newSong(); };
    content.onOpenSong = [this] { openSong(); };

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
}

MainWindow::~MainWindow()
{
    juce::MenuBarModel::setMacMainMenu (nullptr);
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
    return session != nullptr && session->isRecording();
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
        menu.addItem (duplicateSongItem, "Duplicate...", idle && session != nullptr);
        menu.addSeparator();
        menu.addItem (settingsItem, "Settings...", idle);
    }
    else
    {
        const auto canUndo = idle && session != nullptr && session->package().canUndo();
        const auto canRedo = idle && session != nullptr && session->package().canRedo();
        juce::PopupMenu::Item undo { canUndo ? "Undo " + juce::String (session->package().undoLabel()) : juce::String ("Undo") };
        undo.setID (undoItem).setEnabled (canUndo);
        undo.shortcutKeyDescription = juce::String (juce::CharPointer_UTF8 ("\xe2\x8c\x98Z"));
        juce::PopupMenu::Item redo { canRedo ? "Redo " + juce::String (session->package().redoLabel()) : juce::String ("Redo") };
        redo.setID (redoItem).setEnabled (canRedo);
        redo.shortcutKeyDescription = juce::String (juce::CharPointer_UTF8 ("\xe2\x87\xa7\xe2\x8c\x98Z"));
        menu.addItem (undo);
        menu.addItem (redo);
    }
    return menu;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    switch (menuItemID)
    {
        case newSongItem: newSong(); break;
        case openSongItem: openSong(); break;
        case duplicateSongItem: duplicateSong(); break;
        case settingsItem: showSettings(); break;
        case undoItem: content.perform (ui::Command::undo); break;
        case redoItem: content.perform (ui::Command::redo); break;
        default: break;
    }
}

//==============================================================================
// Songs

void MainWindow::newSong()
{
    if (passRunning())
        return;
    chooser = std::make_unique<juce::FileChooser> ("New Song", defaultSongFolder().getChildFile ("Untitled.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File())
            return;
        const auto sampleRate = live->sampleRate() > 0 ? live->sampleRate() : fallbackSampleRate;
        auto created = song::SongPackage::create (asPackage (chosen), song::makeEmptySong (sampleRate));
        if (! created.has_value())
            return content.handleError (created.error());
        showSong (std::move (*created), {});
    });
}

void MainWindow::openSong()
{
    if (passRunning())
        return;
    chooser = std::make_unique<juce::FileChooser> ("Open Song", defaultSongFolder(), "*.overdub");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this] (const juce::FileChooser& finished) {
        auto chosen = finished.getResult();
        if (chosen == juce::File())
            return;
        if (chosen.existsAsFile()) // song.json (or another file) inside the package
            chosen = chosen.getParentDirectory();
        openSongAt (chosen);
    });
}

void MainWindow::duplicateSong()
{
    if (passRunning() || session == nullptr)
        return;
    const auto current = session->package().location();
    chooser = std::make_unique<juce::FileChooser> ("Duplicate Song", current.getSiblingFile (current.getFileNameWithoutExtension() + " copy.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || session == nullptr)
            return;
        const auto destination = asPackage (chosen);
        if (auto duplicated = session->package().duplicateTo (destination); ! duplicated.has_value())
            return content.handleError (duplicated.error());
        // The duplicate becomes the open Song; the original stays as it was.
        openSongAt (destination);
    });
}

void MainWindow::openSongAt (const juce::File& location)
{
    // A package is never open twice: opening the open Song again changes nothing.
    if (session != nullptr && session->package().location() == location)
        return;
    auto opened = song::SongPackage::open (location);
    if (! opened.has_value())
        return content.handleError (opened.error());
    showSong (std::move (opened->package), opened->notices);
}

void MainWindow::showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices)
{
    closeSong();
    session = std::make_unique<ui::SongSession> (std::move (package), live->audioBinding());
    content.setSession (session.get());
    for (const auto& notice : notices)
        content.showNotice (notice.code);
    updateTitle();
    content.grabKeyboardFocus();
}

void MainWindow::closeSong()
{
    content.setSession (nullptr);
    if (session == nullptr)
        return;
    for (const auto& error : session->detachAudio())
        content.handleError (error);
    session.reset();
    updateTitle();
}

void MainWindow::updateTitle()
{
    if (session == nullptr)
        setName (appName);
    else
        setName (appName + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + session->package().location().getFileNameWithoutExtension());
}

//==============================================================================
// Settings

void MainWindow::showSettings()
{
    if (passRunning())
        return;
    if (settingsWindow == nullptr)
    {
        auto panel = std::make_unique<ui::DeviceSettingsPanel>();
        panel->onApply = [this] (const engine::DeviceSettings& settings) { applyDeviceSettings (settings); };
        settingsWindow = std::make_unique<SettingsWindow> (std::move (panel));
    }
    if (auto* panel = dynamic_cast<ui::DeviceSettingsPanel*> (settingsWindow->getContentComponent()))
        panel->show (live->deviceChoices());
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

void MainWindow::applyDeviceSettings (const engine::DeviceSettings& settings)
{
    if (passRunning())
        return;
    core::logEvent (core::Level::info, "app.device_choice_changed", engine::deviceSettingsFields (settings));

    // The engine's shape is fixed per device (engine/Engine.h), so a new choice builds a new
    // LiveEngine, which opens the saved choice (or falls back to the default and logs why).
    if (session != nullptr)
        for (const auto& error : session->detachAudio())
            content.handleError (error);
    live.reset();
    if (auto written = engine::writeDeviceSettings (engine::defaultDeviceSettingsFile(), settings); ! written.has_value())
        content.handleError (written.error());
    live = std::make_unique<LiveEngine>();
    if (session != nullptr)
        session->attachAudio (live->audioBinding());
    content.refresh();

    if (settingsWindow != nullptr)
        if (auto* panel = dynamic_cast<ui::DeviceSettingsPanel*> (settingsWindow->getContentComponent()))
            panel->show (live->deviceChoices());
    if (live->engine() == nullptr)
        content.showNotice ("ui.no_audio_device");
}
} // namespace overdub::app
