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
    capture/LoopbackRunTest.cpp
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
    export/MovieExportTest.cpp
    export/Mp4Decode.mm
    export/MovieSpanTest.cpp
    plugin/AudioUnitIdentifierTest.cpp
    plugin/AudioUnitLoaderTest.cpp
    plugin/InsertBypassTest.cpp
    plugin/InsertHostTest.cpp
    plugin/InvalidationWatchTest.mm
    separation/CrossFadeTest.cpp
    separation/ModelCheckTest.cpp
    separation/SeparateHelperTest.cpp
    separation/SeparationPipelineTest.cpp
    separation/StftTest.cpp
    song/BackingTrackImportTest.cpp
    song/ClipEditsTest.cpp
    song/InsertEditsTest.cpp
    song/MixEditsTest.cpp
    song/SongJsonTest.cpp
    song/SongMigrationTest.cpp
    song/PeaksTest.cpp
    song/SeparationHelperProcessTest.cpp
    song/SeparationTest.cpp
    song/SongPackageTest.cpp
    song/TakeEditsTest.cpp
    song/TrackEditsTest.cpp
    support/RealtimeGuardTest.cpp
    support/RealtimeGuardCatchTest.cpp
    ui/CalibrationPanelTest.cpp
    ui/CalibrationTextTest.cpp
    ui/CommandsTest.cpp
    ui/DeviceSettingsPanelTest.cpp
    ui/EditPageTest.cpp
    ui/ExportMp4PageTest.cpp
    ui/ExportPageTest.cpp
    ui/ExportRunTest.cpp
    ui/InsertChainUiTest.cpp
    ui/LayoutTest.cpp
    ui/MainComponentTest.cpp
    ui/MixerGeometryTest.cpp
    ui/MixerUiTest.cpp
    ui/OpenSongCameraTest.cpp
    ui/OpenSongDeviceChangeTest.cpp
    ui/OpenSongInsertsTest.cpp
    ui/OpenSongTest.cpp
    ui/PreRollTest.cpp
    ui/RecordCameraTest.cpp
    ui/RecordInsertsTest.cpp
    ui/AddTrackTest.cpp
    ui/RecordPageTest.cpp
    ui/RecordViewTest.cpp
    ui/SeparateTest.cpp
    ui/SnapshotTest.cpp
    ui/SongFilesTest.cpp
    ui/TakeLanesTest.cpp
    ui/TakeLanesUiTest.cpp
    ui/TimelineGeometryTest.cpp
    ui/TrackRowEditsTest.cpp
    ui/TransportTextTest.cpp
    ui/UserMessagesTest.cpp
    ui/VideoPreviewTest.cpp
    ui/WorkspaceTest.cpp
    video/FrameSelectionTest.cpp
    video/MovieFileTest.cpp
    video/VideoPlaybackTest.cpp)

set_source_files_properties(export/Mp4Decode.mm plugin/InvalidationWatchTest.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# overdub_ui links overdub_capture and overdub_plugin, which link overdub_engine, overdub_song and overdub_core publicly; listing those too
# makes ld warn about duplicate libraries.
target_link_libraries(overdub_tests PRIVATE
    overdub_ui
    overdub_separation
    overdub_rt_guard
    Catch2::Catch2WithMain
    juce::juce_recommended_warning_flags)

# [separation][helper] tests run the overdub-separate binary as a child process (issue #113).
add_dependencies(overdub_tests overdub-separate)
target_compile_definitions(overdub_tests PRIVATE
    OVERDUB_SEPARATE_HELPER="$<TARGET_FILE:overdub-separate>")

# [snapshot] tests write PNGs here, one folder per slice, for a session to look at (ADR 0005).
target_compile_definitions(overdub_tests PRIVATE
    OVERDUB_UI_SNAPSHOT_DIR="${PROJECT_SOURCE_DIR}/build/ui-snapshots")

include("${PROJECT_SOURCE_DIR}/external/Catch2/extras/Catch.cmake")

# PRE_TEST: list tests when ctest runs, not as a post-build step, so a crashing test binary
# shows up as a failing test instead of a failing build.
catch_discover_tests(overdub_tests DISCOVERY_MODE PRE_TEST)

# The Separation model install script (issue #112) is Python; its tests run on the host's python3 with
# the standard library only (no network, torch or onnx).
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(NAME "separation model install script"
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest -v install_model_test
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/separation")
