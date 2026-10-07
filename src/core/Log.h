#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The process-wide JSON Lines logger (ADR 0006).
// Each line is one flat JSON object: reserved keys t, host_ns, lvl, thread, event, optional msg,
// then the event's own fields. Never call anything here from the audio thread: RT code pushes
// RtEvents into an RtEventRing instead (core/RtEventRing.h).
namespace overdub::core
{
enum class Level
{
    debug,
    info,
    warn,
    error
};

enum class ThreadRole
{
    audio,
    ui,
    video,
    io,
    logger,
    worker
};

// An event's own fields: a JSON object (or null for none). Nesting is allowed only for arrays;
// a nested object value is written as its JSON text.
using Fields = nlohmann::json;

std::string_view levelName (Level level);
std::string_view threadRoleName (ThreadRole role);
std::optional<Level> parseLevel (std::string_view name);

// The level the build logs at: debug in debug builds, info in release builds.
Level defaultLevelForBuild();

// OVERDUB_LOG semantics: a valid level name overrides the fallback; null, empty or unknown keeps it.
Level levelFromOverride (const char* overrideValue, Level fallback);

// mach_absolute_time in nanoseconds: the same clock the audio callback's host time uses.
std::uint64_t hostTimeNowNs();

// The role of the calling thread, written as the `thread` key. Threads start as `worker`.
ThreadRole currentThreadRole();
void setCurrentThreadRole (ThreadRole role);

class ScopedThreadRole
{
public:
    explicit ScopedThreadRole (ThreadRole role);
    ~ScopedThreadRole();

    ScopedThreadRole (const ScopedThreadRole&) = delete;
    ScopedThreadRole& operator= (const ScopedThreadRole&) = delete;

private:
    ThreadRole previousRole;
};

struct LogRecord
{
    Level level = Level::info;
    std::string_view event;
    ThreadRole thread = ThreadRole::worker;
    std::uint64_t hostNs = 0;
    std::chrono::system_clock::time_point wallTime;
    const Fields* fields = nullptr; // may be null
    std::string_view msg;           // omitted from the line when empty
};

// One JSON line, without a trailing newline.
std::string formatLogLine (const LogRecord& record);

class LogSink
{
public:
    virtual ~LogSink() = default;
    // Called with one complete line (no newline), serialised by the Logger.
    virtual void writeLine (std::string_view line) = 0;
};

// Keeps lines in memory. Tests use it through ScopedLogCapture and never read the log file.
class MemoryLogSink final : public LogSink
{
public:
    void writeLine (std::string_view line) override;
    std::vector<std::string> lines() const;

private:
    mutable std::mutex mutex;
    std::vector<std::string> storedLines;
};

class Logger
{
public:
    void setSink (std::shared_ptr<LogSink> newSink);
    std::shared_ptr<LogSink> sink() const;

    void setMinLevel (Level level);
    Level minLevel() const;
    bool isEnabled (Level level) const;

    // Writes the record if its level is enabled and a sink is installed. Thread-safe.
    void write (const LogRecord& record);

private:
    mutable std::mutex mutex;
    std::shared_ptr<LogSink> currentSink;
    std::atomic<Level> currentMinLevel { defaultLevelForBuild() };
};

// The one process-wide logger: a deliberate exception to "no globals" (ADR 0006).
Logger& processLogger();

// Logs an event now, on the calling thread's role.
void logEvent (Level level, std::string_view event, const Fields& fields = {}, std::string_view msg = {});

// Logs an event captured earlier (RT events): host_ns and thread are the capture's, and `t` is
// back-dated from the host-time difference.
void logEventCapturedAt (Level level,
                         std::string_view event,
                         ThreadRole capturedOn,
                         std::uint64_t capturedHostNs,
                         const Fields& fields);

// Test support: installs a MemoryLogSink on the process logger for its lifetime, then restores the
// previous sink and level.
class ScopedLogCapture
{
public:
    explicit ScopedLogCapture (Level minLevel = Level::debug);
    ~ScopedLogCapture();

    ScopedLogCapture (const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator= (const ScopedLogCapture&) = delete;

    std::vector<std::string> lines() const;
    std::vector<nlohmann::json> events() const; // each line parsed

private:
    std::shared_ptr<MemoryLogSink> memorySink;
    std::shared_ptr<LogSink> previousSink;
    Level previousMinLevel;
};

// Installed with juce::Logger::setCurrentLogger so JUCE's own messages become `app.juce_message`.
class JuceLoggerBridge final : public juce::Logger
{
protected:
    void logMessage (const juce::String& message) override;
};
} // namespace overdub::core
