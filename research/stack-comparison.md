# Which stack fits a DAW whose code is fully AI-written?

Research for [issue #2](https://github.com/mbarbieri/overdub/issues/2), which feeds "Choose the tech stack" ([#5](https://github.com/mbarbieri/overdub/issues/5)). This file reports facts and trade-offs only. It does not pick a stack.

- **Date**: 2026-10-07.
- **Versions in view**: JUCE 9.0.3 (released 2026-09-28), Swift 6.3.3, Apple clang 21, macOS 26.6 SDK 26.5 (Command Line Tools), Tracktion Engine v3.2.0, AudioKit 5.7.2.
- **Method**: Apple developer docs (the `.md` and JSON renderings of developer.apple.com), the macOS 26.5 SDK headers, JUCE and Tracktion source on GitHub `master`, swift.org docs and the Swift repo, plus two local compile probes on this machine (x86_64, macOS 26.6). The GitHub counts in §7 are rough volume proxies, not quality measures.

Vocabulary follows [`GLOSSARY.md`](../GLOSSARY.md).

## Candidates

| ID | Stack | Audio core | UI | Camera and A/V export |
|---|---|---|---|---|
| **A** | Swift + AVAudioEngine | `AVAudioEngine` graph with `AVAudioUnit` effects | SwiftUI and/or AppKit | AVFoundation (native Swift) |
| **B** | Swift app over a C/C++ real-time core (hybrid) | Own render callback on Core Audio (AUHAL or `AVAudioSinkNode` / `AVAudioSourceNode`), hosting AUs through `AUAudioUnit.renderBlock` or the AUv2 C API, written in C++ and called through Swift 5.9+ C++ interop | SwiftUI and/or AppKit | AVFoundation (native Swift) |
| **C** | C++ with JUCE 9 | `AudioDeviceManager` + `AudioProcessorGraph` + `AudioPluginFormatManager` | JUCE `Component`s (AppKit underneath) | JUCE `CameraDevice` for capture; export needs Objective-C++ calls into AVFoundation |
| **C+T** | JUCE + Tracktion Engine | Tracktion `Edit` model on top of JUCE | JUCE (Tracktion has no UI) | Same as C |

A variant of B uses JUCE's new `juce_audio_processors_headless` module (JUCE 9 split AU hosting into a headless module, see the `#include <juce_audio_processors_headless/...>` in [`juce_AudioUnitPluginFormat.mm`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors/format_types/juce_AudioUnitPluginFormat.mm)) as the C++ core under a Swift UI. That combines B's UI side with C's licensing.

---

## 1. AU hosting (AUv2 and AUv3, plugin UI, state)

### Apple APIs (A and B)

- **One API hosts both versions.** Apple says the AUv3 API's "bridging layer ... automatically translat[es] AUv3 calls into their AUv2 equivalents, which means you can use a single API and work with both new and legacy audio units." ([Migrating your audio unit host to the AUv3 API](https://developer.apple.com/documentation/audiotoolbox/migrating-your-audio-unit-host-to-the-auv3-api))
- **Discovery**: `AVAudioUnitComponentManager` finds registered units by description, predicate or block, without opening them. ([AVAudioUnitComponentManager](https://developer.apple.com/documentation/avfaudio/avaudiounitcomponentmanager))
- **Instantiation**: `AVAudioUnit.instantiate(with:options:completionHandler:)` is asynchronous. It is required for components that need async instantiation when used with `AVAudioEngine`. ([docs](https://developer.apple.com/documentation/avfaudio/avaudiounit/instantiate(with:options:completionhandler:)))
- **Process model**:
  - "AUv2 audio units are loaded into the host's process, whereas AUv3 audio units are loaded out-of-process by default." On macOS a host may request `.loadInProcess` for AUv3s packaged for it. ([Migrating...](https://developer.apple.com/documentation/audiotoolbox/migrating-your-audio-unit-host-to-the-auv3-api))
  - Out-of-process IPC "adds some small but potentially significant overhead ... especially when rendering at small audio I/O buffer sizes." ([Incorporating Audio Effects and Instruments](https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments))
- **Plugin UI**: `AUAudioUnit.requestViewController(completionHandler:)` returns an `AUViewController`, or `nil` when the unit has no custom view. The completion handler runs "on a thread or dispatch queue internal to the audio unit's implementation", so the host must hop to the main queue. ([docs](https://developer.apple.com/documentation/audiotoolbox/auaudiounit/requestviewcontroller(completionhandler:)), [Migrating...](https://developer.apple.com/documentation/audiotoolbox/migrating-your-audio-unit-host-to-the-auv3-api)) Apple's host sample shows a custom view or a generic view through this path. ([Incorporating...](https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments))
- **State**: `fullState` and `fullStateForDocument` are persistable snapshots, bridged to AUv2 `kAudioUnitProperty_ClassInfo` and `kAudioUnitProperty_ClassInfoFromDocument`. ([fullState](https://developer.apple.com/documentation/audiotoolbox/auaudiounit/fullstate), [fullStateForDocument](https://developer.apple.com/documentation/audiotoolbox/auaudiounit/fullstatefordocument)) They are dictionaries, so they serialize to a plist blob inside the Song file.
- **Sample code**: Apple ships a Swift AU host sample ("Incorporating Audio Effects and Instruments", macOS 10.15+, Xcode 16+) built on `AVAudioEngine`. ([link](https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments))

### JUCE (C, C+T)

- **Implementation**: JUCE's `AudioUnitPluginFormat` uses the AudioComponent C API underneath. AUv3s (`kAudioComponentFlag_IsV3AudioUnit`) are created with `AudioComponentInstantiate(..., kAudioComponentInstantiation_LoadOutOfProcess, ...)`. AUv2s are created in-process with `AudioComponentInstanceNew`. ([`juce_AudioUnitPluginFormatImpl.h`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h), `createAudioUnit`)
- **Plugin UI**: the editor tries the AUv2 `kAudioUnitProperty_CocoaUI` view first, then the AUv3 `kAudioUnitProperty_RequestViewController`. It embeds the resulting `NSView` in a JUCE `AudioProcessorEditor`, with optional generic UI fallback. ([`juce_AudioUnitPluginFormat.mm`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors/format_types/juce_AudioUnitPluginFormat.mm), `createView`)
- **State**: `getStateInformation` / `setStateInformation` wrap `kAudioUnitProperty_ClassInfo`, giving one opaque `MemoryBlock` per plugin. (same Impl header, ~L1681–1730)
- **Graph**: `AudioProcessorGraph` routes the hosted plugins. Tracktion Engine adds an Edit model on top, with "Perfect plugin delay compensation", "Lock-free audio file writing/disk recording", undo/redo and background rendering. ([Tracktion FEATURES.md](https://github.com/Tracktion/tracktion_engine/blob/master/FEATURES.md))

### Architecture of third-party plugins (all stacks)

- From macOS 11, "the system loads audio units into a separate process that depends on the architecture or host preference ... and allows loading x86 audio units into an Apple silicon host app". The hosting processes are `AUHostingServiceXPC` (native) and `AUHostingServiceXPC_arrow` (Rosetta). ([Debugging Out-of-Process Audio Units on Apple Silicon](https://developer.apple.com/documentation/audiotoolbox/debugging-out-of-process-audio-units-on-apple-silicon))
- Apple: "If your Audio Unit contains code for both arm64 and x86_64 architectures, a host app can load your Audio Unit either in-process or out-of-process. The ability to load Audio Units in-process is still important for ... apps ... that require minimal latency." ([Porting your audio code to Apple silicon](https://developer.apple.com/documentation/apple-silicon/porting-your-audio-code-to-apple-silicon))
- This mechanism is OS-level, so it applies equally to A, B and C.

## 2. Low-latency Monitoring through an Insert Chain while recording dry, and latency compensation

The v1 need has three parts:

1. Route each Armed input through its Insert Chain to the output at the device buffer size.
2. Write the dry input to disk without touching the render thread.
3. Place each Take on the timeline shifted by the measured round-trip latency. Plugin latency on the playback path must also be compensated, so that Backing Tracks and older Takes line up.

### Apple APIs (A and B)

- **Live graph edits**: `AVAudioEngine` "supports dynamic connection, disconnection and removal of nodes while running" with limits. "All dynamic reconnections must occur upstream of a mixer", and removing a node with differing in/out channel counts "is likely to result in a broken graph". (SDK `AVFAudio.framework/Headers/AVAudioEngine.h`, class doc)
- **Latency is reported, not compensated**:
  - `AVAudioNode.latency` gives processing latency, and `outputPresentationLatency` the downstream pipeline latency. That value "can change as the engine is reconfigured ... it is recommended not to cache this value". (SDK `AVAudioNode.h`)
  - `AVAudioIONode.presentationLatency` maps to `kAudioDevicePropertyLatency` + `kAudioStreamPropertyLatency`. (SDK `AVAudioIONode.h`; [docs](https://developer.apple.com/documentation/avfaudio/avaudioionode/presentationlatency))
  - The string "compensat" appears in **no** AVFAudio header. No AVAudioEngine API documents automatic plugin delay compensation across parallel tracks, so A has to add its own delay nodes or offset scheduling.
- **Plugin latency contract**: `AUAudioUnit.latency` is bridged to `kAudioUnitProperty_Latency`. Apple notes that "a host is usually only prepared to add delays before starting to render and those delays need to be fixed". ([latency](https://developer.apple.com/documentation/audiotoolbox/auaudiounit/latency))
- **Hardware latency**:
  - `kAudioDevicePropertyLatency` is per direction. "If both the device and the stream say they have latency, then the total latency for the stream is the device latency summed with the stream latency."
  - `kAudioDevicePropertySafetyOffset` gives frames "ahead (for output) or behind (for input) the current hardware position that is safe to do IO".
  - Source: SDK `CoreAudio.framework/Headers/AudioHardwareBase.h`.
- **Capturing dry input**:
  - `installTap(onBus:...)` takes buffers whose requested size has a "Supported range [of] [100, 400] ms". It's fine for writing files, not for Monitoring. (SDK `AVAudioNode.h`)
  - `AVAudioSinkNode` (macOS 10.15+) "receive[s] input audio on the audio IO thread". It is input-chain only, does no format conversion (it must match the hardware rate), and isn't available in manual rendering mode. (SDK `AVAudioSinkNode.h`; [docs](https://developer.apple.com/documentation/avfaudio/avaudiosinknode))
- **Device and format changes**: on a hardware channel-count or sample-rate change "the engine stops itself ... and issues" `AVAudioEngineConfigurationChangeNotification`. The app must re-establish connections whose formats changed. The engine "must not be deallocated from within the client's notification handler ... can deadlock". (SDK `AVAudioEngine.h`)
- **Device selection**: `AVAudioEngine.inputNode` is a singleton. Choosing a specific interface goes through `AVAudioIONode.audioUnit`, "only necessary for certain advanced usages", by setting AUHAL properties on it. (SDK `AVAudioIONode.h`) Apple doesn't document whether input and output on *different* devices work without an aggregate device. **Unverified; spike item.**
- **Offline mixdown**: `enableManualRenderingMode(_:format:maximumFrameCount:)` (macOS 10.13+) renders the same graph faster than realtime. It requires stopping the engine first. ([docs](https://developer.apple.com/documentation/avfaudio/avaudioengine/enablemanualrenderingmode(_:format:maximumframecount:)))
- **Stack B** owns the render callback, so it implements dry capture (lock-free ring buffer to a writer thread), per-track delay lines and offline render itself, on the same Core Audio properties.

### JUCE (C, C+T)

- **Delay compensation**: `AudioProcessorGraph` adds latency compensation automatically. Its render-sequence builder tracks per-node delay (`getNodeDelay`, `totalLatency`) and inserts `DelayChannelOp`s so that inputs with lower latency are delayed to the max. ([`juce_AudioProcessorGraph.cpp`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_processors_headless/processors/juce_AudioProcessorGraph.cpp), ~L731–767, L1085–1264)
- **Plugin latency**: hosted AUs report latency from `kAudioUnitProperty_Latency` through `setLatencySamples`, and JUCE listens for changes to it. (Impl header, ~L1854–1857, L2090)
- **Device layer (JUCE 9)**: JUCE 9.0.0 shipped "a new macOS CoreAudio implementation". ([release notes](https://github.com/juce-framework/JUCE/releases/tag/9.0.0))
  - It creates a private per-process aggregate device (`AudioHardwareCreateAggregateDevice`) over the chosen input and output devices.
  - It reports `getInputLatencyInSamples()` = device latency + safety offset + buffer size + stream latency, and the same for output.
  - Source: [`juce_CoreAudio_mac.cpp`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp), ~L980–1070, L2126–2146. Take alignment uses exactly these numbers, already computed.
  - Caveat: the implementation is new (July 2026), so it has had fewer months of field use than the code it replaced.
- **Tracktion Engine** goes further with "Perfect plugin delay compensation", lock-free disk recording and "Minimal latency with full plugin bypass". ([FEATURES.md](https://github.com/Tracktion/tracktion_engine/blob/master/FEATURES.md))

## 3. Camera capture and A/V export

- **What AVFoundation covers**: capture (`AVCaptureSession`, `AVCaptureMovieFileOutput` "records video and audio to a QuickTime movie file"), writing (`AVAssetWriter`, QuickTime/MPEG-4, one file per writer), editing (`AVMutableComposition`) and export (`AVAssetExportSession`). ([AVCaptureMovieFileOutput](https://developer.apple.com/documentation/avfoundation/avcapturemoviefileoutput), [AVAssetWriter](https://developer.apple.com/documentation/avfoundation/avassetwriter), [AVMutableComposition](https://developer.apple.com/documentation/avfoundation/avmutablecomposition), [AVAssetExportSession](https://developer.apple.com/documentation/avfoundation/avassetexportsession))
- **A/V clock alignment**: `AVCaptureSession.synchronizationClock` (macOS 12.3+): "All capture output sample buffer timestamps are on the synchronization clock's timebase." ([docs](https://developer.apple.com/documentation/avfoundation/avcapturesession/synchronizationclock)) That gives a hook for mapping Video Take timestamps onto the audio device's host time.
- **API churn**: `exportAsynchronously(completionHandler:)` is deprecated as of macOS 15 in favor of Swift-async `export(to:as:)`, and `status`/`progress` in favor of `states(updateInterval:)`. (SDK `AVAssetExportSession.h`, `API_DEPRECATED_WITH_REPLACEMENT("export(to:as:) async throws", macos(10.7, 15.0))`) Most public example code uses the deprecated form.
- **Permissions**: the app needs `NSCameraUsageDescription` and `NSMicrophoneUsageDescription`, plus the `com.apple.security.device.camera` and `audio-input` entitlements. Without them "the system terminates your app." ([Requesting authorization...](https://developer.apple.com/documentation/avfoundation/requesting-authorization-to-capture-and-save-media))
- **Stacks A and B**: everything above is native Swift. No bridging.
- **Stack C**:
  - JUCE's `CameraDevice::startRecordingToFile` records a `.mov` through `AVCaptureSession` + `AVCaptureMovieFileOutput` on macOS. It exposes `getTimeOfFirstRecordedFrame()` and a `quality` of 0–2. ([`juce_CameraDevice.h`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_video/capture/juce_CameraDevice.h), [`juce_CameraDevice_mac.h`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_video/native/juce_CameraDevice_mac.h))
  - JUCE doesn't expose the session's synchronization clock, device or format selection beyond that.
  - JUCE has **no** muxing, composition or export API. MP4 export with trim and black fill, and probably finer capture control, need Objective-C++ `.mm` files calling AVFoundation directly. That means a third language in the codebase, plus manual memory and threading rules at the boundary.

## 4. Universal binary (arm64 + x86_64) on macOS 15+

- **Xcode (A, B)**: Xcode 12.2+ "automatically builds a universal binary for the release version", while debug builds use the active architecture only. You "cannot debug the arm64 slice ... on an Intel-based Mac." ([Building a universal macOS binary](https://developer.apple.com/documentation/apple-silicon/building-a-universal-macos-binary))
- **JUCE CMake (C)**: universal builds use `-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64`. ([JUCE CMake API](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md), "Building universal binaries for macOS") JUCE deploys to macOS 10.11+ for x86_64 and Arm64. ([JUCE README](https://github.com/juce-framework/JUCE/blob/master/README.md))
- **Tracktion Engine** requires C++20. ([README](https://github.com/Tracktion/tracktion_engine/blob/master/README.md))
- **Platform horizon**: "macOS Tahoe will be the final release for Intel Macs." ([WWDC25 Platforms State of the Union](https://developer.apple.com/videos/play/wwdc2025/102/)) An Intel slice therefore targets macOS 15 and 26 only, with no future OS versions. This affects all stacks equally.
- **Local observation**: the machine this research ran on is x86_64 (macOS 26.6). It can build arm64 slices but can't run or debug them. That's relevant to the map's "Intel testing" open question in reverse.

## 5. Real-time safety by language

- **Apple's guidance**: "Don't allocate memory, perform file I/O, take locks, or interact with the Swift or Objective-C runtimes when rendering audio." Apple's own AUv3 sample keeps the Swift `AUAudioUnit` subclass for setup and writes the DSP kernel "in C++ to ensure real-time safety". ([Creating custom audio effects](https://developer.apple.com/documentation/avfaudio/creating-custom-audio-effects)) The sample adds an Objective-C++ adapter "because Swift can't talk directly to C++". That predates Swift 5.9 interop.
- **C/C++ (B core, C)**:
  - The macOS 26.5 SDK marks render callbacks `CA_REALTIME_API`, defined as `[[clang::nonblocking]]`. (SDK `CoreAudioTypes.framework/Headers/CoreAudioBaseTypes.h` L42–46; `AURenderBlock`, `AURenderPullInputBlock` in `AUAudioUnit.h`)
  - **Probe**: with Apple clang 21, `-Wfunction-effects` flagged `std::vector::push_back` inside a `[[clang::nonblocking]]` function at compile time ("must not call non-'nonblocking' function").
  - `-fsanitize=realtime` (RealtimeSanitizer) was rejected by Apple clang 21 on this x86_64 host ("unsupported option"), so runtime RT checking would need upstream LLVM. Not verified on arm64.
- **Swift (A, B UI side)**:
  - `@_noAllocation` and `@_noLocks` make "the compiler issue[] a diagnostic ... if the function calls a runtime function which allocates memory or locks". The Swift docs also warn: "Usage of these attributes outside of the Swift monorepo is STRONGLY DISCOURAGED", and their semantics may change. ([UnderscoredAttributes.md](https://github.com/swiftlang/swift/blob/main/docs/ReferenceGuides/UnderscoredAttributes.md))
  - **Probe**: with Swift 6.3.3 `-O`, an `@_noLocks` pointer loop compiled clean. An `@_noLocks` function creating `[Float]` failed with "Using type 'Float' can cause metadata allocation or locks". So the check works, but it's experimental and strict.
  - In stack A the render thread runs Apple's code. App Swift code runs on it only in `AVAudioSourceNode`/`AVAudioSinkNode` blocks or tap-adjacent code. The tap itself is delivered at 100–400 ms granularity, off the IO thread (see §2).
- **Swift/C++ interop (B)**: "a new feature in Swift 5.9", "an actively evolving feature". Enable it with `interoperabilityMode(.Cxx)` in SwiftPM or the Xcode setting. Swift promises that future changes "will not break code in existing codebases by default". Limits: virtual methods on C++ value types aren't callable directly, and reference-returning members are renamed `__`-prefixed as unsafe. ([swift.org C++ interop](https://www.swift.org/documentation/cxx-interop/))
- **Thread scheduling (all stacks)**: Apple asks real-time threads to join the device's `os_workgroup` (`kAudioDevicePropertyIOThreadOSWorkgroup`, `AUAudioUnit.osWorkgroup`). ([Porting your audio code to Apple silicon](https://developer.apple.com/documentation/apple-silicon/porting-your-audio-code-to-apple-silicon)) JUCE 9 exposes the device workgroup as `getWorkgroup()`. ([`juce_CoreAudio_mac.cpp`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp) ~L2148)

## 6. Build tooling an agent can drive headless

- **`xcodebuild`**: requires a full Xcode install. On this machine (Command Line Tools only) it fails with "tool 'xcodebuild' requires Xcode". `swiftc` and `clang` work without Xcode.
- **SwiftPM**: vends only `executable`, `library` and `plugin` products. ([`Product.swift`](https://github.com/swiftlang/swift-package-manager/blob/main/Sources/Runtimes/PackageDescription/Product.swift)) It doesn't produce a `.app` bundle with `Info.plist`, entitlements and usage descriptions, which camera access requires (§3). Stacks A and B therefore need an `.xcodeproj` (binary-ish, merge-hostile for agents), a generator such as XcodeGen or Tuist (third-party, not evaluated here), or hand-written bundling scripts around `swiftc`/SwiftPM.
- **JUCE CMake**:
  - `juce_add_gui_app` produces a bundled app with `MICROPHONE_PERMISSION_ENABLED`/`_TEXT` and `CAMERA_PERMISSION_ENABLED`/`_TEXT` keys from plain CMake. ([CMake API](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md))
  - The minimum is CMake 3.22. ([BREAKING_CHANGES.md](https://github.com/juce-framework/JUCE/blob/master/BREAKING_CHANGES.md))
  - JUCE 9.0.0 "Improved the CMake build system for headless environments". ([release notes](https://github.com/juce-framework/JUCE/releases/tag/9.0.0))
  - CMake can generate either Ninja/Make (no Xcode needed beyond CLT) or Xcode projects.
- **Signing**: with the Hardened Runtime on, loading third-party in-process AUv2s needs `com.apple.security.cs.disable-library-validation`. ([docs](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.disable-library-validation)) A personal, unnotarized build can leave the Hardened Runtime off. This applies to every stack.

## 7. How reliably LLMs write and debug each stack

**Public code volume** (GitHub search via `gh api`, 2026-10-07; rough proxies):

| Query | Count |
|---|---|
| Code: `AVAudioEngine` in Swift | 20,992 files |
| Code: `AVAudioUnitComponentManager` in Swift (AU *hosting* in Swift) | 166 files |
| Code: `AudioPluginFormatManager` in C++ (JUCE plugin hosting) | 4,416 files |
| Code: `juce_add_gui_app` (JUCE CMake apps) | 1,880 files |
| Repo topic `juce` / `coreaudio` / `avaudioengine` / `audiokit` / `auv3` | 1,083 / 377 / 60 / 50 / 42 repos |

Reading: there's plenty of public AVAudioEngine code overall (mostly playback, recording and taps), but very little Swift code that hosts third-party AUs. JUCE plugin-hosting examples are roughly 25x more common by this proxy. Open-source DAW engines on JUCE exist (Tracktion Engine, ~1.45k stars, active in 2026-10). The best-known Swift audio library, AudioKit (MIT, ~11.5k stars), describes itself as "an audio synthesis, processing, and analysis platform", not a DAW engine. ([README](https://github.com/AudioKit/AudioKit))

**API stability and churn**:

- **JUCE**:
  - It went from 8 to 9 in July 2026 (9.0.0 on 2026-07-21, then 9.0.1–9.0.3 by 2026-09-28). ([releases](https://github.com/juce-framework/JUCE/releases))
  - The 9.0.0 breaking changes are mostly GUI/text (Drawable no longer a Component, SVG parser replaced, font metrics redesign), plus a licensing change and new deployment targets. ([BREAKING_CHANGES.md](https://github.com/juce-framework/JUCE/blob/master/BREAKING_CHANGES.md))
  - Model training data mixes JUCE 5–8 idioms with 9.
  - The JUCE README now tells AI assistants that they "must read LICENSE.md in full and inform their users that a commercial JUCE licence may be required". ([README](https://github.com/juce-framework/JUCE/blob/master/README.md))
- **AVFoundation and Swift**:
  - AVAudioEngine's core API has been stable since macOS 10.10–10.15.
  - The churn sits around it: Swift 6 strict concurrency, async replacements such as `AVAssetExportSession.export(to:as:)` (§3), and engine stop/restart on configuration change (§2).
  - The AU-hosting-specific Swift code that models have seen is thin (above).
- **Debugging**:
  - Out-of-process AUs run in `AUHostingServiceXPC*`. Attaching a debugger there "requires disabling system integrity protection". ([Debugging Out-of-Process Audio Units](https://developer.apple.com/documentation/audiotoolbox/debugging-out-of-process-audio-units-on-apple-silicon))
  - This applies to all stacks whenever an AUv3 is loaded out-of-process. That's JUCE's default for AUv3 (§1), and an option in A and B.

## 8. Licensing for a personal, non-distributed tool

- **Apple frameworks (A, B)**: no extra license.
- **JUCE 9 (C)**:
  - The modules are dual-licensed: "AGPLv3 and the commercial JUCE licence". The examples are ISC. ([LICENSE.md](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md))
  - JUCE states that "if you are not 'propagating' or 'conveying' closed-source software containing JUCE outside of your organisation then you may still be within the terms of the open-source AGPLv3". ([juce.com/get-juce](https://juce.com/get-juce/))
  - The commercial tiers are Starter (free, revenue up to $20k), Indie ($40/user/mo, up to $300k) and Pro ($175/user/mo). (same page)
  - The JUCE 9 changes include moving previously ISC-licensed modules to the dual license. ([BREAKING_CHANGES.md](https://github.com/juce-framework/JUCE/blob/master/BREAKING_CHANGES.md))
  - For a never-distributed personal tool, AGPLv3 imposes nothing in practice. If the repo or binary were ever shared, AGPL source-availability obligations would apply.
- **Tracktion Engine (C+T)**: "GPL/Commercial". Enterprise licensees should check v3 coverage. ([README](https://github.com/Tracktion/tracktion_engine/blob/master/README.md)) The same reasoning applies for personal use.
- **AudioKit**: MIT. (GitHub license metadata)

---

## Trade-offs per candidate

### A. Swift + AVAudioEngine

- **For**:
  - Everything (AU discovery and UI, state, camera, export) is native Swift on one documented API family. Apple ships a Swift AU host sample.
  - Graph edits work live upstream of a mixer.
  - Manual rendering mode gives offline mixdown on the same graph.
  - No third-party license.
- **Against**:
  - No automatic plugin delay compensation (latencies are reported only, and the downstream value changes on reconfiguration).
  - The dry-capture path is either a 100–400 ms tap or a sink node with no format conversion.
  - Device selection and in≠out devices go through the underlying AUHAL ("advanced usages") and aren't documented.
  - Engine stops on device format changes.
  - Very little public Swift code hosts third-party AUs.
  - Packaging an `.app` needs Xcode or a project generator, because SwiftPM alone can't do it.
  - Swift's RT checks (`@_noLocks`) are experimental.

### B. Swift UI over a C/C++ real-time core

- **For**:
  - Native Swift for UI, camera and export (like A).
  - Full control over the render callback, PDC, dry-capture ring buffer and take alignment, written in the language Apple itself uses for DSP.
  - The SDK's `[[clang::nonblocking]]` annotations, enforced by clang's `-Wfunction-effects`, give compile-time RT checking today.
  - Interop is source-stable by promise.
  - The JUCE-headless variant can supply graph, PDC and device code under AGPL.
- **Against**:
  - Most engine code (graph, PDC, device handling, AU render plumbing) is hand-written unless JUCE headless is used.
  - Two languages plus an interop boundary that swift.org calls "actively evolving", with limits on virtuals and references.
  - Same app-bundling issue as A.
  - Fewer public examples of exactly this shape than of either pure form.

### C. C++ with JUCE 9

- **For**:
  - AU hosting covers AUv2 and AUv3, CocoaUI and view-controller UIs, and state blobs.
  - `AudioProcessorGraph` does delay compensation automatically.
  - The new CoreAudio layer builds the aggregate device and reports full input/output latency (device + safety offset + buffer + stream).
  - CMake drives universal builds and permission keys headless.
  - Roughly 25x more public plugin-hosting code than Swift by the proxy above.
  - AGPLv3 suffices for a personal tool.
- **Against**:
  - Video stops at "record a .mov": composition, trim, black fill and MP4 export need Objective-C++ against AVFoundation (a third language).
  - The UI is JUCE Components, not SwiftUI/AppKit.
  - JUCE 9 is three months old, and its CoreAudio rewrite is new.
  - Training data spans JUCE 5–9 idioms.
  - C++ memory safety is the agent's burden.

### C+T. JUCE + Tracktion Engine

- **For**:
  - Everything in C, plus a ready DAW model: clips, recording with lock-free disk writing, "perfect" PDC, undo/redo, background rendering. Much of v1's Clip editing and mixer scope comes off the shelf.
- **Against**:
  - C++20.
  - GPL/commercial.
  - A large, opinionated data model (`Edit`) that generated code must conform to rather than own. That cuts against the "flat, explicit, regenerable" principle.
  - Doxygen docs are described by the authors as "still in the process of fleshing out".
  - The last tagged release is v3.2.0 (2025-05), with active commits on `develop`/`master`.
  - Same video gap as C.

## Open questions for spikes (not answered by docs)

1. **AVAudioEngine devices**: on macOS, does AVAudioEngine accept separate input and output devices, or does it need a user- or app-created aggregate device? How do you select input channels on a multichannel interface? (Affects A.)
2. **Round-trip measurement**: does reported latency (device + safety offset + buffer + stream) match a loopback measurement on the user's interface? (All stacks; JUCE already sums these.)
3. **AUv3 monitoring cost**: what is the out-of-process AUv3 overhead at 64–128-frame buffers with several Armed tracks Monitoring? (All stacks.)
4. **Video alignment**: map `AVCaptureSession.synchronizationClock` to the audio device's host time to align Video Takes. For C, check whether JUCE's `getTimeOfFirstRecordedFrame()` (wall-clock `Time`) is precise enough or AVFoundation must be called directly.
5. **Intel coverage**: which arm64-only behavior can't be checked from this Intel machine, and the reverse for Apple silicon hosts.
