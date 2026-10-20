add_library(overdub_control STATIC)

target_sources(overdub_control PRIVATE
    Controller.cpp
    EventTap.cpp
    Server.cpp)

target_link_libraries(overdub_control
    PUBLIC
        overdub_core
    PRIVATE
        juce::juce_recommended_warning_flags)
