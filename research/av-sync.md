# A/V sync: capturing camera and audio on a shared clock, through playback and export

Research for [issue #3](https://github.com/mbarbieri/overdub/issues/3) (map: [#1](https://github.com/mbarbieri/overdub/issues/1)).
It feeds "Choose the tech stack" and "Spike: record camera + audio, play back in sync, export MP4".
It reports facts and options. It does not make the decision.

Terms follow [`GLOSSARY.md`](../GLOSSARY.md): Song, Track, Take, Video Take, Armed, Monitoring, Insert Chain, Master.

Researched 2026-10-07 against Apple developer documentation (JSON doc pages and the macOS SDK headers), one WWDC session, Apple Developer Forums answers by Apple engineers, ITU-R BT.1359-1, and the source code of OBS Studio, GStreamer and PortAudio. Every claim carries a source tag like **[S3]**. Text marked *Inference* is my reasoning from those facts, not a documented statement.

---

## TL;DR

1. **Host time (`mach_absolute_time`) is the common pivot.** Core Audio IO timestamps carry `mHostTime` in `mach_absolute_time` units [S10]. `AVAudioTime` carries host time and sample time [S7]. Every capture sample buffer is stamped on `AVCaptureSession.synchronizationClock` [S1], and `CMSyncConvertTime(t, sessionClock, CMClockGetHostTimeClock())` maps that onto host time, compensating for measured drift when the clocks differ [S3][S5].
2. **Audio should come from the audio engine, not AVCaptureSession.** Monitoring through Insert Chains of AU plugins needs a low-latency Core Audio / AVAudioEngine render path. AVCaptureSession is then used for the camera only. Both streams get stamped in host time and aligned after capture. OBS Studio does exactly this on macOS: Core Audio `mHostTime` and AVCapture PTS both go onto one `CLOCK_UPTIME_RAW` nanosecond clock [S20][S21][S22].
3. **The audio device clock is the master.** The Song timeline is counted in audio samples, which tick on the interface's crystal, not on host time. `CMAudioDeviceClockCreate` gives a `CMClock` that follows that device [S4][S11]. Converting video timestamps into that clock with `CMSyncConvertTime` removes long-take drift [S3]. Apple's own guidance is to drive video from the audio device's clock "to prevent drift" [S16].
4. **Fixed offsets are partly reported and partly not.** Audio input latency is reported (`kAudioDevicePropertyLatency` + `kAudioStreamPropertyLatency` + safety offset) [S11][S12]. Camera pipeline latency (built-in, USB, Continuity Camera) is not documented by any API I found. It has to be measured with a clap/flash test and stored as a per-camera calibration offset.
5. **Accuracy target.** ITU-R BT.1359-1 puts the detectability thresholds at about +45 ms (sound early) to −125 ms (sound late) [S24]. At 30 fps, one frame is 33.3 ms, so frame-accurate alignment (±½ frame ≈ ±17 ms) sits inside the undetectable zone.
6. **Export.** `AVMutableComposition` can place the Video Take at an arbitrary `CMTime` offset next to a rendered mix (offline `AVAudioEngine` manual rendering [S27]) and export with `AVAssetExportSession`. Alternatively, write frames and mix with `AVAssetWriter`, which preserves sample timestamps and inserts empty edits where needed [S26].
7. **Playback.** Three viable options exist: `AVPlayer` with `sourceClock` set to the audio device clock plus `setRate(_:time:atHostTime:)` [S14][S15]; `AVSampleBufferDisplayLayer` with a `controlTimebase` built on the audio device clock [S16]; or pull frames with `AVPlayerItemVideoOutput.copyPixelBuffer(forItemTime:)` from a display-link callback, computing the item time from the audio transport position [S18][S19].

---

## 1. Clock domains on macOS

| Domain | What it is | API | Source |
|---|---|---|---|
| **Host time** | `mach_absolute_time` ticks; the system's monotonic clock. | `mach_absolute_time()`, `CMClockGetHostTimeClock()`, `CACurrentMediaTime()` | "In macOS, the host time clock uses `mach_absolute_time` but returns a value with a large integer timescale (e.g. nanoseconds)." [S5] |
| **Audio device time** | The interface's sample counter (`mSampleTime`), ticking on its own crystal. | `AudioTimeStamp.mSampleTime`, `AVAudioTime.sampleTime` | `AudioTimeStamp` "holds different representations of the same point in time": `mSampleTime` ("absolute sample frame time"), `mHostTime` ("mach_absolute_time"), `mRateScalar` ("ratio of actual host ticks per sample frame to the nominal host ticks per sample frame") [S10] |
| **Audio device clock (CMClock)** | A `CMClock` that tracks a Core Audio device's actual rate. | `CMAudioDeviceClockCreate(allocator, deviceUID, &clock)`, `CMAudioDeviceClockCreateFromAudioDeviceID` (macOS only) | "Creates a clock that tracks playback through a Core Audio device… When the associated device is completely stopped, the clock continues to advance, tracking `CMClockGetHostTimeClock()`" [S4][S11-header] |
| **Capture session clock** | The timebase for every capture output sample buffer. | `AVCaptureSession.synchronizationClock` (macOS 12.3+; replaces `masterClock`, deprecated at 12.3) | "All capture output sample buffer timestamps are on the synchronization clock's timebase." [S1][S2] |
| **Capture input port clock** | The device's own clock, as reported. | `AVCaptureInput.Port.clock` | "may not reflect the actual clock in the capture device" [S6] |
| **Engine node time** | `AVAudioEngine` node render times. | `AVAudioNode.lastRenderTime`, tap / sink-node timestamps, `AVAudioPlayerNode.playerTime(forNodeTime:)` | [S8][S9] |

### Mapping one onto another

- **CMClock ↔ CMClock / timebase**: `CMSyncConvertTime(time, from:, to:)`. "If they have different host clocks (or are both clocks), this calculation also compensates for measured drift between the clocks. To convert to or from host time, pass `CMClockGetHostTimeClock()`." [S3]
- **Rate between clocks**: `CMSyncGetRelativeRate(a, relativeTo: b)` "takes into account the measured drift between the two clocks, using host time as a pivot" [S3b]. This is a way to log the observed drift between the camera's session clock, the audio device clock and host time during a spike.
- **Host ticks ↔ seconds**: `AVAudioTime.seconds(forHostTime:)` and `AVAudioTime.hostTime(forSeconds:)` [S7].
- **Audio sample time ↔ host time**: every Core Audio IO cycle hands you both `mSampleTime` and `mHostTime` for the same instant [S10], so each buffer is a fresh (sampleTime, hostTime) anchor pair.
- **Capture PTS → host time**: `CMSyncConvertTime(pts, session.synchronizationClock!, CMClockGetHostTimeClock())` [S1][S3]. Apple's documented pattern for the deprecated `masterClock` is the same conversion, to or from an input port's clock [S2]. An Apple engineer on the Developer Forums gives the same recipe for capture-to-playback alignment: `CMSyncConvertTime(captureTime, captureSession.masterClock, playerItem.timebase)`, adding that "Both objects already take care of latency" [S23].

*Inference*: the docs do not say which concrete clock `synchronizationClock` is on macOS (host clock or an audio device clock). It can change, since the property is key-value observable [S1]. Code must therefore always convert through `CMSyncConvertTime` and never assume PTS values are already host time. OBS does assume this: it feeds `CMSampleBufferGetOutputPresentationTimeStamp` straight into its host-time nanosecond clock [S20]. That works in practice for video-only sessions, but it is not a documented guarantee.

---

## 2. Where the audio should come from

### Facts

- **Core Audio input timestamps.** `inInputTime` "indicates the time at which the first frame in the data was acquired from the hardware" [S11-IOProc]. In `AVAudioEngine`, `AVAudioSinkNode`'s receiver block gets an `AudioTimeStamp` per real-time render [S9b]. `installTap` gives an `AVAudioTime` "the system captures the buffer" [S8b]. (`installTap(onBus:…)` shows as deprecated in the 27.0 SDKs, in favour of a new `installAudioTap(onBus:bufferSize:format:tapProvider:)` [S8].)
- **Latency properties.** `kAudioDevicePropertyLatency` is "the number of frames of latency in the AudioDevice… input and output latency may differ… the AudioDevice's AudioStreams may have additional latency… the total latency for the stream is the device latency summed with the stream latency" [S12]. `kAudioDevicePropertySafetyOffset` is the number of frames "behind (for input) the current hardware position that is safe to do IO" [S12]. `AVAudioIONode.presentationLatency` exposes `kAudioDevicePropertyLatency` + `kAudioStreamPropertyLatency` [S13].
- **How PortAudio turns this into "when the sound hit the ADC".** For input, `inputBufferAdcTime = hostTime(inTimeStamp) − inputDeviceLatency`, citing a Core Audio engineer's mailing-list guidance that "the difference between the Apple output timestamp and the PA timestamp is kAudioDevicePropertyLatency" [S25].
- **AVCaptureSession audio.** On macOS, Core Audio input devices also appear as capture devices. `AVCaptureAudioDataOutput` delivers audio sample buffers stamped on the same `synchronizationClock` as video [S1]. Its delivery is buffered and not meant for real-time monitoring. The session offers no hook to run AU effects in the IO cycle.

### Options

| Option | How | Pros | Cons |
|---|---|---|---|
| **A. Audio via AVAudioEngine / Core Audio; video via AVCaptureSession (video only)** | Record the dry Take from the input node (sink node, tap or raw AUHAL IOProc). Record the camera with `AVCaptureVideoDataOutput` → `AVAssetWriter`, or `AVCaptureMovieFileOutput`. Align both via host time. | Monitoring through Insert Chains works. Audio Takes stay sample-accurate to the Song timeline. One audio path. Same pattern as OBS [S20][S21]. | You own the clock mapping, latency compensation and drift. |
| **B. Audio and video both via AVCaptureSession** | `AVCaptureDeviceInput` for the interface + `AVCaptureAudioDataOutput`. | Apple aligns A and V on one session clock [S1][S23]. | No low-latency monitoring through AU plugins. You'd need a second, parallel engine for monitoring, which means two clients on the same device and two copies of the audio. Multi-track, per-channel Takes get awkward. |
| **C. Hybrid: Option A, plus a scratch audio track in the movie file** | As in A, but the camera file also records the mic or interface audio via the capture session. | The scratch track gives a self-check: cross-correlate it against the Take to verify or auto-correct the offset after recording. | Extra complexity. Two clients on the interface. |

*Inference*: Option A is the natural fit for v1. Monitoring through Insert Chains rules out B. C is a useful tool for the spike: it measures the real offset without a manual clap test.

---

## 3. Offsets and drift

### Fixed offsets (constant during a take)

| Offset | Reported by an API? | Notes |
|---|---|---|
| Audio input latency (converter + driver + stream) | **Yes**: `kAudioDevicePropertyLatency` + `kAudioStreamPropertyLatency` (input scope), plus `kAudioDevicePropertySafetyOffset` for the IO position [S12]; `AVAudioInputNode.presentationLatency` [S13]. | Drivers sometimes report it wrong. Logic Pro has a manual "Recording Delay" slider in Audio Devices settings to "compensate for any delays caused by the audio driver", noting "you should not normally need to touch this setting" [S30]. |
| Audio output latency (for playback and Monitoring) | **Yes**: same properties, output scope [S12]. | Needed so that video frames shown during playback line up with what's *heard*, not with what's rendered. |
| Camera pipeline latency (sensor → PTS) | **No public API found.** Not mentioned in the Continuity Camera WWDC22 session either [S29]. | It's undocumented whether a frame's PTS is exposure time or arrival time. GStreamer's `avfvideosrc` treats `CMClockGetTime(inputPort.clock) − PTS` as "how long it spent in AVF queues" and back-dates its timestamp by that amount [S22], which implies the PTS sits earlier than delivery. Third-party USB cameras can misbehave: an Apple Forums thread found audio PTS ~300 ms ahead of video with an Insta360 Link 2 in H.264 mode, which was a camera firmware issue [S28]. |
| Continuity Camera | **No API.** | It's a wireless link from iPhone, so expect a larger and more variable latency than built-in or USB cameras. *Inference*: it needs its own calibration value. |

*Inference*: store a **per-camera calibration offset** (keyed by `AVCaptureDevice.uniqueID` or model), measured with the test in §5. Apply it as a constant shift to the Video Take's start.

### Drift (grows over a take)

- The audio interface's sample clock and the host clock are separate oscillators. Core Audio measures their ratio continuously (`mRateScalar`) [S10]. `CMAudioDeviceClock` exposes the device's real rate as a `CMClock` [S4]. `kAudioDevicePropertyClockDomain` documents that devices with different clock domains "should be assumed to be separate" [S12].
- `CMSyncConvertTime` "compensates for measured drift between the clocks" [S3].
- *Inference (arithmetic, not sourced)*: crystal tolerances are commonly tens of ppm. 50 ppm over a 10-minute take is 30 ms, about one frame at 30 fps, and still inside the BT.1359 detectability window [S24]. Drift is therefore small for song-length takes, but cheap to remove: map each video frame's host-time PTS into the audio device clock (or into the Take's sample timeline via the per-buffer (sampleTime, hostTime) anchors) rather than into wall-clock seconds.
- Video timestamps survive dropped frames. `alwaysDiscardsLateVideoFrames` defaults to `true` [S17], but each delivered frame keeps its own PTS, and `AVAssetWriter` writes by timestamp [S26]. Drops cost smoothness, not sync, as long as nothing rebuilds timestamps from a frame counter.

---

## 4. Recording: a concrete alignment recipe (Option A)

*Inference, assembled from the facts above. This is what the spike should implement and measure.*

1. **Arm.** Start `AVAudioEngine` (input to Insert Chain to output for Monitoring) and `AVCaptureSession` (camera only) before recording starts, so both pipelines are warm.
2. **Audio Take start.** In the input render callback, take the first buffer whose `mSampleTime` ≥ the transport's record-start sample. `takeStartHost = mHostTime − hostTicks(inputLatencyFrames / sampleRate)` [S10][S12][S25]. Store the Take's position on the Song timeline in samples, not in host time.
3. **Video Take start.** Either:
   - `AVCaptureVideoDataOutput` + `AVAssetWriter`: call `startSession(atSourceTime: firstPTS)` [S26] and keep `firstPTS`, or
   - `AVCaptureMovieFileOutput`, using the new delegate `fileOutput(_:didStartRecordingTo:startPTS:from:)` (macOS 15.2+) to get the file's start PTS [S0].

   Then `videoStartHost = CMSyncConvertTime(firstPTS, session.synchronizationClock, hostClock)` [S1][S3].
4. **Offset.** `videoOffsetSeconds = (videoStartHost − takeStartHost) − cameraCalibrationOffset`. Store it on the Video Take in the Song model as a rational `CMTime` (or integer samples at the Song rate), never as a float.
5. **Optional drift correction.** Over long takes, convert via `CMAudioDeviceClock` for the interface rather than host time, or log `CMSyncGetRelativeRate` to confirm drift is negligible [S3][S3b][S4].

---

## 5. How existing apps do it, and how to measure sync error

### Existing implementations

- **OBS Studio (macOS)**: camera: `CMSampleBufferGetOutputPresentationTimeStamp`, rescaled to nanoseconds and used directly as the frame timestamp [S20]. Core Audio input: `AudioConvertHostTimeToNanos(mHostTime)` [S21]. The OBS clock is `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` [S22b], which is `mach_absolute_time` in nanoseconds. So OBS uses **host time as the shared clock**, with a user-set per-source "sync offset" for the remaining fixed latency.
- **GStreamer `avfvideosrc`**: re-bases frames onto the pipeline clock, using the capture port clock to measure and subtract queueing delay [S22].
- **PortAudio (Core Audio backend)**: derives ADC/DAC times from `mHostTime` ± device latency [S25].
- **Logic Pro**: does not record camera video. Its movie track exposes a "Movie Position" field that "moves the position of the full movie to the left or right" in SMPTE frames [S31], and a "Movie to Project" slider that "fine-tunes the start point of a movie" [S32]. In other words, a user-adjustable offset. Logic's "Recording Delay" setting is the analogous manual fix for audio driver latency [S30].

### Measuring sync error

- **Clap / slate test.** Clap sharply in frame. Find the audio transient in the Take (sample-accurate) and the first video frame where the hands meet. Error = (frame PTS on the Song timeline) − (transient time). Frame resolution limits the measurement to ±½ frame. Averaging several claps or using 60 fps helps.
- **Self-calibrating flash + click (Inference)**: the app flashes the screen white and plays a click through the output at a known host time. The camera films the screen while the interface mic records the click. This measures camera latency + display latency vs. audio output + input latency. Isolating camera latency needs the display latency to be known, so a phone flashlight or LED triggered alongside a clap is a cleaner independent reference.
- **Automated in tests (Inference)**: Option C's scratch audio in the movie file lets you cross-correlate against the Take after every recording and log the residual offset. This turns sync verification into a number AI sessions can check.
- **Pass criteria.** ITU-R BT.1359-1: detectability ≈ +45 ms / −125 ms, acceptability ≈ +90 ms / −185 ms (positive = sound early). It also cites ITU-R BR.265's ±½-frame precision for film (≈ ±22 ms at 24 fps) [S24]. EBU R37 (production chains) tightens this to +40 ms / −60 ms. That figure comes from secondary summaries, since I didn't fetch the primary text. *Inference*: target |error| ≤ ½ frame (≤ 17 ms at 30 fps) in the spike.

---

## 6. Export to MP4 with the final mix

### Facts

- `AVAssetWriter.startSession(atSourceTime:)`: "a sample you append with timestamp T plays at movie time (T − startTime)… If the earliest sample for an input has a timestamp later than the start time, the system inserts an empty edit to preserve synchronization between tracks" [S26]. `expectsMediaDataInRealTime` should be `true` for capture sources [S26b].
- `AVMutableCompositionTrack.insertTimeRange(_:of:at:)` places a source range at any `CMTime` [S33]. `insertEmptyTimeRange(_:)` adds gaps [S33b].
- `AVMutableVideoComposition.frameDuration` sets the output frame rate. With `sourceTrackIDForFrameTiming`, "if an empty edit is encountered… the compositor composes frames as needed up to the frequency specified in frameDuration" [S34]. This is how "full song with black elsewhere" gets real frames rather than an edit-list gap.
- `AVAudioEngine.enableManualRenderingMode(.offline, format:, maximumFrameCount:)` renders the engine (players + AU Insert Chains + Master) faster than real time into buffers [S27].
- `AVAssetExportSession.export(to:as:)` is the modern async export API [S35].

### Options

| Option | Pipeline | Notes |
|---|---|---|
| **E1. Composition + export session** | 1) Offline-render the mix to a temp audio file [S27]. 2) Build an `AVMutableComposition`: insert the audio file at 0 and the Video Take at `videoOffset` [S33]. For the trimmed export, set the composition / `timeRange` to the Video Take's span. For the full-song export, add an `AVMutableVideoComposition` so gaps render as black [S34]. 3) Export as MP4 / H.264 or HEVC + AAC [S35]. | Least code. All timing is rational `CMTime`, so the offset is exact. Re-encodes video, unless a passthrough preset is used, which can't synthesize black frames. |
| **E2. AVAssetReader → AVAssetWriter** | Read the Video Take's frames, re-stamp them as `pts − videoStart + videoOffset`, and append them. Append the rendered mix as PCM → AAC. `startSession(atSourceTime: .zero)` [S26]. | Full control (black frames, trimming, timestamp logging per frame). More code. |

*Inference*: "frame-accurate" in practice means the composition's `CMTime` offset is exact, and the output frame grid (`frameDuration`) decides which source frame shows at each output tick. Keep offsets as `CMTime` with a large timescale (for example the audio sample rate, or 600 × fps-friendly). Don't round the offset to whole frames: rounding adds up to ½ frame of avoidable error.

---

## 7. Playback: Video Take slaved to the audio transport

All three options below use the facts in §1. Pick one for the spike.

| Option | How | Source |
|---|---|---|
| **P1. AVPlayer slaved to the audio device clock** | `player.sourceClock = CMAudioDeviceClock(interface)`; `automaticallyWaitsToMinimizeStalling = false`; on transport start, `player.setRate(1, time: songTime − videoOffset, atHostTime: hostTimeWhenThatSampleIsHeard)`. "Setting an explicit source clock is useful to synchronize video-only movies with audio that plays through a different audio device." The stalling flag must be `false`, or `setRate(_:time:atHostTime:)` raises an exception. | [S14][S15][S15b]; WWDC12 517 describes exactly this "synchronize video to custom audio" pattern [S36] |
| **P2. AVSampleBufferDisplayLayer + controlTimebase** | Decode frames yourself (AVAssetReader) and enqueue them. Set `controlTimebase` to a timebase created with `CMTimebaseCreateWithSourceClock(audioDeviceClock)`; set its time and rate on transport changes. "If you're synchronizing video to audio, you should use a timebase whose host clock is a CMClock for the appropriate audio device to prevent drift." | [S16][S16b] |
| **P3. Pull model: display link + AVPlayerItemVideoOutput** | On each display refresh, compute `songTime` from the audio engine's position (last render sample time + output latency), set `itemTime = songTime − videoOffset`, and call `copyPixelBuffer(forItemTime:itemTimeForDisplay:)`. The player itself can stay paused and seek, or play at rate 1. | [S18][S19] |

*Inference*: P1 is the least code for continuous playback. P3 makes the audio engine the single source of truth and handles scrubbing, pause and seeking naturally, at the cost of a display-link loop. In every option, the time that must line up is when audio is *heard*, so add output presentation latency [S13].

---

## 8. Open questions for the spike

1. Which concrete clock does `synchronizationClock` report on macOS 15 for a video-only session? Log `CMSyncGetRelativeRate(sessionClock, hostClock)` [S3b].
2. Measured camera latency for the built-in FaceTime camera, a USB UVC webcam and Continuity Camera (clap test, ≥10 claps each).
3. Measured drift between the interface's `CMAudioDeviceClock` and host time over a 10-minute take.
4. Does running `AVCaptureSession` with an audio input on the same interface as `AVAudioEngine` (Option C) disturb the engine (sample-rate changes, glitches)?
5. `AVCaptureMovieFileOutput` + `startPTS` (macOS 15.2+) vs `AVAssetWriter`: which is simpler and equally precise? Note that the map targets macOS 15+, so 15.0 and 15.1 lack the `startPTS` delegate.

---

## Sources

Apple developer documentation (fetched as doc JSON from `developer.apple.com/tutorials/data/documentation/…`):

- **[S0]** `AVCaptureFileOutputRecordingDelegate.fileOutput(_:didStartRecordingTo:startPTS:from:)`, macOS 15.2+. https://developer.apple.com/documentation/avfoundation/avcapturefileoutputrecordingdelegate/fileoutput(_:didstartrecordingto:startpts:from:)
- **[S1]** `AVCaptureSession.synchronizationClock`, macOS 12.3+. https://developer.apple.com/documentation/avfoundation/avcapturesession/synchronizationclock
- **[S2]** `AVCaptureSession.masterClock` (deprecated macOS 12.3). https://developer.apple.com/documentation/avfoundation/avcapturesession/masterclock
- **[S3]** `CMSyncConvertTime(_:from:to:)`. https://developer.apple.com/documentation/coremedia/cmsyncconverttime(_:from:to:)
- **[S3b]** `CMSyncGetRelativeRate(_:relativeTo:)`. https://developer.apple.com/documentation/coremedia/cmsyncgetrelativerate(_:relativeto:)
- **[S4]** `CMAudioDeviceClockCreate(allocator:deviceUID:clockOut:)`. https://developer.apple.com/documentation/coremedia/cmaudiodeviceclockcreate(allocator:deviceuid:clockout:)
- **[S5]** `CMClockGetHostTimeClock()`. https://developer.apple.com/documentation/coremedia/cmclockgethosttimeclock()
- **[S6]** `AVCaptureInput.Port.clock`. https://developer.apple.com/documentation/avfoundation/avcaptureinput/port/clock
- **[S7]** `AVAudioTime`, `hostTime(forSeconds:)`, `seconds(forHostTime:)`. https://developer.apple.com/documentation/avfaudio/avaudiotime
- **[S8]** `AVAudioNode.installTap(onBus:bufferSize:format:block:)` (deprecated in 27.0 SDKs); `lastRenderTime`. https://developer.apple.com/documentation/avfaudio/avaudionode/installtap(onbus:buffersize:format:block:)
- **[S8b]** `AVAudioNodeTapBlock`. https://developer.apple.com/documentation/avfaudio/avaudionodetapblock
- **[S9]** `AVAudioPlayerNode.playerTime(forNodeTime:)`, `play(at:)`. https://developer.apple.com/documentation/avfaudio/avaudioplayernode/playertime(fornodetime:)
- **[S9b]** `AVAudioSinkNode`, `AVAudioSinkNodeReceiverBlock`. https://developer.apple.com/documentation/avfaudio/avaudiosinknode
- **[S10]** `AudioTimeStamp`: field docs from the macOS SDK header `CoreAudioTypes.framework/Headers/CoreAudioBaseTypes.h`. https://developer.apple.com/documentation/coreaudiotypes/audiotimestamp
- **[S11]** macOS SDK header `CoreMedia.framework/Headers/CMAudioDeviceClock.h` (`CMAudioDeviceClockCreate`, `…FromAudioDeviceID`, `…SetAudioDeviceUID`).
- **[S11-IOProc]** macOS SDK header `CoreAudio.framework/Headers/AudioHardware.h`, `AudioDeviceIOProc` / `AudioDeviceIOBlock` (`inInputTime`).
- **[S12]** macOS SDK header `CoreAudio.framework/Headers/AudioHardwareBase.h`: `kAudioDevicePropertyLatency`, `kAudioDevicePropertySafetyOffset`, `kAudioStreamPropertyLatency`, `kAudioDevicePropertyClockDomain`.
- **[S13]** `AVAudioIONode.presentationLatency`. https://developer.apple.com/documentation/avfaudio/avaudioionode/presentationlatency
- **[S14]** `AVPlayer.sourceClock`, macOS 12+. https://developer.apple.com/documentation/avfoundation/avplayer/sourceclock
- **[S15]** `AVPlayer.setRate(_:time:atHostTime:)`. https://developer.apple.com/documentation/avfoundation/avplayer/setrate(_:time:athosttime:)
- **[S15b]** `AVPlayer.automaticallyWaitsToMinimizeStalling`. https://developer.apple.com/documentation/avfoundation/avplayer/automaticallywaitstominimizestalling
- **[S16]** `AVSampleBufferDisplayLayer.controlTimebase`. https://developer.apple.com/documentation/avfoundation/avsamplebufferdisplaylayer/controltimebase
- **[S16b]** `CMTimebaseCreateWithSourceClock`. https://developer.apple.com/documentation/coremedia/cmtimebasecreatewithsourceclock(allocator:sourceclock:timebaseout:)
- **[S17]** `AVCaptureVideoDataOutput.alwaysDiscardsLateVideoFrames`. https://developer.apple.com/documentation/avfoundation/avcapturevideodataoutput/alwaysdiscardslatevideoframes
- **[S18]** `AVPlayerItemOutput.itemTime(forHostTime:)`. https://developer.apple.com/documentation/avfoundation/avplayeritemoutput/itemtime(forhosttime:)
- **[S19]** `AVPlayerItemVideoOutput.copyPixelBuffer(forItemTime:itemTimeForDisplay:)`. https://developer.apple.com/documentation/avfoundation/avplayeritemvideooutput/copypixelbuffer(foritemtime:itemtimefordisplay:)
- **[S26]** `AVAssetWriter.startSession(atSourceTime:)`. https://developer.apple.com/documentation/avfoundation/avassetwriter/startsession(atsourcetime:)
- **[S26b]** `AVAssetWriterInput.expectsMediaDataInRealTime`. https://developer.apple.com/documentation/avfoundation/avassetwriterinput/expectsmediadatainrealtime
- **[S27]** `AVAudioEngine.enableManualRenderingMode(_:format:maximumFrameCount:)`, `.offline`. https://developer.apple.com/documentation/avfaudio/avaudioengine/enablemanualrenderingmode(_:format:maximumframecount:)
- **[S33]** `AVMutableCompositionTrack.insertTimeRange(_:of:at:)`. https://developer.apple.com/documentation/avfoundation/avmutablecompositiontrack/inserttimerange(_:of:at:)
- **[S33b]** `AVMutableCompositionTrack.insertEmptyTimeRange(_:)`. https://developer.apple.com/documentation/avfoundation/avmutablecompositiontrack/insertemptytimerange(_:)
- **[S34]** `AVVideoComposition.sourceTrackIDForFrameTiming`; `AVMutableVideoComposition.frameDuration`. https://developer.apple.com/documentation/avfoundation/avvideocomposition/sourcetrackidforframetiming
- **[S35]** `AVAssetExportSession.export(to:as:isolation:)`. https://developer.apple.com/documentation/avfoundation/avassetexportsession/export(to:as:isolation:)

Apple sessions, forums and support:

- **[S23]** Apple Developer Forums 651388, Apple engineer answer on syncing capture with playback via `masterClock` + `CMSyncConvertTime`. https://developer.apple.com/forums/thread/651388
- **[S28]** Apple Developer Forums 779984, AVCaptureSession A/V offset traced to Insta360 Link 2 firmware. https://developer.apple.com/forums/thread/779984
- **[S29]** WWDC22 10018 "Bring Continuity Camera to your macOS app" (no latency/sync guidance given). https://developer.apple.com/videos/play/wwdc2022/10018/
- **[S36]** WWDC12 517 "Real-Time Media Effects and Processing during Playback": CMClock (host vs audio clock), CMTimebase, syncing AVPlayer to custom audio. Transcript index: https://nonstrict.eu/wwdcindex/wwdc2012/517/
- **[S30]** Apple Support: Logic Pro "Recording Delay" (Audio Devices settings / manage input monitoring latency). https://support.apple.com/en-us/105040
- **[S31]** Apple Support: Movie project settings in Logic Pro for Mac. https://support.apple.com/guide/logicpro/movie-project-settings-lgcpfbb9186a/mac
- **[S32]** Apple Support: Movie settings in Logic Pro for Mac. https://support.apple.com/guide/logicpro/movie-preferences-lgcp2323c1e6/mac

Standards:

- **[S24]** Recommendation ITU-R BT.1359-1 (1998), "Relative timing of sound and vision for broadcasting". https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1359-1-199811-I!!PDF-E.pdf

Open-source implementations:

- **[S20]** OBS Studio, `plugins/mac-avcapture/OBSAVCapture.m` (PTS → ns timestamp). https://github.com/obsproject/obs-studio/blob/master/plugins/mac-avcapture/OBSAVCapture.m
- **[S21]** OBS Studio, `plugins/mac-capture/mac-audio.c` (`AudioConvertHostTimeToNanos(mHostTime)`). https://github.com/obsproject/obs-studio/blob/master/plugins/mac-capture/mac-audio.c
- **[S22b]** OBS Studio, `libobs/util/platform-cocoa.m` (`os_gettime_ns` = `CLOCK_UPTIME_RAW`). https://github.com/obsproject/obs-studio/blob/master/libobs/util/platform-cocoa.m
- **[S22]** GStreamer, `subprojects/gst-plugins-bad/sys/applemedia/avfvideosrc.m` (`getSampleBuffer:timestamp:duration:`). https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/main/subprojects/gst-plugins-bad/sys/applemedia/avfvideosrc.m
- **[S25]** PortAudio, `src/hostapi/coreaudio/pa_mac_core.c` (`AudioIOProc`, `CalculateFixedDeviceLatency`). https://github.com/PortAudio/portaudio/blob/master/src/hostapi/coreaudio/pa_mac_core.c
