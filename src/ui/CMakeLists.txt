# overdub_ui: the main window's JUCE Components (Record / Edit / Export, the top bar, Notices and
# Alerts) and the pure functions they stay thin over: key commands, geometry, transport text,
# input choices and gesture -> Song operation (ADR 0005 UI amendment, issue #27). Events use the
# `ui.*` area (ADR 0006).

add_library(overdub_ui STATIC)

target_sources(overdub_ui PRIVATE
    CalibrationPanel.cpp
    CalibrationText.cpp
    CameraStage.mm
    Colours.cpp
    Commands.cpp
    DeviceSettingsPanel.cpp
    EditPage.cpp
    ExportPage.cpp
    ExportRun.cpp
    FileDrop.cpp
    InsertChain.cpp
    Layout.cpp
    LookAheadStrip.cpp
    MainComponent.cpp
    MixFocus.cpp
    Mixer.cpp
    MixerGeometry.cpp
    OpenSong.cpp
    PassesPanel.cpp
    PluginWindows.cpp
    Pages.cpp
    RecordPage.cpp
    RecordView.cpp
    SeparationDialog.cpp
    SongFiles.cpp
    SongOverview.cpp
    TakeLanes.cpp
    ThinProgressBar.cpp
    Timeline.cpp
    TimelineGeometry.cpp
    TopBar.cpp
    TrackDrag.cpp
    TrackRowEdits.cpp
    TransportText.cpp
    UserMessages.cpp
    VideoPreviewWindow.cpp
    VideoScreen.mm
    Workspace.cpp)

set_source_files_properties(CameraStage.mm VideoScreen.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(overdub_ui
    PUBLIC
        overdub_capture
        overdub_export
        overdub_plugin
        overdub_video
    PRIVATE
        juce::juce_recommended_warning_flags)
