#!/bin/zsh
# Journey last-song: a new Song becomes the Last Song, and the next launch reopens it.
# Usage: .claude/skills/verify/scripts/journey-last-song.sh [run-dir]   (default: build/verify/latest)
# Needs a launch.sh launch with no Song open. Creates a Song under $RUN_DIR/songs/, quits that launch
# (cleanup.sh), launches again with LAST_SONG_FILE naming the first launch's file, checks the Song
# is open and ui.last_song_reopened was logged, then quits the second launch. Evidence:
# $RUN_DIR/journeys/last-song/ (the second launch's run dir is in relaunch.txt).
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
journey_begin last-song
FIRST_RUN="$RUN_DIR"
LAST_SONG="$RUN_DIR/last-song.json"

check "the first launch opened no Song" "$(state song)" '.song == null'
check_cmd "the first launch has no Last Song yet ($LAST_SONG)" '[[ ! -e "$LAST_SONG" ]]'

SONG="$RUN_DIR/songs/LastSong.overdub"
ctl new_song_at "path=$SONG" >/dev/null
check "new_song_at opened $SONG" "$(state song)" '.song.path == $song' --arg song "$SONG"
# Recorded at the window's next tick (50 ms) after the Song finished opening.
for _ in {1..20}; do [[ -s "$LAST_SONG" ]] && break; sleep 0.25; done
cp "$LAST_SONG" "$EVIDENCE/last-song.json" 2>/dev/null
check "the Last Song file names $SONG" "$(cat "$LAST_SONG" 2>/dev/null || echo null)" \
    '.version == 1 and .path == $song' --arg song "$SONG"

"$SCRIPTS/cleanup.sh" "$FIRST_RUN" >"$EVIDENCE/cleanup-first.txt" 2>&1 || journey_fail "cleanup of the first launch failed"

relaunched="$(LAST_SONG_FILE="$LAST_SONG" "$SCRIPTS/launch.sh" last-song-relaunch 2>&1)" \
    || journey_fail "the second launch failed: $relaunched"
echo "$relaunched" >"$EVIDENCE/relaunch.txt"
SECOND_RUN="$(grep '^RUN_DIR=' <<<"$relaunched" | cut -d= -f2-)"
SOCKET="$(cat "$SECOND_RUN/socket")"
LOG="$(cat "$SECOND_RUN/log-path")"
check "the second launch read the first launch's Last Song file" "$relaunched" \
    '(split("\n") | map(select(startswith("LAST_SONG="))) | .[0]) == ("LAST_SONG=" + $file)' -R -s --arg file "$LAST_SONG"

ctl wait pointer=/song/path "equals=\"$SONG\"" timeout_ms=10000 >/dev/null
snapshot reopened song view
check "the second launch reopened $SONG in Record mode" "$(state song view)" \
    '.song.path == $song and .view.mode == "record"' --arg song "$SONG"
check "ui.last_song_reopened was logged once with its path" \
    "$(jq -c 'select(.event == "ui.last_song_reopened")' "$LOG" | jq -s .)" \
    'length == 1 and .[0].lvl == "info" and .[0].path == $song' --arg song "$SONG"

"$SCRIPTS/cleanup.sh" "$SECOND_RUN" >"$EVIDENCE/cleanup-second.txt" 2>&1 || journey_fail "cleanup of the second launch failed"
journey_end
