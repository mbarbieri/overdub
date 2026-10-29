#!/bin/zsh
# Journey transpose: a new Song with a 4 s synthetic Backing Track, set_transpose -1, +2, undo, a
# fine tune of +30 cents (issue #232), undo and 0, checking the undo labels, the heard Transpose, the renders in the package (as long as the derived
# WAV) and that an out-of-range value is an explicit Error.
# Usage: .claude/skills/verify/scripts/journey-transpose.sh [run-dir]   (default: build/verify/latest)
# Needs a running app from launch.sh with no Song open (it opens its own). Nothing is played.
# Evidence: $RUN_DIR/journeys/transpose/.
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
journey_begin transpose

SONG="$RUN_DIR/songs/transpose.overdub"
INPUT="$EVIDENCE/backing-4s.wav"
write_test_wav "$INPUT" 4
rm -rf "$SONG"
check "no Song is open at the start" "$(state song)" '.song == null'
ctl new_song_at "path=$SONG" >/dev/null
silence_master
ctl import_files "files=$(jq -cn --arg file "$INPUT" '[$file]')" >/dev/null
BACKING="$(state song | jq -r '.song.song.backingTracks[0].id')"
check "the Song starts at Transpose 0" "$(state song transport)" '.song.song.transpose == 0 and .transport.heard_transpose == 0'

# frames FILE: the sample frames of a 32-bit float WAV (the wave module reads integer PCM only).
frames() {
    python3 -I - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
i, channels, size = 12, 1, 0
while i < len(d):
    cid, sz = d[i:i + 4], struct.unpack("<I", d[i + 4:i + 8])[0]
    if cid == b"fmt ":
        channels = struct.unpack("<H", d[i + 10:i + 12])[0]
    if cid == b"data":
        size = sz
    i += 8 + sz + (sz & 1)
print(size // (4 * channels))
PY
}
renders() { ls "$SONG/backing" | grep -c '\.transpose' ; }
# transpose_to N: sets Transpose and waits until playback is at it.
transpose_to() {
    ctl set_transpose "semitones=$1" >/dev/null
    ctl wait pointer=/transport/transposing equals=false timeout_ms=60000 >/dev/null
    check "playback is at Transpose $1" "$(state song transport)" ".song.song.transpose == $1 and .transport.heard_transpose == $1"
}

transpose_to -1
check "the undo label is Transpose −1" "$(state history)" '.history.undo_label == "Transpose −1"'
RENDER="$SONG/backing/$BACKING.transpose-1.wav"
check_cmd "the render $RENDER is in the package" '[[ -f "$RENDER" ]]'
DERIVED_FRAMES="$(frames "$SONG/backing/$BACKING.wav")"
RENDER_FRAMES="$(frames "$RENDER")"
check_cmd "the render is as long as the derived WAV ($RENDER_FRAMES = $DERIVED_FRAMES frames)" '[[ "$RENDER_FRAMES" == "$DERIVED_FRAMES" && "$RENDER_FRAMES" -gt 0 ]]'

transpose_to 2
check_cmd "only the +2 render is left ($(renders) render)" '[[ "$(renders)" == 1 && -f "$SONG/backing/$BACKING.transpose+2.wav" ]]'

ctl undo >/dev/null
ctl wait pointer=/transport/transposing equals=false timeout_ms=60000 >/dev/null
check "undo went back to −1 and played it once rendered again" "$(state song transport history)" \
    '.song.song.transpose == -1 and .transport.heard_transpose == -1 and .history.redo_label == "Transpose +2"'
check_cmd "the −1 render is back and the +2 one gone" '[[ "$(renders)" == 1 && -f "$RENDER" ]]'

REFUSED="$("$CTL" --socket "$SOCKET" set_transpose semitones=13 2>&1)"
echo "$REFUSED" >"$EVIDENCE/out-of-range.json"
check "+13 is refused with song.transpose_out_of_range" "$REFUSED" '.ok == false and .error.code == "song.transpose_out_of_range"'
check "the refused value changed nothing" "$(state song)" '.song.song.transpose == -1'

# The fine tune (issue #232): -1 semitones +30 cents is one render, and undo goes back to -1.
ctl set_transpose semitones=-1 cents=30 >/dev/null
ctl wait pointer=/transport/transposing equals=false timeout_ms=60000 >/dev/null
check "playback is at Transpose −1 +30 cents" "$(state song transport history)" \
    '.song.song.transpose == -1 and .song.song.transposeCents == 30 and .transport.heard_transpose == -1 and .transport.heard_transpose_cents == 30 and .history.undo_label == "Transpose −1 +30 ct"'
CENTS_RENDER="$SONG/backing/$BACKING.transpose-1+30c.wav"
check_cmd "only the −1 +30 cents render is left ($(renders) render)" '[[ "$(renders)" == 1 && -f "$CENTS_RENDER" ]]'
CENTS_FRAMES="$(frames "$CENTS_RENDER")"
check_cmd "the cents render is as long as the derived WAV ($CENTS_FRAMES = $DERIVED_FRAMES frames)" '[[ "$CENTS_FRAMES" == "$DERIVED_FRAMES" ]]'
CENTS_REFUSED="$("$CTL" --socket "$SOCKET" set_transpose semitones=-1 cents=51 2>&1)"
echo "$CENTS_REFUSED" >"$EVIDENCE/cents-out-of-range.json"
check "+51 cents is refused with song.transpose_cents_out_of_range" "$CENTS_REFUSED" '.ok == false and .error.code == "song.transpose_cents_out_of_range"'
ctl undo >/dev/null
ctl wait pointer=/transport/transposing equals=false timeout_ms=60000 >/dev/null
check "undo went back to −1 with no fine tune" "$(state song transport)" \
    '.song.song.transpose == -1 and .song.song.transposeCents == 0 and .transport.heard_transpose_cents == 0'
check_cmd "the −1 render is back and the cents one gone" '[[ "$(renders)" == 1 && -f "$RENDER" ]]'

transpose_to 0
check_cmd "at Transpose 0 no render is left" '[[ "$(renders)" == 0 ]]'
snapshot final song history transport
check "song.json on disk is at Transpose 0" "$(jq -c . "$SONG/song.json")" '.transpose == 0 and .transposeCents == 0 and .formatVersion == 11'
journey_end
