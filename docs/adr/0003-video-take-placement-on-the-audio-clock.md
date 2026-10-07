# Video Takes are placed on the audio clock by heard time, and follow the audio transport

The audio interface's output is the only clock. Every duplex callback yields an anchor `(song sample at buffer start, host time)`. JUCE exposes only the output `mHostTime` in duplex, and that is enough. A Video Take is captured through `AVCaptureSession` (camera only) into `AVAssetWriter`. Its first frame's PTS is converted to host time with `CMSyncConvertTime`, then mapped to the song sample that is **heard** at that instant: output IO time + output latency, through a least-squares fit of the take's anchors. That is the same reference Takes use (ADR 0002), so a sound and the frame that shows it land on the same song sample. The Video Take stores that start position in song samples, plus the take's audio-clock rate against host time (the drift ratio). Camera latency isn't reported by any API, so a per-camera **calibration offset** is subtracted; it defaults to 0.

Playback pulls frames: on each display refresh, the heard song position (latest anchor + elapsed host time − output latency + one refresh) is converted to movie time, and the matching decoded frame is shown. The video never runs a clock of its own. Export builds an `AVMutableComposition`, with the offline-rendered mix at 0 and the Video Take at its sample-exact offset, scaled by the drift ratio. `AVAssetExportSession` writes the MP4. The full-song variant uses an `AVMutableVideoComposition` with a black background over the gaps.

The spike (issue #7, branch `spike/av-sync`) measured this on the built-in FaceTime camera with the built-in mic and speakers over an 11.3-minute take. Clap offsets were +0.6 ms at the start and +2.0 ms at the end (sd ≈ 10 ms, against a ±17 ms frame-resolution limit at 29 fps), with no measurable drift. Without the fit, the audio clock's −8.8 ppm would have shifted the video 6 ms by the end. The trimmed export reproduced every clap within 0.8 ms. Playback error was pure frame hold, in [−35, 0] ms.

## Considered Options

- **Wall-clock seconds from the first audio anchor (no fit)**: simplest, but it accumulates the interface's drift against host time. That's only 6 ms over 11 minutes here, but a real interface on its own crystal can be worse, and the fit costs nothing.
- **AVPlayer or AVSampleBufferDisplayLayer slaved to a `CMAudioDeviceClock` (P1/P2)**: less code for continuous play, but it is a second clock to keep in step with the engine's transport on every start, seek and scrub.
- **Camera audio as a scratch track, cross-correlated against the Take (option C)**: automatic per-take offset verification, but it opens the input a second time and was left untested.
- **`AVAssetReader` → `AVAssetWriter` re-stamping export (E2)**: full control, more code. E1 already reproduces offsets to under 1 ms.

## Consequences

- The Song stores a Video Take as: movie file, start position in song samples, drift ratio (song samples per movie second, or ppm), and the camera ID with the calibration value applied at record time.
- The camera calibration also absorbs any error in reported output latency. It is therefore a per-machine setting keyed by camera *and* output device, like ADR 0002's offset. It is never part of a Song. The built-in camera with the built-in devices measured 0 ms.
- Cameras can stall for over a second without a drop callback, so nothing may rebuild timestamps from frame counts. Playback holds the last frame across a gap.
- The full-song export re-times frames onto its output frame grid. That costs up to ±½ frame per frame, but adds no offset.
- Untested, and needed before the spec is final: a USB or Continuity Camera's calibration and a real interface's drift.

## Amendment: the engine renders ahead by the plan latency (issue #24)

The engine's graph delays every Track path by the plan latency L (PDC, ADR 0001), so the transport renders L samples ahead: on each callback the Track nodes read from `position + L`, and after the graph's delay, output sample n of the block is Song sample `position + n`. The anchor's song sample is therefore the Song sample at the block's first **output** sample, after PDC. We chose this so that every consumer (Recording, Video, the playhead) maps an output sample to a Song sample with no latency term of its own, and a plan swap that changes L moves nothing in time.

- Song samples `start .. start + L − 1` are never heard after playback starts at `start` (play, seek, or the first plan). Those output samples are silence: after a seek, play, stop or plan swap, the engine silences the first L output samples, because the graph's delay lines still hold audio rendered before the change. Hearing a few milliseconds less after a start is accepted; outputting stale audio is not.
- The anchor's host time is the device's output IO time for that buffer and **excludes the device's output latency**. Heard time stays output IO time + output latency (+ the calibration offset, ADR 0002), applied by the consumer, as above.
- `Engine::position()` and the anchor both count in output samples: they do not move by L when the plan changes.
