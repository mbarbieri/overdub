#include "app/LiveEngine.h"
#include "app/LoopbackSelftest.h"
#include "app/MainWindow.h"
#include "app/MicrophonePermission.h"
#include "capture/ClapSelftest.h"
#include "control/EventTap.h"
#include "control/Server.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include "core/LogFiles.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace overdub::app
{
class OverdubApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Overdub"; }
    const juce::String getApplicationVersion() override { return core::currentBuildInfo().version; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        core::setCurrentThreadRole (core::ThreadRole::ui);
        const auto logPath = startLogging();
        const auto args = juce::StringArray::fromTokens (commandLine, true);
        const auto selftestIndex = args.indexOf ("--selftest");
        // The tap goes in before the first line so a client's lookback holds the whole launch.
        const auto controlConfig = control::serverConfigFrom (std::getenv ("OVERDUB_CONTROL"), std::getenv ("OVERDUB_CONTROL_SOCKET"));
        if (selftestIndex < 0 && controlConfig.enabled)
            core::processLogger().addTap (eventTap);

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

        if (selftestIndex >= 0)
        {
            runSelftest (args[selftestIndex + 1], args[selftestIndex + 2]);
            return;
        }

        // Microphone access first: the audio device opens only once the user has decided (ADR 0002).
        requestMicrophoneAccess ([this, controlConfig] (bool) {
            mainWindow = std::make_unique<MainWindow> (getApplicationName(), std::make_unique<LiveEngine>(), controlConfig, eventTap);
        });
    }

    void shutdown() override
    {
        selftest.reset();
        mainWindow.reset();
        liveEngine.reset();

        core::logEvent (core::Level::info, "app.stopped");
        core::processLogger().removeTap (eventTap);
        juce::Logger::setCurrentLogger (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            return mainWindow->requestQuit();
        quit();
    }

private:
    // `overdub --selftest <name>` (ADR 0005): runs the selftest without a window, prints its JSON
    // report as one line on stdout, logs app.selftest_finished and quits with 0 (pass) or 1
    // (fail). An unknown name prints an error report and quits with 2.
    // `--selftest clap <package>` reads a recorded Song (no device opens); `argument` is its path.
    void runSelftest (const juce::String& name, const juce::String& argument)
    {
        if (name == "clap")
        {
            const auto package = juce::File::getCurrentWorkingDirectory().getChildFile (argument.unquoted());
            const auto report = capture::runClapSelftest (package);
            finishSelftest (report, report.value ("pass", false) ? 0 : 1);
            return;
        }
        if (name != "loopback")
        {
            finishSelftest ({ { "selftest", name.toStdString() }, { "pass", false }, { "error", "app.selftest_unknown" } }, 2);
            return;
        }

        requestMicrophoneAccess ([this] (bool granted) {
            if (! granted)
            {
                finishSelftest ({ { "selftest", "loopback" }, { "pass", false }, { "error", "app.microphone_denied" } }, 1);
                return;
            }
            liveEngine = std::make_unique<LiveEngine>();
            selftest = std::make_unique<LoopbackSelftest> (*liveEngine, [this] (const core::Fields& report) {
                // Quit after this callback returns: the selftest is still on the stack here.
                juce::MessageManager::callAsync ([this, report] { finishSelftest (report, report.value ("pass", false) ? 0 : 1); });
            });
            selftest->start();
        });
    }

    void finishSelftest (const core::Fields& report, int exitCode)
    {
        std::cout << report.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
        core::logEvent (exitCode == 0 ? core::Level::info : core::Level::warn, "app.selftest_finished", report);
        setApplicationReturnValue (exitCode);
        quit();
    }

    // Opens this launch's JSONL file (ADR 0006) and routes JUCE's own messages into it.
    // Returns the log file path, or "" when the file could not be opened and stderr is used instead.
    std::string startLogging()
    {
#if JUCE_DEBUG
        constexpr bool echoToStderr = true;
#else
        constexpr bool echoToStderr = false;
#endif

        const auto logFile = core::startProcessLogging (core::ProcessLoggingOptions {
            .directory = core::defaultLogDirectory(),
            .launchTime = std::chrono::system_clock::now(),
            .processId = static_cast<int> (getpid()),
            .minLevel = core::levelFromOverride (std::getenv ("OVERDUB_LOG"), core::defaultLevelForBuild()),
            .echoToStderr = echoToStderr,
        });

        juce::Logger::setCurrentLogger (&juceLoggerBridge);

        if (! logFile.has_value())
        {
            core::logError (logFile.error(), core::Level::warn);
            return {};
        }

        return logFile->getFullPathName().toStdString();
    }

    core::JuceLoggerBridge juceLoggerBridge;
    std::shared_ptr<control::EventTap> eventTap = std::make_shared<control::EventTap>();
    std::unique_ptr<LiveEngine> liveEngine; // only in --selftest; the window owns its own
    std::unique_ptr<MainWindow> mainWindow; // owns the live engine and the open Song
    std::unique_ptr<LoopbackSelftest> selftest; // only in --selftest loopback; destroyed before liveEngine
};
} // namespace overdub::app

START_JUCE_APPLICATION (overdub::app::OverdubApplication)
