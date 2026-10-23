# Export

Export mode renders the Song's mix to a 24-bit WAV, a 256 kbit/s AAC M4A, or an MP4 with one Video Take. It writes a hidden `.partial` file and renames it into place when done.

## Sub-features

- `export-wav`, `export-m4a`: audio mixdown over the Song span plus Insert Chain tails.
- `export-mp4`: the mix plus the chosen Video Take, trimmed or full-song length.
- `export-progress`: progress, then **Reveal in Finder**.

## How to get to it (user POV)

- Top bar **Export** (key **3**), pick the format, **Choose…** a destination, then **Export**.

## Driving it with journey-export.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed. An open Song with at least one Clip and no Insert. When no Song is open, the script runs `journey-record-pass.sh` first.

Run `.claude/skills/verify/scripts/journey-export.sh`. It exits 0 when every check passed, writes `exports/song.wav` and `exports/song.m4a` in the run dir, and keeps its evidence in `journeys/export/` (`afinfo-*.txt`, `span.txt`). The recipe, once per format:

1. `show_mode mode=export`, `set_export_format format=wav` (or `m4a`), `set_export_destination path=$RUN_DIR/exports/song.wav`. `/view/export_form` shows that format and destination.
2. `set_master_volume volume_db=-60`, then `export`. Read `/activity/counter`, `wait pointer=/activity/last_finish/counter equals=<counter>`, and `/activity/last_finish` is `{"kind": "export", "error_code": null}`.
3. The file exists with nonzero length and no `.partial` file is left. `afinfo` says `WAVE`, `2 ch, <rate> Hz, lpcm ... 24-bit` and a duration within 0.01 s of the Song span (the end of the last Clip over the sample rate). For M4A it says `m4af`, `2 ch, <rate> Hz, aac` and a duration within 0.05 s.

`export-mp4` is not driven: it needs a Video Take, and the camera stays unarmed under the audio rules. `export-progress`'s Reveal in Finder is not driven.

## Gotchas

- The form's default destination is under `~/Music/Overdub/`. Always set it to `$RUN_DIR/` first; an export through the socket overwrites the file it names.
- An export first hands the engine an empty Song. A device that stops calling back makes the export give up after 5 s with `ui.export_audio_unresponsive`.
