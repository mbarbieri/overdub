# overdub_control: the local control socket (issue #169). The protocol (Controller) has no file
# descriptors so tests drive it with strings; the Server moves bytes between sockets and it. Events
# and Error codes use the `control.*` area (ADR 0006).

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
