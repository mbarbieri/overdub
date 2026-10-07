# overdub_ui: the main window's JUCE Components (Record / Edit / Export, the top bar, Notices and
# Alerts) and the pure functions they stay thin over: key commands, geometry, transport text,
# input choices and gesture -> Song operation (ADR 0005 UI amendment, issue #27). Events use the
# `ui.*` area (ADR 0006).

add_library(overdub_ui STATIC)

target_sources(overdub_ui PRIVATE
    Commands.cpp
    DeviceSettingsPanel.cpp
    Layout.cpp
    MainComponent.cpp
    OpenSong.cpp
    Pages.cpp
    RecordPage.cpp
    SongFiles.cpp
    TopBar.cpp
    TrackRowEdits.cpp
    TransportText.cpp
    UserMessages.cpp)

target_link_libraries(overdub_ui
    PUBLIC
        overdub_capture
    PRIVATE
        juce::juce_recommended_warning_flags)
