"""Null test: how far each part in <candidate_dir> is from the same part in <reference_dir>.

Usage: nulltest.py <reference_dir> <candidate_dir>
Prints, per part, the RMS of (candidate - reference) relative to (a) the reference part's RMS and
(b) the mix's RMS (sum of all reference parts). (a) is meaningless for a near-silent part (float
noise over a tiny level), so the verdict uses (b): -60 dB or lower on every part.
"""

import pathlib
import sys

import numpy as np
import soundfile as sf


def db(value):
    return 20 * np.log10(max(float(value), 1e-12))


def main(reference_dir, candidate_dir):
    pairs = []
    for reference_path in sorted(pathlib.Path(reference_dir).glob("*.wav")):
        reference, _ = sf.read(reference_path, dtype="float32", always_2d=True)
        candidate, _ = sf.read(pathlib.Path(candidate_dir) / reference_path.name, dtype="float32", always_2d=True)
        pairs.append((reference_path.stem, reference, candidate))
    length = min(min(len(r), len(c)) for _, r, c in pairs)
    mix_rms = np.sqrt(np.mean(sum(r[:length] for _, r, _ in pairs) ** 2))

    worst = -999.0
    for name, reference, candidate in pairs:
        difference_rms = np.sqrt(np.mean((candidate[:length] - reference[:length]) ** 2))
        vs_part = db(difference_rms) - db(np.sqrt(np.mean(reference[:length] ** 2)))
        vs_mix = db(difference_rms) - db(mix_rms)
        worst = max(worst, vs_mix)
        print(f"{name:7} vs part {vs_part:7.1f} dB   vs mix {vs_mix:7.1f} dB   (lengths {len(reference)} / {len(candidate)})")
    print(f"worst vs mix {worst:.1f} dB -> {'PASS' if worst <= -60 else 'FAIL'} (threshold -60 dB)")


if __name__ == "__main__":
    main(*sys.argv[1:])
