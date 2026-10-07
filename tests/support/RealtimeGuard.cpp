#include "support/RealtimeGuard.h"

#include <malloc/malloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdlib.h>

// Invariant: nothing reachable from an interposed function may allocate or lock, or the
// wrapper recurses into itself. That is why the active guard state lives in a pthread TSD slot
// (pthread_getspecific never allocates) and not in a C++ thread_local (macOS allocates
// thread_local storage lazily with malloc on first access).

namespace
{
using overdub::testing::RealtimeViolations;

pthread_key_t activeStateKey;
bool activeStateKeyReady = false; // false until this dylib's initializer runs

__attribute__ ((constructor)) void createActiveStateKey()
{
    activeStateKeyReady = pthread_key_create (&activeStateKey, nullptr) == 0;
}

overdub::testing::detail::GuardState* activeState()
{
    if (! activeStateKeyReady)
        return nullptr;
    return static_cast<overdub::testing::detail::GuardState*> (pthread_getspecific (activeStateKey));
}

enum class ViolationKind
{
    allocation,
    deallocation,
    lock
};

// Counts one intercepted call against the thread's active guard, if any. Calls made while
// another intercepted call is in progress on the same thread are not counted: one logical
// operation (operator new -> malloc_type_malloc -> malloc) is one violation.
class InterceptedCall
{
public:
    InterceptedCall (ViolationKind kind, const char* functionName, bool isViolation = true)
        : state (activeState())
    {
        if (state == nullptr)
            return;
        if (state->callDepth == 0 && isViolation)
            count (state->record, kind, functionName);
        ++state->callDepth;
    }

    ~InterceptedCall()
    {
        if (state != nullptr)
            --state->callDepth;
    }

    InterceptedCall (const InterceptedCall&) = delete;
    InterceptedCall& operator= (const InterceptedCall&) = delete;

private:
    static void count (RealtimeViolations& record, ViolationKind kind, const char* functionName)
    {
        switch (kind)
        {
            case ViolationKind::allocation: ++record.allocations; break;
            case ViolationKind::deallocation: ++record.deallocations; break;
            case ViolationKind::lock: ++record.locks; break;
        }
        if (record.firstCall == nullptr)
            record.firstCall = functionName;
    }

    overdub::testing::detail::GuardState* state;
};

// Calls below reach the real libsystem functions: dyld never applies an image's own
// interpose tuples to that image's references.

void* guardedMalloc (size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "malloc");
    return malloc (size);
}

void* guardedCalloc (size_t count, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "calloc");
    return calloc (count, size);
}

void* guardedRealloc (void* pointer, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "realloc");
    return realloc (pointer, size);
}

void* guardedReallocf (void* pointer, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "reallocf");
    return reallocf (pointer, size);
}

void* guardedValloc (size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "valloc");
    return valloc (size);
}

void* guardedAlignedAlloc (size_t alignment, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "aligned_alloc");
    return aligned_alloc (alignment, size);
}

int guardedPosixMemalign (void** result, size_t alignment, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "posix_memalign");
    return posix_memalign (result, alignment, size);
}

void guardedFree (void* pointer)
{
    InterceptedCall call (ViolationKind::deallocation, "free", pointer != nullptr);
    free (pointer);
}

void* guardedZoneMalloc (malloc_zone_t* zone, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_zone_malloc");
    return malloc_zone_malloc (zone, size);
}

void* guardedZoneCalloc (malloc_zone_t* zone, size_t count, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_zone_calloc");
    return malloc_zone_calloc (zone, count, size);
}

void* guardedZoneRealloc (malloc_zone_t* zone, void* pointer, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_zone_realloc");
    return malloc_zone_realloc (zone, pointer, size);
}

void* guardedZoneValloc (malloc_zone_t* zone, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_zone_valloc");
    return malloc_zone_valloc (zone, size);
}

void* guardedZoneMemalign (malloc_zone_t* zone, size_t alignment, size_t size)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_zone_memalign");
    return malloc_zone_memalign (zone, alignment, size);
}

void guardedZoneFree (malloc_zone_t* zone, void* pointer)
{
    InterceptedCall call (ViolationKind::deallocation, "malloc_zone_free", pointer != nullptr);
    malloc_zone_free (zone, pointer);
}

// Typed-allocation entry points (macOS 14+). Clang rewrites malloc calls to these when
// typed memory operations are enabled, and system libraries call them directly.

void* guardedTypeMalloc (size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_malloc");
    return malloc_type_malloc (size, typeId);
}

void* guardedTypeCalloc (size_t count, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_calloc");
    return malloc_type_calloc (count, size, typeId);
}

void* guardedTypeRealloc (void* pointer, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_realloc");
    return malloc_type_realloc (pointer, size, typeId);
}

void* guardedTypeValloc (size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_valloc");
    return malloc_type_valloc (size, typeId);
}

void* guardedTypeAlignedAlloc (size_t alignment, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_aligned_alloc");
    return malloc_type_aligned_alloc (alignment, size, typeId);
}

int guardedTypePosixMemalign (void** result, size_t alignment, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_posix_memalign");
    return malloc_type_posix_memalign (result, alignment, size, typeId);
}

void guardedTypeFree (void* pointer, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::deallocation, "malloc_type_free", pointer != nullptr);
    malloc_type_free (pointer, typeId);
}

void* guardedTypeZoneMalloc (malloc_zone_t* zone, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_zone_malloc");
    return malloc_type_zone_malloc (zone, size, typeId);
}

void* guardedTypeZoneCalloc (malloc_zone_t* zone, size_t count, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_zone_calloc");
    return malloc_type_zone_calloc (zone, count, size, typeId);
}

void* guardedTypeZoneRealloc (malloc_zone_t* zone, void* pointer, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_zone_realloc");
    return malloc_type_zone_realloc (zone, pointer, size, typeId);
}

void* guardedTypeZoneValloc (malloc_zone_t* zone, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_zone_valloc");
    return malloc_type_zone_valloc (zone, size, typeId);
}

void* guardedTypeZoneMemalign (malloc_zone_t* zone, size_t alignment, size_t size, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::allocation, "malloc_type_zone_memalign");
    return malloc_type_zone_memalign (zone, alignment, size, typeId);
}

void guardedTypeZoneFree (malloc_zone_t* zone, void* pointer, malloc_type_id_t typeId)
{
    InterceptedCall call (ViolationKind::deallocation, "malloc_type_zone_free", pointer != nullptr);
    malloc_type_zone_free (zone, pointer, typeId);
}

// Blocking lock and wait calls. try-lock variants never block, so they are not violations.

int guardedMutexLock (pthread_mutex_t* mutex)
{
    InterceptedCall call (ViolationKind::lock, "pthread_mutex_lock");
    return pthread_mutex_lock (mutex);
}

int guardedRwlockRdlock (pthread_rwlock_t* rwlock)
{
    InterceptedCall call (ViolationKind::lock, "pthread_rwlock_rdlock");
    return pthread_rwlock_rdlock (rwlock);
}

int guardedRwlockWrlock (pthread_rwlock_t* rwlock)
{
    InterceptedCall call (ViolationKind::lock, "pthread_rwlock_wrlock");
    return pthread_rwlock_wrlock (rwlock);
}

int guardedCondWait (pthread_cond_t* condition, pthread_mutex_t* mutex)
{
    InterceptedCall call (ViolationKind::lock, "pthread_cond_wait");
    return pthread_cond_wait (condition, mutex);
}

int guardedCondTimedwait (pthread_cond_t* condition, pthread_mutex_t* mutex, const struct timespec* deadline)
{
    InterceptedCall call (ViolationKind::lock, "pthread_cond_timedwait");
    return pthread_cond_timedwait (condition, mutex, deadline);
}

void guardedUnfairLock (os_unfair_lock_t lock)
{
    InterceptedCall call (ViolationKind::lock, "os_unfair_lock_lock");
    os_unfair_lock_lock (lock);
}

void guardedUnfairLockWithFlags (os_unfair_lock_t lock, os_unfair_lock_flags_t flags)
{
    InterceptedCall call (ViolationKind::lock, "os_unfair_lock_lock_with_flags");
    os_unfair_lock_lock_with_flags (lock, flags);
}

// dyld interpose tuples (the layout of DYLD_INTERPOSE in dyld's <mach-o/dyld-interposing.h>).
struct InterposeTuple
{
    const void* replacement;
    const void* original;
};

#define OVERDUB_INTERPOSE(replacement, original)                                                \
    InterposeTuple                                                                              \
    {                                                                                           \
        reinterpret_cast<const void*> (&replacement), reinterpret_cast<const void*> (&original) \
    }

__attribute__ ((used, section ("__DATA,__interpose"))) const InterposeTuple interposeTuples[] = {
    OVERDUB_INTERPOSE (guardedMalloc, malloc),
    OVERDUB_INTERPOSE (guardedCalloc, calloc),
    OVERDUB_INTERPOSE (guardedRealloc, realloc),
    OVERDUB_INTERPOSE (guardedReallocf, reallocf),
    OVERDUB_INTERPOSE (guardedValloc, valloc),
    OVERDUB_INTERPOSE (guardedAlignedAlloc, aligned_alloc),
    OVERDUB_INTERPOSE (guardedPosixMemalign, posix_memalign),
    OVERDUB_INTERPOSE (guardedFree, free),
    OVERDUB_INTERPOSE (guardedZoneMalloc, malloc_zone_malloc),
    OVERDUB_INTERPOSE (guardedZoneCalloc, malloc_zone_calloc),
    OVERDUB_INTERPOSE (guardedZoneRealloc, malloc_zone_realloc),
    OVERDUB_INTERPOSE (guardedZoneValloc, malloc_zone_valloc),
    OVERDUB_INTERPOSE (guardedZoneMemalign, malloc_zone_memalign),
    OVERDUB_INTERPOSE (guardedZoneFree, malloc_zone_free),
    OVERDUB_INTERPOSE (guardedTypeMalloc, malloc_type_malloc),
    OVERDUB_INTERPOSE (guardedTypeCalloc, malloc_type_calloc),
    OVERDUB_INTERPOSE (guardedTypeRealloc, malloc_type_realloc),
    OVERDUB_INTERPOSE (guardedTypeValloc, malloc_type_valloc),
    OVERDUB_INTERPOSE (guardedTypeAlignedAlloc, malloc_type_aligned_alloc),
    OVERDUB_INTERPOSE (guardedTypePosixMemalign, malloc_type_posix_memalign),
    OVERDUB_INTERPOSE (guardedTypeFree, malloc_type_free),
    OVERDUB_INTERPOSE (guardedTypeZoneMalloc, malloc_type_zone_malloc),
    OVERDUB_INTERPOSE (guardedTypeZoneCalloc, malloc_type_zone_calloc),
    OVERDUB_INTERPOSE (guardedTypeZoneRealloc, malloc_type_zone_realloc),
    OVERDUB_INTERPOSE (guardedTypeZoneValloc, malloc_type_zone_valloc),
    OVERDUB_INTERPOSE (guardedTypeZoneMemalign, malloc_type_zone_memalign),
    OVERDUB_INTERPOSE (guardedTypeZoneFree, malloc_type_zone_free),
    OVERDUB_INTERPOSE (guardedMutexLock, pthread_mutex_lock),
    OVERDUB_INTERPOSE (guardedRwlockRdlock, pthread_rwlock_rdlock),
    OVERDUB_INTERPOSE (guardedRwlockWrlock, pthread_rwlock_wrlock),
    OVERDUB_INTERPOSE (guardedCondWait, pthread_cond_wait),
    OVERDUB_INTERPOSE (guardedCondTimedwait, pthread_cond_timedwait),
    OVERDUB_INTERPOSE (guardedUnfairLock, os_unfair_lock_lock),
    OVERDUB_INTERPOSE (guardedUnfairLockWithFlags, os_unfair_lock_lock_with_flags),
};

#undef OVERDUB_INTERPOSE
} // namespace

namespace overdub::testing
{
ScopedRealtimeGuard::ScopedRealtimeGuard()
{
    if (! activeStateKeyReady)
        abort(); // the dylib initializer did not run: the guard would silently see nothing
    previousState = pthread_getspecific (activeStateKey);
    pthread_setspecific (activeStateKey, &state);
}

ScopedRealtimeGuard::~ScopedRealtimeGuard()
{
    pthread_setspecific (activeStateKey, previousState);
}
} // namespace overdub::testing
