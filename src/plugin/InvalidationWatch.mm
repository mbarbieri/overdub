#include "plugin/InvalidationWatch.h"

#include "core/Log.h"

#include <juce_events/juce_events.h>

#include <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

namespace overdub::plugin
{
struct InvalidationWatch::Observer
{
    Handler handler;
};

namespace
{
// Core Audio may be delivering a notification on its thread while the watch is destroyed on the
// message thread, so the CF observer is a number, never a pointer: a delivery looks its watch up
// here under the lock and finds nothing once the watch has unregistered.
struct Registry
{
    std::mutex lock;
    std::uintptr_t nextKey = 1;
    std::map<std::uintptr_t, std::weak_ptr<InvalidationWatch::Observer>> observers;
};

std::atomic<InvalidationWatch::Receipt> receiptsSoFar { 0 };

Registry& registry()
{
    static Registry instance;
    return instance;
}

// The AU instance the notification names (the "audioUnit" NSValue), or null.
const void* audioUnitIn (CFDictionaryRef userInfo)
{
    NSDictionary* info = (__bridge NSDictionary*) userInfo;
    if (! [info isKindOfClass:[NSDictionary class]])
        return nullptr;
    id value = info[@"audioUnit"];
    if (! [value isKindOfClass:[NSValue class]])
        return nullptr;
    return [(NSValue*) value pointerValue];
}

void notified (CFNotificationCenterRef, void* observerKey, CFNotificationName, const void*, CFDictionaryRef userInfo)
{
    // Numbered before anything else: an AU that becomes ready from here on is not the one named.
    const auto receipt = receiptsSoFar.fetch_add (1, std::memory_order_acq_rel) + 1;
    std::weak_ptr<InvalidationWatch::Observer> weak;
    {
        auto& known = registry();
        const std::scoped_lock held { known.lock };
        const auto found = known.observers.find (reinterpret_cast<std::uintptr_t> (observerKey));
        if (found == known.observers.end())
            return;
        weak = found->second;
    }
    const auto* audioUnit = audioUnitIn (userInfo);
    juce::MessageManager::callAsync ([weak, audioUnit, receipt] {
        const auto alive = weak.lock();
        if (alive == nullptr)
            return;
        if (audioUnit == nullptr)
        {
            core::logEvent (core::Level::debug, "plugin.invalidation_without_unit", {});
            return;
        }
        alive->handler (audioUnit, receipt);
    });
}
} // namespace

InvalidationWatch::Receipt InvalidationWatch::lastReceipt()
{
    return receiptsSoFar.load (std::memory_order_acquire);
}

InvalidationWatch::InvalidationWatch (Handler handler) : observer (std::make_shared<Observer> (Observer { std::move (handler) }))
{
    {
        auto& known = registry();
        const std::scoped_lock held { known.lock };
        key = known.nextKey++;
        known.observers[key] = observer;
    }
    CFNotificationCenterAddObserver (CFNotificationCenterGetLocalCenter(),
                                     reinterpret_cast<const void*> (key),
                                     &notified,
                                     kAudioComponentInstanceInvalidationNotification,
                                     nullptr,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
}

InvalidationWatch::~InvalidationWatch()
{
    CFNotificationCenterRemoveObserver (CFNotificationCenterGetLocalCenter(), reinterpret_cast<const void*> (key), kAudioComponentInstanceInvalidationNotification, nullptr);
    auto& known = registry();
    const std::scoped_lock held { known.lock };
    known.observers.erase (key);
}
} // namespace overdub::plugin
