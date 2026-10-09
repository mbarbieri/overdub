#include "plugin/InvalidationWatch.h"

#include "core/Log.h"

#include <juce_events/juce_events.h>

#include <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

namespace overdub::plugin
{
struct InvalidationWatch::Observer
{
    Handler handler;
    // Expires with the watch, so a notification already handed to the message thread is dropped.
    std::weak_ptr<Observer> self;
};

namespace
{
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

void notified (CFNotificationCenterRef, void* observerPointer, CFNotificationName, const void*, CFDictionaryRef userInfo)
{
    const auto* observer = static_cast<InvalidationWatch::Observer*> (observerPointer);
    const auto* audioUnit = audioUnitIn (userInfo);
    std::weak_ptr<InvalidationWatch::Observer> weak = observer->self;
    juce::MessageManager::callAsync ([weak, audioUnit] {
        const auto alive = weak.lock();
        if (alive == nullptr)
            return;
        if (audioUnit == nullptr)
        {
            core::logEvent (core::Level::debug, "plugin.invalidation_without_unit", {});
            return;
        }
        alive->handler (audioUnit);
    });
}
} // namespace

InvalidationWatch::InvalidationWatch (Handler handler) : observer (std::make_shared<Observer> (Observer { std::move (handler), {} }))
{
    observer->self = observer;
    CFNotificationCenterAddObserver (CFNotificationCenterGetLocalCenter(),
                                     observer.get(),
                                     &notified,
                                     kAudioComponentInstanceInvalidationNotification,
                                     nullptr,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
}

InvalidationWatch::~InvalidationWatch()
{
    CFNotificationCenterRemoveObserver (CFNotificationCenterGetLocalCenter(), observer.get(), kAudioComponentInstanceInvalidationNotification, nullptr);
}
} // namespace overdub::plugin
