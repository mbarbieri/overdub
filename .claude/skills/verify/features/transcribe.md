# Transcribe mode

Transcribe mode is the third mode, beside Record and Edit, for learning a part by ear from the Song's Backing Tracks (ADR 0010). Its page shows an overview of the whole Song on top, with the heard mix's waveform and a rectangle for the visible window, then a ruler over one tall, zoomable waveform of the heard mix, and a right column with Practice Speed and Transpose that the user can hide. Transcribe mode never changes a Clip: split, delete, nudge and the other Clip edits are refused there.

## Sub-features

- `transcribe-mode`: `3` or the top bar's **Transcribe** button runs `show_mode mode=transcribe`. `/view/mode` is `transcribe`, and `ui.mode_changed` names it `transcribe`.
- `transcribe-overview`: clicking or dragging the overview moves the visible window there (`set_transcribe_view`). The window is `/view/transcribe` (`first_sample`, `samples_per_pixel`).
- `transcribe-zoom`: the wheel scrolls time, and ⌘-wheel or a trackpad pinch zooms around the pointer (`set_transcribe_view`), in until about 2 s fills the width and out until the whole Song fits. The socket's values are clamped the same way.
- `transcribe-seek`: clicking the ruler or the waveform seeks (`seek`). Space plays and stops, as in the other modes.
- `transcribe-column`: the right column's Practice Speed and Transpose run `set_practice_speed` and `set_transpose`, as the top bar's fields do. `I` runs `set_transcribe_column_visible` with the opposite of `/view/transcribe_column_visible`. The choice is remembered per Mac in `window-settings.json` beside the Last Song (`OVERDUB_WINDOW_SETTINGS_FILE`; a scripted launch without it neither reads nor writes it), never in the Song.
- `transcribe-refusals`: in Transcribe mode `split_selected_clip`, `delete_selection`, `nudge`, `move_clip`, the trims, fades, gain and `take_lane_click` answer `ui.transcribing` (field `op`, message "Transcribe mode never changes a Clip") and change nothing.

## How to get to it (user POV)

- Press `3`, or click **Transcribe** in the top bar, after Record and Edit.
- Press `I` to show or hide the right column.

## Driving it with overdub-ctl

Preconditions:

- `launch.sh` and `doctor.sh` passed. Make a Song with `new_song_at`, set the Master volume to its minimum, and `import_files` a synthetic WAV.

1. `show_mode mode=transcribe`, then read `state`: `/view/mode` is `transcribe`.
2. `set_transcribe_view first_sample=0 samples_per_pixel=1`: `/view/transcribe/samples_per_pixel` is the finest zoom (about 2 s across the waveform), never 1.
3. `seek sample=48000`: `/transport/position` is 48000.
4. `set_transcribe_column_visible visible=false`: `/view/transcribe_column_visible` is false.
5. `split_selected_clip` answers `ui.transcribing`, and `song.json` is unchanged.

## Gotchas

- No journey script drives Transcribe mode yet: every sub-feature is **unverified** in the real app. Tests cover them (`tests/ui/TranscribeModeTest.cpp`, `tests/ui/TranscribeGeometryTest.cpp`, `tests/control/IntentOpsTest.cpp`, the gesture census).
- The waveforms are drawn from the peaks cache with the Song's mute, solo, volumes and Clip gains, not from the audio heard: fades and pan are not drawn, and the Transcribe Mix, the Focus Track, Loops and Markers are later slices.
- The view does not follow the playhead during playback.
