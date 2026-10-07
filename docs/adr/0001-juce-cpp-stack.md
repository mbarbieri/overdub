# JUCE 9 + C++20, with Objective-C++ only for AVFoundation

Overdub is written in C++20 on JUCE 9.0.3, built with CMake + Ninja, not in Swift/SwiftUI. The hard parts of v1 are AU hosting, plugin delay compensation and latency-aligned dry Takes. JUCE solves those (`AudioPluginFormatManager`, `AudioProcessorGraph`'s automatic PDC, the JUCE 9 CoreAudio latency reporting), and it has about 25x more public plugin-hosting code than Swift does, which matters because every line is AI-written. Camera capture, the synced video preview and MP4 export would call AVFoundation directly in any stack, so they live in a few `.mm` files behind plain C++ headers.

## Considered Options

- **Swift + AVAudioEngine**: native and one language, but no PDC, thin AU-hosting examples, and no supported compile-time real-time-safety check (`@_noLocks` is underscored).
- **Swift UI over a C++ core (incl. JUCE headless)**: native widgets, but every feature crosses an evolving Swift/C++ interop boundary, and the timeline and mixer barely use native widgets.
- **JUCE + Tracktion Engine**: a ready DAW model, but its large, opinionated `Edit` model works against flat, regenerable code.

## Consequences

- JUCE is a git submodule at tag 9.0.3 in `external/JUCE`, so sessions grep the real headers instead of recalling JUCE 7/8 APIs. Catch2 v3 is a submodule too.
- `CMakePresets.json`: `debug` and `release` both build `x86_64` only. v1 ships Intel-only because the only test machine is an Intel Mac and there is no CI (issue #11). Code stays arm64-clean: no SSE/AVX intrinsics, inline asm or `x86_64` assumptions; vector math goes through `FloatVectorOperations` or plain C++. Full Xcode is optional.
- `[[clang::nonblocking]]` with `-Werror=function-effects` applies only to our own leaf DSP functions, the ones that call no JUCE APIs. JUCE isn't annotated, so every JUCE call in an annotated callback (the `AudioBuffer` constructor, `AudioProcessorGraph::processBlock`, `FloatVectorOperations`) trips the check. The AU spike (issue #6) hit 15 warnings in a few hundred lines. Callbacks that call into JUCE stay unannotated.
- One `AudioProcessorGraph` per Song: Track nodes → Insert Chain plugin nodes → Master chain.
- The UI is JUCE `Component`s. Plugin editors open in their own `DocumentWindow`. The video preview is an `AVSampleBufferDisplayLayer` in an `NSViewComponent`, timed by the interface's `CMAudioDeviceClock`.
- The app is signed with a stable local identity and no hardened runtime, so camera and mic permissions survive rebuilds and AUs need no entitlements.
