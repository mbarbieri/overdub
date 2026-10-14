# Converter image for the Separation model (ADR 0007, issue #112): exports unblend's bs_roformer_sw
# checkpoint to fp32 ONNX. Linux x86_64 (install.sh builds with --platform linux/amd64) because
# PyTorch has no current Intel-macOS build.
# Repeatable export: the base image is pinned by digest (python:3.12.12-slim), and requirements.txt
# pins every package, installed with --no-deps so a package missing from it fails `pip check`.
FROM python:3.12.12-slim@sha256:f3fa41d74a768c2fce8016b98c191ae8c1bacd8f1152870a3f9f87d350920b7c

COPY requirements.txt /converter/requirements.txt
RUN pip install --no-cache-dir --no-deps --extra-index-url https://download.pytorch.org/whl/cpu -r /converter/requirements.txt \
    && pip check

COPY install_model.py /converter/install_model.py
ENTRYPOINT ["python", "/converter/install_model.py"]
