# Edit Clips

In Edit mode the user selects a Clip, splits it at the playhead, moves it, trims either end, sets its fades, and walks the history back and forth with undo and redo. Each edit is one undo step with its own label.

## Sub-features

- `edit-select`: clicking a Clip selects it.
- `edit-split`: **S** (Split) cuts the selected Clip at the playhead and selects the right half.
- `edit-move`: dragging a Clip's body moves it on the timeline.
- `edit-trim`: dragging either edge trims the Clip's start or end.
- `edit-fades`: the fade handles set the fade-in and fade-out lengths.
- `edit-undo-redo`: command-Z and shift-command-Z, or the Edit menu.

## How to get to it (user POV)

- Top bar **Edit** (key **2**), click a Clip, click the ruler to place the playhead, press **S**. Drag the Clip or its edges and fade handles. Undo and redo from the Edit menu.

## Driving it with journey-edit.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed. The open Song has a Take whose Clip is at least 1.5 s long. When no Song is open, the script runs `journey-record-pass.sh` first.

Run `.claude/skills/verify/scripts/journey-edit.sh`. It exits 0 when every check passed and writes `journeys/edit/` in the run dir. All positions are samples at the Song's rate (R). The recipe:

1. `show_mode mode=edit`, then `select_clip clip_id=<the Take's Clip>`. `/view/selected_clip` is that id.
2. `seek sample=<start + R/2>`. `/transport/position` is that sample.
3. `split_selected_clip`. Its `created_id` is the right half. The left half keeps its start and `sourceOffset` and is R/2 long. The right half starts at the playhead, its `sourceOffset` is R/2 further, it is selected, and the undo label is `Split Clip`.
4. `move_clip clip_id=<right> start=<+R/4>`: same length and `sourceOffset`, label `Move Clip`.
5. `trim_clip_start clip_id=<right> start=<+R/4>`: R/4 shorter, `sourceOffset` R/4 further, label `Trim Clip`.
6. `trim_clip_end clip_id=<right> end=<start + R/2>` (`end` is a timeline sample): R/2 long, label `Trim Clip`.
7. `set_clip_fade_in clip_id=<right> length=<R/10>`: `fadeInLength` R/10, label `Fade In`.
8. `undo`: no fade, undo `Trim Clip`, redo `Fade In`. `undo` again: the length before step 6 is back, redo `Trim Clip`. `redo`: R/2 long again, undo `Trim Clip`, redo `Fade In`.
9. `song.json` on disk has the right half as `state` shows it.

## Gotchas

- `split_selected_clip` answers `ui.not_in_edit_mode` outside Edit mode and `ui.no_clip_selected` with no selection. Neither is presented to the user.
- `trim_clip_end`'s `end` is where the Clip ends on the timeline, not a length.
- The undo labels of both trims are the same, `Trim Clip`; tell them apart by the Clip's `timelineStart` and `length`.
