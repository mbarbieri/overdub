"""Objective numbers and listening files for the Separation spike.

For each /out/<song>/<model>/ with separated parts:
  - per part: peak and RMS in dBFS (spots near-silent parts)
  - reconstruction residual: original minus the sum of all parts, in dB relative to the
    original's RMS (how far the parts are from adding back up to the mix)
  - without_<part>.wav: the sum of every other part, i.e. what plays in Overdub when that
    part's Track is muted and the source Track is muted (the overdub scenario)
Writes /out/report.md.
"""

import csv
import pathlib
import sys

import librosa
import numpy as np
import soundfile as sf

PART_NAMES = ["vocals", "drums", "bass", "guitar", "piano", "other"]
RATE = 44100


def db(value):
    return 20 * np.log10(max(value, 1e-12))


def part_name_of(path):
    lowered = path.stem.lower()
    for name in PART_NAMES:
        if f"({name})" in lowered:
            return name
    return None


def load(path):
    audio, _ = librosa.load(path, sr=RATE, mono=False)
    if audio.ndim == 1:
        audio = np.stack([audio, audio])
    return audio


def analyse_run(original, run_dir):
    parts = {}
    for wav in sorted(run_dir.glob("*.wav")):
        name = part_name_of(wav)
        if name is not None and not wav.name.startswith("without_"):
            parts[name] = load(wav)
    if not parts:
        return None

    length = min(original.shape[1], *(p.shape[1] for p in parts.values()))
    original = original[:, :length]
    parts = {name: audio[:, :length] for name, audio in parts.items()}
    total = sum(parts.values())

    for name in parts:
        others = total - parts[name]
        sf.write(run_dir / f"without_{name}.wav", others.T, RATE, subtype="FLOAT")

    original_rms = np.sqrt(np.mean(original**2))
    residual_rms = np.sqrt(np.mean((original - total) ** 2))
    rows = {
        name: (db(np.max(np.abs(audio))), db(np.sqrt(np.mean(audio**2))))
        for name, audio in parts.items()
    }
    return {"residual_db": db(residual_rms) - db(original_rms), "parts": rows}


def main(in_dir, out_dir):
    runs = {}
    with open(out_dir / "runs.tsv") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            runs[(row["song"], row["model"])] = row

    lines = ["# Separation spike: objective numbers", ""]
    lines.append("| Song | Model | Wall time | Peak memory | Residual vs mix |")
    lines.append("|---|---|---|---|---|")
    details = []
    for source in sorted(in_dir.iterdir()):
        if source.name.startswith("."):
            continue
        song = source.stem
        song_dir = out_dir / song
        if not song_dir.is_dir():
            continue
        original = load(source)
        duration = original.shape[1] / RATE
        for run_dir in sorted(p for p in song_dir.iterdir() if p.is_dir()):
            result = analyse_run(original, run_dir)
            run = runs.get((song, run_dir.name), {})
            wall = float(run.get("wall_seconds", "nan"))
            residual = f"{result['residual_db']:.1f} dB" if result else "n/a"
            lines.append(
                f"| {song} ({duration:.0f} s) | {run_dir.name} | {wall:.0f} s ({wall / duration:.2f}x realtime) "
                f"| {run.get('peak_rss_mb', '?')} MB | {residual} |"
            )
            if result:
                details.append(f"\n## {song} / {run_dir.name}\n\n| Part | Peak dBFS | RMS dBFS |\n|---|---|---|")
                for name in PART_NAMES:
                    if name in result["parts"]:
                        peak, rms = result["parts"][name]
                        details.append(f"| {name} | {peak:.1f} | {rms:.1f} |")
    report = "\n".join(lines + details) + "\n"
    (out_dir / "report.md").write_text(report)
    print(report)


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
