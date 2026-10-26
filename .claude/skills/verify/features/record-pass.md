# Record a Pass

The user Arms one or more Tracks (and optionally the camera) and presses record. Every Armed Track gets one Take, and an Armed camera adds one Video Take, all placed on the timeline as one undo step.

## Sub-features

- `record-new-song`: File → New Song... creates an empty `.overdub` package.
- `record-import`: Record mode → **Import...** (or dropping files on the window) adds Backing Tracks.
- `record-arm`: the arm button on a Record row. The input picker and Monitoring are on the same row.
- `record-pass`: **R** (or ●) starts a Pass; Space (or ■) stops it. The clock reads `REC · PASS n`.
- `record-video`: the camera arm in the stage bar, or the Camera lane header's A in Edit mode, records a Video Take with the Pass. While the camera is Armed, Edit mode shows the Video Preview at the Mixer region's right end (`state window` has `video_preview_shown`).

## How to get to it (user POV)

- File → New Song..., then Record mode (key **1**), **Import...**, arm a row, press **R**, then press Space to stop.

## Driving it with journey-record-pass.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed, and no Song is open (a fresh launch).
- The device offers at least one input (`state` `/audio/inputs`). The built-in microphone is fine; it records room sound.

Run `.claude/skills/verify/scripts/journey-record-pass.sh`. It exits 0 when every check passed and writes `journeys/record-pass/` in the run dir (`checks.txt`, `requests.jsonl`, `state-*.json`, `afinfo-take.txt`, `take-peak.txt`, `ids.json`). It leaves the Song open for `journey-edit.sh` and `journey-export.sh`. The recipe, all through `overdub-ctl`:

1. `new_song_at path=$RUN_DIR/songs/record-pass.overdub`. `state song` has that `path`, and `song.json` exists.
2. `show_mode mode=record`, then `import_files files=["<3 s WAV>"]`. The Song has one Backing Track on one Track with one Clip.
3. `add_track`. Its reply's `created_id` is the new Track's id.
4. Silence: `set_master_volume volume_db=-60` (the fader's bottom) and `set_track_mute` on the Backing Track's Track. `state monitoring` is `[]`.
5. `set_input_channels track_id=<id> channels=[0]` and `set_armed track_id=<id> armed=true`. `state camera` has `armed: false`.
6. `record`, then read `/activity/counter` (this Pass's number). `wait pointer=/transport/running_pass_number equals=1`, then poll `state transport` until `position` passes 2 s.
7. `play_or_stop` stops the Pass. `wait pointer=/activity/last_finish/counter equals=<counter>`, and `/activity/last_finish` is `{"kind": "pass", "error_code": null}`.
8. `state` shows one Pass, one Take on the Armed Track with `passId` of that Pass, one Clip on that Track playing it (`source: {kind: take, id}`), `history.undo_label` `Record Pass 1`, and the transport stopped. `audio/<take id>.wav` exists with nonzero length, no `.wav.tmp` remains, `afinfo` reads at least 1.5 s, and the Take is not all zeros (its 16-bit peak, converted with `afconvert`, is above 0).

`record-video` is not driven: the camera stays unarmed under the audio rules in `../SKILL.md`.

## Gotchas

- `record` while a Pass runs does nothing (`OpenSong::record` returns early). Stop with `play_or_stop`.
- Recording uses the real input device and microphone permission. The built-in microphone records room sound.
- During the Pass the Backing Track plays through the output device. The journey mutes its Track and puts the Master at −60 dB first, so nothing is heard. Never turn Monitoring on.
- The Take's Clip starts at the record point and skips the reported latency (`sourceOffset` equals `compensation.reportedLatencySamples`), so its `length` is a little shorter than the Take's `lengthSamples`.
- A run that is killed mid-Pass leaves `.wav.tmp` files.
