# Headless verification uses computed expectations, driven through one engine callback

Every implementation slice must build and pass the full `ctest` suite, and it must also name its own observable check in its acceptance criteria. Audio, latency and A/V sync are checked headlessly, without a human listening. The engine's duplex callback is a single plain function, `process(inputs, outputs, numSamples, hostTimeNs)`. The live `AudioIODevice` calls it, and tests call it in a loop with synthetic input and fake host times. That way the record path (dry Take writing, anchors, latency placement) runs in tests exactly as it does live. Assertions are computed from synthetic stimuli: impulses for placement and PDC, null tests for gain, fades, mute/solo and layering, and sines for pan law. We chose this because every line is AI-written. A session can't judge whether a changed golden file is right, so it would regenerate goldens to make a test pass. A computed expectation can't be gamed that way, and its failure explains itself ("impulse at 1012, expected 1024").

## Considered Options

- **Golden WAV/MP4 files with a tolerance**: cheap to write, but they're opaque to an AI session and easy to "fix" by regenerating them.
- **Calling `AudioProcessorGraph::processBlock` directly**: no seam to design, but the record path, anchors and Take placement go untested.
- **A fake JUCE `AudioIODeviceType` registered in tests**: closer to the live wiring, but more JUCE surface to get right, and no extra coverage over the plain callback.
- **RealtimeSanitizer (`-fsanitize=realtime`)**: the proper real-time check, but Apple clang 21 rejects it on x86_64, and an upstream LLVM would be a second toolchain to maintain.

## Consequences

- Logic tests host test-only `AudioProcessor`s: gain, a fixed N-sample delay that reports N samples of latency, and silence. Tests tagged `[au]` load Apple's AUPeakLimiter (which reports real latency) and AUDelay out-of-process through the real loader, so they also prove ADR 0002's JUCE patch. They run in plain `ctest`, because those AUs exist on every Mac.
- A/V sync: the anchor fit, heard-time mapping, drift ratio and playback frame selection are pure functions with unit tests. Their synthetic anchors include injected ppm drift and camera stalls. Export is tested as a round trip. A synthetic movie with flash frames and a mix with clicks at known samples go through the real trimmed and full-song export, then the test decodes the MP4 and measures each marker.
- A test-only allocation and lock guard wraps the callback under `ctest`. It is set through a thread-local flag and fails the test when the audio path allocates or locks. That includes allocations inside JUCE's graph.
- Only real devices can prove loopback latency, camera offset and drift on a real interface. Those checks are app modes, `overdub --selftest loopback` and `overdub --selftest clap <dir>`, and each one writes a JSON pass/fail report. Only slices that touch capture or calibration carry "a human runs the selftest and pastes the JSON" as a HITL acceptance step. The user-facing calibration flow reuses the same measurement code.
- Every slice runs the full suite, which stays under about 90 s through the test presets' parallel `jobs` (amended, issue #63; the export test uses a 10 s movie with a 1 s trim). If the suite grows, slow tests get a tag and a `ctest -L fast` inner loop, but the gate is still the full run.
- These tolerances are part of the spec. Loosening one is a spec change, not a test fix:

  | Check | Tolerance |
  |---|---|
  | Synthetic placement and PDC | sample-exact |
  | Null tests | peak residual ≤ −120 dBFS |
  | Export A/V offset | within ±½ frame per marker, end − start ≤ 1 ms |
  | Clock-fit unit tests | recovered drift within 0.1 ppm of the injected value |
  | Loopback selftest | jitter ≤ 5 samples across bursts |
  | Backing Track import, lossless (WAV, AIFF, CAF, FLAC, ALAC) | click lands sample-exact; length = source length × Song rate / source rate |
  | Backing Track import, AAC (M4A) and MP3 | click (loudest sample) sample-exact: error measured as 0 samples in issue #25 at 48 → 48 kHz and 48 → 44.1 kHz (AAC at Core Audio's default bit rate, MP3 128 kbit/s CBR from `lame`); length as lossless, priming trimmed |
  | Backing Track import, resampled sine | frequency within 0.01 Hz (1 kHz, 48 → 44.1 kHz) |
- UI verification is covered by the amendment below.

## Amendment: UI verification (issue #17)

The JUCE Component UI is checked the same way as audio: by computed expectations in `ctest`, never by comparing pixels. Components stay thin. They translate events and draw. Timeline geometry (sample ↔ x, hit-testing), and the mapping from a gesture to a Song operation, are pure functions next to the Component, unit-tested without JUCE. Component tests build the real Component tree under `ScopedJuceInitialiser_GUI` at a fixed size against a fixture Song. They deliver synthesized mouse and key events, then assert on the resulting Song, the history step label, and child bounds and visibility. Screenshots exist only so someone can look at them. A `[snapshot]`-tagged test renders each surface off-screen with `createComponentSnapshot` into `build/ui-snapshots/<slice>/`. The session inspects the PNGs, fixes what's visibly wrong, and attaches them to the slice's closing comment. Nothing passes or fails on pixels. We chose this for the same reason as golden audio files: a session would regenerate a golden screenshot to make a test pass.

### Considered Options

- **Golden screenshots with a pixel tolerance**: catch visual regressions, but they're opaque to a session and easy to "fix" by regenerating, and fonts and antialiasing shift with OS updates.
- **Driving the launched app through macOS accessibility**: exercises the real app, but needs real devices and permissions, and is flaky without CI.
- **A human UI checklist on every slice**: catches feel problems, but makes every UI slice wait on a person. Feel problems become new issues instead.

### Consequences

- Fixture Songs are built in code by one shared test-support builder, with generated media (sines, clicks). No `.overdub` packages are committed as fixtures.
- Slice 6 (App shell and bare Record mode) builds the harness: fixed-size Component helpers, event synthesis, the snapshot test and the fixture builder. It confirms that synthesized `MouseEvent`s work without an on-screen peer. If they don't, tests call the Component's handlers directly, and this amendment records which approach is used.
- Each UI slice's acceptance names its pure-logic tests, its Component tests and the surfaces it snapshots.
- Human UI checks stay where the hardware or third-party code demands them: the skeleton walkthrough in slice 6, and third-party AU editor windows in slice 13.

### Settled by slice 6 (issue #27)

- **Events without a peer.** A synthesized `MouseEvent` (built on the desktop's main mouse source) works without an on-screen peer when it is delivered by calling the target Component's `mouseDown` / `mouseUp` handlers: a `Button` clicked that way fires `onClick` exactly as it does live. JUCE's own routing (`ComponentPeer::handleMouseEvent`) needs a peer, so tests never go through it. Keys are delivered to `keyPressed` as a `KeyPress`. A `ComboBox` popup needs a peer, so tests pick an item with `setSelectedItemIndex (…, sendNotificationSync)`, which runs the same `onChange` as a user's pick. The helpers are in `tests/ui/UiHarness.h`.
- **Harness.** `tests/ui/UiHarness.h` (fixed-size layout, click, key, ComboBox pick, `isShown`, `writeSnapshot`) and `tests/ui/MainWindowFixture.h` (the real `ui::MainComponent` over a `ui::OpenSong`, with an `Engine` driven by the loopback fake device and injected Alert and file-chooser hooks). The fixture Song builder is `tests/song/FixtureSongBuilder.h`: Backing Tracks are generated sines imported through the real import path, plain Tracks are added as one "Add Tracks" step.
- **Snapshots.** `[snapshot]` tests write `build/ui-snapshots/slice-NN/<surface>.png` (the path is fixed at configure time, whatever the preset). The window's timer never fires in tests; they call `MainComponent::tick()` where the timer would.

## Amendment: suite time target (issue #63)

Measured on a 6-core, 12-thread i7-9750H with 642 test cases: 444 s serial, 102 s at `-j8`, 82 s at `-j12`. No single case is the cause: the slowest is 6 s (the MP4 and `[snapshot]` cases at 3 to 6 s, under load), and the median is 0.6 s. Every case is its own ctest process, and launching `overdub_tests` costs about 0.5 s (dyld binding of the JUCE and Apple frameworks; the app binary pays the same, and `-dead_strip_dylibs` or `-no_fixup_chains` do not change it). That fixed cost times the case count is about 350 s serial and is the floor of the suite.

- The target is about 90 s for the full `ctest --preset debug` run, which now runs `jobs: 12` by default. The earlier 60 s target predates 642 cases and was not reachable without batching cases into fewer processes, which would lose per-case `ctest -R` selection.
- Running in parallel is reliable: test folders use UUID names (PR #56) and no test opens a device.
- No tolerance in the table above changes.
- If the suite outgrows 90 s, add the `fast` label described above before touching any tolerance.
