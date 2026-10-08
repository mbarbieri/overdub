# overdub_tests: one Catch2 executable for every test, registered with ctest per test case.
# Add test files to target_sources below, in a folder mirroring src/ (tests/core/, tests/engine/, ...).

# Test-support libraries linked only by overdub_tests (never by Overdub.app).
add_subdirectory(support)

add_executable(overdub_tests)

target_sources(overdub_tests PRIVATE
    capture/CameraCalibrationTest.cpp
    capture/ClapMeasurementTest.cpp
    capture/ClapSelftestTest.cpp
    capture/LoopbackMeasurementTest.cpp
    capture/MovieRoundTripTest.cpp
    capture/PassRecorderTest.cpp
    capture/PassRecorderVideoTest.cpp
    capture/VideoPlacementTest.cpp
    core/BuildInfoTest.cpp
    core/ErrorTest.cpp
    core/LogFilesTest.cpp
    core/LogTest.cpp
    core/RtEventRingTest.cpp
    engine/DeviceBindingTest.cpp
    engine/DeviceSettingsTest.cpp
    engine/EngineClipEditTest.cpp
    engine/EngineClipGainFadeTest.cpp
    engine/EngineEventsTest.cpp
    engine/EngineInsertChainTest.cpp
    engine/EngineRealtimeTest.cpp
    engine/EngineRecordTest.cpp
    engine/EngineMixerTest.cpp
    engine/LatencyCalibrationTest.cpp
    engine/MixLawTest.cpp
    engine/EngineTakeLanesTest.cpp
    engine/EngineTest.cpp
    export/MixExportTest.cpp
    export/MixRenderTest.cpp
    plugin/AudioUnitIdentifierTest.cpp
    plugin/AudioUnitLoaderTest.cpp
    plugin/InsertBypassTest.cpp
    plugin/InsertHostTest.cpp
    song/BackingTrackImportTest.cpp
    song/ClipEditsTest.cpp
    song/InsertEditsTest.cpp
    song/MixEditsTest.cpp
    song/SongJsonTest.cpp
    song/SongMigrationTest.cpp
    song/PeaksTest.cpp
    song/SongPackageTest.cpp
    song/TakeEditsTest.cpp
    support/RealtimeGuardTest.cpp
    support/RealtimeGuardCatchTest.cpp
    ui/CommandsTest.cpp
    ui/DeviceSettingsPanelTest.cpp
    ui/EditPageTest.cpp
    ui/ExportPageTest.cpp
    ui/InsertChainUiTest.cpp
    ui/LayoutTest.cpp
    ui/MainComponentTest.cpp
    ui/MixerGeometryTest.cpp
    ui/MixerUiTest.cpp
    ui/OpenSongCameraTest.cpp
    ui/OpenSongInsertsTest.cpp
    ui/OpenSongTest.cpp
    ui/PreRollTest.cpp
    ui/RecordCameraTest.cpp
    ui/RecordInsertsTest.cpp
    ui/RecordPageTest.cpp
    ui/RecordViewTest.cpp
    ui/SnapshotTest.cpp
    ui/SongFilesTest.cpp
    ui/TakeLanesTest.cpp
    ui/TakeLanesUiTest.cpp
    ui/TimelineGeometryTest.cpp
    ui/TrackRowEditsTest.cpp
    ui/TransportTextTest.cpp
    ui/UserMessagesTest.cpp
    ui/VideoPreviewTest.cpp
    video/FrameSelectionTest.cpp
    video/MovieFileTest.cpp
    video/VideoPlaybackTest.cpp)

# overdub_ui links overdub_capture and overdub_plugin, which link overdub_engine, overdub_song and overdub_core publicly; listing those too
# makes ld warn about duplicate libraries.
target_link_libraries(overdub_tests PRIVATE
    overdub_ui
    overdub_rt_guard
    Catch2::Catch2WithMain
    juce::juce_recommended_warning_flags)

# [snapshot] tests write PNGs here, one folder per slice, for a session to look at (ADR 0005).
target_compile_definitions(overdub_tests PRIVATE
    OVERDUB_UI_SNAPSHOT_DIR="${PROJECT_SOURCE_DIR}/build/ui-snapshots")

include("${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake")

# PRE_TEST: list tests when ctest runs, not as a post-build step, so a crashing test binary
# shows up as a failing test instead of a failing build.
catch_discover_tests(overdub_tests DISCOVERY_MODE PRE_TEST)
