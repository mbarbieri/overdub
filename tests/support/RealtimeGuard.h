#pragma once

namespace overdub::testing
{
// What the calling thread did inside a guarded scope that real-time code must never do.
struct RealtimeViolations
{
    int allocations = 0;             // malloc-family and operator new calls
    int deallocations = 0;           // free-family and operator delete calls (null pointers excluded)
    int locks = 0;                   // blocking lock and condition-wait calls
    const char* firstCall = nullptr; // name of the first violating function, e.g. "malloc"
};

namespace detail
{
    // Per-guard state the interposed functions reach through a pthread TSD slot.
struct GuardState
{
    RealtimeViolations record;
    int callDepth = 0; // > 0 while an intercepted call is running on this thread
};
} // namespace detail

// Test-only realtime guard (ADR 0005). While a guard is alive, the constructing thread's
// heap allocations, deallocations and blocking locks are counted into its record. Other
// threads, and the same thread outside the scope, are never counted.
//
// Lives in the overdub_rt_guard dylib, which interposes the libsystem functions through
// dyld, so it also sees calls made inside JUCE, libc++ and system frameworks:
//   allocation: malloc, calloc, realloc, reallocf, valloc, aligned_alloc, posix_memalign,
//               malloc_zone_*, malloc_type_* (operator new/delete reach these via libc++)
//   locks:      pthread_mutex_lock, pthread_rwlock_rdlock/wrlock, pthread_cond_wait/timedwait,
//               os_unfair_lock_lock(_with_flags)
// Not caught: try-lock calls (they never block), spin locks built on atomics (juce::SpinLock),
// calls libsystem makes internally within one library, and direct syscalls.
// Guards nest: an inner guard records into its own record and restores the outer one.
class ScopedRealtimeGuard
{
public:
    ScopedRealtimeGuard();
    ~ScopedRealtimeGuard();
    ScopedRealtimeGuard (const ScopedRealtimeGuard&) = delete;
    ScopedRealtimeGuard& operator= (const ScopedRealtimeGuard&) = delete;

    RealtimeViolations violations() const { return state.record; }

private:
    detail::GuardState state;
    void* previousState = nullptr;
};

// Runs fn on this thread inside a guard and returns what it did. fn must not use Catch2
// assertions or anything else that allocates unless that is what the test measures.
template <typename Fn>
RealtimeViolations runRealtimeGuarded (Fn&& fn)
{
    ScopedRealtimeGuard guard;
    fn();
    return guard.violations();
}

// Lets a pointer escape to the optimizer, and returns it. Release builds may elide a matched
// new/delete or malloc/free pair whose pointer never escapes, so a test that measures a
// deliberate allocation passes it through here: `delete keepAllocation (new int (1));`.
template <typename T>
T* keepAllocation (T* pointer)
{
    asm volatile ("" : : "r"(pointer) : "memory");
    return pointer;
}
} // namespace overdub::testing
