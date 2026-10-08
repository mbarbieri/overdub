# overdub_ui: the main window's JUCE Components (Record / Edit / Export, the top bar, Notices and
# Alerts) and the pure functions they stay thin over: key commands, geometry, transport text,
# input choices and gesture -> Song operation (ADR 0005 UI amendment, issue #27). Events use the
# `ui.*` area (ADR 0006).

add_library(overdub_ui STATIC)

target_sources(overdub_ui PRIVATE
    CameraStage.mm
    Commands.cpp
    DeviceSettingsPanel.cpp
    EditPage.cpp
    InsertChain.cpp
    Layout.cpp
    LookAheadStrip.cpp
    MainComponent.cpp
    Mixer.cpp
    MixerGeometry.cpp
    OpenSong.cpp
    PassesPanel.cpp
    PluginWindows.cpp
    Pages.cpp
    RecordPage.cpp
    RecordView.cpp
    SongFiles.cpp
    SongOverview.cpp
    TakeLanes.cpp
    Timeline.cpp
    TimelineGeometry.cpp
    TopBar.cpp
    TrackRowEdits.cpp
    TransportText.cpp
    UserMessages.cpp)

set_source_files_properties(CameraStage.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(overdub_ui
    PUBLIC
        overdub_capture
        overdub_plugin
        overdub_video
    PRIVATE
        juce::juce_recommended_warning_flags)
