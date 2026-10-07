# overdub_rt_guard: test-only realtime guard (ADR 0005). Never link it into Overdub.app.
# It is a SHARED library because it interposes malloc-family and lock functions through dyld's
# __DATA,__interpose section, which dyld only honours in dylibs loaded at launch, not in the
# main executable. See RealtimeGuard.h for what it catches.

add_library(overdub_rt_guard SHARED)

target_sources(overdub_rt_guard PRIVATE
    RealtimeGuard.cpp)

target_include_directories(overdub_rt_guard PUBLIC "${PROJECT_SOURCE_DIR}/tests")
