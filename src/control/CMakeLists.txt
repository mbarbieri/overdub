add_library(overdub_control STATIC)

target_sources(overdub_control PRIVATE
    AppState.cpp
    Controller.cpp
    EventTap.cpp
    IntentOps.cpp
    Server.cpp)

target_link_libraries(overdub_control
    PUBLIC
        overdub_ui
    PRIVATE
        juce::juce_recommended_warning_flags)
