#!/usr/bin/env bash
# Runs every candidate model on every file in /in, writing to /out/<song>/<model>/.
# Records wall time and peak memory per run in /out/runs.tsv, then runs analyse.py.
set -euo pipefail

MODELS=("htdemucs_6s.yaml" "BS-Roformer-SW.ckpt")

mkdir -p /out
[[ -f /out/runs.tsv ]] || printf "song\tmodel\twall_seconds\tpeak_rss_mb\texit\n" > /out/runs.tsv

shopt -s nullglob
for input in /in/*.{wav,mp3,flac,m4a,aif,aiff}; do
    song="$(basename "${input%.*}")"
    for model in "${MODELS[@]}"; do
        dir="/out/$song/${model%.*}"
        if [[ -f "$dir/.done" ]]; then
            echo "skip $song / $model (done)"
            continue
        fi
        mkdir -p "$dir"
        echo "=== $song / $model"
        set +e
        /usr/bin/time -f "%e %M" -o "$dir/time.txt" \
            audio-separator "$input" --model_filename "$model" --model_file_dir /models \
            --output_dir "$dir" --output_format WAV 2>&1 | tee "$dir/log.txt"
        code=${PIPESTATUS[0]}
        set -e
        # GNU time prepends "Command exited with non-zero status N" on failure; the numbers are last.
        read -r wall rss_kb < <(tail -n 1 "$dir/time.txt")
        printf "%s\t%s\t%s\t%s\t%s\n" "$song" "${model%.*}" "$wall" "$((rss_kb / 1024))" "$code" >> /out/runs.tsv
        [[ $code -eq 0 ]] && touch "$dir/.done"
    done
done

python /spike/analyse.py /in /out
