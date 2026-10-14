"""Installs the BS-RoFormer SW Separation model as an fp32 ONNX graph (ADR 0007, issue #112).

Runs inside the converter container (`install.sh` starts it). Steps, in order:

  existing  a valid model already at the final path: say so and stop
  download  unblend's pinned bs_roformer_sw.safetensors into the cache (reused when its hash matches)
  verify    SHA-256 of the download; a mismatch stops the install
  export    unblend's export_to_onnx (fp32, opset 18, static batch 1) into a temp folder next to the model
  check     the exported graph's metadata, opset and input shape
  install   rename the temp file to the final path

Invariant: the final model path only ever receives a file through one os.replace after `check`
passed, so any failure leaves no file there (or the previous one untouched). Every failure names
its step.

The module imports only the standard library at top level so its tests run on the host's python3
(3.9 or later); onnx and unblend are imported inside the container-only functions.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

MODEL_NAME = "bs_roformer_sw"
MODEL_FILE_NAME = "bs_roformer_sw.fp32.onnx"
OPSET_VERSION = 18


@dataclass(frozen=True)
class Checkpoint:
    url: str
    sha256: str


# Pinned in unblend 1.0.0's metadata.yaml (revision 9ba75778 of huggingface.co/Ryan5453/unblend).
PINNED_CHECKPOINT = Checkpoint(
    url="https://huggingface.co/Ryan5453/unblend/resolve/9ba75778acf1b64fc40a9ea05137a2c6af9cd1d5/bs_roformer_sw.safetensors",
    sha256="56c131428745d83d63f72293d237fdbb56f40d8e4b6980cbcde70e28a9f7330f",
)

# What overdub-separate relies on (ADR 0007). Values as unblend writes them into metadata_props.
EXPECTED_METADATA = {
    "architecture": "bs_roformer",
    "sources": ["bass", "drums", "other", "vocals", "guitar", "piano"],
    "segment_samples": 588800,
    "sample_rate": 44100,
    "stft_n_fft": 2048,
    "stft_hop_length": 512,
    "stft_win_length": 2048,
    "stft_normalized": False,
}
EXPECTED_INPUT_SHAPES = {
    "spec_real": [1, 2, 1025, 1151],
    "spec_imag": [1, 2, 1025, 1151],
}


@dataclass(frozen=True)
class ModelFacts:
    """What the install checks about an ONNX file: metadata_props, opset and input shapes."""

    metadata: dict
    opset: int
    input_shapes: dict


class InstallError(Exception):
    def __init__(self, step: str, message: str):
        super().__init__(f"step '{step}' failed: {message}")
        self.step = step
        self.message = message


def log(step: str, message: str) -> None:
    print(f"[{step}] {message}", flush=True)


# ---- pure checks -------------------------------------------------------------------------------


def _parse_metadata_value(key: str, raw: str):
    expected = EXPECTED_METADATA[key]
    if isinstance(expected, list):
        return json.loads(raw)
    if isinstance(expected, bool):
        return {"true": True, "false": False}.get(raw.strip().lower(), raw)
    if isinstance(expected, int):
        return int(raw)
    return raw


def model_problems(facts: ModelFacts) -> list:
    """Every way `facts` differs from the model overdub-separate needs; empty when it is valid."""
    problems = []
    for key, expected in EXPECTED_METADATA.items():
        if key not in facts.metadata:
            problems.append(f"metadata '{key}' is missing")
            continue
        raw = facts.metadata[key]
        try:
            actual = _parse_metadata_value(key, raw)
        except (ValueError, json.JSONDecodeError):
            problems.append(f"metadata '{key}' is unreadable: {raw!r}")
            continue
        if actual != expected:
            problems.append(f"metadata '{key}' is {raw!r}, expected {expected!r}")
    if facts.opset != OPSET_VERSION:
        problems.append(f"opset is {facts.opset}, expected {OPSET_VERSION}")
    for name, shape in EXPECTED_INPUT_SHAPES.items():
        actual_shape = facts.input_shapes.get(name)
        if actual_shape != shape:
            problems.append(f"input '{name}' has shape {actual_shape}, expected {shape}")
    return problems


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ---- container-only steps (real onnx / unblend) -----------------------------------------------


def download_url(url: str, destination: Path) -> None:
    """Streams `url` into `destination`. Raises OSError (urllib errors included) on failure."""
    with urllib.request.urlopen(url, timeout=60) as response, open(destination, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)


def export_with_unblend(cache_dir: Path, output_path: Path) -> None:
    """The spike's known-good export. unblend finds the verified checkpoint in UNBLEND_CACHE_DIR."""
    os.environ["UNBLEND_CACHE_DIR"] = str(cache_dir)
    from unblend.onnx import export_to_onnx

    export_to_onnx(
        MODEL_NAME,
        str(output_path),
        opset_version=OPSET_VERSION,
        precision="fp32",
        static_batch=True,
    )


def read_onnx_facts(path: Path) -> ModelFacts:
    import onnx

    model = onnx.load(str(path), load_external_data=False)
    opset = next((o.version for o in model.opset_import if o.domain in ("", "ai.onnx")), -1)
    shapes = {}
    for tensor in model.graph.input:
        shapes[tensor.name] = [d.dim_value if d.HasField("dim_value") else d.dim_param for d in tensor.type.tensor_type.shape.dim]
    return ModelFacts(
        metadata={p.key: p.value for p in model.metadata_props},
        opset=opset,
        input_shapes=shapes,
    )


# ---- the install --------------------------------------------------------------------------------


def checkpoint_cache_path(cache_dir: Path, checkpoint: Checkpoint) -> Path:
    # unblend's content-addressed cache name: <first 16 hex digits of the SHA-256>.safetensors.
    return cache_dir / f"{checkpoint.sha256[:16]}.safetensors"


def existing_model_is_valid(final_path: Path, read_facts: Callable) -> bool:
    if not final_path.exists():
        return False
    try:
        problems = model_problems(read_facts(final_path))
    except Exception as error:  # an unreadable file is replaced like an invalid one
        log("existing", f"{final_path} is unreadable ({error}); it will be replaced")
        return False
    if problems:
        log("existing", f"{final_path} is not the expected model ({'; '.join(problems)}); it will be replaced")
        return False
    return True


def fetch_checkpoint(cache_dir: Path, checkpoint: Checkpoint, download: Callable) -> Path:
    """Returns the cached checkpoint, downloading it when absent or corrupt. Steps: download, verify."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    cached = checkpoint_cache_path(cache_dir, checkpoint)
    if cached.exists():
        if file_sha256(cached) == checkpoint.sha256:
            log("download", f"using the cached checkpoint {cached}")
            return cached
        log("download", f"cached checkpoint {cached} has the wrong SHA-256; downloading it again")
        cached.unlink()

    partial = cache_dir / f".{cached.name}.partial"
    try:
        log("download", f"downloading {checkpoint.url}")
        try:
            download(checkpoint.url, partial)
        except Exception as error:
            raise InstallError("download", f"{checkpoint.url}: {error}") from error

        actual = file_sha256(partial)
        if actual != checkpoint.sha256:
            raise InstallError("verify", f"SHA-256 of the download is {actual}, expected {checkpoint.sha256}; refusing to continue")
        log("verify", f"SHA-256 {actual} matches")
        os.replace(partial, cached)
        return cached
    finally:
        partial.unlink(missing_ok=True)


def install(models_dir: Path, cache_dir: Path, checkpoint: Checkpoint, download: Callable, export: Callable, read_facts: Callable) -> str:
    """Installs the model at models_dir/MODEL_FILE_NAME. Returns "already_installed" or "installed".

    `download(url, path)`, `export(cache_dir, output_path)` and `read_facts(path) -> ModelFacts`
    are the side effects; tests pass fakes.
    """
    final_path = models_dir / MODEL_FILE_NAME
    if existing_model_is_valid(final_path, read_facts):
        log("existing", f"a valid model is already installed at {final_path}; nothing to do")
        return "already_installed"

    fetch_checkpoint(cache_dir, checkpoint, download)

    models_dir.mkdir(parents=True, exist_ok=True)
    temp_dir = Path(tempfile.mkdtemp(prefix=".install-", dir=models_dir))
    try:
        exported = temp_dir / MODEL_FILE_NAME
        log("export", f"exporting {MODEL_NAME} to fp32 ONNX (opset {OPSET_VERSION}, static batch 1); this takes a few minutes")
        try:
            export(cache_dir, exported)
        except Exception as error:
            raise InstallError("export", f"{type(error).__name__}: {error}") from error
        if not exported.is_file():
            raise InstallError("export", f"the exporter wrote no {exported.name}")
        extra = sorted(p.name for p in temp_dir.iterdir() if p.name != exported.name)
        if extra:
            raise InstallError("export", f"the exporter wrote external data files {extra}; expected one self-contained file")

        try:
            facts = read_facts(exported)
        except Exception as error:
            raise InstallError("check", f"cannot read the exported model: {error}") from error
        problems = model_problems(facts)
        if problems:
            raise InstallError("check", "; ".join(problems))
        log("check", "metadata, opset and input shapes match")

        os.replace(exported, final_path)
        log("install", f"installed {final_path}")
        return "installed"
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--models-dir", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        install(args.models_dir, args.cache_dir, PINNED_CHECKPOINT, download_url, export_with_unblend, read_onnx_facts)
    except InstallError as error:
        print(f"[{error.step}] FAILED: {error.message}", file=sys.stderr, flush=True)
        print("No model was installed.", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
