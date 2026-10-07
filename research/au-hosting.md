# Hosting AUv2/AUv3 effects outside the sandbox

Research for [issue #4](https://github.com/mbarbieri/overdub/issues/4). It feeds "Choose the tech stack" and "Spike: dry recording while Monitoring through an AU".

Context: Overdub is a non-sandboxed, universal (arm64 + x86_64) app for macOS 15+. Each Track and the Master has an **Insert Chain** of third-party AU effects. Takes are recorded dry while the user is **Monitoring** through the chain.

Researched 2026-10-07. Local checks ran on macOS 26.6 (25G72), arm64, with SDK MacOSX26.5. Sources are listed at the end and cited inline as [S1], [S2] and so on.

---

## TL;DR

1. **One API for both versions.** `AUAudioUnit`/`AVAudioUnit` (AUv3 API) hosts AUv2 and AUv3 alike, because the framework bridges each call to its AUv2 equivalent [S1][S3]. Discovery goes through `AVAudioUnitComponentManager` [S1]. Instantiation goes through `AVAudioUnit.instantiate(with:options:)` or `AudioComponentInstantiate`, which are asynchronous. Never block the main thread waiting on them [S3].
2. **Default process placement.** AUv3 units load **out of process** by default. AUv2 units load **in process** by default [S1]. On macOS a host can ask for either:
   - `.loadInProcess` for AUv3 works only if the plug-in is packaged for it, and it falls back to out-of-process **silently**. Check `isLoadedInProcess` [S3].
   - `.loadOutOfProcess` for AUv2 runs the unit in Apple's `AUHostingService` XPC service. This works today: Mixxx ships it [S9], `auval` has an `-oop` flag [S12], and validation has a `LoadOutOfProcess` parameter [S2].
3. **Cost of out-of-process.** Apple measured about **40 µs per render cycle per out-of-process unit**. At a 32-frame buffer that is about 5.5% of the 1 ms cycle, and it adds up across units [S6]. Out-of-process units add CPU and IPC overhead but no extra audio latency.
4. **Crash isolation.** A crash in an in-process plug-in kills the host [S6][S9]. A crash in an out-of-process plug-in kills only the service. The host gets `kAudioComponentInstanceInvalidationNotification` and keeps running [S2][S6].
5. **Architecture mismatch.** One process cannot mix arm64 and x86_64 code [S7][S8]. An Intel-only AUv2 can only run in an arm64 host **out of process**. Apple ships two services for this:
   - `AUHostingService` (`com.apple.audio.AUHostingService.arm64e`)
   - `AUHostingCompatibilityService` (`com.apple.audio.AUHostingService.x86-64`), which runs under Rosetta

   Both were verified on disk [S12]. Logic loads Intel AUs this way when Rosetta is installed [S10]. **Rosetta as a general tool ends after macOS 27** [S7], so Intel-only plug-ins have a shelf life.
6. **Entitlements.** Not sandboxed + no hardened runtime → no entitlements needed to load AUs. With hardened runtime (needed only for notarization), **in-process** third-party AUv2 needs `com.apple.security.cs.disable-library-validation` [S4]. In-process plug-ins also inherit every host entitlement and restriction [S5]. Out-of-process units run under Apple's service, which carries its own exceptions, so the host needs none of these for them [S12].
7. **State.** Use `fullStateForDocument` when saving a Song. It falls back to `fullState` (AUv2 `ClassInfoFromDocument` → `ClassInfo`) [S2]. The value is a property-list dictionary. Store it as a binary plist blob per Insert Chain slot, as JUCE does [S11].
8. **Latency and tail.** `AUAudioUnit.latency` and `tailTime` give seconds (AUv2 `kAudioUnitProperty_Latency` / `TailTime`) [S2]. Latency can change at runtime. JUCE listens for changes [S11].
9. **Validation.** The system caches validation results. `AVAudioUnitComponent.passesAUVal` reads that cache [S1]. `AudioComponentValidateWithResults` (macOS 13+) runs validation in-API, with timeout, force and out-of-process options [S2]. Logic validates every new or updated plug-in and hides failures until the user enables them [S13].

---

## 1. AUv2 vs AUv3: discovery and loading

### Discovery

- **`AVAudioUnitComponentManager.shared()`** searches by `AudioComponentDescription`, by `NSPredicate`, or by test block, and returns `AVAudioUnitComponent` objects without opening them [S1][S14]. Apple's example for "all effects" uses `componentType: kAudioUnitType_Effect` with zeros elsewhere [S1].
- Useful `AVAudioUnitComponent` properties on macOS [S14]:
  - `availableArchitectures`: Mach-O CPU types. Use it to detect Intel-only plug-ins before loading.
  - `passesAUVal`: the cached validation result.
  - `hasCustomView`, `isSandboxSafe`, `allTagNames`, `typeName`.
- To react to installs and uninstalls, observe `AVAudioUnitComponentManagerRegistrationsChangedNotification` (macOS 10.15+) [S14] or `kAudioComponentRegistrationsChangedNotification` [S2].
- **C API:** `AudioComponentFindNext` with a description where 0 acts as a wildcard [S2].
- Where AUv2 bundles live: the system scans `~/Library/Audio/Plug-Ins/Components`, `/Library/Audio/Plug-Ins/Components` and `/System/Library/Components` non-recursively [S2]. AUv3 units are app extensions, registered when their containing app is installed or launched [S2].
- How to tell the versions apart: the system sets `kAudioComponentFlag_IsV3AudioUnit` in `componentFlags` for v3 units [S2]. JUCE branches on this flag [S11].
- **Do not use the Carbon Component Manager.** Apps that target arm64 or link the macOS 11+ SDK cannot open AUs through it [S8].

### Instantiation

- The modern path is asynchronous:
  - Objective-C/Swift: `+[AUAudioUnit instantiateWithComponentDescription:options:completionHandler:]` or `AVAudioUnit.instantiate(with:options:completionHandler:)`.
  - C: `AudioComponentInstantiate`.

  Apple's docs: *"Do not block the main thread while waiting for the completion handler to be called; this can deadlock."* [S2] Units with views may require async instantiation (`kAudioComponentFlag_RequiresAsyncInstantiation`) [S2].
- The legacy synchronous `AudioComponentInstanceNew` always loads in process [S9].
- Options are **requests**: *"These options are just requests to the implementation. It may fail and fall back to the default."* [S2]

| | AUv2 (`.component` bundle) | AUv3 (app extension) |
|---|---|---|
| Default placement | In the host process [S1] | Separate extension process [S1][S2] |
| In-process on macOS | Always possible if the architecture matches | Only if the plug-in ships its code in a separate bundle and opts in with an `AudioComponentBundle` Info.plist entry, and the host passes `.loadInProcess` [S2][S6]. Falls back to out-of-process silently. Check `AUAudioUnit.isLoadedInProcess` [S3] |
| Out-of-process | `.loadOutOfProcess` / `kAudioComponentInstantiation_LoadOutOfProcess`. Runs in `AUHostingService` [S9][S12]. Check `kAudioUnitProperty_LoadedOutOfProcess` [S2] | Default |

- **Doc discrepancy:** Apple's sample-code article still says AUv2 plug-ins are *"always run as part of the host app's process"* [S15]. Newer evidence contradicts that:
  - `kAudioUnitProperty_LoadedOutOfProcess` exists, described as *"might happen at the request of the host or when loading in-process is not possible"* [S2].
  - `auval -oop` exists [S12].
  - Mixxx shipped out-of-process AUv2 loading in 2026 [S9].

  Treat the sample-code article as outdated on this point.
- What open-source hosts do:
  - **JUCE** (`juce_AudioUnitPluginFormatImpl.h`, `createAudioUnit`): AUv3 → `AudioComponentInstantiate(..., kAudioComponentInstantiation_LoadOutOfProcess, ...)`. AUv2 → `AudioComponentInstanceNew`, so always in process [S11].
  - **Mixxx**: switched AUv2 to `AudioComponentInstantiate` + `LoadOutOfProcess` so that a plug-in crashing in its static constructor no longer takes Mixxx down. It also waits at most 2 s per unit, using a dispatch group [S9].
  - **Apple's sample host**: `.loadOutOfProcess` by default, with an in-process toggle on macOS [S15].

### Costs

| | In process | Out of process |
|---|---|---|
| Render overhead | None beyond the plug-in itself | About **40 µs per render cycle** per unit (Apple measurement). *"if you are rendering at a very low latency of 32 frames, that's a 1 millisecond render interval, so overhead of 40 microseconds could be significant at 5.5 percent"* [S6] |
| Extra audio latency | None | None reported. Apple frames the cost as CPU per cycle, not added buffering [S6][S15] |
| Stability | A plug-in crash is a host crash [S6][S9] | The service process dies. The host is notified and keeps playing [S2][S6] |
| Security | Third-party code inside your process. It inherits your entitlements [S5] | Isolated in an Apple-signed service [S12] |
| Apple's own guidance | *"tradeoff here between safety and performance"* [S6]. In-process loading *"is still important for some apps, such as those that require minimal latency"* [S8] | *"safer and more secure, because a misbehaving plug-in can't corrupt or crash your app"* [S15] |

For Overdub, assume a 64–128 frame Monitoring buffer at 48 kHz (1.3–2.7 ms per cycle). A few out-of-process units per armed Track then cost a few percent of the cycle each. That is acceptable, but the spike should measure it, because it multiplies across Tracks × Insert Chain length.

## 2. Architecture mismatch (Intel-only AUv2 in an arm64 host, and the reverse)

- **The hard rule:** *"The system prevents you from mixing `arm64` code and `x86_64` code in the same process. Rosetta translation applies to an entire process, including all code modules that the process loads dynamically."* [S7]
- **Apple's plug-in guidance:** *"Code running in the same process must support the same architecture. If your app attempts to load a plug-in with an incompatible architecture, the system reports an error at load time."* and *"Plug-ins that run out-of-process using an XPC service may run using a different architecture … provide two non-universal XPC services — one to run `arm64` plug-ins and one to run `x86_64` plug-ins. A single XPC service can manage either native or translated plug-ins, but not both at the same time."* [S8]
- **What Apple built for AUs.** These are local observations on macOS 26.6 [S12]. Both services live in `/System/Library/Frameworks/AudioToolbox.framework/XPCServices/`:

  | Bundle | Display name | Bundle ID | Arch |
  |---|---|---|---|
  | `AUHostingServiceXPC_arrow.xpc` | `AUHostingService` | `com.apple.audio.AUHostingService.arm64e` | arm64e |
  | `AUHostingServiceXPC.xpc` | `AUHostingCompatibilityService` | `com.apple.audio.AUHostingService.x86-64` | x86_64 (runs under Rosetta) |

  Both declare `_MultipleInstances = true` and `NSPrincipalClass = NSViewServiceApplication`, which means they also vend remote views. This is exactly the two-service pattern from [S8].
- **What Logic does.** Apple Support says Logic Pro and Final Cut Pro *"support most Audio Units v2 and Audio Units v3 plug-ins on Mac computers with Apple silicon, whether or not the plug-in was built for use with Apple silicon"*. Intel-built AUs work *"if you install Rosetta"*, and are recognized only once Rosetta is installed [S10]. Logic itself needs to run under Rosetta only for in-process cases such as Melodyne in ARA mode [S16], because ARA has to live in the host process. Community reports, which are secondary, say Logic runs all AUs out of process in `AUHostingService`, with one compatibility service for Intel plug-ins [S17].
- **What a third-party host can do:**
  - **Native arm64 Overdub + universal or arm64 AUs:** load in process or out of process, the host's choice [S8].
  - **Native arm64 Overdub + Intel-only AUv2:** load with `kAudioComponentInstantiation_LoadOutOfProcess`. The system should route the unit to `AUHostingCompatibilityService`, and Rosetta must be installed [S10].
    - The header wording ("when loading in-process is not possible") suggests the system may also fall back automatically [S2]. Do not rely on that. Check `availableArchitectures` up front and request out-of-process explicitly.
    - **Verify in the spike**, because no Apple document states this flow for third-party hosts in so many words.
  - **Intel Mac (x86_64 Overdub) + arm64-only AU:** impossible. Such a plug-in cannot run on Intel hardware at all. This is not a hosting question.
  - **Overdub run under Rosetta on Apple silicon:** the whole process becomes x86_64. Intel AUv2 then loads in process, and arm64-only AUs need out-of-process [S7].
- **Shelf life.** *"Rosetta … will be available through macOS 27 — as a general-purpose tool for Intel apps … Beyond this timeframe, we will keep a subset of Rosetta functionality aimed at supporting older unmaintained gaming titles"* [S7]. Intel-only AU support therefore disappears for everyone after macOS 27. Related: Apple announced at WWDC25 that macOS 26 is the last macOS for Intel Macs [S18]. That matters for the "universal binary" requirement, which belongs to another ticket.

## 3. Plug-in UIs

- **AUv3 API (bridged for v2 too).** Check `AUAudioUnit.providesUserInterface`, then call `requestViewController(completionHandler:)`. The handler runs on an internal queue, so hop to main before inserting the view [S1][S2][S19]. You get an `AUViewControllerBase`, which is `NSViewController` on macOS [S19]. Size it from `preferredContentSize`, falling back to the view frame. JUCE clamps to at least 20×20 [S11].
- **Apple's sample host** uses this path for every unit. If `providesUserInterface` is false it falls back to `AUGenericViewController` (macOS 13+) [S15]. Optional extras for AUv3: `supportedViewConfigurations(_:)` / `select(_:)` [S19].
- **AUv2 Cocoa view factory (classic).** Read `kAudioUnitProperty_CocoaUI` (`AudioUnitCocoaViewInfo`: bundle URL and class names). Load the bundle, check `AUCocoaUIBase` conformance, then call `uiViewForAudioUnit:withSize:` to get an `NSView` [S2][S11].
  - This loads plug-in UI code **into the host process**, so it only makes sense for in-process AUv2.
  - JUCE tries `kAudioUnitProperty_CocoaUI` first, then `kAudioUnitProperty_RequestViewController` (the v2 property that vends a v3-style controller), then `AUGenericView` [S11].
- **Out-of-process units** have no class in the host to load. Their views arrive as remote view controllers vended by the hosting service. The service's principal class is `NSViewServiceApplication` [S12]. A forum report shows an `AUAudioUnitRemoteViewController` in this mode, along with a keyboard-event assertion crash that affected only out-of-process units, in 2022 [S20]. **Verify in the spike** that `requestViewController` returns the custom Cocoa view of an AUv2 loaded out of process, and that keyboard focus works.
- **SwiftUI:** there is no native AU view. Wrap the returned `NSViewController` in `NSViewControllerRepresentable`, or host it in a plain AppKit `NSWindow` or `NSPanel`, which is the common DAW pattern of one floating window per plug-in. The forum report reproduced the same issue with both SwiftUI and a plain floating panel [S20].
- **JUCE:** `AudioPluginInstance::createEditor()` wraps all of the above (`AudioUnitPluginWindowCocoa`) [S11].

## 4. State (saving an Insert Chain inside a Song)

- `AUAudioUnit.fullStateForDocument` is *"suitable for saving in a user's document … Hosts saving documents should use this property. If the audio unit does not implement it, the base class simply sets/gets fullState."* It is bridged to `kAudioUnitProperty_ClassInfoFromDocument` [S2].
- `fullState` is the preset-level snapshot, bridged to `kAudioUnitProperty_ClassInfo` [S2].
- For raw AUv2: *"a host app should use this property [ClassInfoFromDocument] first … If the audio unit returns an error (or doesn't implement this property) then the host should use … kAudioUnitProperty_ClassInfo."* [S2]
- **Value type:** `NSDictionary<NSString*, id>` / `CFDictionary`, a property list that may contain `NSData` blobs.
  - JUCE serializes `ClassInfo` with `CFPropertyListWrite(..., kCFPropertyListBinaryFormat_v1_0)`.
  - It restores with `CFPropertyListCreateWithStream` and `AudioUnitSetProperty(ClassInfo)`, then broadcasts parameter changes [S11].
  - JUCE uses `ClassInfo`, not `ClassInfoFromDocument`.
- After you set state, the host must tell listeners that parameters changed. The AUv2 header shows `AUParameterListenerNotify`. AUv3 emits KVO on the pseudo-property `allParameterValues` [S2].
- **Per slot, the Song needs:**
  - the `AudioComponentDescription` (type/subtype/manufacturer)
  - the plug-in name and version, for "missing plug-in" UI
  - the binary-plist state blob
  - bypass
  - optionally the preset name (`currentPreset`)
- **Presets (optional):**
  - `factoryPresets`
  - `currentPreset`
  - `userPresets` / `saveUserPreset` / `presetState(for:)` (macOS 10.15+, when `supportsUserPresets`)

  The dictionary from `presetState(for:)` is assignable to `fullState` / `fullStateForDocument` [S2][S15].

## 5. Latency and tail

- `AUAudioUnit.latency` (seconds; v2 `kAudioUnitProperty_Latency`, `Float64` seconds). It means *"delay due to signal processing … not delay or reverberation"*. A variable latency should be reported as a fixed maximum, or left as 0 [S2].
- `AUAudioUnit.tailTime` (seconds; v2 `kAudioUnitProperty_TailTime`). Tail is the decay after input stops, *"a conservative estimate"* [S2]. Use it to extend export renders and to decide when a chain is silent.
- **Latency can change at runtime**, for example on a lookahead setting. JUCE registers a property listener on `kAudioUnitProperty_Latency` and recomputes `latencySecs * sampleRate` [S11]. With `AUAudioUnit`, use KVO on `latency`.
- **Chain latency:** a host can tell each unit its context latency through `AUAudioUnitBus.contextPresentationLatency` (set per bus; v2 `kAudioUnitProperty_PresentationLatency`). Each chained unit's presentation latency is the previous unit's latency plus that unit's processing latency, so a chain's processing latency is the sum of its units' latencies [S2].
- **What this means for Overdub:**
  - Takes are recorded dry, so plug-in latency does not shift recorded audio. It only delays what the player hears while Monitoring.
  - On playback and export, compensating for Insert Chain latency per Track (delay compensation) keeps Tracks aligned.
  - Plug-ins that report 0 while actually adding latency cannot be detected by the host.

## 6. Crash isolation

- **In process:** a fault in a plug-in, including in its static constructor at load time, kills the host. Mixxx reproduced this with a test AU that writes to read-only memory: SIGBUS kills Mixxx [S9]. Apple: *"if it crashes inside your app, then users might blame you instead of the misbehaving plug-in"* [S6].
- **Out of process:** the extension or hosting-service process dies, and the host gets **`kAudioComponentInstanceInvalidationNotification`**. Its `object` is the `AUAudioUnit`, and its `userInfo` may include `audioUnit`, `Service PID`, `Host PID`, `Executable Path` and `Descriptions`. *"There can be multiple notifications for the same event"* [S2]. In Apple's WWDC demo, the AU process was force-quit: *"the view went blank but the music kept playing"* [S6]. The error `kAudioComponentErr_InstanceInvalidated` (-66749) exists for later calls [S2].
- **Caveat:** the userInfo carries an *array* of `Descriptions` that "may be responsible", and the services declare `_MultipleInstances` [S2][S12]. One hosting-service process can therefore host more than one unit, and one crash may invalidate several units at once. Treat every unit on that notification as dead: bypass it, mark it, and offer a reload.
- **Hang or timeout at instantiation:** async instantiation with a timeout lets the host skip a plug-in that never answers. Mixxx uses 2 s [S9].

## 7. Validation (auval)

- `auval` / `auvaltool`, version 1.10.0 here. Useful flags: `-a`, `-al`, `-l` (list without instantiating), `-v TYPE SUBT MANU`, **`-oop`** (load out of process), `-strict`, `-r N`, `-stress`, `-real-time-safety`. Exit codes: 0 OK, -1 AU failed, 4/5 unauthorized open/init, 1 general error, 2 fatal [S12].
- **In-process API:**
  - `AudioComponentValidate` (macOS 10.7+).
  - `AudioComponentValidateWithResults` (macOS 13+). It is asynchronous and returns a dictionary with `Output` (auval text), per-test `Tests`, and `WasCached`.
  - Parameters: `kAudioComponentValidationParameter_TimeOut`, `_ForceValidation` (ignore the cache), and `_LoadOutOfProcess` (*"allows a host that plans on loading the Audio Unit out-of-process to make sure that it passes the validation checks in this mode"*).
  - Results are cached per CPU architecture (`kAudioComponentConfigurationInfo_ValidationResult`, keyed by arch).

  Source for all of the above: [S2].
- `AVAudioUnitComponent.passesAUVal` exposes the cached result without running anything [S14].
- **What Logic does:** it validates automatically on first launch, after Logic updates, and when plug-ins are added or updated. Results show in the Plug-in Manager. Failed plug-ins are disabled but can be enabled manually, with the warning that they *"can cause problems"*. "Reset & Rescan Selection" re-validates [S13].
- Validation is slow. Listing every AU with `auval -al` here did not finish within 2 minutes on this machine [S12]. Overdub should never validate on the audio path or block launch on validation.
- **Options for Overdub:**
  - (a) Trust `passesAUVal` and the system cache, and show a warning badge on failures.
  - (b) On first sight of a new or changed component (key on description + version), run `AudioComponentValidateWithResults` in the background with a timeout and `LoadOutOfProcess` when that matches how the unit will be loaded. Persist the result, and hide failures behind a "show failed plug-ins" toggle, like Logic.
  - (c) Skip validation and rely on out-of-process loading for safety.

  These are options, not a decision.

## 8. Entitlements and code signing

Overdub is **not sandboxed**, so none of the sandbox AU rules apply:

- `sandboxSafe`
- `resourceUsage`
- `com.apple.security.temporary-exception.audio-unit-host`

Those rules exist only for sandboxed hosts [S2].

- **Hardened runtime is optional for a personal tool.** It is required to notarize [S5]. Without it, library validation is not enforced, and the host can load any AUv2 in process.
- **With hardened runtime:**
  - **Library validation** is on by default: *"prevents a program from loading frameworks, plug-ins, or libraries unless they're either signed by Apple or signed with the same Team ID as the main executable … Use the `com.apple.security.cs.disable-library-validation` [entitlement] if your program loads plug-ins that are signed by other third-party developers."* Gatekeeper runs extra checks on programs with it disabled [S4].
  - *"Shared libraries, frameworks, and in-process plug-ins inherit the entitlements of their host executable."* [S5] In-process plug-ins that use JIT or unsigned executable memory, such as some DRM or scripting engines, may then need `com.apple.security.cs.allow-jit` or `com.apple.security.cs.allow-unsigned-executable-memory` on the host [S21].
  - Apple's own `AUHostingService` and `AUHostingCompatibilityService` are signed with `com.apple.security.cs.disable-library-validation`, `com.apple.security.cs.allow-unsigned-executable-memory` and `com.apple.security.temporary-exception.audio-unit-host`, plus private entitlements [S12]. This is the effective policy for out-of-process units, and why a host that loads everything out of process needs none of these itself.
  - Audio input under hardened runtime needs `com.apple.security.device.audio-input` [S22]. A microphone usage string and the TCC prompt apply as usual.
- **Universal binary:** the host's architecture determines what loads in process, so ship Overdub universal [S8].

## Options for Overdub (facts only, no decision)

| Option | Approach | Pros | Cons |
|---|---|---|---|
| A. All out of process | Every unit via `.loadOutOfProcess` | Crash isolation; Intel-only AUv2 works with Rosetta; no host entitlements; one code path; system remote views | About 40 µs/cycle/unit [S6]; remote-view quirks [S20]; one service crash may take several units [S2] |
| B. Logic-like default | Out of process by default, per-plug-in "load in process" override | Same as A, plus low-overhead escape hatch | Two code paths for UI (remote vs Cocoa factory) |
| C. JUCE default | v2 in process, v3 out of process [S11] | Lowest CPU for v2; simplest with JUCE | v2 crash = Overdub crash; no Intel-only v2 on arm64; needs disable-library-validation if hardened |

## Open questions for the spike

1. With `.loadOutOfProcess`, does `requestViewController` return the custom Cocoa UI of a typical AUv2 (e.g. the NAM plug-ins installed on this machine)? Do key events work [S20]?
2. What is the measured render overhead per out-of-process unit at 64 and 128 frames on this Mac, and does it hold with 8 Tracks × 3 units?
3. Does an Intel-only AUv2 load from an arm64 build via `.loadOutOfProcess` with Rosetta installed, and what error appears without Rosetta?
4. How many `AUHostingService` processes appear for N units? This decides the blast radius of a crash.
5. Does `fullStateForDocument` round-trip for out-of-process AUv2 units, and how big are typical state blobs?

---

## Sources

| ID | Source | Type / trust |
|---|---|---|
| S1 | Apple, "Migrating Your Audio Unit Host to the AUv3 API", https://developer.apple.com/documentation/audiotoolbox/migrating-your-audio-unit-host-to-the-auv3-api | Apple docs (high) |
| S2 | macOS SDK 26.5 headers, `AudioToolbox.framework/Headers/AudioComponent.h`, `AUComponent.h`, `AUAudioUnit.h`, `AudioUnitProperties.h` (in `/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`) | Apple headers (high, authoritative API contract) |
| S3 | `AUAudioUnit.h`: `instantiateWithComponentDescription:…`, `isLoadedInProcess` (same SDK) | Apple headers (high) |
| S4 | Apple, `com.apple.security.cs.disable-library-validation`, https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.disable-library-validation | Apple docs (high) |
| S5 | Apple, "Hardened Runtime", https://developer.apple.com/documentation/security/hardened-runtime | Apple docs (high) |
| S6 | WWDC15 session 508 "Audio Unit Extensions", https://developer.apple.com/videos/play/wwdc2015/508/ (transcript via https://asciiwwdc.com/2015/sessions/508) | Apple talk (high; transcript is a third-party copy) |
| S7 | Apple, "About the Rosetta translation environment", https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment | Apple docs (high) |
| S8 | Apple, "Porting your audio code to Apple silicon", https://developer.apple.com/documentation/apple-silicon/porting-your-audio-code-to-apple-silicon and "Porting your macOS apps to Apple silicon", https://developer.apple.com/documentation/apple-silicon/porting-your-macos-apps-to-apple-silicon | Apple docs (high) |
| S9 | Mixxx PR #16106 "Fix AudioUnit startup crash by loading out-of-process (macOS)" (merged 2026-03-05), https://github.com/mixxxdj/mixxx/pull/16106; `src/effects/backends/audiounit/audiounitmanager.mm` | Open-source host (medium-high, working code) |
| S10 | Apple Support 102082, "About third-party Audio Units … on Mac computers with Apple silicon", https://support.apple.com/en-us/102082 | Apple support (high) |
| S11 | JUCE @ `be29c81` (2026-09-28): `modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h`, `modules/juce_audio_processors/format_types/juce_AudioUnitPluginFormat.mm`, https://github.com/juce-framework/JUCE | Open-source host framework (high for what JUCE does) |
| S12 | Local inspection on macOS 26.6 (25G72), arm64: `plutil`/`lipo`/`codesign -d --entitlements` on `/System/Library/Frameworks/AudioToolbox.framework/XPCServices/AUHostingServiceXPC*.xpc`; `auval -h` | Direct observation (high, but undocumented internals may change) |
| S13 | Apple, Logic Pro User Guide "Use the Plug-in Manager", https://support.apple.com/guide/logicpro/use-the-plug-in-manager-lgcp9e26ef17/10.7/mac/11.0 | Apple support (high) |
| S14 | macOS SDK 26.5 `AVFAudio.framework/Headers/AVAudioUnitComponent.h` | Apple headers (high) |
| S15 | Apple sample "Incorporating Audio Effects and Instruments", https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments (article and `Shared/AudioUnitManager.swift`) | Apple sample (high; AUv2 "always in-process" note is outdated) |
| S16 | Apple Support 121464, "Turn off Rosetta for Logic Pro for Mac", https://support.apple.com/en-us/121464 | Apple support (high) |
| S17 | MacRumors forum, "Native M1 plugins are NOT handled natively in Logic Pro? (AuHostingService)", https://forums.macrumors.com/threads/native-m1-plugins-are-not-handled-natively-in-logic-pro-auhostingservice.2326554/ | Community (low; corroborates S12) |
| S18 | Apple Newsroom, macOS Tahoe 26 (June 2025), https://www.apple.com/newsroom/2025/06/macos-tahoe-26-makes-the-mac-more-capable-productive-and-intelligent-than-ever/ (and WWDC25 Platforms State of the Union) | Apple (medium; the "last Intel release" statement is from the keynote/SOTU) |
| S19 | macOS SDK 26.5 `CoreAudioKit.framework/Headers/AUViewController.h` | Apple headers (high) |
| S20 | Apple Developer Forums thread 703547 "Audio Unit hosting: Mysterious error…" (Apr 2022), https://developer.apple.com/forums/thread/703547 | Developer report (low-medium; no Apple reply) |
| S21 | Apple, `com.apple.security.cs.allow-jit` and `com.apple.security.cs.allow-unsigned-executable-memory`, https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit, https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-unsigned-executable-memory | Apple docs (high) |
| S22 | Apple, `com.apple.security.device.audio-input`, https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.device.audio-input | Apple docs (high) |
