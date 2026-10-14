"""Exports bs_roformer_sw to /models/bs_roformer_sw.<precision>.onnx and prints its metadata."""

import sys

import onnx
from unblend.onnx import export_to_onnx

for precision in sys.argv[1:] or ["fp32", "fp16"]:
    path = f"/models/bs_roformer_sw.{precision}.onnx"
    export_to_onnx("bs_roformer_sw", path, precision=precision, static_batch=True)
    model = onnx.load(path, load_external_data=False)
    print(f"== {path}")
    for prop in model.metadata_props:
        print(f"{prop.key} = {prop.value}")
    for tensor in list(model.graph.input) + list(model.graph.output):
        dims = [d.dim_value or d.dim_param for d in tensor.type.tensor_type.shape.dim]
        print(f"{tensor.name} {dims}")
