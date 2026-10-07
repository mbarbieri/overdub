#include "app/MainWindow.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include "core/LogFiles.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <unistd.h>

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
        core::setCurrentThreadRole (core::ThreadRole::ui);
        const auto logPath = startLogging();

        const auto build = core::currentBuildInfo();
        core::logEvent (core::Level::info,
                        "app.started",
                        {
                            { "version", build.version },
                            { "git_sha", build.gitSha },
                            { "macos", juce::SystemStats::getOperatingSystemName().toStdString() },
                            { "arch", build.architecture },
                            { "log_path", logPath },
                            { "pid", static_cast<int> (getpid()) },
                        });

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();

        core::logEvent (core::Level::info, "app.stopped");
        juce::Logger::setCurrentLogger (nullptr);
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    // Opens this launch's JSONL file (ADR 0006) and routes JUCE's own messages into it.
    // Returns the log file path, or "" when the file could not be opened and stderr is used instead.
    std::string startLogging()
    {
        const auto level = core::levelFromOverride (std::getenv ("OVERDUB_LOG"), core::defaultLevelForBuild());
        const bool echoToStderr = JUCE_DEBUG != 0;

        const auto logFile = core::startProcessLogging (core::defaultLogDirectory(),
                                                        std::chrono::system_clock::now(),
                                                        static_cast<int> (getpid()),
                                                        level,
                                                        echoToStderr);

        juce::Logger::setCurrentLogger (&juceLoggerBridge);

        if (! logFile.has_value())
        {
            core::processLogger().setSink (std::make_shared<core::StderrLogSink>());
            core::processLogger().setMinLevel (level);
            core::logError (logFile.error(), core::Level::warn);
            return {};
        }

        return logFile->getFullPathName().toStdString();
    }

    core::JuceLoggerBridge juceLoggerBridge;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace overdub::app

START_JUCE_APPLICATION (overdub::app::OverdubApplication)
