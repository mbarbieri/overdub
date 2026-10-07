# AUs run out-of-process; Takes are placed by reported latency plus a calibration offset

Every AU in an Insert Chain is instantiated out-of-process, in Apple's `AUHostingService`. JUCE 9 always loads AUv2 in-process (`AudioComponentInstanceNew`), so we keep a one-line patch to `external/JUCE` that passes `kAudioComponentInstantiation_LoadOutOfProcess` for AUv2 too. In the AU spike (issue #6), NeuralAmpModeler's own Cocoa view segfaulted in-process and killed the host. Out-of-process, the same plugin and its editor worked. The isolation cost about 30 µs per unit per callback, added no latency, and caused no xruns at 32 frames. Out-of-process loading is asynchronous, so a plugin can't block the message thread while it instantiates. On Apple Silicon it is also the only way to load Intel-only AUv2s.

A Take is placed at its first dry sample's timeline position minus (device input latency + output latency + graph PDC latency + **calibration offset**). JUCE's reported latency is exactly the CoreAudio device latency + safety offset + buffer + stream latency. On the built-in MacBook devices that over-reported the real round trip by a constant 821 samples (18.6 ms at 44.1 kHz) at every buffer size from 32 to 256, with 2–3 samples of jitter. So the app ships a loopback calibration (noise bursts out, cross-correlate what comes back in) that stores one offset per device and sample rate. The offset defaults to 0, so reported latency is used until the user calibrates.

## Considered Options

- **In-process by default, out-of-process per plugin or only when required**: no JUCE patch, but one buggy plugin view or DSP crash takes down the Song being recorded.
- **Manual latency nudge instead of calibration**: simpler, but users can't measure 18 ms by ear, and the spike already showed the measurement is cheap and stable.
- **Always calibrate, ignore reported latency**: robust, but blocks first use of every device behind a cable-patching step.

## Consequences

- `external/JUCE` carries a local patch, re-applied whenever JUCE is bumped. The spike's version is `spikes/au-monitoring/juce-force-auv2-oop.patch` on branch `spike/au-monitoring`.
- Plugin editors arrive as remote views. They open at a placeholder size and resize once the view attaches, so `DocumentWindow` must stay resizable to content. Set the native title bar *before* attaching an editor, because changing it afterwards re-parents the plugin's `NSView`.
- Watch `kAudioComponentInstanceInvalidationNotification` to detect a crashed hosting process. One `AUHostingService` process can host several units.
- The calibration offset is a per-machine device setting, not part of a Song.
- JUCE's `RuntimePermissions` is a no-op on macOS. Request microphone access through `AVCaptureDevice` before opening the audio device, or the TCC prompt blocks the message thread inside CoreAudio's device open.

## Amendment: recording details settled by the implementation (issue #26)

- **Placement under render-ahead.** The engine renders L (plan PDC) samples ahead (ADR 0003 amendment), so the first captured input sample was captured while the Track nodes rendered Song sample start + L. A Take's `origin` is `start + L − (input latency + output latency + L + calibration offset)`, which is `start − (input + output + calibration)`. `TakeCompensation.reportedLatencySamples` stores `input + output + L`.
- **The Take keeps every dry sample**; its Clip starts at the Pass start (the record point) or at `origin` if that is later, with `sourceOffset` set so each sample stays at its placed position. Input captured before the record point is in the file but not heard.
- **A Pass covers one continuous run of the transport.** It starts on the first playing block after record is pressed, and ends at stop, or when the transport stops or seeks.
- **Calibration store.** `~/Library/Application Support/Overdub/latency-calibration.json` keeps one offset per (input device, output device, sample rate); a missing entry is 0. `engine.device_opened` logs the offset in use.
- **Selftest.** On the built-in MacBook devices at 44.1 kHz with a 512 buffer, `--selftest loopback` measured `offset_samples` −821 and a jitter of 3 samples, matching the spike: the calibration flow (slice 18) stores `offset_samples` added to the current offset.
- **Overruns.** A block that does not fit an Armed Track's FIFO is recorded as silence of the same length, so later samples keep their place; the count is in `capture.pass_stopped` (`overrun_frames`).
- **Anchors** of a Pass (one per callback, ADR 0003) are returned in memory with the finished Pass (`capture::RecordedPass::anchors`), not stored in the package; slice 14 keeps what the Video Take needs.
- **Plain Monitoring** adds an Armed Track's input at its Track node, so it is delayed by the plan's PDC like the Track's playback. With no AUs (slice 12) that delay is 0.
