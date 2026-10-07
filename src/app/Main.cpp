#include "app/MainWindow.h"
#include "core/BuildInfo.h"

#include <memory>

namespace overdub::app
{
class OverdubApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Overdub"; }
    const juce::String getApplicationVersion() override { return core::currentBuildInfo().version; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& /*commandLine*/) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace overdub::app

START_JUCE_APPLICATION (overdub::app::OverdubApplication)
