# Building and testing Overdub

Requirements: macOS, Apple clang with C++23 (Command Line Tools are enough), CMake 3.25+, Ninja. v1 builds `x86_64` only (ADR 0001, issue #11).

## First checkout

```sh
git submodule update --init --recursive   # external/JUCE (9.0.3), external/Catch2 (v3), external/json
```

## Build and test

```sh
cmake --preset debug            # configure into build/debug; applies the JUCE patch
cmake --build --preset debug    # builds Overdub.app, overdub_core and overdub_tests
ctest --preset debug            # runs every Catch2 test case
open build/debug/src/app/Overdub_artefacts/Debug/Overdub.app
```

Use `release` in place of `debug` for an optimised build. The first build compiles JUCE once into `overdub_juce` (about 1.5 minutes on an i7-9750H); later builds only recompile what changed.

## Layout

- `src/<area>/`: one folder per feature area from ADR 0006 (`engine`, `plugin`, `song`, `capture`, `video`, `export`, `ui`, `app`), created when a slice needs it. Includes are rooted at `src/`: `#include "core/BuildInfo.h"`.
- `src/core/` → `overdub_core`: shared infrastructure every area links (logging, errors, RT event ring). It is not an ADR 0006 event area and has no `core.*` events: the events it emits use the areas of the callers it serves, `app.*` for launch logging (e.g. `app.log_open_failed`, `app.log_prune_failed`, `app.juce_message`) and `engine.*` for the RT event path (e.g. `engine.rt_events_dropped`).
- `src/engine/` → `overdub_engine`: the headless engine (ADR 0005). `Engine::process (inputs, numIn, outputs, numOut, numSamples, hostTimeNs)` is the one duplex callback the device and tests call; it owns the transport, the per-callback anchor (ADR 0003) and the `engine.*` RT event ring (catalog `engine/EngineEvents.h`). Each Song snapshot becomes a `RenderPlan`: one `AudioProcessorGraph` (Track nodes → Insert Chain nodes → the Track's Mixer strip → the Master strip → outputs, ADR 0001) built, prepared and warmed up off the audio thread, then handed over through an atomic pointer; the replaced plan is freed by `loadSong` or `housekeeping`, never on the audio thread. The Mixer (issue #31) is the law in `engine/MixLaw.h` (volume in dB, the −3 dB constant-power pan law for mono and stereo sources alike, mute, solo-in-place, Master volume): `Engine::setMix` writes each strip's gains into atomics of the latest plan without rebuilding it (the next block ramps to them; `engine.mix_applied` / `engine.mix_refused` at debug), and `Engine::takeMeterPeaks` reads each strip's post-fader peak. A Track at the default mix is heard at √½ on each channel. The transport renders `latency` samples ahead, so after PDC output sample n is Song sample n, and the first `latency` output samples after a seek, play, stop or plan swap are silence (ADR 0003 amendment). Test-only processors (gain, N-sample delay, silence) live in `tests/engine/TestProcessors.h`. `engine::DeviceBinding` is the `juce::AudioIODeviceCallback` that calls `process()` from a live device (logging `engine.device_opened` / `engine.device_closed`); the device choice is a per-machine setting in `~/Library/Application Support/Overdub/audio-device.json` (`engine/DeviceSettings.h`), never in the Song. Tests drive the binding with `tests/engine/FakeAudioIODevice.h` and fake buffers; no test opens real audio hardware.
- `src/song/` → `overdub_song`: the Song model, `song.json`, migrations and the `.overdub` package with persisted history (ADR 0004). Backing Track import decodes through Core Audio's `ExtAudioFile` (`song/AudioFileDecode.h`, `song/BackingTrackImport.h`) and keeps the peaks cache (`song/Peaks.h`). The Mixer's operations (Track volume, pan, mute, solo, Master volume) are pure functions in `song/MixEdits.h`, each one undo step (`Guitar Volume`, `Guitar Pan`, `Mute Guitar`, `Solo Guitar`, `Master Volume`); `OpenSong` applies a change that only touches the mix with `Engine::setMix` instead of rebuilding the plan. The Edit timeline's Clip operations (move, trim start / end, split, delete; issue #28) are pure functions over the Song in `song/ClipEdits.h`: a Clip keeps its layer, splits keep both halves on it, and each is one undo step labelled `Move Clip`, `Trim Clip`, `Split Clip` or `Delete Clip`. Import tests encode their fixtures in the test with `ExtAudioFile` (WAV, AIFF, CAF, FLAC, ALAC, AAC). Core Audio has no MP3 encoder, so the MP3 fixture is made by `lame` (`brew install lame`); without it the MP3 test is skipped.
- `src/capture/` → `overdub_capture`: recording Passes (issue #26). The engine's `process()` copies each Armed Track's input channels into lock-free FIFOs (`engine/RecordSession.h`, started and stopped through `Engine::startRecording` / `stopRecording`); `capture::PassRecorder` drains them on an `io` thread into `audio/<take id>.wav.tmp`, and on stop renames them, writes peaks and adds the Pass, its Takes and their Clips as one undo step (`capture.pass_started` / `capture.pass_stopped`). Placement uses the device's reported latencies plus the per-machine calibration offset in `~/Library/Application Support/Overdub/latency-calibration.json` (`engine/LatencyCalibration.h`). `capture/LoopbackMeasurement.h` is the burst cross-correlation that `overdub --selftest loopback` and the calibration flow share. Recording tests drive `process()` with `LoopbackDevice` (`tests/engine/EngineFixture.h`), which echoes the output into the input after a fixed delay.
- `src/ui/` → `overdub_ui`: the main window's JUCE Components (issue #27, issue #9 resolution): the top bar (Record · Edit · Export, keys 1–3; ⏮ ▶/■ ● with the clock and REC · PASS n; Undo / Redo naming the step), bare Record mode (one row per Track: arm, name, input picker, Monitoring; Import), Edit mode (issue #28: the Clip toolbar, the `Timeline` with Track lanes, waveforms from the peaks cache, covered-span hatching, the playhead and a Camera lane placeholder, and, in the resizable and hideable region under the timeline, the Mixer (issue #31, `ui/Mixer.h`: one strip per Track with pan, fader, meter and M/S, and the Master strip; a fader or pan drag is previewed through `OpenSong::previewMix` and becomes one undo step on mouse up; M/S are also in the Record rows and Edit headers); drag a Clip to move it, its edges to trim, S splits at the playhead, backspace deletes), the Export placeholder, Notices and Alerts (ADR 0006), and the settings window's device picker. Components stay thin over pure functions (`ui/Commands.h`, `ui/Layout.h`, `ui/TimelineGeometry.h` (sample ↔ x at a whole number of samples per pixel, Clip hit-testing, drag → Clip operation, waveform columns), `ui/TransportText.h`, `ui/TrackRowEdits.h`, `ui/MixerGeometry.h` (fader position ↔ dB, pan drag, meter scale), `ui/UserMessages.h`, which holds the one `Error.code` → sentence table). `ui::OpenSong` is the open Song bound to the engine: it records Passes and finishes every Pass it starts (from the window's poll, or by waiting for the audio thread when the Song closes, the device changes or the app quits). `ui/SongFiles.h` holds what New Song, Open and Duplicate do once their dialog has a path. Monitoring is per Track and transient: the engine is given the Song with un-monitored Tracks not Armed. Events use the `ui` area (`ui.notice_shown`, `ui.alert_shown`, `ui.alert_deferred`, `ui.mode_changed`; Notice codes `ui.no_audio_device`, `ui.stop_recording_first`). UI tests follow the ADR 0005 UI amendment; `[snapshot]` tests write PNGs to `build/ui-snapshots/`.
- `src/app/` → `Overdub` (`Overdub.app`): entry point, `MainWindow` (File menu New Song / Open / Duplicate, Edit menu Undo / Redo, the settings window; it owns the live engine and the open Song) and `LiveEngine`, which opens the saved (or default) device, builds the `Engine` for its output channels and buffer size, binds them (after asking for microphone access through `AVCaptureDevice`, `app/MicrophonePermission.h`), and runs the RT event drain thread and the `Engine::housekeeping` timer. Its own events use the `app` area (`app.device_open_failed`, `app.device_unavailable`, and `app.device_choice_changed` when the settings window applies a new device choice, which rebuilds the `LiveEngine`); the engine's device events (`engine.device_opened` / `engine.device_closed`) come from `src/engine`.
- `external/` → `overdub_juce`: the JUCE modules, compiled once. Our targets get JUCE through `overdub_core`, never by linking `juce::juce_*` modules directly (that would compile JUCE again and duplicate symbols).
- `tests/<area>/` → `overdub_tests`: one Catch2 executable. Each `TEST_CASE` becomes one ctest test.
- `tests/support/` → `overdub_rt_guard` (test-only dylib, never linked into `Overdub.app`): the allocation and lock guard from ADR 0005. Wrap real-time code in `REQUIRE_REALTIME_SAFE (...)` from `support/RealtimeGuardCatch.h`; it fails the test if the code allocates, frees or takes a blocking lock on this thread, including inside JUCE and libc++. It is a dylib because it interposes `malloc`-family and lock functions through dyld's `__interpose` section, which dyld ignores in the main executable. `RealtimeGuard.h` lists what it catches and what it does not.

## Loopback selftest

`overdub --selftest loopback` plays seven noise bursts through the current output device, records them on input channel 1 through the real record path, and prints one JSON line: `pass`, `offset_samples` (how late Takes land with the current calibration), `jitter_samples` (must be ≤ 5, ADR 0005), the lags per burst and the device's latencies. Use the built-in speakers and microphone, or a cable from output 1 to input 1. It exits 0 on pass and 1 on fail.

```sh
build/debug/src/app/Overdub_artefacts/Debug/Overdub.app/Contents/MacOS/Overdub --selftest loopback
```

## Logs

Each launch writes `~/Library/Logs/Overdub/overdub-<UTC timestamp>-<pid>.jsonl` (ADR 0006); the last 20 launches are kept. Debug builds log `debug` and up and echo every line to stderr; release builds log `info` and up. Override at launch with `OVERDUB_LOG=debug` (or `info`, `warn`, `error`):

```sh
OVERDUB_LOG=debug build/release/src/app/Overdub_artefacts/Release/Overdub.app/Contents/MacOS/Overdub
jq -c 'select(.lvl == "error")' "$(ls -1 ~/Library/Logs/Overdub/overdub-*.jsonl | tail -1)"
```

In code: `core::logEvent` (`core/Log.h`) off the audio thread; `core::RtEventRing` + an `RtEventSpec` catalog (`core/RtEventRing.h`) on it; `core::Expected<T>`, `core::logError` and `OVERDUB_CHECK` (`core/Error.h`) for failures. Tests use `core::ScopedLogCapture` and never read the log file.

## The JUCE patches

- `patches/juce-force-auv2-oop.patch` makes JUCE load AUv2 plugins out-of-process through `AUHostingService`, as it already does for AUv3 (ADR 0002).
- `patches/juce-graph-realtime-safe.patch` makes `AudioProcessorGraph::processBlock` pass the realtime guard (ADR 0001, ADR 0005). Unpatched, each node's `processBlock` runs under a blocking `ScopedLock` on its callback lock (one `pthread_mutex_lock` per node per block), and the graph reallocates its output buffer whenever the block size changes. The patch takes the callback lock with `ScopedTryLock` (a node whose lock is held elsewhere outputs silence for that block, counted by `juce::getAudioProcessorGraphBusyNodeBlockCount()` and reported as `engine.nodes_silenced`; ADR 0001 amendment) and resizes the output buffer with `avoidReallocating`. Without it the `[engine][rt]` tests fail.

Configuring applies each patch listed in `external/CMakeLists.txt` to `external/JUCE` idempotently: if it is already applied nothing happens, if it applies cleanly it is applied, otherwise configure stops with an error. `external/JUCE` shows as modified inside the submodule; `.gitmodules` sets `ignore = dirty` so the superproject stays clean.

When a patch file itself changes (a pull that edits `patches/`), a checkout that applied the old version fails configure because neither version matches. Reset the submodule and configure again: `git -C external/JUCE checkout -- . && cmake --preset debug`.

### Re-applying the JUCE patch (after bumping JUCE)

1. `git -C external/JUCE checkout -- . && git -C external/JUCE checkout <new-tag>`
2. `cmake --preset debug`. If the patches still apply, you are done.
3. If configure fails on `juce-graph-realtime-safe.patch`, redo its `OVERDUB PATCH` edits: in `juce_AudioProcessorGraph.cpp` (`GraphRenderSequence::perform`, `ProcessOp::processWithBuffer`, and the busy-node counter above `GraphRenderSequence`) and in `juce_AudioProcessorGraph.h` (the `getAudioProcessorGraphBusyNodeBlockCount` declaration at the end). Regenerate it from those two files alone: `git -C external/JUCE diff -- modules/juce_audio_processors_headless/processors/juce_AudioProcessorGraph.cpp modules/juce_audio_processors_headless/processors/juce_AudioProcessorGraph.h > patches/juce-graph-realtime-safe.patch`.
4. If configure fails on `juce-force-auv2-oop.patch`, edit `createAudioUnit` in `external/JUCE/modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h` by hand so the `AudioComponentInstantiate(..., kAudioComponentInstantiation_LoadOutOfProcess, ...)` branch runs for every AU, then regenerate the patch from that file alone: `git -C external/JUCE diff -- modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h > patches/juce-force-auv2-oop.patch`. JUCE sources use CRLF line endings; `.gitattributes` keeps the patch bytes exact.
5. Commit the new submodule commit and the patches together.

## Code signing

Overdub.app is signed after every build with the keychain identity named by the `OVERDUB_CODESIGN_IDENTITY` cache variable (default `Overdub Local Development`), without the hardened runtime. A stable identity keeps camera and microphone permissions across rebuilds (ADR 0001). If the identity is missing, configure warns and the app is signed ad-hoc: it still runs, but macOS asks for permissions again after each build.

To create the identity once: Keychain Access → Certificate Assistant → Create a Certificate… → Name `Overdub Local Development`, Identity Type `Self Signed Root`, Certificate Type `Code Signing`. Then re-run `cmake --preset debug`. Check with `security find-identity -p codesigning` (a self-signed certificate may be listed as not trusted; codesign still uses it).
