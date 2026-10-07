# overdub_tests: one Catch2 executable for every test, registered with ctest per test case.
# Add test files to target_sources below, in a folder mirroring src/ (tests/core/, tests/engine/, ...).

# Test-support libraries linked only by overdub_tests (never by Overdub.app).
add_subdirectory(support)

add_executable(overdub_tests)

target_sources(overdub_tests PRIVATE
    core/BuildInfoTest.cpp
    core/ErrorTest.cpp
    core/LogFilesTest.cpp
    core/LogTest.cpp
    core/RtEventRingTest.cpp
    engine/DeviceBindingTest.cpp
    engine/DeviceSettingsTest.cpp
    engine/EngineEventsTest.cpp
    engine/EngineRealtimeTest.cpp
    engine/EngineTest.cpp
    song/BackingTrackImportTest.cpp
    song/SongJsonTest.cpp
    song/SongMigrationTest.cpp
    song/PeaksTest.cpp
    song/SongPackageTest.cpp
    support/RealtimeGuardTest.cpp
    support/RealtimeGuardCatchTest.cpp)

# overdub_engine links overdub_song and overdub_core publicly; listing those too makes ld warn
# about duplicate libraries.
target_link_libraries(overdub_tests PRIVATE
    overdub_engine
    overdub_rt_guard
    Catch2::Catch2WithMain
    juce::juce_recommended_warning_flags)

include("${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake")

# PRE_TEST: list tests when ctest runs, not as a post-build step, so a crashing test binary
# shows up as a failing test instead of a failing build.
catch_discover_tests(overdub_tests DISCOVERY_MODE PRE_TEST)
