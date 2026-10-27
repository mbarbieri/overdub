# Practice Speed

Practice Speed plays the whole Song slower or faster, from 50% to 125%, without changing its pitch, so the user can rehearse a hard part. Backing Tracks and the Takes some Clip plays slow down together, so everything stays in time. It is for rehearsal only: it is not an undo step and is not saved, so the Song always opens at 100%. At any other value, every source a Clip plays is rendered off the audio thread from its derived audio, with the Song's Transpose applied to the Backing Tracks, into a temporary folder outside the package that is deleted when the Song closes. Until every render is ready, playback keeps the previous audio and the field shows a spinner; then it switches at the same Song time. The timeline stays in Song time: Clips, the ruler and loop points don't move, and the playhead moves at the Practice Speed times real time.

## Sub-features

- `practice-speed-set`: typing a whole number into the top bar's **PRACTICE SPEED [100] %** field and pressing Return runs `set_practice_speed` (`speed_percent`). The field is highlighted whenever the value is not 100.
- `practice-speed-render`: each source a Clip plays is rendered (`song.practice_speed_started`, `song.practice_speed_rendered`), then playback switches to the renders (`ui.practice_speed_heard`). A source with no render yet (a Clip of a Take just brought back) plays silence until its render is there.
- `practice-speed-playhead`: the playhead moves at the Practice Speed times real time; a seek is to a Song time.
- `practice-speed-refused`: a value outside 50…125, or not a whole number, is an Error over the socket (`song.practice_speed_out_of_range`, `control.bad_field`). It is refused while a Pass, a Separation or a conversion runs.
- `practice-speed-back-to-100`: 100% plays the Song's own audio at once, and the renders are deleted.
- `practice-speed-transpose`: a Transpose change at a Practice Speed renders the Backing Tracks again at the new Transpose.

## How to get to it (user POV)

- Click the field under **PRACTICE SPEED** in the top bar, between Redo and Transpose, type `80` and press Return.

## Driving it with journey-practice-speed.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed, and no Song is open. The journey makes its own Song under `$RUN_DIR/songs/`.

Run `.claude/skills/verify/scripts/journey-practice-speed.sh`. It exits 0 when every check passed and writes `journeys/practice-speed/` in the run dir. The recipe:

1. `new_song_at`, the Master volume to its minimum, then `import_files` with an 8 s synthetic WAV.
2. `set_practice_speed speed_percent=80`: `/transport/practice_speed` is 80 and the undo label is unchanged. Then `wait pointer=/transport/practice_rendering equals=false`: `/transport/heard_practice_speed` is 80, and neither `song.json` nor the package has a render or the value.
3. `play_or_stop`, two `/transport/position` reads 1 s apart, `play_or_stop`: the playhead moved at 0.7…0.9 times real time.
4. `set_practice_speed speed_percent=126` answers `song.practice_speed_out_of_range` and changes nothing.
5. `set_practice_speed speed_percent=100`: heard at once, with no render running.

## Gotchas

- Watch `/transport/practice_rendering` and `/transport/heard_practice_speed`, not `/activity`: a render is not an activity, and the transport plays on while it runs.
- The render folder is under the system temp folder (`overdub-practice-<uuid>`), not in the package; the app state does not name it. Tests check that it is outside the package and deleted when the Song closes (`tests/ui/OpenSongPracticeSpeedTest.cpp`).
- The silent-until-rendered source, the switch at the same Song time, a failed render and the Transpose interplay are **unverified** in the real app: no journey drives them. Tests cover them (`tests/ui/OpenSongPracticeSpeedTest.cpp`, `tests/engine/EnginePracticeSpeedTest.cpp`).
- What a render sounds like is not checked here. `tests/song/PracticeSpeedRenderTest.cpp` measures a sine and an impulse (ADR 0005).
- Record and Export at a Practice Speed other than 100%, and the Video Preview, are issue #213.
