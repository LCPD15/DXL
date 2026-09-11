"""Build one YOLO11n-seg engine for the external TensorRT Lean runtime."""
import argparse
import hashlib
import json
import pathlib
import os
import time

import tensorrt_bindings as trt


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--onnx", required=True, type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    source = args.onnx.resolve().parent
    out = (args.out or pathlib.Path(os.environ.get("DXL_WORKSPACE", root.parent / "DXL-Workspace")) / "dependencies/models-lean").resolve()
    if out == root or root in out.parents:
        raise SystemExit("Model output must be outside source.")
    out.mkdir(parents=True, exist_ok=True)
    if (out / "yolo11n-seg.plan").exists():
        raise SystemExit("Choose an empty output directory; an existing plan will not be overwritten.")
    logger = trt.Logger(trt.Logger.INFO)
    report = {"tensorrt_version": trt.__version__, "workspace_bytes": 2 << 30,
              "flags_added": ["VERSION_COMPATIBLE", "EXCLUDE_LEAN_RUNTIME"],
              "precision": "Unmodified ONNX strong types; original builder defaults",
              "hardware_compatibility": "NONE (not a cross-GPU portable engine)",
              "models": []}
    models = [(args.onnx.name, "yolo11n-seg.plan")]
    for onnx_name, plan_name in models:
        builder = trt.Builder(logger)
        network = builder.create_network()
        onnx_parser = trt.OnnxParser(network, logger)
        # The native parser's Windows file API rejects some Unicode paths.
        # The exported ONNX embeds its weights, so parse exact bytes.
        if not onnx_parser.parse((source / onnx_name).read_bytes()):
            raise RuntimeError([str(onnx_parser.get_error(i)) for i in range(onnx_parser.num_errors)])
        plugins = onnx_parser.get_used_vc_plugin_libraries()
        if plugins:
            raise RuntimeError(f"External plugins need explicit packaging: {plugins}")
        config = builder.create_builder_config()
        config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 2 << 30)
        config.set_flag(trt.BuilderFlag.VERSION_COMPATIBLE)
        config.set_flag(trt.BuilderFlag.EXCLUDE_LEAN_RUNTIME)
        entry = {"onnx": onnx_name, "onnx_sha256": sha(source / onnx_name),
                 "plan": plan_name,
                 "vc_plugin_libraries": plugins,
                 "inputs": [{"name": network.get_input(i).name,
                             "shape": list(network.get_input(i).shape),
                             "dtype": str(network.get_input(i).dtype)} for i in range(network.num_inputs)],
                 "outputs": [{"name": network.get_output(i).name,
                              "shape": list(network.get_output(i).shape),
                              "dtype": str(network.get_output(i).dtype)} for i in range(network.num_outputs)]}
        print(f"BUILD START {plan_name} plugins={plugins}", flush=True)
        start = time.perf_counter()
        serialized = builder.build_serialized_network(network, config)
        if serialized is None:
            raise RuntimeError(f"Failed to build {plan_name}")
        data = bytes(serialized)
        (out / plan_name).write_bytes(data)
        entry.update(build_seconds=time.perf_counter() - start,
                     bytes=len(data), sha256=sha(out / plan_name))
        report["models"].append(entry)
        (out / "build-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"BUILD DONE {plan_name}: {entry}", flush=True)
        del serialized, config, onnx_parser, network, builder


if __name__ == "__main__":
    main()
