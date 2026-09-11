"""Own one remote probe server until an explicit stop, SSH EOF, or a 40-second limit."""

import argparse
import hashlib
import json
from pathlib import Path
import select
import sys
import time

from testkit.support import ROOT, available_memory, temporary_directory
from testkit.transport.run import owned, process_sample, ready, shape_arguments, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transport", choices=("tcp", "grpc"), required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--fanout", type=int, default=4)
    parser.add_argument("--rate", type=int, default=2000)
    parser.add_argument("--burst", choices=("true", "false"), default="false")
    parser.add_argument("--push-seconds", type=float, default=3)
    parser.add_argument("--registries", type=int, default=1000)
    parser.add_argument("--catalogs", type=int, default=1000)
    parser.add_argument("--catalog-bytes", type=int, default=256)
    parser.add_argument("--data-mode", choices=("mixed", "registry", "catalog"), default="mixed")
    args = parser.parse_args()
    binary = ROOT / "build/transport/target/release/verdandi-transport-probe"
    with temporary_directory("transport-remote-") as directory:
        log = Path(directory) / "server.log"
        command = [
            binary,
            "serve",
            f"--transport={args.transport}",
            f"--address={args.address}",
            "--seconds=45",
            "--scenario=push",
            f"--fanout={args.fanout}",
            f"--rate={args.rate}",
            f"--burst={args.burst}",
            f"--push-seconds={args.push_seconds}",
            f"--data-mode={args.data_mode}",
            *shape_arguments(vars(args), args.catalog_bytes),
        ]
        with owned(command, log) as server:
            address = ready(server, log)
            print(
                json.dumps(
                    {
                        "ready": True,
                        "address": address,
                        "pid": server.pid,
                        "source_digest": source_digest(),
                        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                    }
                ),
                flush=True,
            )
            peak, cpu = 0, 0.0
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                if sample := process_sample(server):
                    peak = max(peak, sample["rss_bytes"])
                    cpu = max(cpu, sample["cpu_seconds"])
                if available_memory() < 256 * 1024 * 1024:
                    raise RuntimeError("remote memory guard")
                if server.poll() is not None:
                    raise RuntimeError("remote server exited early")
                if select.select([sys.stdin], [], [], 0.02)[0]:
                    command = sys.stdin.readline(32)
                    if command not in ("", "stop\n"):
                        raise RuntimeError("invalid remote lifecycle command")
                    break
            else:
                raise TimeoutError("remote owner deadline")
        print(json.dumps({"cleanup": "passed", "server_peak_rss_bytes": peak, "server_process_cpu_seconds": cpu}), flush=True)


if __name__ == "__main__":
    main()
