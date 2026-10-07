#include "core/Log.h"
#include "core/LogFiles.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

using overdub::core::Level;
using overdub::core::ScopedLogCapture;

namespace
{
// A fresh, empty directory under the system temp folder, deleted at the end of the test.
struct TempDirectory
{
    juce::File path = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("overdub-logfiles-test", "", false);

    TempDirectory() { path.createDirectory(); }
    ~TempDirectory()
    {
        path.setReadOnly (false, false);
        path.deleteRecursively();
    }

    std::vector<std::string> fileNames() const
    {
        std::vector<std::string> names;
        for (const auto& file : path.findChildFiles (juce::File::findFiles, false))
            names.push_back (file.getFileName().toStdString());
        std::sort (names.begin(), names.end());
        return names;
    }
};

std::chrono::system_clock::time_point utcTime (int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    using namespace std::chrono;
    return sys_days { std::chrono::year { year } / std::chrono::month { month } / std::chrono::day { day } } + hours { hour } + minutes { minute } + seconds { second };
}

overdub::core::ProcessLoggingOptions loggingOptions (const juce::File& directory,
                                                     std::chrono::system_clock::time_point launchTime,
                                                     int processId)
{
    return overdub::core::ProcessLoggingOptions {
        .directory = directory,
        .launchTime = launchTime,
        .processId = processId,
        .minLevel = Level::info,
        .echoToStderr = false,
    };
}
} // namespace

TEST_CASE ("a launch log file is named by its UTC start time and process id", "[core][log]")
{
    CHECK (overdub::core::launchLogFileName (utcTime (2026, 10, 7, 9, 41, 5), 4321) == "overdub-2026-10-07T09-41-05Z-4321.jsonl");
}

TEST_CASE ("pruning keeps only the newest launch logs and leaves other files alone", "[core][log]")
{
    TempDirectory dir;
    for (int minute = 0; minute < 5; ++minute)
        dir.path.getChildFile (overdub::core::launchLogFileName (utcTime (2026, 1, 1, 0, minute, 0), 100)).create();
    dir.path.getChildFile ("notes.txt").create();

    overdub::core::pruneLaunchLogs (dir.path, 2);

    CHECK (dir.fileNames()
           == std::vector<std::string> { "notes.txt",
                                         "overdub-2026-01-01T00-03-00Z-100.jsonl",
                                         "overdub-2026-01-01T00-04-00Z-100.jsonl" });
}

TEST_CASE ("pruning orders launches from the same second by numeric process id", "[core][log]")
{
    TempDirectory dir;
    dir.path.getChildFile (overdub::core::launchLogFileName (utcTime (2026, 1, 1, 0, 0, 0), 1000)).create();
    dir.path.getChildFile (overdub::core::launchLogFileName (utcTime (2026, 1, 1, 0, 0, 0), 999)).create();

    overdub::core::pruneLaunchLogs (dir.path, 1);

    CHECK (dir.fileNames() == std::vector<std::string> { "overdub-2026-01-01T00-00-00Z-1000.jsonl" });
}

TEST_CASE ("pruning logs a warning for a launch log it cannot delete", "[core][log]")
{
    ScopedLogCapture capture;
    TempDirectory dir;
    dir.path.getChildFile (overdub::core::launchLogFileName (utcTime (2026, 1, 1, 0, 0, 0), 1)).create();
    dir.path.getChildFile (overdub::core::launchLogFileName (utcTime (2026, 1, 1, 0, 1, 0), 1)).create();
    // Files in a read-only directory cannot be deleted.
    dir.path.setReadOnly (true, false);

    overdub::core::pruneLaunchLogs (dir.path, 1);

    const auto events = capture.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0]["event"] == "app.log_prune_failed");
    CHECK (events[0]["lvl"] == "warn");
    CHECK (events[0]["path"].get<std::string>().ends_with ("overdub-2026-01-01T00-00-00Z-1.jsonl"));
}

TEST_CASE ("starting process logging creates a new launch file, keeps the last 20 launches and writes flushed lines to it", "[core][log]")
{
    ScopedLogCapture restoresProcessSinkAfterwards;
    TempDirectory dir;
    for (int minute = 0; minute < 25; ++minute)
        dir.path.getChildFile (overdub::core::launchLogFileName (utcTime (2026, 1, 1, 0, minute, 0), 100)).create();

    const auto started = overdub::core::startProcessLogging (loggingOptions (dir.path, utcTime (2026, 2, 1, 12, 0, 0), 777));

    REQUIRE (started.has_value());
    CHECK (started->getFileName() == "overdub-2026-02-01T12-00-00Z-777.jsonl");
    const auto names = dir.fileNames();
    CHECK (names.size() == 20);
    CHECK (names.front() == "overdub-2026-01-01T00-06-00Z-100.jsonl");
    CHECK (names.back() == "overdub-2026-02-01T12-00-00Z-777.jsonl");

    overdub::core::logEvent (Level::debug, "app.below_level");
    overdub::core::logEvent (Level::info, "app.first");
    overdub::core::logEvent (Level::warn, "app.second");

    juce::StringArray lines;
    started->readLines (lines);
    lines.removeEmptyStrings();
    REQUIRE (lines.size() == 2);
    CHECK (nlohmann::json::parse (lines[0].toStdString())["event"] == "app.first");
    CHECK (nlohmann::json::parse (lines[1].toStdString())["event"] == "app.second");
}

TEST_CASE ("starting process logging in a directory that cannot be created returns app.log_dir_create_failed", "[core][log]")
{
    ScopedLogCapture restoresProcessSinkAfterwards;
    TempDirectory dir;
    const auto blockingFile = dir.path.getChildFile ("not-a-directory");
    blockingFile.create();

    const auto started = overdub::core::startProcessLogging (loggingOptions (blockingFile.getChildFile ("Logs"), utcTime (2026, 2, 1, 12, 0, 0), 1));

    REQUIRE_FALSE (started.has_value());
    CHECK (started.error().code == "app.log_dir_create_failed");
}

TEST_CASE ("starting process logging when the launch file cannot be opened returns app.log_open_failed with errno", "[core][log]")
{
    ScopedLogCapture restoresProcessSinkAfterwards;
    TempDirectory dir;
    const auto launchTime = utcTime (2026, 2, 1, 12, 0, 0);
    // A directory in place of the launch file makes fopen fail with EISDIR.
    dir.path.getChildFile (overdub::core::launchLogFileName (launchTime, 5)).createDirectory();

    const auto started = overdub::core::startProcessLogging (loggingOptions (dir.path, launchTime, 5));

    REQUIRE_FALSE (started.has_value());
    CHECK (started.error().code == "app.log_open_failed");
    CHECK (started.error().fields["errno"] == EISDIR);
}
