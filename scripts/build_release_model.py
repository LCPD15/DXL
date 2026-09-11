"""Build the 0.1 model using its original TensorRT settings (no VC flags)."""
import argparse
from pathlib import Path
import tensorrt as trt


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument('--onnx', type=Path, required=True)
    cli.add_argument('--out', type=Path, required=True)
    args = cli.parse_args()
    output = args.out.resolve()
    source_root = Path(__file__).resolve().parents[1]
    if output == source_root or source_root in output.parents:
        raise SystemExit('Model output must be outside the source checkout.')
    if output.exists():
        raise SystemExit('Choose a new output file.')
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    network = builder.create_network()
    parser = trt.OnnxParser(network, logger)
    if not parser.parse(args.onnx.read_bytes()):
        raise RuntimeError([str(parser.get_error(i)) for i in range(parser.num_errors)])
    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 2 << 30)
    serialized = builder.build_serialized_network(network, config)
    if serialized is None:
        raise RuntimeError('TensorRT model build failed.')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(bytes(serialized))
    print(f'Built {output.name} with TensorRT {trt.__version__}; validate on target GPU.')


if __name__ == '__main__':
    main()
