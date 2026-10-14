# Converter image for the Separation model (ADR 0007, issue #112): exports unblend's bs_roformer_sw
# checkpoint to fp32 ONNX. Linux x86_64 because PyTorch has no current Intel-macOS build.
# requirements.txt pins every package, so the export is repeatable.
FROM --platform=linux/amd64 python:3.12.12-slim

COPY requirements.txt /converter/requirements.txt
RUN pip install --no-cache-dir --extra-index-url https://download.pytorch.org/whl/cpu -r /converter/requirements.txt

COPY install_model.py /converter/install_model.py
ENTRYPOINT ["python", "/converter/install_model.py"]
