# Throwaway runtime spike: export BS-RoFormer SW to ONNX with unblend and run the Python
# reference of the chunked ONNX pipeline the C++ helper must match. Not shipped.
FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg build-essential && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir torch==2.14.1 torchaudio --index-url https://download.pytorch.org/whl/cpu
# onnxruntime pinned to the last release with Intel-macOS binaries, the one the C++ helper uses.
RUN pip install --no-cache-dir "unblend[onnx]==1.0.0" onnxruntime==1.23.2 soundfile

WORKDIR /spike
COPY export.py reference.py ./
