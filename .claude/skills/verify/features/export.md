# Export

Export is an action, not a mode (issue #200): the Export sheet renders the Song's mix to a 24-bit WAV, a 256 kbit/s AAC M4A, or an MP4 with one Video Take. It writes a hidden `.partial` file and renames it into place when done.

## Sub-features

- `export-wav`, `export-m4a`: the audio mix over the Song span plus Insert Chain tails.
- `export-mp4`: the mix plus the chosen Video Take, trimmed or full-song length.
- `export-progress`: the top bar's progress, then the `ui.export_finished` Notice with **Reveal in Finder**.
- `export-cancel` (issue #202): **✕** beside the top bar's progress, or File ▸ Cancel Export, runs `cancel_export`. The render stops, its `.partial` file goes, a file that was at the destination stays, and the `ui.export_cancelled` Notice says "The export was cancelled." (event `ui.export_cancelled`: `path`, `phase`). A cancel that arrives after the file is written is too late: the export ends as written.

## How to get to it (user POV)

- Top bar **Export…** (or File ▸ Export…, ⌘E) opens the Export sheet over the window. Pick the format, **Choose…** a destination, then **Export** (or Return). The sheet closes, the top bar shows the progress, and a Notice names the file. Cancel or Esc closes the sheet without exporting.

## Driving it with journey-export.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed. An open Song with at least one Clip and no Insert. When no Song is open, the script runs `journey-record-pass.sh` first.

Run `.claude/skills/verify/scripts/journey-export.sh`. It exits 0 when every check passed, writes `exports/song.wav` and `exports/song.m4a` in the run dir, and keeps its evidence in `journeys/export/` (`afinfo-*.txt`, `span.txt`). The recipe, once per format:

1. `show_export shown=true` once (`/view/export_sheet_shown` is `true`), then per format `set_export_format format=wav` (or `m4a`), `set_export_destination path=$RUN_DIR/exports/song.wav`. `/view/export_form` shows that format and destination. The form's Intents and `export` work with the sheet closed too.
2. `set_master_volume volume_db=-60`, then `export`. Read `/activity/counter`, `wait pointer=/activity/last_finish/counter equals=<counter>`, and `/activity/last_finish` is `{"kind": "export", "error_code": null}`. `/view/export_sheet_shown` is `false`: starting the export closed the sheet.
3. The file exists with nonzero length and no `.partial` file is left. `afinfo` says `WAVE`, `2 ch, <rate> Hz, lpcm ... 24-bit` and a duration within 0.01 s of the Song span (the end of the last Clip over the sample rate). For M4A it says `m4af`, `2 ch, <rate> Hz, aac` and a duration within 0.05 s.

`export-cancel` is not driven by the journey: a short Song's export ends before a `cancel_export` sent after `export` arrives. Over the socket, `cancel_export` while no export runs answers `ui.no_export_running`.

`export-mp4` is not driven: it needs a Video Take, and the camera stays unarmed under the audio rules. `export-progress`'s Reveal in Finder is not driven.

## Gotchas

- `show_export shown=true` is refused while a Pass or an export runs (`ui.stop_recording_first`, `ui.export_running`).
- The form's default destination is under `~/Music/Overdub/`. Always set it to `$RUN_DIR/` first; an export through the socket overwrites the file it names (`/view/export_form/replaces_existing` is `true` then, while the sheet shows).
- The format, length and destination are saved in the Song (issue #201): they survive closing the Song, so a Song exported by an earlier run already points at that run's file.
- `set_export_destination` needs an existing folder (`export.folder_missing` otherwise); create it first.
- An export first hands the engine an empty Song. A device that stops calling back makes the export give up after 5 s with `ui.export_audio_unresponsive`.
