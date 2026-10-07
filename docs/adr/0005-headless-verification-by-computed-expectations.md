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
- Every slice runs the full suite, which stays under about 60 s (the export test uses a 10 s movie with a 1 s trim). If the suite grows, slow tests get a tag and a `ctest -L fast` inner loop, but the gate is still the full run.
- These tolerances are part of the spec. Loosening one is a spec change, not a test fix:

  | Check | Tolerance |
  |---|---|
  | Synthetic placement and PDC | sample-exact |
  | Null tests | peak residual ≤ −120 dBFS |
  | Export A/V offset | within ±½ frame per marker, end − start ≤ 1 ms |
  | Clock-fit unit tests | recovered drift within 0.1 ppm of the injected value |
  | Loopback selftest | jitter ≤ 5 samples across bursts |
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
