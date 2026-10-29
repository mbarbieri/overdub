# Transpose

Transpose takes every Backing Track of the Song up or down by whole semitones (−12…+12) plus a fine tune of whole cents (−50…+50, issue #232) and leaves its timing alone, so the user can play along in a key that suits them. Takes are never transposed. Each change is one undo step ("Transpose −1", "Transpose +2 −30 ct"), saved in `song.json` (`transpose`, format 6; `transposeCents`, format 11). Each Backing Track is rendered off the audio thread, semitones and cents in one pass, into `backing/<id>.transpose<±n>.wav` (with cents `backing/<id>.transpose<±n><±c>c.wav`, e.g. `.transpose+0+30c.wav`). Until every render of the new value is ready, playback keeps the previous audio, the top bar's field shows a spinner, and Record, Export... and the sheet's Export are unavailable with the tooltip "Transposing…" (`ui.transposing`; `record`, `record_again` and `export` over the socket are refused with it). An export plays the Backing Tracks at the Song's Transpose. The package keeps only the current value's renders. A Backing Track imported or made by a Separation in a transposed Song is rendered at its Transpose the same way (Separation splits the original audio), and undoing it deletes its render.

## Sub-features

- `transpose-set`: typing a whole number into the top bar's **TRANSPOSE [0] st** field and pressing Return runs `set_transpose` with the Song's cents kept. The field is highlighted whenever Transpose (semitones or cents) is not 0, and its tooltip names the whole value while the cents are not 0.
- `transpose-cents`: the Transcribe column's **TRANSPOSE [0] st [0] ct** row sets the fine tune; `set_transpose` takes `cents` (0 when omitted). `/transport/heard_transpose_cents` is the cents playback uses.
- `transpose-render`: each Backing Track is rendered, pitch shifted and as long as its derived WAV. Then playback switches to it (`ui.transpose_heard`, `song.transpose_rendered`).
- `transpose-undo`: undo and redo restore the value. Undoing to a value whose renders were deleted renders it again.
- `transpose-refused`: a value outside −12…+12 semitones or −50…+50 cents, or not a whole number, is an Error over the socket (`song.transpose_out_of_range`, `song.transpose_cents_out_of_range`, `control.bad_field`).
- `transpose-failed-render`: a render that fails leaves the previous audio playing, with the Notice `song.transpose_render_failed`.

## How to get to it (user POV)

- Click the field under **TRANSPOSE** in the top bar, left of Export..., type `-1` or `+2`, and press Return. Undo and redo from the Edit menu.

## Driving it with journey-transpose.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed, and no Song is open. The journey makes its own Song under `$RUN_DIR/songs/`.

Run `.claude/skills/verify/scripts/journey-transpose.sh`. It exits 0 when every check passed and writes `journeys/transpose/` in the run dir. The recipe:

1. `new_song_at`, the Master volume to its minimum, then `import_files` with a 4 s synthetic WAV.
2. `set_transpose semitones=-1`, then `wait pointer=/transport/transposing equals=false`. `/transport/heard_transpose` is −1, the undo label is `Transpose −1`, and `backing/<id>.transpose-1.wav` has as many frames as `backing/<id>.wav`.
3. `set_transpose semitones=2`. Once it is heard, only the `+2` render is left.
4. `undo`. Once it is heard again, Transpose is −1, the redo label is `Transpose +2`, and only the `-1` render is left.
5. `set_transpose semitones=13` answers `song.transpose_out_of_range` and changes nothing.
6. `set_transpose semitones=-1 cents=30`: once heard, `/transport/heard_transpose_cents` is 30, the undo label is `Transpose −1 +30 ct`, and only `backing/<id>.transpose-1+30c.wav` is left, as long as the derived WAV. `cents=51` answers `song.transpose_cents_out_of_range`. `undo` goes back to −1 with no fine tune.
7. `set_transpose semitones=0`: no render is left, and `song.json` (format 11) has `"transpose": 0` and `"transposeCents": 0`.

## Gotchas

- Watch `/transport/transposing`, not `/activity`: a Transpose render is not an activity, and the transport plays on while it runs.
- `transposing` stays true after a render fails, until Transpose changes: Record and Export stay refused.
- A Song opened at a Transpose whose renders are missing plays its derived audio (`heard_transpose` 0) until they are there. A Backing Track imported while a Transpose other than 0 is heard plays silence until its render is there.
- `transpose-cents` is driven over the socket by the journey; the Transcribe column's cents field itself is not clicked (no window driving). Tests cover it (`tests/control/IntentOpsTest.cpp`, `tests/ui/OpenSongTransposeTest.cpp`, `tests/ui/TransposeRecordExportTest.cpp`, `tests/song/TransposeRenderTest.cpp`).
- The `failed-render` and `refused while transposing` sub-features are **unverified** in the real app: no journey drives them. Tests cover them (`tests/ui/OpenSongTransposeTest.cpp`, `tests/ui/AvailabilityTest.cpp`, `tests/ui/TransposeRecordExportTest.cpp`).
- What a render sounds like is not checked here. `tests/song/TransposeRenderTest.cpp` measures a sine (ADR 0005).
