#include "core/LogFiles.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <utility>

namespace overdub::core
{
namespace
{
struct LaunchOrderKey
{
    juce::String timestamp; // fixed-width UTC timestamp, so text order is time order
    juce::int64 processId = 0;
};

// Splits overdub-<timestamp>-<pid>.jsonl into its timestamp and numeric pid. A name without a
// numeric pid gets pid 0.
LaunchOrderKey launchOrderKey (const juce::File& logFile)
{
    const auto stem = logFile.getFileNameWithoutExtension();
    const auto pidSeparator = stem.lastIndexOfChar ('-');
    return LaunchOrderKey {
        .timestamp = stem.substring (0, pidSeparator),
        .processId = stem.substring (pidSeparator + 1).getLargeIntValue(),
    };
}

bool launchedBefore (const juce::File& a, const juce::File& b)
{
    const auto keyA = launchOrderKey (a);
    const auto keyB = launchOrderKey (b);
    if (keyA.timestamp != keyB.timestamp)
        return keyA.timestamp < keyB.timestamp;
    return keyA.processId < keyB.processId;
}
} // namespace

juce::File defaultLogDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Logs/Overdub");
}

std::string launchLogFileName (std::chrono::system_clock::time_point launchTime, int processId)
{
    const std::time_t launchSeconds = std::chrono::system_clock::to_time_t (launchTime);
    std::tm utc {};
    gmtime_r (&launchSeconds, &utc);

    std::array<char, 64> buffer {};
    std::snprintf (buffer.data(),
                   buffer.size(),
                   "overdub-%04d-%02d-%02dT%02d-%02d-%02dZ-%d.jsonl",
                   utc.tm_year + 1900,
                   utc.tm_mon + 1,
                   utc.tm_mday,
                   utc.tm_hour,
                   utc.tm_min,
                   utc.tm_sec,
                   processId);
    return std::string (buffer.data());
}

void pruneLaunchLogs (const juce::File& directory, int keep)
{
    auto logs = directory.findChildFiles (juce::File::findFiles, false, "overdub-*.jsonl");
    std::sort (logs.begin(), logs.end(), launchedBefore);

    const int excess = logs.size() - std::max (keep, 0);
    for (int index = 0; index < excess; ++index)
    {
        const auto& oldLog = logs.getReference (index);
        if (! oldLog.deleteFile())
            logEvent (Level::warn, "app.log_prune_failed", { { "path", oldLog.getFullPathName().toStdString() } });
    }
}

Expected<std::shared_ptr<FileLogSink>> FileLogSink::open (const juce::File& file, bool echoToStderr)
{
    std::FILE* openedFile = std::fopen (file.getFullPathName().toRawUTF8(), "a");
    if (openedFile == nullptr)
    {
        // Captured before any other call can overwrite errno.
        const int openErrno = errno;
        return std::unexpected (Error {
            "app.log_open_failed",
            "could not open the log file",
            { { "path", file.getFullPathName().toStdString() }, { "errno", openErrno } },
        });
    }

    return std::shared_ptr<FileLogSink> (new FileLogSink (openedFile, echoToStderr));
}

FileLogSink::FileLogSink (std::FILE* openedFile, bool echo)
    : file (openedFile),
      echoToStderr (echo)
{
}

FileLogSink::~FileLogSink()
{
    std::fclose (file);
}

void FileLogSink::writeLine (std::string_view line)
{
    std::fwrite (line.data(), 1, line.size(), file);
    std::fputc ('\n', file);
    std::fflush (file);

    if (echoToStderr)
    {
        std::fwrite (line.data(), 1, line.size(), stderr);
        std::fputc ('\n', stderr);
    }
}

namespace
{
Expected<std::pair<juce::File, std::shared_ptr<LogSink>>> openLaunchLogFile (const ProcessLoggingOptions& options)
{
    if (! options.directory.createDirectory())
        return std::unexpected (Error {
            "app.log_dir_create_failed",
            "could not create the log directory",
            { { "path", options.directory.getFullPathName().toStdString() } },
        });

    // Leave room for this launch's file so launchLogsKept launches remain.
    pruneLaunchLogs (options.directory, launchLogsKept - 1);

    const auto logFile = options.directory.getChildFile (launchLogFileName (options.launchTime, options.processId));
    auto sink = FileLogSink::open (logFile, options.echoToStderr);
    if (! sink.has_value())
        return std::unexpected (sink.error());

    return std::pair<juce::File, std::shared_ptr<LogSink>> { logFile, *sink };
}
} // namespace

Expected<juce::File> startProcessLogging (const ProcessLoggingOptions& options)
{
    auto opened = openLaunchLogFile (options);
    const std::shared_ptr<LogSink> sink = opened.has_value() ? opened->second : std::make_shared<StderrLogSink>();

    processLogger().setSink (sink);
    processLogger().setMinLevel (options.minLevel);

    if (! opened.has_value())
        return std::unexpected (opened.error());

    return opened->first;
}
} // namespace overdub::core
