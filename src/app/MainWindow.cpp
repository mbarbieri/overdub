#include "app/MainWindow.h"

#include "capture/AvCamera.h"
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
    redoItem,
    // File > Convert Sample Rate: this plus the rate's index in the device's offered rates.
    convertSampleRateItemBase = 100
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
    explicit SettingsWindow (std::unique_ptr<SettingsContent> content)
        : DocumentWindow ("Settings", juce::Colours::black, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content.release(), false);
        centreWithSize (settingsWidth, devicePanelHeight + calibrationPanelHeight);
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

    // Before the menu: installing it asks getMenuForIndex at once, which reads the Workspace.
    workspace = std::make_unique<ui::Workspace> (
        content,
        std::move (liveEngine),
        [] (std::optional<int> songSampleRate) -> std::unique_ptr<ui::LiveAudio> { return std::make_unique<LiveEngine> (songSampleRate); },
        camera.get(),
        pluginLoader.instantiator(),
        ui::WorkspaceFiles::defaults());
    workspace->onOpenSongChanged = [this] {
        updateTitle();
        content.grabKeyboardFocus();
    };

    juce::PopupMenu appleMenuExtras;
    appleMenuExtras.addItem (settingsItem, "Settings...");
    juce::MenuBarModel::setMacMainMenu (this, &appleMenuExtras);

    setVisible (true);
    content.grabKeyboardFocus();
    workspaceTick.startTimer (workspaceTickIntervalMs);

    controlServer = control::Server::start (controlConfig, control::Controller { std::move (eventTap) });
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

bool MainWindow::mayQuitOrNotice()
{
    return workspace == nullptr || workspace->mayQuitOrNotice();
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
    const auto idle = workspace->isIdle();
    auto* openSong = workspace->openSong();
    if (topLevelMenuIndex == 0)
    {
        menu.addItem (newSongItem, "New Song...", idle);
        menu.addItem (openSongItem, "Open...", idle);
        menu.addItem (duplicateSongItem, "Duplicate...", idle && openSong != nullptr);
        const auto rates = convertSampleRateMenu();
        menu.addSubMenu ("Convert Sample Rate", rates, idle && openSong != nullptr && ! offeredMenuRates.empty());
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

juce::PopupMenu MainWindow::convertSampleRateMenu()
{
    // The open device's rates (issue #160); the Song's own is ticked and disabled.
    juce::PopupMenu rates;
    offeredMenuRates = workspace->liveAudio().deviceChoices().sampleRates;
    const auto* openSong = workspace->openSong();
    const auto songRate = openSong != nullptr ? openSong->song().sampleRate : 0;
    const auto canConvert = openSong != nullptr && ! openSong->isExporting();
    for (std::size_t i = 0; i < offeredMenuRates.size(); ++i)
    {
        const auto rate = offeredMenuRates[i];
        rates.addItem (convertSampleRateItemBase + (int) i, juce::String (rate) + " Hz", canConvert && rate != songRate, rate == songRate);
    }
    return rates;
}

void MainWindow::convertSampleRate (int toRate)
{
    if (workspace->idleOrNotice())
        content.convertSampleRate (toRate);
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    if (const auto index = menuItemID - convertSampleRateItemBase; index >= 0 && index < (int) offeredMenuRates.size())
        return convertSampleRate (offeredMenuRates[(std::size_t) index]);
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
    if (! workspace->idleOrNotice())
        return;
    chooser = std::make_unique<juce::FileChooser> ("New Song", defaultSongFolder().getChildFile ("Untitled.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || ! workspace->idleOrNotice())
            return;
        auto created = ui::createSongAt (chosen, workspace->liveAudio().sampleRate());
        if (! created.has_value())
            return content.handleError (created.error());
        workspace->showSong (std::move (*created), {});
    });
}

void MainWindow::chooseSongToOpen()
{
    if (! workspace->idleOrNotice())
        return;
    chooser = std::make_unique<juce::FileChooser> ("Open Song", defaultSongFolder(), "*.overdub");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (flags, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        if (chosen == juce::File() || ! workspace->idleOrNotice())
            return;
        workspace->openSongFrom (chosen);
    });
}

void MainWindow::duplicateSong()
{
    if (workspace->openSong() == nullptr || ! workspace->idleOrNotice())
        return;
    const auto current = workspace->openSong()->package().location();
    chooser = std::make_unique<juce::FileChooser> ("Duplicate Song", current.getSiblingFile (current.getFileNameWithoutExtension() + " copy.overdub"), "*.overdub");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& finished) {
        const auto chosen = finished.getResult();
        auto* openSong = workspace->openSong();
        if (chosen == juce::File() || openSong == nullptr || ! workspace->idleOrNotice())
            return;
        auto duplicated = ui::duplicateSongTo (openSong->package(), chosen);
        if (! duplicated.has_value())
            return content.handleError (duplicated.error());
        // The duplicate becomes the open Song; the original stays as it was.
        workspace->openSongFrom (*duplicated);
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

void MainWindow::showSettings()
{
    if (! workspace->idleOrNotice())
        return;
    if (settingsWindow == nullptr)
    {
        auto settingsContent = std::make_unique<SettingsContent>();
        workspace->attachSettingsPanels (&settingsContent->devices, &settingsContent->calibration);
        settingsWindow = std::make_unique<SettingsWindow> (std::move (settingsContent));
    }
    workspace->refreshSettingsPanels();
    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}
} // namespace overdub::app
