# Separation: model quality spike

Branch `spike/stem-separation` (the `spikes/stem-separation/` scripts below live only there). Question: which 6-part model gives the best Separation quality on real material, and is its time and memory bearable on the v1 machine? Quality decides; speed only has to be bearable (Separation is rare and modal).

## Setup

- Machine: Intel Core i7-9750H (6 cores / 12 threads), 32 GB RAM. Runs inside Docker Desktop's Linux x86_64 VM (12 CPUs, 7.75 GB), because PyTorch ships no Intel-macOS wheels after 2.2.2 and every `audio-separator` release with BS-RoFormer SW needs torch >= 2.3. CPU only.
- Tool: `audio-separator` 0.47.0, torch 2.8.0 CPU (`spikes/stem-separation/`: `Dockerfile`, `separate.sh`, `analyse.py`).
- Candidates: `htdemucs_6s` (Demucs v4, StemDeck's model) and `BS-Roformer-SW` (jarredou, CC-BY-NC-SA-4.0).
- Material: 3 of the user's songs, MP3 44.1 kHz stereo.
- Reproduce: `docker build -t overdub-separation-spike spikes/stem-separation && docker run --rm -v <songs>:/in:ro -v <models cache>:/models -v <out>:/out overdub-separation-spike`.

## Objective numbers

| Song | Model | Wall time | x realtime | Peak memory | Residual vs mix |
|---|---|---|---|---|---|
| song1 (3:35) | htdemucs_6s | 207 s | 0.96 | 3.3 GB | -23.7 dB |
| song1 (3:35) | BS-Roformer-SW | 1245 s | 5.80 | 3.6 GB | -29.8 dB |
| song2 (4:57) | htdemucs_6s | 277 s | 0.93 | 3.9 GB | -19.1 dB |
| song2 (4:57) | BS-Roformer-SW | 1570 s | 5.29 | 4.0 GB | -13.1 dB |
| song3 (3:34) | htdemucs_6s | 192 s | 0.90 | 3.4 GB | -23.9 dB |
| song3 (3:34) | BS-Roformer-SW | 1013 s | 4.73 | 3.7 GB | -28.2 dB |

- Residual = original minus the sum of all six parts, relative to the original's RMS. It says how well the parts add back up, not how cleanly they are separated (bleed adds up perfectly).
- Speed: Demucs runs at about realtime; RoFormer at about 5-6x realtime (a 4-minute song takes ~20-25 minutes). Both peak at 3.3-4 GB.
- Song2 is the only song with real piano: RoFormer's piano part is at -29 dB RMS, Demucs's at -44 dB, so RoFormer assigns far more of the song to piano. Song2 is also the one where RoFormer's parts add back up worst (-13 dB): unexplained so far.
- Songs 1 and 3 have no keys: both models leave piano near silence (-64 to -77 dB RMS).

Per-part peak and RMS for every run: `report.md` in the output folder.

## Listening verdict

The user listened to every part solo and every `without_<part>.wav` (the other five summed, what plays when that part's Track is muted):

- BS-RoFormer SW is far better, especially on guitar.
- Demucs's `without_*` mixes have audible artifacts; RoFormer's do not.
- Song2's poor RoFormer residual (-13 dB) was not audible as a problem.

## Decision

BS-RoFormer SW. Quality outweighs speed: a Separation of a typical song takes 20-25 minutes on the v1 machine (PyTorch CPU; the shipped runtime may differ), which the modal Separation dialog accepts.

## Licence of the weights

- BS-RoFormer SW has no licence grant: unblend's registry (`metadata.yaml`) records it as "unlicensed" (original trainer unidentified; jarredou's repackaged checkpoint survives only at https://huggingface.co/enerjazzer/BS-ROFO-SW-Fixed, "License: unknown"). An earlier source calling it CC-BY-NC-SA-4.0 is not backed by any card. Demucs weights are "unlicensed" in the same registry.
- So nobody may redistribute the weights: they are not bundled in `Overdub.app`, not committed, not attached to releases.
- Decision: the user brings the model. A repo script downloads the checkpoint (unblend's pinned safetensors, ~700 MB, SHA-256 56c131428745d83d63f72293d237fdbb56f40d8e4b6980cbcde70e28a9f7330f), converts it to ONNX and installs it in `~/Library/Application Support/Overdub/models/`. Until it is there, "Separate…" explains how to get it.

## Runtime spike: ONNX Runtime from C++

Question: can the helper run BS-RoFormer SW from C++ on the v1 machine, producing the same parts as a reference implementation, at a bearable time and memory?

Setup:
- `spikes/stem-separation/onnx/`: unblend 1.0.0 exports `bs_roformer_sw` to ONNX (opset 18, static batch 1). fp32: 668 MB, fp16: 335 MB. The graph holds only the network: input `spec_real`/`spec_imag` `[1, 2, 1025, 1151]`, output `out_spec_real`/`out_spec_imag` `[1, 6, 2, 1025, 1151]`. Metadata carries `sources` (bass, drums, other, vocals, guitar, piano), `segment_samples` 588800 (13.35 s), STFT n_fft 2048 / hop 512 / win 2048, Hann, not normalized; `external_normalization` false.
- `reference.py`: Python reference of the chunked pipeline (torch.stft/istft + onnxruntime). Chunks of `segment_samples`, stride 0.75 x segment, zero-padded past the end, unblend's triangular cross-fade, divided by the summed weight.
- `spikes/stem-separation/cpp/main.cpp`: the same pipeline in C++ (pocketfft for the FFTs) against the prebuilt ONNX Runtime 1.23.2 `osx-x86_64` (the last release with Intel-macOS binaries; 1.24 dropped them). Built with `xcrun clang++ -std=c++23 -arch x86_64`, no other dependency.
- `nulltest.py`: per part, RMS of (candidate - reference), relative to the part and to the mix. Pass: -60 dB or lower relative to the mix on every part (relative to a near-silent part it only measures float noise).

Results, song1 (3:35), 12 intra-op threads:

| Run | Time | x realtime | Peak memory | vs Python reference (relative to the mix) |
|---|---|---|---|---|
| C++ fp32 | 916 s | 4.27 | 6.3 GB | -76.7 dB worst part: PASS |
| C++ fp16 | 973 s | 4.53 | 9.2 GB | -31.4 dB worst part |
| Python reference fp32 (same ORT) | 806 s | 3.76 | n/a | - |
| audio-separator PyTorch (Docker) | 1245 s | 5.80 | 3.6 GB | - |

- The C++ pipeline is correct: every part matches the reference to -77 to -87 dB relative to the mix.
- fp32, not fp16: on an Intel CPU fp16 is no faster, takes more memory (casts), and moves the parts by -23 to -31 dB relative to each part.
- Peak memory is 6.3 GB, against 3.6 GB in PyTorch; not broken down (ORT's arena and the 668 MB of weights are the likely bulk). Worth trying later: `DisableCpuMemArena`, fewer threads.
- The ONNX output is not bit-identical to the audio-separator parts the user approved (about -22 dB apart on the loud parts): same model, different chunking. audio-separator uses 409600-sample chunks (dim_t 801 x hop 512), 50% overlap (`num_overlap: 2`), a Hamming cross-fade and an end-aligned last chunk; the exported graph is fixed at 588800 samples.
- The user compared the C++ fp32 parts and `without_*` mixes for song1 with the approved audio-separator ones: the results are good. Decision: keep the exported graph's chunking (588800-sample chunks, stride 0.75, triangular cross-fade), the fastest of the two.
