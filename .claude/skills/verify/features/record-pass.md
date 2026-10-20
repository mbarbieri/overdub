# Record a Pass

The user Arms one or more Tracks (and optionally the camera) and presses record. Every Armed Track gets one Take, and an Armed camera adds one Video Take, all placed on the timeline as one undo step.

## Sub-features

- `record-new-song`: File → New Song... creates an empty `.overdub` package.
- `record-import`: Record mode → **Import...** (or dropping files on the window) adds Backing Tracks.
- `record-arm`: the arm button on a Record row. The input picker and Monitoring are on the same row.
- `record-pass`: **R** (or ●) starts a Pass and stops it. The clock reads `REC · PASS n`.
- `record-video`: the camera arm in the stage bar records a Video Take with the Pass.

## How to get to it (user POV)

- File → New Song..., then Record mode (key **1**), **Import...**, arm a row, press **R**, then press **R** or Space to stop.

## Driving it with (no driver available)

Preconditions:

- A GUI driver: cmux-cua with Computer Use onboarding finished, or Accessibility and Screen Recording granted to the terminal.

- **Not drivable here.** Report every sub-feature as unverified (no GUI driver). Once a driver exists, the proof is: the log has `song.created`, then `song.backing_track_imported`, then `capture.pass_started` and `capture.pass_stopped`. The new package's `song.json` has one entry in `passes`, a Take per Armed Track in `takes`, and `audio/<take id>.wav` files with nonzero length.

## Gotchas

- Recording uses the real input device and microphone permission. The built-in microphone records room sound.
- New Song... opens a native save dialog. Point it at `$RUN_DIR/songs/`, never `~/Music`.
- `capture.pass_stopped` and the Pass's undo step come only after stop. A run that is killed mid-Pass leaves `.wav.tmp` files.
