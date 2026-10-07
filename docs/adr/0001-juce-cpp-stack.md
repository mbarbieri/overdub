# JUCE 9 + C++20, with Objective-C++ only for AVFoundation

Overdub is written in C++20 on JUCE 9.0.3, built with CMake + Ninja, not in Swift/SwiftUI. The hard parts of v1 are AU hosting, plugin delay compensation and latency-aligned dry Takes. JUCE solves those (`AudioPluginFormatManager`, `AudioProcessorGraph`'s automatic PDC, the JUCE 9 CoreAudio latency reporting), and it has about 25x more public plugin-hosting code than Swift does, which matters because every line is AI-written. Camera capture, the synced video preview and MP4 export would call AVFoundation directly in any stack, so they live in a few `.mm` files behind plain C++ headers.

## Considered Options

- **Swift + AVAudioEngine**: native and one language, but no PDC, thin AU-hosting examples, and no supported compile-time real-time-safety check (`@_noLocks` is underscored).
- **Swift UI over a C++ core (incl. JUCE headless)**: native widgets, but every feature crosses an evolving Swift/C++ interop boundary, and the timeline and mixer barely use native widgets.
- **JUCE + Tracktion Engine**: a ready DAW model, but its large, opinionated `Edit` model works against flat, regenerable code.

## Consequences

- JUCE is a git submodule at tag 9.0.3 in `external/JUCE`, so sessions grep the real headers instead of recalling JUCE 7/8 APIs. Catch2 v3 is a submodule too.
- `CMakePresets.json`: `debug` builds the native architecture, `release` builds `arm64;x86_64`. Full Xcode is optional.
- Audio-thread entry points are `[[clang::nonblocking]]`, built with `-Werror=function-effects` on our own targets.
- One `AudioProcessorGraph` per Song: Track nodes → Insert Chain plugin nodes → Master chain.
- The UI is JUCE `Component`s. Plugin editors open in their own `DocumentWindow`. The video preview is an `AVSampleBufferDisplayLayer` in an `NSViewComponent`, timed by the interface's `CMAudioDeviceClock`.
- The app is signed with a stable local identity and no hardened runtime, so camera and mic permissions survive rebuilds and AUs need no entitlements.
