# Overdub.app: the JUCE application entry point and its main window.

juce_add_gui_app(Overdub
    PRODUCT_NAME "Overdub"
    BUNDLE_ID com.mbarbieri.overdub
    COMPANY_NAME "Overdub"
    VERSION "${PROJECT_VERSION}"
    HARDENED_RUNTIME_ENABLED FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Overdub records your instruments and voice from your audio interface."
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "Overdub can record video of your performance alongside the audio.")

target_sources(Overdub PRIVATE
    LiveEngine.cpp
    LoopbackSelftest.cpp
    Main.cpp
    MainWindow.cpp
    MicrophonePermission.mm)

# overdub_capture links overdub_engine, overdub_song and overdub_core publicly.
target_link_libraries(Overdub PRIVATE
    overdub_capture
    "-framework AVFoundation"
    juce::juce_recommended_warning_flags)

# ADR 0001: sign with one stable local identity (no hardened runtime) so camera and microphone
# permissions survive rebuilds. Without that identity in the keychain, fall back to ad-hoc signing,
# which works but makes macOS ask for permissions again after every build.
execute_process(
    COMMAND security find-identity -p codesigning
    OUTPUT_VARIABLE overdub_codesign_identities
    ERROR_QUIET)
string(FIND "${overdub_codesign_identities}" "\"${OVERDUB_CODESIGN_IDENTITY}\"" overdub_identity_index)

if(overdub_identity_index EQUAL -1)
    message(WARNING "overdub: code-signing identity '${OVERDUB_CODESIGN_IDENTITY}' not found; "
                    "Overdub.app will be signed ad-hoc. See docs/building.md, 'Code signing'.")
    set(overdub_sign_with "-")
else()
    message(STATUS "overdub: signing Overdub.app with '${OVERDUB_CODESIGN_IDENTITY}'")
    set(overdub_sign_with "${OVERDUB_CODESIGN_IDENTITY}")
endif()

add_custom_command(TARGET Overdub POST_BUILD
    COMMAND codesign --force --sign "${overdub_sign_with}" --timestamp=none "$<TARGET_BUNDLE_DIR:Overdub>"
    COMMENT "Signing Overdub.app (${overdub_sign_with})"
    VERBATIM)
