# Throwaway quality spike (Separation, branch spike/stem-separation). Not shipped.
# Linux x86_64 because PyTorch has no Intel-macOS wheels after 2.2 and audio-separator needs >= 2.3.
FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg time && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir torch==2.8.0 --index-url https://download.pytorch.org/whl/cpu
RUN apt-get update && apt-get install -y --no-install-recommends build-essential && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir "audio-separator[cpu]==0.47.0"
# Imported by audio-separator but not declared as a dependency.
RUN pip install --no-cache-dir audioread

WORKDIR /spike
COPY separate.sh analyse.py ./
ENTRYPOINT ["bash", "/spike/separate.sh"]
