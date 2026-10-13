#include "plugin/InvalidationWatch.h"
#include "plugin/InsertHost.h"
#include "plugin/PluginKind.h"

#include "PluginTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <thread>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::plugin::audioUnitHandle;
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

// A hosting process id no test used before (above macOS's highest pid, 99998): the watch
// remembers every process whose death it heard, for the whole test run.
int freshServicePid()
{
    static std::atomic<int> next { 1'000'000 };
    return next++;
}

void postInvalidationOf (const void* audioUnit, int servicePid)
{
    postInvalidation (@{@"audioUnit" : [NSValue valueWithPointer:audioUnit],
                        @"Service PID" : @(servicePid)});
}

void postInvalidationOf (const void* audioUnit)
{
    postInvalidationOf (audioUnit, freshServicePid());
}

bool unmatchedLogged (const ScopedLogCapture& capture, bool reusedAddress)
{
    for (const auto& event : capture.events())
        if (event.at ("event") == "plugin.invalidation_unmatched" && event.at ("reused_address") == reusedAddress)
            return true;
    return false;
}
} // namespace

TEST_CASE ("an invalidation notification reaches the handler on the message thread with the AU it names", "[plugin][crash][invalidation]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    int fakeUnit = 0;
    const void* named = nullptr;
    InvalidationWatch::Receipt receipt = 0;
    bool onMessageThread = false;
    InvalidationWatch watch { [&] (const void* audioUnit, InvalidationWatch::Receipt received) {
        named = audioUnit;
        receipt = received;
        onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
    } };
    const auto receivedBefore = InvalidationWatch::lastReceipt();

    postInvalidationOf (&fakeUnit);

    REQUIRE (runMessagesUntil ([&] { return named != nullptr; }, std::chrono::seconds (5)));
    CHECK (named == &fakeUnit);
    CHECK (onMessageThread);
    // Numbered when received, after every notification received before it.
    CHECK (receipt > receivedBefore);
    CHECK (receipt == InvalidationWatch::lastReceipt());
}

TEST_CASE ("a repeated notification for a hosting process reaches the handler with the Receipt of the first", "[plugin][crash][invalidation]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    int fakeUnit = 0;
    std::vector<InvalidationWatch::Receipt> receipts;
    InvalidationWatch watch { [&] (const void*, InvalidationWatch::Receipt received) { receipts.push_back (received); } };
    const auto deadService = freshServicePid();
    ScopedLogCapture capture;

    postInvalidationOf (&fakeUnit, deadService);
    const auto first = InvalidationWatch::lastReceipt();
    postInvalidationOf (&fakeUnit, deadService); // the same death, posted again
    postInvalidationOf (&fakeUnit, freshServicePid()); // another process
    const auto other = InvalidationWatch::lastReceipt();
    postInvalidation (@ { @"audioUnit" : [NSValue valueWithPointer:&fakeUnit] }); // names no process
    const auto unnamed = InvalidationWatch::lastReceipt();

    REQUIRE (runMessagesUntil ([&] { return receipts.size() == 4; }, std::chrono::seconds (5)));
    CHECK (receipts == std::vector<InvalidationWatch::Receipt> { first, first, other, unnamed });
    CHECK (other == first + 2); // the repeat was numbered all the same
    CHECK (unnamed == other + 1);
    int repeatsLogged = 0;
    for (const auto& event : capture.events())
        if (event.at ("event") == "plugin.invalidation_repeat")
        {
            ++repeatsLogged;
            CHECK (event.at ("service_pid") == deadService);
            CHECK (event.at ("first_receipt") == first);
        }
    CHECK (repeatsLogged == 1);
}

TEST_CASE ("an invalidation that names no AU, or arrives after the watch is gone, calls nothing", "[plugin][crash][invalidation]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    int calls = 0;
    ScopedLogCapture capture;
    {
        InvalidationWatch watch { [&calls] (const void*, InvalidationWatch::Receipt) { ++calls; } };
        postInvalidation (@{@"Service PID" : @(freshServicePid())});
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

TEST_CASE ("a notification received before a slot's AU loaded leaves that AU ready even when it names its address", "[plugin][au][crash]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    InsertHost host { loader.instantiator(), {}, 48000.0, 256 };
    auto song = overdub::song::makeEmptySong (48000);
    song.master.insertChain = { peakLimiterSlot ("slot-old") };
    host.sync (song);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-old") == InsertStatus::ready; }));
    // The old AU's process dies: its notification is received now and reaches the message thread
    // only after the old AU is gone and a new one is ready, possibly at the same address.
    postInvalidationOf (overdub::plugin::audioUnitHandle (*host.processor ("slot-old")));
    // The local center delivers synchronously on the posting thread: the receipt is already taken.
    const auto receivedForOldUnit = InvalidationWatch::lastReceipt();
    song.master.insertChain = { peakLimiterSlot ("slot-new") };
    host.sync (song);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-new") == InsertStatus::ready; }));
    const auto* newUnit = overdub::plugin::audioUnitHandle (*host.processor ("slot-new"));
    REQUIRE (newUnit != nullptr);
    host.takeCrashes();
    ScopedLogCapture capture;

    host.audioUnitInvalidated (newUnit, receivedForOldUnit); // the old AU's address, reused

    CHECK (host.status ("slot-new") == InsertStatus::ready);
    CHECK (host.takeCrashes().empty());
    bool reusedAddressLogged = false;
    for (const auto& event : capture.events())
        if (event.at ("event") == "plugin.invalidation_unmatched")
            reusedAddressLogged = event.at ("reused_address") == true;
    CHECK (reusedAddressLogged);

    // A notification received once the new AU is ready still crashes it.
    postInvalidationOf (newUnit);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-new") == InsertStatus::crashed; }, std::chrono::seconds (5)));
    CHECK (host.takeCrashes().size() == 1);
}

TEST_CASE ("a notification received while a slot's AU loads still crashes it once it is ready", "[plugin][au][crash]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    InsertHost host { loader.instantiator(), {}, 48000.0, 256 };
    auto song = overdub::song::makeEmptySong (48000);
    song.master.insertChain = { peakLimiterSlot() };
    host.sync (song);
    int otherUnit = 0;
    postInvalidationOf (&otherUnit); // received after the load started, as the loading AU's own would be
    const auto receivedWhileLoading = InvalidationWatch::lastReceipt();
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::ready; }));
    const auto* audioUnit = overdub::plugin::audioUnitHandle (*host.processor ("slot-limiter"));
    REQUIRE (audioUnit != nullptr);

    host.audioUnitInvalidated (audioUnit, receivedWhileLoading);

    CHECK (host.status ("slot-limiter") == InsertStatus::crashed);
    CHECK (host.takeCrashes().size() == 1);
}

TEST_CASE ("a repeated notification for a dead AU leaves the AU reloaded at its address ready, while that AU's own crash still crashes it", "[plugin][au][crash]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    InsertHost host { loader.instantiator(), {}, 48000.0, 256 };
    auto song = overdub::song::makeEmptySong (48000);
    song.master.insertChain = { peakLimiterSlot() };
    host.sync (song);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::ready; }));
    const auto deadService = freshServicePid();
    postInvalidationOf (audioUnitHandle (*host.processor ("slot-limiter")), deadService);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::crashed; }, std::chrono::seconds (5)));
    REQUIRE (host.takeCrashes().size() == 1);
    REQUIRE (host.reloadCrashed (song) == 1);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::ready; }));
    const auto* reloadedUnit = audioUnitHandle (*host.processor ("slot-limiter"));
    REQUIRE (reloadedUnit != nullptr);
    ScopedLogCapture capture;

    // Core Audio repeats the dead process's notification after the reload was requested; the
    // dead AU's address is the reloaded AU's.
    postInvalidationOf (reloadedUnit, deadService);
    REQUIRE (runMessagesUntil ([&] { return unmatchedLogged (capture, true); }, std::chrono::seconds (5)));

    CHECK (host.status ("slot-limiter") == InsertStatus::ready);
    CHECK (host.takeCrashes().empty());

    // The reloaded AU's own hosting process dies.
    postInvalidationOf (reloadedUnit, freshServicePid());
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::crashed; }, std::chrono::seconds (5)));
    CHECK (host.takeCrashes().size() == 1);
}

TEST_CASE ("a repeat without Service PID for a dead AU's address crashes the AU reloaded there (ADR 0002)", "[plugin][au][crash]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioUnitLoader loader;
    InsertHost host { loader.instantiator(), {}, 48000.0, 256 };
    auto song = overdub::song::makeEmptySong (48000);
    song.master.insertChain = { peakLimiterSlot() };
    host.sync (song);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::ready; }));
    postInvalidationOf (audioUnitHandle (*host.processor ("slot-limiter")));
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::crashed; }, std::chrono::seconds (5)));
    REQUIRE (host.takeCrashes().size() == 1);
    REQUIRE (host.reloadCrashed (song) == 1);
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::ready; }));
    const auto* reloadedUnit = audioUnitHandle (*host.processor ("slot-limiter"));
    REQUIRE (reloadedUnit != nullptr);
    ScopedLogCapture capture;

    // Without a Service PID the watch cannot tell a repeat of the dead process's notification
    // from the reloaded AU's own crash, so it keeps its own Receipt and the slot crashes.
    postInvalidation (@{@"audioUnit" : [NSValue valueWithPointer:reloadedUnit]});
    REQUIRE (runMessagesUntil ([&] { return host.status ("slot-limiter") == InsertStatus::crashed; }, std::chrono::seconds (5)));

    CHECK (host.takeCrashes().size() == 1);
    bool invalidatedLogged = false;
    for (const auto& event : capture.events())
    {
        CHECK (event.at ("event") != "plugin.invalidation_repeat");
        CHECK (event.at ("event") != "plugin.invalidation_unmatched");
        if (event.at ("event") == "plugin.invalidated")
            invalidatedLogged = event.at ("slot_id") == "slot-limiter";
    }
    CHECK (invalidatedLogged);
}
