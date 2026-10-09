#include "plugin/InvalidationWatch.h"
#include "plugin/InsertHost.h"
#include "plugin/PluginKind.h"

#include "PluginTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <thread>

using overdub::core::ScopedLogCapture;
using overdub::plugin::AudioUnitLoader;
using overdub::plugin::InsertHost;
using overdub::plugin::InsertStatus;
using overdub::plugin::InvalidationWatch;
using namespace overdub::plugin::testing;

// Crash detection (issue #40, ADR 0002). Killing a real AUHostingService would take down units of
// other apps, so these tests post kAudioComponentInstanceInvalidationNotification themselves, as
// Core Audio does when that process dies: from another thread, naming the AU in its userInfo.
namespace
{
void postInvalidation (NSDictionary* userInfo)
{
    // Core Audio posts from its own threads, never the message thread.
    std::thread poster ([userInfo] {
        CFNotificationCenterPostNotification (CFNotificationCenterGetLocalCenter(), kAudioComponentInstanceInvalidationNotification, nullptr, (__bridge CFDictionaryRef) userInfo, true);
    });
    poster.join();
}

void postInvalidationOf (const void* audioUnit)
{
    postInvalidation (@{ @"audioUnit" : [NSValue valueWithPointer:audioUnit], @"Service PID" : @1 });
}
} // namespace

TEST_CASE ("an invalidation notification reaches the handler on the message thread with the AU it names", "[plugin][crash][invalidation]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    int fakeUnit = 0;
    const void* named = nullptr;
    bool onMessageThread = false;
    InvalidationWatch watch { [&] (const void* audioUnit) {
        named = audioUnit;
        onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
    } };

    postInvalidationOf (&fakeUnit);

    REQUIRE (runMessagesUntil ([&] { return named != nullptr; }, std::chrono::seconds (5)));
    CHECK (named == &fakeUnit);
    CHECK (onMessageThread);
}

TEST_CASE ("an invalidation that names no AU, or arrives after the watch is gone, calls nothing", "[plugin][crash][invalidation]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    int calls = 0;
    ScopedLogCapture capture;
    {
        InvalidationWatch watch { [&calls] (const void*) { ++calls; } };
        postInvalidation (@{ @"Service PID" : @1 });
        REQUIRE (runMessagesUntil ([&] {
            for (const auto& event : capture.events())
                if (event.at ("event") == "plugin.invalidation_without_unit")
                    return true;
            return false;
        }, std::chrono::seconds (5)));

        int fakeUnit = 0;
        postInvalidationOf (&fakeUnit); // handed to the message thread, delivered after the watch is gone
    }
    runMessagesFor (std::chrono::milliseconds (200));

    CHECK (calls == 0);
}

TEST_CASE ("an AU whose hosting process is invalidated makes its slot crashed in the InsertHost", "[plugin][au][crash]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    InsertHost host { loader.instantiator(), {}, 48000.0, 256 };
    auto song = overdub::song::makeEmptySong (48000);
    song.master.insertChain = { peakLimiterSlot() };
    host.sync (song);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::ready; }));
    const auto* audioUnit = overdub::plugin::audioUnitHandle (*host.processor ("slot-limiter"));
    REQUIRE (audioUnit != nullptr);
    ScopedLogCapture capture;

    postInvalidationOf (audioUnit);

    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::crashed; }, std::chrono::seconds (5)));
    const auto crashes = host.takeCrashes();
    REQUIRE (crashes.size() == 1);
    CHECK (crashes[0].slotId == "slot-limiter");
    bool logged = false;
    for (const auto& event : capture.events())
        if (event.at ("event") == "plugin.invalidated")
            logged = event.at ("out_of_process") == true && event.at ("component") == "aufx,lmtr,appl";
    CHECK (logged);
}
