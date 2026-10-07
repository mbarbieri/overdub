# JUCE 9 + C++23, with Objective-C++ only for AVFoundation

Overdub is written in C++23 on JUCE 9.0.3, built with CMake + Ninja, not in Swift/SwiftUI. The hard parts of v1 are AU hosting, plugin delay compensation and latency-aligned dry Takes. JUCE solves those (`AudioPluginFormatManager`, `AudioProcessorGraph`'s automatic PDC, the JUCE 9 CoreAudio latency reporting), and it has about 25x more public plugin-hosting code than Swift does, which matters because every line is AI-written. Camera capture, the synced video preview and MP4 export would call AVFoundation directly in any stack, so they live in a few `.mm` files behind plain C++ headers.

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
- The language standard is C++23 (raised from C++20 for `std::expected`, ADR 0006). The AU spike, JUCE included, builds cleanly with Apple clang 21 at `-std=gnu++23`.

## Amendment: the realtime-safe graph patch changes what a busy node does (issue #24)

Unpatched, `AudioProcessorGraph` takes each node's callback lock with a blocking `ScopedLock` on every block, and reallocates its output buffer when the block size changes. Both fail ADR 0005's realtime guard, so `patches/juce-graph-realtime-safe.patch` changes them. This is a behaviour change, not just a performance fix: when another thread holds a node's callback lock (`suspendProcessing`, or a plugin's own state calls), the patched graph does not wait for it. That node outputs **silence** for the block and the rest of the graph plays on. Unpatched JUCE would block the audio thread until the lock was free, which risks a dropout across the whole output. We chose silence because one node's gap is local and audible only on its path, while a blocked audio thread is global.

- The patch counts every such node block in a process-wide counter, `juce::getAudioProcessorGraphBusyNodeBlockCount()` (lock-free, any thread). `RenderPlan::render` returns the count for its block. The engine pushes `engine.nodes_silenced` (warn, RT path) when a block has a silenced node and the previous block did not, and sums every one into `engine.load`'s `silenced_node_blocks`. So the gap is never silent in the logs.
- The counter is process-wide, so the engine assumes only its own audio thread renders graphs. A second engine rendering at the same time would mix its counts into the first one's.
- The output buffer is resized with `avoidReallocating`, so a block no larger than the prepared size never allocates.
- Bumping JUCE means re-applying both edits by hand (`docs/building.md`, "Re-applying the JUCE patch").
