---
name: verify
description: Launch the real Overdub.app (macOS JUCE desktop app) from this checkout and prove a change works the way a user meets it — startup and audio device, the Separation helper, the loopback and clap selftests — with logs, exit codes, files and the app's control socket (overdub-ctl) as evidence. Use after changing src/ to confirm behavior in the running app, not just in ctest; the feature map in features/ lists each user journey and which ones are drivable today.
---

# Verify Overdub in the real app

Overdub is a single-window macOS app (JUCE). Users touch its window (Record · Edit · Export modes, the File/Edit menus, Settings) and, for diagnostics, two command-line selftests. The app runs the `overdub-separate` helper process for every Separation.

**The control socket is the driver.** The running app listens on a local socket (ADR 0008): one JSON request per line in, one reply per line out, plus a live stream of its log events. `scripts/overdub-ctl` is the client. Every user operation is becoming a request there (issues #169, #170, #168, #171); until a journey's operations answer over the socket, report that journey as unverified and name the missing piece. There is still no way to click or screenshot the window itself. Never claim that a unit test or a log line proves a journey you did not drive. `features/README.md` lists what is drivable today.

All helpers live in `.claude/skills/verify/scripts/`. Run them from the repo root.

## Launch

```sh
.claude/skills/verify/scripts/launch.sh <run-name>
```

- Builds the `Overdub` target of the debug preset. A cold build needs `cmake --preset debug` first (see `docs/building.md`).
- Refuses to start when any `Overdub` process is running. The app is single-instance (`moreThanOneInstanceAllowed` is false), and every copy shares `~/Library/Application Support/Overdub/` (device choice, calibrations, the model). Never quit a copy you did not start: ask the user.
- Gives the run its own control socket in a short private folder under `/tmp` (a socket path must fit in 104 bytes) and records it in `$RUN_DIR/socket`.
- Starts the bundle through `open -n -g` with `OVERDUB_LOG=debug` and `OVERDUB_CONTROL_SOCKET`, in the background so it does not take focus. It must be `open` and not the bare binary, because only a LaunchServices launch answers the quit Apple Event that cleanup sends.
- **Ready** means this pid's log has `engine.device_opened` (the saved device, or the fallback to the default device after `app.device_open_failed`) or `app.device_unavailable`, and `control.server_listening`. The window and the socket are usable from then on. Ready takes about 3 s.
- Prints `RUN_DIR=`, `SOCKET=`, `PID=`, `LOG=` and `READY=`. `build/verify/latest` links to the run dir.

The app opens with no Song. A first launch on a fresh Mac asks for microphone access in a system dialog; the app waits until the user answers.

## Doctor

```sh
.claude/skills/verify/scripts/doctor.sh            # or: doctor.sh build/verify/<run>
```

This is read-only. It checks that the recorded pid is alive and runs this checkout's debug binary, and that it is the only Overdub. It checks that the log's `git_sha` matches `HEAD` (`-dirty` is fine) and that the binary is newer than every `src/` file. It checks that a device opened, that the control socket answers `hello`, and that there are no `error`-level log lines. Then it prints the device and Notice events. Run it first whenever anything looks off. A FAIL on "binary is newer" means the build is stale, so relaunch.

## Drive

The drivable surfaces are:

- **Control socket:** `.claude/skills/verify/scripts/overdub-ctl <op> [key=value ...]` sends one request to `$RUN_DIR/socket` (or `--socket PATH`) and prints the reply; it exits 0 when the reply is ok and 1 when it is an error. Values parse as JSON when they can: `overdub-ctl set_track_armed track_id=t-1 armed=true`. `overdub-ctl wait event=song.saved since=0 timeout_ms=5000` waits for a log event. `overdub-ctl subscribe events=ui.,song. level=info --seconds 10` prints events as they happen. `overdub-ctl ops` lists every operation and its fields once #169 lands.

- **Separation helper:** `.claude/skills/verify/scripts/separation-proof.sh`. It runs the bundled `Contents/MacOS/overdub-separate` on a synthetic 6 s input (a 110 Hz sine plus clicks) with the installed model. It writes `separation/` in the run dir. It takes about 1 minute and about 4.6 GB of memory. See `features/separation.md`.
- **Selftests:** `Overdub.app/Contents/MacOS/Overdub --selftest loopback` and `--selftest clap <package>`. Each prints one JSON line on stdout and exits 0 on pass, 1 on fail. Neither opens a window, but each is still the single app instance, so quit the launched app first. See `features/selftests.md`.
- **Logs:** every launch writes `~/Library/Logs/Overdub/overdub-<UTC>-<pid>.jsonl`. Assert `event` and fields, never `msg`, for example: `jq -c 'select(.event=="engine.device_opened")' "$LOG"`.
- **Song packages:** `<name>.overdub/song.json` plus `audio/`, `video/`, `backing/`, `peaks/` and `history/`. A Song that a journey writes goes under `$RUN_DIR/songs/`, never under `~/Music`. Read a user's Song only from a `ditto` copy in the run dir.

## Evidence

Every artifact goes in the run dir, `build/verify/<stamp>-<run-name>/`. Cleanup keeps it, but `make clean` deletes it, so copy anything you need to keep longer.

- `overdub.jsonl` is this launch's log, which cleanup copies in. `stdout.txt` and `stderr.txt` are the app's streams. A debug build echoes every log line to stderr.
- For a CLI or helper proof, keep the command, stdout, stderr and exit code (`separation/command.txt`, `stdout.jsonl`, `stderr.txt`, `exit-code`).
- Prove side effects as well as status. The Separation proof checks that six part files exist, that each is as long as the input, and that the sine lands in `bass.wav` (around −13 dBFS while the others are below −90). An exit code of 0 alone is not proof.
- Exercise the real path: the bundled helper, the real model and the real binary. Do not substitute test stand-ins (`FakeSeparationHelper` or `OnnxTestModel`); those are for ctest.
- When a UI journey is in scope, write down that it was not driven and why. Do not substitute a nearby headless path and call it verified.

## Cleanup

```sh
.claude/skills/verify/scripts/cleanup.sh           # or: cleanup.sh build/verify/<run>
```

- Sends the quit Apple Event to `com.mbarbieri.overdub`. That runs JUCE's normal shutdown: a running Pass is finished first and `app.stopped` is logged. If the pid is still alive after 10 s, cleanup SIGKILLs that pid only. A modal Separation dialog refuses quit.
- Never kill by process name. A plain `kill -TERM` skips JUCE shutdown, so no `app.stopped` is logged.
- Copies the launch log into the run dir as `overdub.jsonl`, removes `pid`, and prints `EVIDENCE=<run dir>`. Run it after every failed attempt too.

## Gotchas

- The saved device in `audio-device.json` may be an interface that is unplugged (`app.device_open_failed` with "No such device"). The app then falls back to the built-in device, which is normal. Do not edit that file: it is the user's per-machine setting.
- `ui.latency_not_calibrated` shows on every launch until calibration has run. It is expected, not a failure.
- `--selftest loopback` plays seven noise bursts out of the real speakers. Ask the user before you run it.
- The Separation model is never in the repo. It is installed per Mac by `make separation-model`, which needs Docker. Without it, the Separation proof reports `no model` and fails. Do not fake the model.
