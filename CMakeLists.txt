cmake_minimum_required(VERSION 3.25)

project(Overdub VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

# ADR 0001: C++23 with GNU extensions (-std=gnu++23) for C++ and Objective-C++.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS ON)

# ADR 0001 / issue #11: v1 builds x86_64 only. The presets set this; this guards direct configures.
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    message(FATAL_ERROR "overdub: CMAKE_OSX_ARCHITECTURES must be x86_64 (got '${CMAKE_OSX_ARCHITECTURES}'). Use 'cmake --preset debug'.")
endif()

set(OVERDUB_CODESIGN_IDENTITY "Overdub Local Development" CACHE STRING
    "Keychain code-signing identity for Overdub.app. Falls back to ad-hoc signing when missing.")

# Third-party code: JUCE (patched), Catch2, nlohmann/json, and the overdub_juce library.
add_subdirectory(external)

# Shared infrastructure every feature links (logger, errors, RT ring, test guard).
add_subdirectory(src/core)

# The Song model, its JSON form and the .overdub package (ADR 0004).
add_subdirectory(src/song)

# The headless engine: process() callback, transport, render graph, Clip playback (ADR 0005).
add_subdirectory(src/engine)

# Insert Chain plugins: out-of-process AUs, kept alive across render plans (issue #33).
add_subdirectory(src/plugin)

# Recording Passes and the loopback latency measurement (issue #26).
add_subdirectory(src/capture)

# Video Take playback: frame selection on the audio clock and the frame decoder (issue #36).
add_subdirectory(src/video)

# Export mode's mixdown: the offline render of the Song's graph to WAV or M4A (issue #37).
add_subdirectory(src/export)

# Separation: the overdub-separate helper and the signal processing around its graph (ADR 0007).
add_subdirectory(src/separation)

# The main window's Components and the pure UI logic under them (issue #27).
add_subdirectory(src/ui)

# The local control socket: newline-delimited JSON over AF_UNIX, and the log tap it streams.
add_subdirectory(src/control)

# Overdub.app
add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)
