#!/bin/zsh
# Journey practice-speed: a new Song with an 8 s synthetic Backing Track, set_practice_speed 80,
# checking that it is outside history and song.json, that playback switches to it once rendered and
# then moves the playhead at 0.8 times real time, that an out-of-range value is an explicit Error,
# and that 100 plays the Song's own audio at once.
# Usage: .claude/skills/verify/scripts/journey-practice-speed.sh [run-dir]   (default: build/verify/latest)
# Needs a running app from launch.sh with no Song open (it opens its own). It plays, with the
# Master at its minimum.
# Evidence: $RUN_DIR/journeys/practice-speed/.
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
zmodload zsh/datetime # EPOCHREALTIME
journey_begin practice-speed

SONG="$RUN_DIR/songs/practice-speed.overdub"
INPUT="$EVIDENCE/backing-8s.wav"
write_test_wav "$INPUT" 8
rm -rf "$SONG"
check "no Song is open at the start" "$(state song)" '.song == null'
ctl new_song_at "path=$SONG" >/dev/null
silence_master
ctl import_files "files=$(jq -cn --arg file "$INPUT" '[$file]')" >/dev/null
UNDO_LABEL="$(state history | jq -r '.history.undo_label')"
check "the Song starts at 100%" "$(state transport)" '.transport.practice_speed == 100 and .transport.heard_practice_speed == 100'

ctl set_practice_speed speed_percent=80 >/dev/null
check "Practice Speed is 80% and no undo step" "$(state transport history)" ".transport.practice_speed == 80 and .history.undo_label == \"$UNDO_LABEL\""
ctl wait pointer=/transport/practice_rendering equals=false timeout_ms=60000 >/dev/null
check "playback is at 80% once rendered" "$(state transport)" '.transport.heard_practice_speed == 80'
check_cmd "song.json has no Practice Speed" '! grep -qi practice "$SONG/song.json"'
check_cmd "no render is in the package" '[[ -z "$(find "$SONG" -name "*.speed*.wav")" ]]'

# The playhead moves at 0.8 times real time: two positions 1 s apart.
RATE="$(state song | jq -r '.song.song.sampleRate')"
ctl play_or_stop >/dev/null
sleep 0.5
P1="$(state transport | jq -r '.transport.position')"; T1="$EPOCHREALTIME"
sleep 1
P2="$(state transport | jq -r '.transport.position')"; T2="$EPOCHREALTIME"
ctl play_or_stop >/dev/null
SPEED="$(python3 -I -c "print(round(($P2 - $P1) / (($T2 - $T1) * $RATE), 3))")"
echo "measured speed $SPEED" >>"$EVIDENCE/checks.txt"
check_cmd "the playhead moved at about 0.8 times real time ($SPEED)" "python3 -I -c 'import sys; sys.exit(0 if 0.7 <= $SPEED <= 0.9 else 1)'"

REFUSED="$("$CTL" --socket "$SOCKET" set_practice_speed speed_percent=126 2>&1)"
echo "$REFUSED" >"$EVIDENCE/out-of-range.json"
check "126 is refused with song.practice_speed_out_of_range" "$REFUSED" '.ok == false and .error.code == "song.practice_speed_out_of_range"'
check "the refused value changed nothing" "$(state transport)" '.transport.practice_speed == 80'

ctl set_practice_speed speed_percent=100 >/dev/null
check "100% plays the Song's own audio at once" "$(state transport)" '.transport.practice_speed == 100 and .transport.heard_practice_speed == 100 and .transport.practice_rendering == false'
snapshot final song history transport
journey_end
