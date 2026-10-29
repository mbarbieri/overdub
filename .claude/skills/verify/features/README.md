# Overdub verification map

This directory is the maintained source for verifying the user-facing behavior of Overdub. Read the index before you drive the app, then use the matching feature file as the recipe. `../SKILL.md` covers launch, doctor and cleanup.

## Baseline preconditions

- Run `launch.sh <run-name>`, which builds this checkout's debug app. Run `doctor.sh` and require every check to be `ok`.
- No other Overdub may be running. The app is single-instance and shares `~/Library/Application Support/Overdub/` with the user's copy.
- The app starts with no Song open: `launch.sh` gives each launch its own Last Song file, `$RUN_DIR/last-song.json`, which does not exist yet. Any Song a journey creates lives under `$RUN_DIR/songs/`.
- Never drive an instance that this run did not start.

## Driving conventions

- Drive the app through the control socket (`../scripts/overdub-ctl`) and the journey scripts, plus the helper process, the selftest CLI, the JSONL log and Song package files. Nothing clicks or screenshots the window (see `../SKILL.md`).
- Nothing may be heard: before any play, record or export, set the Master volume to its minimum with `set_master_volume volume_db=-60`, and never turn Monitoring on. Never run `measure_latency` or the loopback selftest without asking the user.
- Assert log `event` names and fields, never `msg`.
- Work on copies (`ditto`) of any user Song. Never write under `~/Music` or edit per-machine settings files.
- Use GLOSSARY.md terms (Song, Track, Pass, Take, Clip, Backing Track, Separation) in reports.

## Proof and skip reporting

- A CLI proof keeps the command, stdout, stderr and exit code in the run dir.
- A side effect is proven by reading the result: part files and their levels, `song.json` contents, log events.
- A journey script proves itself: `journeys/<name>/checks.txt` ends `RESULT pass` or `RESULT fail`, next to every request and reply (`requests.jsonl`) and the state snapshots it checked.
- A sub-feature no journey drives is reported as **unverified**, with the feature ID and the missing piece. Never report it as verified through a neighbour.

## Feature entry contract

Each feature file starts with an H1 title and one paragraph that describes the user-visible behavior. Four H2 sections follow, in this order: `Sub-features`, `How to get to it (user POV)`, `Driving it with <harness>` (which starts with `Preconditions:`), and `Gotchas`.

## Features

| Feature | Drivable today |
| --- | --- |
| [App startup and audio device](./app-startup.md) | yes: launch, doctor, log; the Last Song: `journey-last-song.sh` (reopen and missing, proven 2026-10-26); a Last Song that fails to open: no |
| [Separation](./separation.md) | yes: `separation-proof.sh` (helper) and `journey-separation.sh` (in a Song, proven 2026-10-23); cancel: no |
| [Selftests](./selftests.md) | yes, with conditions (loopback plays sound; clap needs a clap Pass) |
| [Record a Pass](./record-pass.md) | yes: `journey-record-pass.sh` (proven 2026-10-23); the camera: no |
| [Edit Clips](./edit.md) | yes: `journey-edit.sh` (split, move, trim, fade, undo, redo; proven 2026-10-23) |
| [Export](./export.md) | yes: `journey-export.sh` (WAV and M4A through the Export sheet, proven 2026-10-26); MP4: no |
| [Transpose](./transpose.md) | yes: `journey-transpose.sh` (set, render, undo, out of range, back to 0; proven 2026-10-27); a failed render and the refusals while Transposing: no |
| [Practice Speed](./practice-speed.md) | yes: `journey-practice-speed.sh` (set, render, playhead rate, out of range, back to 100; proven 2026-10-27); the silent-until-rendered source, a failed render and the Transpose interplay: no |
| [Transcribe mode](./transcribe.md) | over the socket by hand (show_mode, set_transcribe_view, seek, set_transcribe_column_visible, the ui.transcribing refusals); no journey script yet |
