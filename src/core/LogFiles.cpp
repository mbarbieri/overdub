#include "core/LogFiles.h"

#include <algorithm>
#include <array>
#include <ctime>

namespace overdub::core
{
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
    std::sort (logs.begin(), logs.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });

    const int excess = logs.size() - std::max (keep, 0);
    for (int index = 0; index < excess; ++index)
        logs.getReference (index).deleteFile();
}

Expected<std::shared_ptr<FileLogSink>> FileLogSink::open (const juce::File& file, bool echoToStderr)
{
    std::FILE* openedFile = std::fopen (file.getFullPathName().toRawUTF8(), "a");
    if (openedFile == nullptr)
        return std::unexpected (Error { "app.log_open_failed", "could not open the log file", { { "path", file.getFullPathName().toStdString() }, { "errno", errno } } });

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

Expected<juce::File> startProcessLogging (const juce::File& directory,
                                          std::chrono::system_clock::time_point launchTime,
                                          int processId,
                                          Level minLevel,
                                          bool echoToStderr)
{
    if (! directory.createDirectory())
        return std::unexpected (Error { "app.log_open_failed", "could not create the log directory", { { "path", directory.getFullPathName().toStdString() } } });

    // Leave room for this launch's file so launchLogsKept launches remain.
    pruneLaunchLogs (directory, launchLogsKept - 1);

    const auto logFile = directory.getChildFile (launchLogFileName (launchTime, processId));
    auto sink = FileLogSink::open (logFile, echoToStderr);
    if (! sink.has_value())
        return std::unexpected (sink.error());

    processLogger().setSink (*sink);
    processLogger().setMinLevel (minLevel);
    return logFile;
}
} // namespace overdub::core
