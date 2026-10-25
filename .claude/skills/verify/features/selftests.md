# Selftests

Two command-line diagnostics of the real record path. `--selftest loopback` measures how late recorded Takes land with the current latency calibration. `--selftest clap <package>` measures a Song's newest Video Take against its Pass's audio. Settings → Calibration runs the same measurements in the UI.

## Sub-features

- `selftest-loopback`: seven noise bursts through the output device, recorded on input channel 1. Prints `pass`, `offset_samples` and `jitter_samples` (≤ 5, from ADR 0005).
- `selftest-clap`: claps in a camera Pass, measured in audio and video. Prints `pass` (every clap within ±½ frame) and `mean_offset_ms`.
- `selftest-unknown`: an unknown selftest name prints `"error":"app.selftest_unknown"` and exits 2.

## How to get to it (user POV)

- Terminal: `Overdub.app/Contents/MacOS/Overdub --selftest loopback` or `--selftest clap <Song.overdub>`.
- Settings… → Calibration → **Measure Latency** / **Measure Last Clap Pass**. The socket runs them as the `measure_latency` and `measure_clap_pass` Intents, and `state calibration` shows each run. Not in a journey: `measure_latency` plays test signals out of the speakers, so ask the user first.

## Driving it with the Overdub binary

Preconditions:

- No Overdub is running, so run `cleanup.sh` first. The selftests run as the single app instance.
- `BIN=build/debug/src/app/Overdub_artefacts/Debug/Overdub.app/Contents/MacOS/Overdub`

- **Unknown name.** Run `$BIN --selftest nope 2>/dev/null; echo $?`. It prints `{"selftest":"nope","pass":false,"error":"app.selftest_unknown"}` and exits `2`.
- **Clap.** Copy a Song that has a clap Pass: `ditto <Song>.overdub "$RUN_DIR/songs/clap.overdub"`. Run `$BIN --selftest clap "$RUN_DIR/songs/clap.overdub" 2>/dev/null | tail -1 > "$RUN_DIR/clap.json"; echo $?`. A real clap Pass gives `"pass":true` and exit 0. Each entry in `claps` has a numeric `offset_ms`.
- **Loopback.** Ask the user first, because it plays noise out loud. Run `$BIN --selftest loopback 2>/dev/null | tail -1 > "$RUN_DIR/loopback.json"; echo $?`. On a pass, the exit code is 0 and `jitter_samples` ≤ 5.

## Gotchas

- The only Song on this Mac with Video Takes (`~/Music/Untitled.overdub`) is not a clap recording. The selftest runs on it, but most claps have `offset_ms: null`. That proves only that the command runs, not the measurement. A real proof needs a clap Pass recorded through the UI.
- In a debug build, stderr echoes every log line, so take the report from the last stdout line (`tail -1`).
- The loopback test uses the current device. If that device is the built-in speakers and microphone, room noise can fail the jitter check.
