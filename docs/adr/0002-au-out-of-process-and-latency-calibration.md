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
