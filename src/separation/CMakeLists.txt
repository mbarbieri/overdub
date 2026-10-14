# Separation (ADR 0007): splitting one Backing Track into six parts with BS-RoFormer SW.
#
# overdub_separation: the signal processing around the graph (STFT, chunking, cross-fade) and the
# model check, with pocketfft. Tests link it; Overdub.app does not.
# overdub-separate: the helper executable Overdub starts for each Separation. It alone links ONNX
# Runtime; the dylib is copied next to it here and into Overdub.app/Contents/Frameworks by src/app.
# Events and Error codes use the `separation.*` area (ADR 0006).

add_library(overdub_separation STATIC)

target_sources(overdub_separation PRIVATE
    CrossFade.cpp
    ModelCheck.cpp
    SeparationPipeline.cpp
    Stft.cpp)

target_link_libraries(overdub_separation
    PUBLIC
        overdub_song
    PRIVATE
        overdub_pocketfft
        juce::juce_recommended_warning_flags)

# pocketfft and the STFT loops are unusably slow unoptimised; tests run them on whole chunks.
set_source_files_properties(Stft.cpp CrossFade.cpp SeparationPipeline.cpp PROPERTIES COMPILE_OPTIONS "-O2")

add_executable(overdub-separate
    GraphSession.cpp
    HelperMain.cpp
    SeparateCommand.cpp)

target_link_libraries(overdub-separate PRIVATE
    overdub_separation
    overdub_onnxruntime
    juce::juce_recommended_warning_flags)

# The dylib sits next to the helper in the build tree and in Contents/Frameworks inside Overdub.app
# (the helper is in Contents/MacOS). Only relative rpaths, so neither copy refers to the build tree.
set_target_properties(overdub-separate PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@executable_path;@executable_path/../Frameworks")

add_custom_command(TARGET overdub-separate POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OVERDUB_ORT_DYLIB}" "$<TARGET_FILE_DIR:overdub-separate>/"
    COMMENT "Copying the ONNX Runtime dylib next to overdub-separate"
    VERBATIM)
