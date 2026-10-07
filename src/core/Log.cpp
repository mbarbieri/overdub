#include "core/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <string>
#include <time.h>

namespace overdub::core
{
namespace
{
// Invariant: the role lives in a pthread TSD slot, not a C++ thread_local. macOS allocates a
// thread's thread_local storage with malloc on its first access, and the audio thread sets its
// role on every callback (engine/DeviceBinding.cpp), including the first one on a new thread.
// pthread_getspecific / pthread_setspecific on a created key never allocate or lock.
// The slot stores role + 1, so a thread that never set it (null) reads as `worker`.
pthread_key_t threadRoleKey()
{
    static const pthread_key_t key = [] {
        pthread_key_t created {};
        const auto result = pthread_key_create (&created, nullptr);
        if (result != 0)
        {
            std::fprintf (stderr, "overdub: pthread_key_create for the thread role failed (%d)\n", result);
            std::abort();
        }
        return created;
    }();
    return key;
}

// Creates the key at static initialisation, before any audio thread can run.
[[maybe_unused]] const pthread_key_t threadRoleKeyAtStartup = threadRoleKey();

constexpr std::array<std::string_view, 5> reservedKeys { "t", "host_ns", "lvl", "thread", "event" };

bool isReservedKey (std::string_view key)
{
    if (key == "msg")
        return true;

    for (const auto reserved : reservedKeys)
        if (key == reserved)
            return true;

    return false;
}

// UTC wall time with millisecond precision, e.g. 2026-10-07T09:41:05.123Z.
std::string formatWallTime (std::chrono::system_clock::time_point wallTime)
{
    const auto sinceEpoch = wallTime.time_since_epoch();
    const auto seconds = std::chrono::floor<std::chrono::seconds> (sinceEpoch);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds> (sinceEpoch - seconds).count();

    const std::time_t secondsAsTime = static_cast<std::time_t> (seconds.count());
    std::tm utc {};
    gmtime_r (&secondsAsTime, &utc);

    std::array<char, 32> buffer {};
    std::snprintf (buffer.data(),
                   buffer.size(),
                   "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                   utc.tm_year + 1900,
                   utc.tm_mon + 1,
                   utc.tm_mday,
                   utc.tm_hour,
                   utc.tm_min,
                   utc.tm_sec,
                   static_cast<int> (millis));
    return std::string (buffer.data());
}
} // namespace

std::string_view levelName (Level level)
{
    switch (level)
    {
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warn";
        case Level::error: return "error";
    }
    return "error";
}

std::string_view threadRoleName (ThreadRole role)
{
    switch (role)
    {
        case ThreadRole::audio: return "audio";
        case ThreadRole::ui: return "ui";
        case ThreadRole::video: return "video";
        case ThreadRole::io: return "io";
        case ThreadRole::logger: return "logger";
        case ThreadRole::worker: return "worker";
    }
    return "worker";
}

std::optional<Level> parseLevel (std::string_view name)
{
    for (const auto level : { Level::debug, Level::info, Level::warn, Level::error })
        if (levelName (level) == name)
            return level;

    return std::nullopt;
}

Level defaultLevelForBuild()
{
#if JUCE_DEBUG
    return Level::debug;
#else
    return Level::info;
#endif
}

Level levelFromOverride (const char* overrideValue, Level fallback)
{
    if (overrideValue == nullptr)
        return fallback;

    return parseLevel (overrideValue).value_or (fallback);
}

std::uint64_t hostTimeNowNs()
{
    // CLOCK_UPTIME_RAW is mach_absolute_time converted to nanoseconds.
    return clock_gettime_nsec_np (CLOCK_UPTIME_RAW);
}

ThreadRole currentThreadRole()
{
    const auto stored = reinterpret_cast<std::uintptr_t> (pthread_getspecific (threadRoleKey()));
    return stored == 0 ? ThreadRole::worker : static_cast<ThreadRole> (stored - 1);
}

void setCurrentThreadRole (ThreadRole role)
{
    pthread_setspecific (threadRoleKey(), reinterpret_cast<void*> (static_cast<std::uintptr_t> (role) + 1));
}

ScopedThreadRole::ScopedThreadRole (ThreadRole role)
    : previousRole (currentThreadRole())
{
    setCurrentThreadRole (role);
}

ScopedThreadRole::~ScopedThreadRole()
{
    setCurrentThreadRole (previousRole);
}

std::string formatLogLine (const LogRecord& record)
{
    nlohmann::ordered_json line = nlohmann::ordered_json::object();
    line["t"] = formatWallTime (record.wallTime);
    line["host_ns"] = record.hostNs;
    line["lvl"] = levelName (record.level);
    line["thread"] = threadRoleName (record.thread);
    line["event"] = record.event;

    if (! record.msg.empty())
        line["msg"] = record.msg;

    if (record.fields != nullptr && record.fields->is_object())
    {
        for (const auto& [key, value] : record.fields->items())
        {
            // Invariant: reserved keys always mean what ADR 0006 says, so a clashing field is renamed.
            const auto outputKey = isReservedKey (key) ? "field_" + key : key;
            line[outputKey] = value.is_object() ? nlohmann::ordered_json (value.dump()) : nlohmann::ordered_json (value);
        }
    }

    return line.dump (-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void MemoryLogSink::writeLine (std::string_view line)
{
    const std::scoped_lock lock (mutex);
    storedLines.emplace_back (line);
}

std::vector<std::string> MemoryLogSink::lines() const
{
    const std::scoped_lock lock (mutex);
    return storedLines;
}

void StderrLogSink::writeLine (std::string_view line)
{
    std::fwrite (line.data(), 1, line.size(), stderr);
    std::fputc ('\n', stderr);
}

void Logger::setSink (std::shared_ptr<LogSink> newSink)
{
    const std::scoped_lock lock (mutex);
    currentSink = std::move (newSink);
}

std::shared_ptr<LogSink> Logger::sink() const
{
    const std::scoped_lock lock (mutex);
    return currentSink;
}

void Logger::setMinLevel (Level level)
{
    currentMinLevel.store (level);
}

Level Logger::minLevel() const
{
    return currentMinLevel.load();
}

bool Logger::isEnabled (Level level) const
{
    return static_cast<int> (level) >= static_cast<int> (currentMinLevel.load());
}

void Logger::write (const LogRecord& record)
{
    if (! isEnabled (record.level))
        return;

    const auto line = formatLogLine (record);

    const std::scoped_lock lock (mutex);
    if (currentSink != nullptr)
        currentSink->writeLine (line);
}

Logger& processLogger()
{
    static Logger logger;
    return logger;
}

void logEvent (Level level, std::string_view event, const Fields& fields, std::string_view msg)
{
    processLogger().write (LogRecord {
        .level = level,
        .event = event,
        .thread = currentThreadRole(),
        .hostNs = hostTimeNowNs(),
        .wallTime = std::chrono::system_clock::now(),
        .fields = &fields,
        .msg = msg,
    });
}

void logEventCapturedAt (Level level,
                         std::string_view event,
                         ThreadRole capturedOn,
                         std::uint64_t capturedHostNs,
                         const Fields& fields)
{
    const auto nowHostNs = hostTimeNowNs();
    const auto ageNs = nowHostNs > capturedHostNs ? nowHostNs - capturedHostNs : 0;
    const auto capturedWallTime = std::chrono::system_clock::now()
                                - std::chrono::duration_cast<std::chrono::system_clock::duration> (std::chrono::nanoseconds (ageNs));

    processLogger().write (LogRecord {
        .level = level,
        .event = event,
        .thread = capturedOn,
        .hostNs = capturedHostNs,
        .wallTime = capturedWallTime,
        .fields = &fields,
        .msg = {},
    });
}

ScopedLogCapture::ScopedLogCapture (Level minLevel)
    : memorySink (std::make_shared<MemoryLogSink>()),
      previousSink (processLogger().sink()),
      previousMinLevel (processLogger().minLevel())
{
    processLogger().setSink (memorySink);
    processLogger().setMinLevel (minLevel);
}

ScopedLogCapture::~ScopedLogCapture()
{
    processLogger().setSink (previousSink);
    processLogger().setMinLevel (previousMinLevel);
}

std::vector<std::string> ScopedLogCapture::lines() const
{
    return memorySink->lines();
}

std::vector<nlohmann::json> ScopedLogCapture::events() const
{
    std::vector<nlohmann::json> parsed;
    for (const auto& line : memorySink->lines())
        parsed.push_back (nlohmann::json::parse (line));
    return parsed;
}

void JuceLoggerBridge::logMessage (const juce::String& message)
{
    if (currentThreadRole() == ThreadRole::audio)
        return;

    logEvent (Level::info, "app.juce_message", {}, message.toStdString());
}
} // namespace overdub::core
