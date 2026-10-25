# Overdub verification map

This directory is the maintained source for verifying the user-facing behavior of Overdub. Read the index before you drive the app, then use the matching feature file as the recipe. `../SKILL.md` covers launch, doctor and cleanup.

## Baseline preconditions

- Run `launch.sh <run-name>`, which builds this checkout's debug app. Run `doctor.sh` and require every check to be `ok`.
- No other Overdub may be running. The app is single-instance and shares `~/Library/Application Support/Overdub/` with the user's copy.
- The app starts with no Song open. Any Song a journey creates lives under `$RUN_DIR/songs/`.
- Never drive an instance that this run did not start.

## Driving conventions

- Headless paths only: the helper process, the selftest CLI, the JSONL log and Song package files. No window driver works on this machine (see `../SKILL.md`).
- Assert log `event` names and fields, never `msg`.
- Work on copies (`ditto`) of any user Song. Never write under `~/Music` or edit per-machine settings files.
- Use GLOSSARY.md terms (Song, Track, Pass, Take, Clip, Backing Track, Separation) in reports.

## Proof and skip reporting

- A CLI proof keeps the command, stdout, stderr and exit code in the run dir.
- A side effect is proven by reading the result: part files and their levels, `song.json` contents, log events.
- A UI entry point that was not driven is reported as **unverified (no GUI driver)**, with the feature ID. Never report it as verified through a headless neighbour.

## Feature entry contract

Each feature file starts with an H1 title and one paragraph that describes the user-visible behavior. Four H2 sections follow, in this order: `Sub-features`, `How to get to it (user POV)`, `Driving it with <harness>` (which starts with `Preconditions:`), and `Gotchas`.

## Features

| Feature | Drivable today |
| --- | --- |
| [App startup and audio device](./app-startup.md) | yes: launch, doctor, log |
| [Separation](./separation.md) | helper: yes (proven 2026-10-20); the Separate... menu: no |
| [Selftests](./selftests.md) | yes, with conditions (loopback plays sound; clap needs a clap Pass) |
| [Record a Pass](./record-pass.md) | no (GUI only) |
| [Export](./export.md) | no (GUI only) |
