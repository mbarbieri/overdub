#!/bin/zsh
# Journey edit: on the recorded Song, select the Take's Clip, seek 0.5 s into it, split it, move and
# trim the right half, fade it in, undo twice and redo once, checking Clips and undo labels each step.
# Usage: .claude/skills/verify/scripts/journey-edit.sh [run-dir]   (default: build/verify/latest)
# Runs journey-record-pass.sh first when no Song is open. Evidence: $RUN_DIR/journeys/edit/.
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
journey_begin edit
if [[ "$(state song | jq -r '.song == null')" == "true" ]]; then
    "$SCRIPTS/journey-record-pass.sh" "$RUN_DIR" || journey_fail "the setup journey-record-pass.sh failed"
fi

s="$(state song)"
SONG="$(jq -r '.song.path' <<<"$s")"
RATE="$(jq -r '.song.song.sampleRate' <<<"$s")"
TAKE="$(jq -r '.song.song.takes[-1].id' <<<"$s")"
TRACK="$(jq -r '.song.song.takes[-1].trackId' <<<"$s")"
LEFT="$(jq -r "[.song.song.tracks[].clips[] | select(.source.id == \"$TAKE\")][0].id" <<<"$s")"
check "the Take $TAKE has one Clip ($LEFT) on its Track" "$s" \
    "[.song.song.tracks[] | select(.id == \"$TRACK\") | .clips[] | select(.source.id == \"$TAKE\")] | length == 1"
START="$(jq -r "[.song.song.tracks[].clips[] | select(.id == \"$LEFT\")][0].timelineStart" <<<"$s")"
LENGTH="$(jq -r "[.song.song.tracks[].clips[] | select(.id == \"$LEFT\")][0].length" <<<"$s")"
OFFSET="$(jq -r "[.song.song.tracks[].clips[] | select(.id == \"$LEFT\")][0].sourceOffset" <<<"$s")"
HALF=$((RATE / 2))
QUARTER=$((RATE / 4))
check_cmd "the Take's Clip is at least 1.5 s long ($LENGTH samples)" '((LENGTH >= RATE * 3 / 2))'

# clip ID: prints that Clip's fields from the current state.
clip() { state song | jq -c "[.song.song.tracks[].clips[] | select(.id == \"$1\")][0] | {timelineStart, length, sourceOffset, fadeInLength, fadeOutLength}"; }
undo_labels() { state history | jq -c '.history'; }

ctl show_mode mode=edit >/dev/null
ctl select_clip "clip_id=$LEFT" >/dev/null
check "select_clip selected the Take's Clip" "$(state view)" ".view.selected_clip == \"$LEFT\""

SPLIT=$((START + HALF))
ctl seek "sample=$SPLIT" >/dev/null
check "seek put the playhead 0.5 s into the Clip ($SPLIT)" "$(state transport)" ".transport.position == $SPLIT"

RIGHT="$(ctl split_selected_clip | jq -r '.created_id')"
check "split_selected_clip answered the right half's id" "{\"id\": \"$RIGHT\"}" '.id | test("^[0-9a-f-]{36}$")'
check "the left half ends at the split" "$(clip "$LEFT")" \
    ".timelineStart == $START and .length == $HALF and .sourceOffset == $OFFSET"
RIGHT_LENGTH=$((LENGTH - HALF))
check "the right half starts at the split and plays on from it" "$(clip "$RIGHT")" \
    ".timelineStart == $SPLIT and .length == $RIGHT_LENGTH and .sourceOffset == $((OFFSET + HALF))"
check "the right half is selected" "$(state view)" ".view.selected_clip == \"$RIGHT\""
check "the undo label is Split Clip" "$(undo_labels)" '.undo_label == "Split Clip"'
snapshot after-split song history view

MOVED=$((SPLIT + QUARTER))
ctl move_clip "clip_id=$RIGHT" "start=$MOVED" >/dev/null
check "move_clip moved the right half 0.25 s later, same length" "$(clip "$RIGHT")" \
    ".timelineStart == $MOVED and .length == $RIGHT_LENGTH and .sourceOffset == $((OFFSET + HALF))"
check "the undo label is Move Clip" "$(undo_labels)" '.undo_label == "Move Clip"'

TRIMMED=$((MOVED + QUARTER))
ctl trim_clip_start "clip_id=$RIGHT" "start=$TRIMMED" >/dev/null
TRIMMED_LENGTH=$((RIGHT_LENGTH - QUARTER))
check "trim_clip_start cut 0.25 s off the right half's start" "$(clip "$RIGHT")" \
    ".timelineStart == $TRIMMED and .length == $TRIMMED_LENGTH and .sourceOffset == $((OFFSET + HALF + QUARTER))"
check "the undo label is Trim Clip" "$(undo_labels)" '.undo_label == "Trim Clip"'

END_LENGTH=$((QUARTER * 2))
ctl trim_clip_end "clip_id=$RIGHT" "end=$((TRIMMED + END_LENGTH))" >/dev/null
check "trim_clip_end left the right half 0.5 s long" "$(clip "$RIGHT")" \
    ".timelineStart == $TRIMMED and .length == $END_LENGTH"
check "the undo label is Trim Clip" "$(undo_labels)" '.undo_label == "Trim Clip"'

FADE=$((RATE / 10))
ctl set_clip_fade_in "clip_id=$RIGHT" "length=$FADE" >/dev/null
check "set_clip_fade_in set a 0.1 s fade in" "$(clip "$RIGHT")" ".fadeInLength == $FADE and .fadeOutLength == 0"
check "the undo label is Fade In" "$(undo_labels)" '.undo_label == "Fade In"'
snapshot after-fade song history

ctl undo >/dev/null
check "undo removed the fade in" "$(clip "$RIGHT")" ".fadeInLength == 0 and .length == $END_LENGTH"
check "after one undo: undo Trim Clip, redo Fade In" "$(undo_labels)" '.undo_label == "Trim Clip" and .redo_label == "Fade In"'
ctl undo >/dev/null
check "a second undo put the right half's end back" "$(clip "$RIGHT")" \
    ".timelineStart == $TRIMMED and .length == $TRIMMED_LENGTH and .fadeInLength == 0"
check "after two undos: undo Trim Clip, redo Trim Clip" "$(undo_labels)" '.undo_label == "Trim Clip" and .redo_label == "Trim Clip"'
ctl redo >/dev/null
check "redo trimmed the end again" "$(clip "$RIGHT")" ".timelineStart == $TRIMMED and .length == $END_LENGTH and .fadeInLength == 0"
check "after the redo: undo Trim Clip, redo Fade In" "$(undo_labels)" '.undo_label == "Trim Clip" and .redo_label == "Fade In"'
snapshot final song history view

check "song.json on disk has the edited right half" "$(jq -c . "$SONG/song.json")" \
    "[.tracks[].clips[] | select(.id == \"$RIGHT\")][0] | .timelineStart == $TRIMMED and .length == $END_LENGTH and .fadeInLength == 0"
cp "$SONG/song.json" "$EVIDENCE/song.json"
journey_end
