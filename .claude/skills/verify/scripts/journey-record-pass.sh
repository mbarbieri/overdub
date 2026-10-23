#!/bin/zsh
# Journey record-pass: a new Song, a 3 s synthetic Backing Track, one Armed Track on the first
# input, a ~2 s Pass from the built-in microphone (camera not armed), all through the control socket.
# Usage: .claude/skills/verify/scripts/journey-record-pass.sh [run-dir]   (default: build/verify/latest)
# Needs a running app from launch.sh with no Song open. Leaves the recorded Song open, so
# journey-edit.sh and journey-export.sh can run next. Evidence: $RUN_DIR/journeys/record-pass/.
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
journey_begin record-pass

SONG="$RUN_DIR/songs/record-pass.overdub"
BACKING="$EVIDENCE/backing-3s.wav"
write_test_wav "$BACKING" 3
rm -rf "$SONG"

check "no Song is open at the start" "$(state song)" '.song == null'
ctl new_song_at "path=$SONG" >/dev/null
s="$(state song)"
check "new_song_at opened the Song at $SONG" "$s" '.song.path == $song' --arg song "$SONG"
check_cmd "the Song package exists on disk" '[[ -f "$SONG/song.json" ]]'

ctl show_mode mode=record >/dev/null
ctl import_files "files=$(jq -cn --arg file "$BACKING" '[$file]')" >/dev/null
s="$(state song)"
check "the import added one Backing Track on one Track with one Clip" "$s" \
    '(.song.song.backingTracks | length) == 1 and (.song.song.tracks | length) == 1 and (.song.song.tracks[0].clips | length) == 1'
BACKING_TRACK="$(jq -r '.song.song.tracks[0].id' <<<"$s")"

TRACK="$(ctl add_track | jq -r '.created_id')"
check "add_track answered the new Track's id ($TRACK)" "$(state song)" \
    "(.song.song.tracks | map(.id)) == [\"$BACKING_TRACK\", \"$TRACK\"]"

silence_master
ctl set_track_mute "track_id=$BACKING_TRACK" mute=true >/dev/null

INPUT_NAMES="$(state audio | jq -c '.audio.inputs')"
check "the device offers at least one input ($INPUT_NAMES)" "$(state audio)" '(.audio.inputs | length) >= 1'
ctl set_input_channels "track_id=$TRACK" "channels=[0]" >/dev/null
ctl set_armed "track_id=$TRACK" armed=true >/dev/null
s="$(state song camera monitoring)"
check "the new Track is Armed on input 0 only" "$s" \
    ".song.song.tracks[1].armed == true and .song.song.tracks[1].inputChannels == [0]"
check "the camera is not armed" "$s" '.camera.armed == false'
check "Monitoring is off" "$s" '.monitoring == []'

RATE="$(jq -r '.song.song.sampleRate' <<<"$s")"
TARGET=$((RATE * 2))
ctl record >/dev/null
activity_started
ctl wait pointer=/transport/running_pass_number equals=1 timeout_ms=10000 >/dev/null
for _ in {1..100}; do
    pos="$(state transport | jq -r '.transport.position')"
    ((pos >= TARGET)) && break
    sleep 0.1
done
s="$(state transport activity)"
echo "$s" | jq . >"$EVIDENCE/state-recording.json"
check "the transport passed 2 s while recording Pass 1" "$s" \
    ".transport.position >= $TARGET and .activity.kind == \"pass\" and .transport.running_pass_number == 1"
# record during a Pass does nothing (OpenSong::record); Space, play_or_stop, stops it.
ctl play_or_stop >/dev/null
wait_finish pass 20000

snapshot after-pass song history transport activity
s="$(cat "$EVIDENCE/state-after-pass.json")"
check "the Song has one Pass" "$s" '(.song.song.passes | length) == 1'
check "the Pass has one Take, on the Armed Track" "$s" \
    "(.song.song.takes | length) == 1 and .song.song.takes[0].passId == .song.song.passes[0].id and .song.song.takes[0].trackId == \"$TRACK\""
TAKE="$(jq -r '.song.song.takes[0].id' <<<"$s")"
check "the Armed Track has one Clip playing the Take" "$s" \
    ".song.song.tracks[1].clips | length == 1 and .[0].source == {id: \"$TAKE\", kind: \"take\"}"
check "the undo label is Record Pass 1" "$s" '.history.undo_label == "Record Pass 1"'
check "the transport stopped" "$s" '.transport.playing == false and .transport.running_pass_number == 0'

WAV="$SONG/audio/$TAKE.wav"
check_cmd "the Take's audio/$TAKE.wav exists with nonzero length" '[[ -s "$WAV" ]]'
check_cmd "no .wav.tmp is left in audio/" '[[ -z "$(ls "$SONG/audio" | grep -F .tmp)" ]]'
afinfo "$WAV" >"$EVIDENCE/afinfo-take.txt" 2>&1
check_cmd "afinfo reads the Take as audio at least 1.5 s long" \
    'awk "/estimated duration/ { f = 1; ok = (\$3 >= 1.5) } END { exit !(f && ok) }" "$EVIDENCE/afinfo-take.txt"'

# take_peak WAV: the largest absolute sample of the file converted to 16-bit PCM.
take_peak() {
    local pcm16
    pcm16="$(mktemp -t overdub-take)"
    afconvert -f WAVE -d LEI16 "$1" "$pcm16" && python3 -I - "$pcm16" <<'PY'
import struct, sys, wave
with wave.open(sys.argv[1], "rb") as w:
    frames = w.readframes(w.getnframes())
samples = struct.unpack("<%dh" % (len(frames) // 2), frames)
print(max((abs(v) for v in samples), default=0))
PY
    rm -f "$pcm16"
}
PEAK="$(take_peak "$WAV")"
echo "$PEAK" >"$EVIDENCE/take-peak.txt"
check_cmd "the Take is not all zeros (16-bit peak $PEAK)" '[[ "$PEAK" == <-> ]] && ((PEAK > 0))'

jq -n --arg song "$SONG" --arg track "$TRACK" --arg take "$TAKE" --arg backing_track "$BACKING_TRACK" \
    '{song: $song, track: $track, take: $take, backing_track: $backing_track}' >"$EVIDENCE/ids.json"
journey_end
