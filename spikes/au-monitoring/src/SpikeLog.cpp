// PROTOTYPE — issue #6 spike.
#include "SpikeLog.h"
#include <juce_events/juce_events.h>
#include <cstdio>

namespace spikelog
{
namespace
{
std::unique_ptr<juce::FileOutputStream> logFile;
std::function<void (const juce::String&)> uiSink;
juce::CriticalSection lock;
}

juce::File logDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
        .getChildFile ("Library/Logs/OverdubAuSpike");
}

void install()
{
    logDirectory().createDirectory();
    auto file = logDirectory().getChildFile ("spike.log");
    logFile = std::make_unique<juce::FileOutputStream> (file);
    std::fprintf (stderr, "log file: %s\n", file.getFullPathName().toRawUTF8());
}

void setUiSink (std::function<void (const juce::String&)> sink)
{
    const juce::ScopedLock sl (lock);
    uiSink = std::move (sink);
}

void line (const juce::String& event, const juce::StringPairArray& fields)
{
    juce::String text = juce::Time::getCurrentTime().toISO8601 (true) + " event=" + event;

    for (auto& key : fields.getAllKeys())
    {
        auto value = fields[key];
        text << " " << key << "=" << (value.containsChar (' ') ? value.quoted() : value);
    }

    const juce::ScopedLock sl (lock);
    std::fprintf (stderr, "%s\n", text.toRawUTF8());
    std::fflush (stderr);

    if (logFile != nullptr)
    {
        logFile->writeText (text + "\n", false, false, nullptr);
        logFile->flush();
    }

    if (uiSink)
    {
        auto sink = uiSink;
        juce::MessageManager::callAsync ([sink, text] { sink (text); });
    }
}
}

void spikelog::line (const juce::String& event, std::initializer_list<std::pair<juce::String, juce::String>> fields)
{
    juce::StringPairArray pairs;
    for (auto& [key, value] : fields)
        pairs.set (key, value);
    line (event, pairs);
}
