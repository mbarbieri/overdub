#include "app/MainWindow.h"

#include "capture/AvCamera.h"
#include "control/AppState.h"
#include "control/IntentOps.h"
#include "ui/LastSong.h"
#include "ui/Layout.h"
#include "ui/SongFiles.h"
#include "ui/SongMenuItems.h"

#include <cstdlib>

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
    redoItem,
    // File > Convert Sample Rate: this plus the rate's index in the device's offered rates.
    convertSampleRateItemBase = 100,
    // The File menu's Song items: this plus the item's index in ui::songMenuItems.
    songMenuItemBase = 200
};

juce::File defaultSongFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

constexpr int devicePanelHeight = 240;
constexpr int calibrationPanelHeight = 520;
constexpr int settingsWidth = 460;
// Answers device changes and advances a loopback calibration run.
constexpr int workspaceTickIntervalMs = 50;
constexpr int controlPollIntervalMs = 10;

// The settings window's content: the device picker above the calibration panel.
class SettingsContent final : public juce::Component
{
public:
    SettingsContent()
    {
        addAndMakeVisible (devices);
        addAndMakeVisible (calibration);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        devices.setBounds (area.removeFromTop (devicePanelHeight));
        calibration.setBounds (area);
    }

    ui::DeviceSettingsPanel devices;
    ui::CalibrationPanel calibration;
};

// The settings window, hidden (not destroyed) when closed.
class SettingsWindow final : public juce::DocumentWindow
{
public:
    SettingsWindow (std::unique_ptr<SettingsContent> content, std::function<void()> whenClosed)
        : DocumentWindow ("Settings", juce::Colours::black, juce::DocumentWindow::closeButton),
          onClose (std::move (whenClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content.release(), false);
        centreWithSize (settingsWidth, devicePanelHeight + calibrationPanelHeight);
    }

    void closeButtonPressed() override { onClose(); }

private:
    std::function<void()> onClose;
};

juce::PopupMenu::Item historyItem (int id, const juce::String& verb, bool enabled, const std::string& label, const char* shortcut)
{
    juce::PopupMenu::Item item { enabled ? verb + " " + juce::String::fromUTF8 (label.c_str()) : verb };
    item.setID (id).setEnabled (enabled);
    item.shortcutKeyDescription = juce::String (juce::CharPointer_UTF8 (shortcut));
    return item;
}
} // namespace

MainWindow::MainWindow (const juce::String& name,
                        std::unique_ptr<LiveEngine> liveEngine,
                        const control::ServerConfig& controlConfig,
                        std::shared_ptr<control::EventTap> eventTap)
    : DocumentWindow (name,
                      juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      juce::DocumentWindow::allButtons),
      appName (name),
      camera (capture::makeAvCamera())
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

    // A scripted launch (its socket moved) never touches the user's Last Song (issue #206).
    auto files = ui::WorkspaceFiles::defaults();
    files.lastSong = ui::lastSongFileFor (std::getenv ("OVERDUB_LAST_SONG_FILE"), controlConfig.enabled && ! controlConfig.privateFolder);

    // Before the menu: installing it asks getMenuForIndex at once, which reads the Workspace.
    workspace = std::make_unique<ui::Workspace> (
        content,
        std::move (liveEngine),
        [] (std::optional<int> songSampleRate) -> std::unique_ptr<ui::LiveAudio> { return std::make_unique<LiveEngine> (songSampleRate); },
        camera.get(),
        pluginLoader.instantiator(),
        std::move (files));
    workspace->onOpenSongChanged = [this] {
        updateTitle();
        content.grabKeyboardFocus();
    };
    workspace->onShowSettings = [this] (bool shown) { showSettingsWindow (shown); };
    workspace->onQuit = [] { juce::JUCEApplicationBase::quit(); };

    juce::PopupMenu appleMenuExtras;
    appleMenuExtras.addItem (settingsItem, "Settings...");
    juce::MenuBarModel::setMacMainMenu (this, &appleMenuExtras);

    setVisible (true);
    content.grabKeyboardFocus();
    // Holding Shift at launch skips the Last Song, as macOS apps skip restoring windows (issue #208).
    ui::reopenLastSongAtLaunch (content, juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown());
    workspaceTick.startTimer (workspaceTickIntervalMs);

    controlServer = control::Server::start (controlConfig, control::Controller { std::move (eventTap), control::intentOps (content), control::appStateReader (content, *workspace) });
    if (controlServer != nullptr)
        controlTick.startTimer (controlPollIntervalMs);
}

MainWindow::~MainWindow()
{
    controlTick.stopTimer();
    controlServer.reset();
    juce::MenuBarModel::setMacMainMenu (nullptr);
    workspaceTick.stopTimer();
    workspace->attachSettingsPanels (nullptr, nullptr);
    settingsWindow.reset();
    workspace.reset(); // closes the Song while the device still runs
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
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
    const auto mayRun = [this] (std::string_view op) { return ! content.refusalOf (op).has_value(); };
    auto* openSong = workspace->openSong();
    if (topLevelMenuIndex == 0)
    {
        menu.addItem (newSongItem, "New Song...", mayRun (ui::intent::NewSongAt::op));
        menu.addItem (openSongItem, "Open...", mayRun (ui::intent::OpenSongAt::op));
        menu.addItem (duplicateSongItem, "Duplicate...", mayRun (ui::intent::DuplicateSongTo::op) && openSong != nullptr);
        const auto rates = convertSampleRateMenu();
        menu.addSubMenu ("Convert Sample Rate", rates, mayRun (ui::intent::ConvertSampleRate::op) && openSong != nullptr && ! offeredMenuRates.empty());
        menu.addSeparator();
        const auto& songItems = ui::songMenuItems;
        for (std::size_t i = 0; i < songItems.size(); ++i)
        {
            const auto& item = songItems[i];
            juce::PopupMenu::Item menuItem { juce::String::fromUTF8 (item.name.data(), (int) item.name.size()) };
            menuItem.setID (songMenuItemBase + (int) i).setEnabled (mayRun (ui::opName (ui::Intent { item.intent })) && openSong != nullptr);
            menuItem.shortcutKeyDescription = juce::String::fromUTF8 (item.shortcut.data(), (int) item.shortcut.size());
            menu.addItem (std::move (menuItem));
        }
        menu.addSeparator();
        menu.addItem (settingsItem, "Settings...", mayRun (ui::intent::ShowSettings::op));
        return menu;
    }

    if (openSong == nullptr)
    {
        menu.addItem (historyItem (undoItem, "Undo", false, {}, "\xe2\x8c\x98Z"));
        menu.addItem (historyItem (redoItem, "Redo", false, {}, "\xe2\x87\xa7\xe2\x8c\x98Z"));
        return menu;
    }
    const auto& package = openSong->package();
    menu.addItem (historyItem (undoItem, "Undo", mayRun (ui::intent::Undo::op) && package.canUndo(), package.undoLabel(), "\xe2\x8c\x98Z"));
    menu.addItem (historyItem (redoItem, "Redo", mayRun (ui::intent::Redo::op) && package.canRedo(), package.redoLabel(), "\xe2\x87\xa7\xe2\x8c\x98Z"));
    return menu;
}

juce::PopupMenu MainWindow::convertSampleRateMenu()
{
    // The open device's rates (issue #160); the Song's own is ticked and disabled.
    juce::PopupMenu rates;
    offeredMenuRates = workspace->liveAudio().deviceChoices().sampleRates;
    const auto* openSong = workspace->openSong();
    const auto songRate = openSong != nullptr ? openSong->song().sampleRate : 0;
    for (std::size_t i = 0; i < offeredMenuRates.size(); ++i)
    {
        const auto rate = offeredMenuRates[i];
        rates.addItem (convertSampleRateItemBase + (int) i, juce::String (rate) + " Hz", rate != songRate, rate == songRate);
    }
    return rates;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    if (const auto index = menuItemID - convertSampleRateItemBase; index >= 0 && index < (int) offeredMenuRates.size())
        return (void) content.run (ui::AppIntent { ui::intent::ConvertSampleRate { offeredMenuRates[(std::size_t) index] } });
    if (const auto index = menuItemID - songMenuItemBase; index >= 0 && index < (int) ui::songMenuItems.size())
        return (void) content.run (ui::songMenuItems[(std::size_t) index].intent);
    switch (menuItemID)
    {
        case newSongItem: newSong(); break;
        case openSongItem: chooseSongToOpen(); break;
        case duplicateSongItem: duplicateSong(); break;
        case settingsItem: content.run (ui::AppIntent { ui::intent::ShowSettings { true } }); break;
        case undoItem: content.run (ui::SongIntent { ui::intent::Undo {} }); break;
        case redoItem: content.run (ui::SongIntent { ui::intent::Redo {} }); break;
        default: break;
    }
}

//==============================================================================
// Songs

void MainWindow::newSong()
{
    if (! content.mayRun (ui::intent::NewSongAt::op))
        return;
    chooser = std::make_unique<juce::FileChooser> ("New Song", defaultSongFolder().getChildFile ("Untitled.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        if (const auto chosen = finished.getResult(); chosen != juce::File())
            content.run (ui::AppIntent { ui::intent::NewSongAt { { chosen.getFullPathName().toStdString() } } });
    });
}

void MainWindow::chooseSongToOpen()
{
    if (! content.mayRun (ui::intent::OpenSongAt::op))
        return;
    chooser = std::make_unique<juce::FileChooser> ("Open Song", defaultSongFolder(), "*.overdub");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this] (const juce::FileChooser& finished) {
        if (const auto chosen = finished.getResult(); chosen != juce::File())
            content.run (ui::AppIntent { ui::intent::OpenSongAt { { chosen.getFullPathName().toStdString() } } });
    });
}

void MainWindow::duplicateSong()
{
    if (workspace->openSong() == nullptr || ! content.mayRun (ui::intent::DuplicateSongTo::op))
        return;
    const auto current = workspace->openSong()->package().location();
    chooser = std::make_unique<juce::FileChooser> ("Duplicate Song", current.getSiblingFile (current.getFileNameWithoutExtension() + " copy.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        if (const auto chosen = finished.getResult(); chosen != juce::File())
            content.run (ui::AppIntent { ui::intent::DuplicateSongTo { { chosen.getFullPathName().toStdString() } } });
    });
}

void MainWindow::updateTitle()
{
    auto* openSong = workspace != nullptr ? workspace->openSong() : nullptr;
    if (openSong == nullptr)
        setName (appName);
    else
        setName (appName + juce::String (juce::CharPointer_UTF8 (" \xe2\x80\x94 ")) + openSong->package().location().getFileNameWithoutExtension());
}

//==============================================================================
// Settings

void MainWindow::showSettingsWindow (bool shown)
{
    if (! shown)
    {
        if (settingsWindow != nullptr)
            settingsWindow->setVisible (false);
        return;
    }
    if (settingsWindow == nullptr)
    {
        auto settingsContent = std::make_unique<SettingsContent>();
        workspace->attachSettingsPanels (&settingsContent->devices, &settingsContent->calibration);
        settingsWindow = std::make_unique<SettingsWindow> (std::move (settingsContent), [this] { content.run (ui::AppIntent { ui::intent::ShowSettings { false } }); });
    }
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

void MainWindow::requestQuit()
{
    content.run (ui::AppIntent { ui::intent::Quit {} });
}
} // namespace overdub::app
