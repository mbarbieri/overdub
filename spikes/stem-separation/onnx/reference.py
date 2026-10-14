"""Python reference of the chunked ONNX Separation the C++ helper must reproduce.

Usage: reference.py <model.onnx> <input.wav> <out_dir> [<audio-separator run dir>]

Algorithm (the one the C++ spike implements):
  segment L = metadata segment_samples, stride S = int(0.75 * L)
  for each offset 0, S, 2S, ... < N: take L samples (zero-padded past the end),
  STFT (torch.stft: center, reflect pad, periodic Hann, metadata n_fft/hop/win/normalized),
  run the graph, iSTFT each part to L samples, multiply by unblend's triangular weight,
  accumulate; finally divide by the summed weight.
Writes <out_dir>/<part>.wav (float). With a 4th argument, prints how far each part is from the
audio-separator (PyTorch) part the user listened to.
"""

import json
import pathlib
import sys
import time

import numpy as np
import onnxruntime as ort
import soundfile as sf
import torch


def triangular_weight(length):
    half = length // 2
    rising = np.arange(1, half + 1, dtype=np.float64)
    falling = np.arange(length - half, 0, -1, dtype=np.float64)
    weight = np.concatenate([rising, falling])
    return np.maximum(weight / weight.max(), 1e-20).astype(np.float32)


def db(value):
    return 20 * np.log10(max(float(value), 1e-12))


def main(model_path, input_path, out_dir, separator_dir=None):
    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    meta = session.get_modelmeta().custom_metadata_map
    sources = json.loads(meta["sources"])
    segment = int(meta["segment_samples"])
    n_fft, hop, win = int(meta["stft_n_fft"]), int(meta["stft_hop_length"]), int(meta["stft_win_length"])
    normalized = meta["stft_normalized"] == "true"
    window = torch.hann_window(win)

    audio, rate = sf.read(input_path, dtype="float32", always_2d=True)
    audio = audio.T.copy()  # [channels, samples]
    channels, length = audio.shape
    stride = int(0.75 * segment)
    weight = triangular_weight(segment)

    out = np.zeros((len(sources), channels, length), dtype=np.float32)
    weight_sum = np.zeros(length, dtype=np.float32)
    started = time.time()
    for offset in range(0, length, stride):
        count = min(segment, length - offset)
        chunk = np.zeros((channels, segment), dtype=np.float32)
        chunk[:, :count] = audio[:, offset : offset + count]
        spec = torch.stft(torch.from_numpy(chunk), n_fft=n_fft, hop_length=hop, win_length=win, window=window,
                          normalized=normalized, center=True, pad_mode="reflect", return_complex=True)
        real, imag = session.run(None, {"spec_real": spec.real.numpy()[None], "spec_imag": spec.imag.numpy()[None]})
        parts_spec = torch.complex(torch.from_numpy(real[0]), torch.from_numpy(imag[0]))  # [S, C, F, T]
        flat = parts_spec.reshape(-1, parts_spec.shape[-2], parts_spec.shape[-1])
        parts = torch.istft(flat, n_fft=n_fft, hop_length=hop, win_length=win, window=window,
                            normalized=normalized, center=True, length=segment).numpy()
        parts = parts.reshape(len(sources), channels, segment)
        out[:, :, offset : offset + count] += parts[:, :, :count] * weight[:count]
        weight_sum[offset : offset + count] += weight[:count]
    out /= weight_sum
    print(f"reference: {time.time() - started:.1f} s for {length / rate:.1f} s of audio, segment {segment}, stride {stride}")

    out_dir = pathlib.Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for index, name in enumerate(sources):
        sf.write(out_dir / f"{name}.wav", out[index].T, rate, subtype="FLOAT")

    if separator_dir:
        for index, name in enumerate(sources):
            matches = list(pathlib.Path(separator_dir).glob(f"*({name})*.wav"))
            if not matches:
                continue
            theirs, _ = sf.read(matches[0], dtype="float32", always_2d=True)
            theirs = theirs.T[:, :length]
            ours = out[index][:, : theirs.shape[1]]
            diff = np.sqrt(np.mean((ours - theirs) ** 2))
            level = np.sqrt(np.mean(theirs**2))
            print(f"vs audio-separator {name:7} difference {db(diff) - db(level):6.1f} dB relative to the part")


if __name__ == "__main__":
    main(*sys.argv[1:])
