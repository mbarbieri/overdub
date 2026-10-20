# Export

Export mode renders the Song's mix to a 24-bit WAV, a 256 kbit/s AAC M4A, or an MP4 with one Video Take. It writes a hidden `.partial` file and renames it into place when done.

## Sub-features

- `export-wav`, `export-m4a`: audio mixdown over the Song span plus Insert Chain tails.
- `export-mp4`: the mix plus the chosen Video Take, trimmed or full-song length.
- `export-progress`: progress, then **Reveal in Finder**.

## How to get to it (user POV)

- Top bar **Export** (key **3**), pick the format, **Choose…** a destination, then **Export**.

## Driving it with (no driver available)

Preconditions:

- A GUI driver, plus an open Song with at least one Clip.

- **Not drivable here.** Report it as unverified (no GUI driver). Once a driver exists, the proof is: the log has `ui.export_requested`, then `export.started` and `export.finished`. The destination file exists and has no `.partial` sibling, and `afinfo` shows the expected format, rate and duration (the Song span, plus tails).

## Gotchas

- An export first hands the engine an empty Song. A device that stops calling back makes the export give up after 5 s with `ui.export_audio_unresponsive`.
- Send the output to `$RUN_DIR/`, not the default destination.
