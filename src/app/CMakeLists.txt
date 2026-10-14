# Overdub.app: the JUCE application entry point and its main window.

juce_add_gui_app(Overdub
    PRODUCT_NAME "Overdub"
    BUNDLE_ID com.mbarbieri.overdub
    COMPANY_NAME "Overdub"
    VERSION "${PROJECT_VERSION}"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.png"
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

# overdub_ui links overdub_capture, overdub_engine, overdub_song and overdub_core publicly.
target_link_libraries(Overdub PRIVATE
    overdub_ui
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

# The Separation helper (ADR 0007): overdub-separate in Contents/MacOS, the ONNX Runtime dylib in
# Contents/Frameworks (the helper's rpath), the licences in Contents/Resources. ONNX Runtime is
# never linked into the Overdub binary itself. Nested code is signed before the bundle.
add_dependencies(Overdub overdub-separate)
set(overdub_bundle_contents "$<TARGET_BUNDLE_CONTENT_DIR:Overdub>")
set(overdub_bundled_helper "${overdub_bundle_contents}/MacOS/overdub-separate")
set(overdub_bundled_ort "${overdub_bundle_contents}/Frameworks/${OVERDUB_ORT_DYLIB_NAME}")

add_custom_command(TARGET Overdub POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${overdub_bundle_contents}/Frameworks" "${overdub_bundle_contents}/Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:overdub-separate>" "${overdub_bundled_helper}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${OVERDUB_ORT_DYLIB}" "${overdub_bundled_ort}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES" "${overdub_bundle_contents}/Resources/THIRD_PARTY_NOTICES"
    COMMAND "${CMAKE_COMMAND}" -E copy "${OVERDUB_ORT_ROOT}/ThirdPartyNotices.txt" "${overdub_bundle_contents}/Resources/onnxruntime-ThirdPartyNotices.txt"
    COMMAND codesign --force --sign "${overdub_sign_with}" --timestamp=none "${overdub_bundled_ort}"
    COMMAND codesign --force --sign "${overdub_sign_with}" --timestamp=none "${overdub_bundled_helper}"
    COMMAND codesign --force --sign "${overdub_sign_with}" --timestamp=none "$<TARGET_BUNDLE_DIR:Overdub>"
    COMMENT "Bundling overdub-separate and ONNX Runtime, then signing Overdub.app (${overdub_sign_with})"
    VERBATIM)
