#pragma once

#include <functional>
#include <memory>

// Hears kAudioComponentInstanceInvalidationNotification (issue #40, ADR 0002): Core Audio posts
// it when the connection to an AU's hosting process (AUHostingService) is lost, for instance
// because that process crashed. One process may host several AUs, and one crash may post more
// than once, so the handler must be idempotent (plugin::InsertHost::audioUnitInvalidated is).
//
// The notification arrives on whatever thread Core Audio posts it from; the handler is called
// later on the message thread with the AU instance (AudioUnit) its userInfo names. A notification
// that names no instance is dropped (logged as plugin.invalidation_without_unit, debug). The
// handler is never called once the watch is destroyed.
//
// Construct and destroy on the message thread.
namespace overdub::plugin
{
class InvalidationWatch
{
public:
    using Handler = std::function<void (const void* audioUnit)>;

    explicit InvalidationWatch (Handler handler);
    ~InvalidationWatch();

    InvalidationWatch (const InvalidationWatch&) = delete;
    InvalidationWatch& operator= (const InvalidationWatch&) = delete;

    // Core Audio's callback: hands the named AU instance to the message thread.
    struct Observer;

private:
    std::shared_ptr<Observer> observer;
};
} // namespace overdub::plugin
