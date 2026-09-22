"""使用项目内已批准工具显式生成 Go 控制面协议, 不构建、测试或下载依赖."""

from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MODULE = "github.com/eosforge/verdandi/astra"
NAMES = ("astra", "orbit", "comet", "pulsar", "polaris")


def main():
    """每个协议使用独立 Go 包, 不把新业务消息混入冻结 Supervisor 的旧 wire 包."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Compare generated sources without changing tracked files",
    )
    options = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    tools = {
        "protoc": ROOT / f"build/tools/protoc/36.1/bin/protoc{suffix}",
        "go": ROOT / f"build/tools/protoc-gen-go/1.36.12/protoc-gen-go{suffix}",
        "go-grpc": ROOT
        / f"build/tools/protoc-gen-go-grpc/1.6.2/protoc-gen-go-grpc{suffix}",
    }
    if any(not path.is_file() for path in tools.values()):
        raise RuntimeError(
            "Missing approved project protocol tools; no automatic download"
        )
    if (
        subprocess.check_output([tools["protoc"], "--version"], text=True).strip()
        != "libprotoc 36.1"
    ):
        raise RuntimeError("Expected protoc 36.1")
    temporary = ROOT / "build/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="astra-go-proto-", dir=temporary) as target:
        command = [str(tools["protoc"]), "--proto_path=" + str(ROOT / "proto")]
        for plugin in ("go", "go-grpc"):
            command.extend(
                [
                    f"--plugin=protoc-gen-{plugin}={tools[plugin]}",
                    f"--{plugin}_out={target}",
                    f"--{plugin}_opt=module={MODULE}",
                    *[
                        f"--{plugin}_opt=M{name}.proto={MODULE}/internal/generated/{name};{name}"
                        for name in NAMES
                    ],
                ]
            )
        command.extend(name + ".proto" for name in NAMES)
        subprocess.run(command, check=True, cwd=ROOT)
        files = {
            source.relative_to(target): source for source in Path(target).rglob("*.go")
        }
        expected = {
            Path("internal/generated") / name / filename
            for name in NAMES
            for filename in (f"{name}.pb.go", f"{name}_grpc.pb.go")
        }
        if set(files) != expected:
            raise RuntimeError(
                "Protocol generators did not produce the complete expected Go source set"
            )
        if options.check:
            for relative, source in files.items():
                destination = ROOT / "astra" / relative
                if (
                    not destination.is_file()
                    or destination.read_bytes() != source.read_bytes()
                ):
                    raise RuntimeError(f"Generated Go source differs: {relative}")
            actual = {
                source.relative_to(ROOT / "astra")
                for source in (ROOT / "astra/internal/generated").rglob("*.go")
            }
            if actual != expected:
                raise RuntimeError(
                    "Unexpected stale Go files in generated protocol packages"
                )
            print("Go generated protocol sources match; no tracked files changed")
            return
        for source in sorted(Path(target).rglob("*.go")):
            destination = ROOT / "astra" / source.relative_to(target)
            destination.parent.mkdir(parents=True, exist_ok=True)
            data = source.read_bytes()
            if not destination.is_file() or destination.read_bytes() != data:
                destination.write_bytes(data)
    print("Generated Go control-plane protocol sources; no build or tests executed")


if __name__ == "__main__":
    main()
