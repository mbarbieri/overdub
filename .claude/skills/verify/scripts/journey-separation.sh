#!/bin/zsh
# Journey separation-in-song: a new Song with a 6 s synthetic Backing Track (110 Hz sine + clicks),
# start_separation on its Track, then six new Tracks, each a Backing Track separatedFrom the source,
# and the source Track muted, as one undo step.
# Usage: .claude/skills/verify/scripts/journey-separation.sh [run-dir]   (default: build/verify/latest)
# Needs the installed model and a running app from launch.sh with no Song open (it opens its own).
# Takes about 30-60 s and about 4.6 GB of memory. Evidence: $RUN_DIR/journeys/separation/.
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
journey_begin separation

MODEL="${OVERDUB_MODELS_DIR:-$HOME/Library/Application Support/Overdub/models}/bs_roformer_sw.fp32.onnx"
check_cmd "the Separation model is installed ($MODEL)" '[[ -f "$MODEL" ]]'

SONG="$RUN_DIR/songs/separation.overdub"
INPUT="$EVIDENCE/backing-6s.wav"
write_test_wav "$INPUT" 6
rm -rf "$SONG"
ctl new_song_at "path=$SONG" >/dev/null
check "new_song_at opened the Song at $SONG" "$(state song)" ".song.path == \"$SONG\""
silence_master
ctl import_files "files=[\"$INPUT\"]" >/dev/null
s="$(state song)"
check "the import added one Backing Track on one Track" "$s" \
    '(.song.song.backingTracks | length) == 1 and (.song.song.tracks | length) == 1'
SOURCE_TRACK="$(jq -r '.song.song.tracks[0].id' <<<"$s")"
SOURCE_BACKING="$(jq -r '.song.song.backingTracks[0].id' <<<"$s")"
check "start_separation is not refused" "$(state refused)" '.refused.start_separation == null'

ctl start_separation "track_id=$SOURCE_TRACK" >/dev/null
activity_started
check "a Separation is running" "$(state activity)" '.activity.kind == "separation"'
wait_finish separation 300000

snapshot final song history activity window
s="$(cat "$EVIDENCE/state-final.json")"
check "the Song has seven Tracks: the source and six new ones" "$s" '(.song.song.tracks | length) == 7'
check "the source Track is first and muted" "$s" ".song.song.tracks[0].id == \"$SOURCE_TRACK\" and .song.song.tracks[0].mute == true"
check "six new Backing Tracks are separatedFrom the source Backing Track" "$s" \
    "[.song.song.backingTracks[] | select(.separatedFrom.backingTrackId == \"$SOURCE_BACKING\")] | length == 6"
check "each new Track has one Clip that plays one of those Backing Tracks" "$s" \
    "([.song.song.backingTracks[] | select(.separatedFrom.backingTrackId == \"$SOURCE_BACKING\") | .id]) as \$parts
     | [.song.song.tracks[1:][] | .clips | length == 1 and (.[0].source.kind == \"backing\") and (.[0].source.id | IN(\$parts[]))] | all"
check "each part is as long as the source" "$s" \
    "(.song.song.backingTracks[0].lengthSamples) as \$n | [.song.song.backingTracks[1:][] | .lengthSamples == \$n] | all"
check "the run dialog is closed" "$s" '.window.run_dialog.showing == false'
check "the Separation is one undo step, Separate backing-6s.wav" "$s" '.history.undo_label == "Separate backing-6s.wav"'
jq -r '.song.song.tracks[] | "\(.id) \(.name) mute=\(.mute)"' <<<"$s" | tee "$EVIDENCE/tracks.txt"
jq -r '.history.undo_label' <<<"$s" | tee "$EVIDENCE/undo-label.txt"
check_cmd "song.json on disk lists seven Tracks" '[[ "$(jq ".tracks | length" "$SONG/song.json")" == 7 ]]'
for id in $(jq -r '.song.song.backingTracks[1:][] | .id' <<<"$s"); do
    check_cmd "backing/ has a file for part $id" '[[ -n "$(ls "$SONG/backing" | grep -F "$id")" ]]'
done
cp "$SONG/song.json" "$EVIDENCE/song.json"
journey_end
