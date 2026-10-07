# PROTOTYPE: Video Take + audio on one clock, synced playback, MP4 export (issue #7)

Throwaway spike. It lives only on branch `spike/av-sync` and is never merged to main.

**Question:** can the chosen stack (JUCE 9 + Objective-C++ AVFoundation) capture a Video Take alongside audio, play it back in sync, and export it in sync? And what clock mapping works, with what offsets and drift over a ≥10-minute take?

## Run

```sh
spikes/av-sync/build.sh            # Release build
open -n "build/spike-av-sync-Release/OverdubAvSpike_artefacts/Release/Overdub AV Spike.app"
```

Interactive: **Record**, clap in frame, wait, clap, **Stop**, then **Play**, then **Export MP4s + analyse**. Sessions go to `~/Library/Logs/OverdubAvSpike/sessions/session-*`, and the log goes to `~/Library/Logs/OverdubAvSpike/spike.log`.

Headless re-runs on a recorded session (no camera needed):

```sh
open -n -W "<app>" --args --session <dir> --play --quit      # plays through the default output, writes playback-*.csv
open -n -W "<app>" --args --session <dir> --export --quit    # both MP4s, then the clap analysis
open -n -W "<app>" --args --session <dir> --analyse --quit   # clap analysis only
```

`--rt-calibration <samples>` (default 821, from ADR 0002) and `--camera-calibration-ms <ms>` (default 0) change the placement of new recordings.

## How it works

- **One clock: the audio output.** The JUCE duplex callback stamps every buffer with `(song sample at buffer start, host ns)`. In duplex, JUCE passes only the *output* `mHostTime`. A least-squares fit over all anchors gives `hostNs = a + b·songSample`. `b` carries the audio clock's drift against host time. A video frame captured at host time `H` belongs at the song sample that is **heard** at `H`: `(H − outputLatency − a) / b`. Takes are placed the same way: by "heard" time, via the round-trip compensation of ADR 0002.
- **Capture** (`CameraRecorder.mm`). It uses `AVCaptureSession` with the camera only, then `AVCaptureVideoDataOutput` and `AVAssetWriter` (H.264 `.mov`), with `startSession(atSourceTime: firstPTS)`. Each frame's PTS goes through `CMSyncConvertTime(pts, session.synchronizationClock, hostClock)`. `frames.csv` logs PTS, host time and delivery time.
- **Video Take placement** (`session.json`): `videoOffsetSamples` = song sample heard at the first frame's host time, minus the camera calibration. `clockNsPerSample` is the drift-corrected rate. Movie time `t` plays at song sample `videoOffsetSamples + t·1e9/clockNsPerSample`.
- **Playback** (`VideoView.mm`, option P3). On every vblank: heard song sample (latest anchor + elapsed host time − output latency + one refresh of display lead) → movie time → show the latest decoded frame ≤ that time. Frames come from `AVAssetReader` (BGRA, IOSurface) and are set as `CALayer.contents`. The audio transport is the only clock.
- **Export** (`Exporter.mm`, option E1). `mix.wav` is inserted at 0 and the Video Take at `CMTimeMake(round(videoOffsetSamples), sampleRate)` in an `AVMutableComposition`. `scaleTimeRange` applies the drift ratio. `AVAssetExportSession` writes the MP4, either trimmed to the video span or as the full song with an `AVMutableVideoComposition` (black background) over the gaps.
- **Clap analysis** (`ClapAnalysis.mm`). Audio transients are found by threshold crossing. For each one, it writes a contact sheet of every frame within ±300 ms, labelled `d = frame time − clap time`. The same code runs on the session (`take.wav` + `video.mov` at the stored offset) and on each exported MP4 (its own audio and video), so export error shows up as a difference between the tables.

## Results (2026-10-07, Intel i7-9750H MacBook Pro, macOS 26.6)

Rig: built-in mic in, built-in speakers out (JUCE CoreAudio aggregate), 44.1 kHz, 512-frame buffer, reported in 2876 / out 1057 samples, round-trip calibration 821. Camera: FaceTime HD (built-in), 1280×720, 29 fps nominal, 34.4–34.7 ms mean frame interval in this light. Files: `results/`.

### A/V offset, 11.3-minute take (`long-take-*`)

The clap moment lies between the last frame with the hands apart and the first frame with them together (exposure ≈ frame interval, so both are blurred). The estimate is the midpoint, ±½ frame (±17 ms). The readings are mine, from the contact sheets.

| clap | audio s | last apart d (ms) | first together d (ms) | midpoint (ms) |
|---|---|---|---|---|
| 0 | 6.30 | −21.6 | +13.4 | −4.1 |
| 1 | 7.09 | −27.0 | +6.3 | −10.4 |
| 2 | 7.93 | −3.6 | +31.4 | +13.9 |
| 3 | 8.78 | −21.3 | +13.7 | −3.8 |
| 4 | 9.62 | −10.2 | +24.8 | +7.3 |
| 5 | 673.67 | −34.4 | +0.6 | −16.9 |
| 6 | 674.37 | −16.7 | +18.3 | +0.8 |
| 7 | 675.05 | −8.5 | +24.8 | +8.2 |
| 8 | 675.85 | −9.6 | +25.4 | +7.9 |
| 9 | 676.74 | −7.3 | +27.7 | +10.2 |

- **Offset:** start mean +0.6 ms, end mean +2.0 ms (positive = video late), sd ≈ 10 ms. That's indistinguishable from 0 at frame resolution. So for this camera and device pair, the camera calibration is **0 ms**.
- **Drift:** +1.4 ms of change in offset across 11 minutes, inside measurement noise. Without the clock fit, the audio clock (−8.78 ppm against host) would have moved the video by **−6.0 ms** by the end (`driftAtEndMs`).
- **Clocks:** `AVCaptureSession.synchronizationClock` *is* `CMClockGetHostTimeClock` on macOS 26.6. Relative rate is exactly 1.000000000, and converted vs raw PTS diverged by 0.0 µs. Audio clock against host: −8.4 to −11.8 ppm across 4 takes (−8.48 in playback vs −8.44 in recording on the same take). Anchor fit residual: 15.6 µs RMS, 58 µs max over 58,603 anchors.
- **Frames:** 19,448 frames, 0 writer drops, 0 `didDrop` callbacks, mean delivery delay 4.5 ms (max 116). But the camera **stalled** 5 times for 0.4–1.5 s near the end (`long-take-frame-gaps.txt`), and nothing reported it. Timestamps stayed true across the gaps, so sync was unaffected. Playback holds the last frame.
- **Playback:** shown frame minus target over 59 Hz vblanks: mean −17.6 ms, range [−35, 0] ms, aside from one −64 ms at the 69 ms startup gap. That's exactly the sample-and-hold error at 29 fps, with no systematic offset. The user watched the claps and judged them in sync.
- **Export:** the trimmed MP4 reproduces the session's per-clap `d` within 0.8 ms at both ends, so the offset and `scaleTimeRange` drift correction survive AAC encoding (the audio's first PTS is 0.000 ms, so no priming leaks). The full-song MP4 re-times frames onto the composition's frame grid, so individual `d` values move by up to ±½ frame (for example, clap 0: −21.6 → −11.6). Its black lead-in and tail render correctly. An 11.3-minute export took ~45 s per MP4 on this Intel Mac.

### Smoke take (`smoke-*`)
A 21 s take gave the same picture: midpoint offsets −10.5 and +11.2 ms, trimmed export within 0.2 ms, and playback error range [−35, 0] ms.

## Framework notes

1. **JUCE hides the input timestamp in duplex.** `AudioIODeviceCallbackContext::hostTimeNs` is `inOutputTime` whenever outputs are active. That's sufficient, because a Video Take aligns to *heard* time like the audio Takes. But it means the camera calibration absorbs any output-latency error, so it's really per camera *and* output device.
2. `AVCaptureVideoDataOutput` + `AVAssetWriter` gives the first frame's PTS directly. `AVCaptureMovieFileOutput`'s `startPTS` delegate (15.2+) wasn't needed.
3. A late-arriving camera stall isn't reported as a drop. Only PTS gaps in `frames.csv` show it.
4. The AVFoundation TCC prompts are requested before CoreAudio opens (ADR 0002 finding 4).

## Not tested

A USB or Continuity Camera; a real interface (separate crystal); the arm64 half; recording while a Backing Track plays (the same anchors would apply); camera audio as a scratch track for automatic offset checks (option C); `AVPlayer`/`AVSampleBufferDisplayLayer` playback (P1/P2); seeking and scrubbing (`VideoView` is forward-only).
