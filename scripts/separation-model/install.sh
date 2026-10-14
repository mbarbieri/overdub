#!/bin/bash
# Installs the BS-RoFormer SW Separation model on this Mac (ADR 0007, issue #112; "Separation model" in docs/building.md).
#
# Builds the converter image (Linux x86_64: PyTorch has no current Intel-macOS build) and runs
# install_model.py in it, which downloads unblend's pinned checkpoint, checks its SHA-256, exports
# it to fp32 ONNX and installs it. Re-running with a valid model installed does nothing.
#
# OVERDUB_MODELS_DIR and OVERDUB_MODEL_CACHE_DIR override the two folders (both outside the repo).
#
# Provenance: BS-RoFormer SW's original trainer is unidentified. jarredou published a repackaged
# checkpoint (BS-ROFO-SW-Fixed); that account is deleted and the checkpoint survives only at
# https://huggingface.co/enerjazzer/BS-ROFO-SW-Fixed ("License: unknown"). unblend's mirror, which
# this script downloads, records it as "unlicensed". With no licence grant, the weights are never
# bundled in Overdub.app, committed or attached to releases: each user installs them with this script.
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
models_dir="${OVERDUB_MODELS_DIR:-"$HOME/Library/Application Support/Overdub/models"}"
cache_dir="${OVERDUB_MODEL_CACHE_DIR:-"$HOME/Library/Caches/Overdub/separation-model"}"
image="overdub-separation-model:1"

fail ()
{
    echo "[setup] FAILED: $1" >&2
    echo "No model was installed." >&2
    exit 1
}

command -v docker > /dev/null || fail "docker is not installed. Install Docker Desktop (https://www.docker.com/products/docker-desktop/) and run this again."
docker info > /dev/null 2>&1 || fail "Docker is not running. Start Docker Desktop and run this again."

mkdir -p "$models_dir" "$cache_dir"

echo "[setup] building the converter image $image (the first run downloads about 1 GB)"
docker build --platform linux/amd64 --tag "$image" "$script_dir" || fail "building the converter image failed"

echo "[setup] models folder: $models_dir"
echo "[setup] checkpoint cache: $cache_dir"
docker run --rm --platform linux/amd64 \
    --volume "$models_dir:/models" \
    --volume "$cache_dir:/cache" \
    "$image" --models-dir /models --cache-dir /cache
