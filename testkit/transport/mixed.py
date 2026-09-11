"""Compare Windows-to-Ubuntu transport sessions using existing project tools and credentials."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex

from testkit.resources import Remote
from testkit.support import ROOT, atomic_json, temporary_directory
from testkit.transport.run import trial, push_cases, shape_arguments, source_hashes, source_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "build/testkit/services-remote.json")
    parser.add_argument("--output", type=Path, default=ROOT / "build/transport/mixed.json")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--suite", choices=("all", "standard", "cardinality", "catalog"), default="all")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 20 or not 0.2 <= args.seconds <= 20:
        parser.error("invalid rounds or seconds")
    remote = Remote(**json.loads(args.config.read_text(encoding="utf-8")), sudo=False)
    binary = ROOT / "build/transport/target/release/verdandi-transport-probe.exe"
    report = {
        "schema": "verdandi.transport.v3",
        "status": "running",
        "direction": "Windows client -> Ubuntu server",
        "rounds": args.rounds,
        "seconds": args.seconds,
        "workload": "shared_source_push",
        "notes": [
            "Public fixture TLS and signed admissions; no SDK or live Supervisor state machine.",
            "Remote server is sampled locally on Ubuntu, CPU includes setup/warmup.",
        ],
        "samples": [],
        "failures": [],
        "source_sha256": source_hashes(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    }
    try:
        with temporary_directory("transport-mixed-") as directory:
            for index in range(args.rounds):
                for case, push in push_cases(args.suite):
                    role, size, fanout, _, rate = case
                    for transport in (("tcp", "grpc") if index % 2 == 0 else ("grpc", "tcp")):
                        command = (
                            "cd "
                            + shlex.quote(remote.project)
                            + " && exec "
                            + shlex.join(
                                [
                                    remote.project + "/build/tools/python-build/bin/python",
                                    "-B",
                                    "-m",
                                    "testkit.transport.remote",
                                    f"--transport={transport}",
                                    f"--address={remote.host}:0",
                                    f"--fanout={fanout}",
                                    f"--rate={rate}",
                                    f"--burst={str(push['burst']).lower()}",
                                    f"--push-seconds={args.seconds}",
                                    f"--data-mode={push.get('mode', 'mixed')}",
                                    *shape_arguments(push, size),
                                ]
                            )
                        )
                        stdin, stdout, stderr = remote._client.exec_command(command, timeout=40)
                        try:
                            ready = json.loads(stdout.readline(4096))
                            if ready.get("source_digest") != source_digest():
                                raise RuntimeError("cross-host source fingerprint mismatch")
                            report["remote_binary_sha256"] = ready["binary_sha256"]
                            sample = None
                            try:
                                sample = trial(binary, transport, case, args.seconds, Path(directory), address=ready["address"], push=push)
                            except (RuntimeError, TimeoutError) as error:
                                report["failures"].append({"round": index + 1, "transport": transport, "case": case, "push": push, "error": str(error)})
                            stdin.write("stop\n")
                            stdin.flush()
                            stdin.channel.shutdown_write()
                            cleanup = json.loads(stdout.readline(4096))
                            status = stdout.channel.recv_exit_status()
                            if status or cleanup.get("cleanup") != "passed":
                                raise RuntimeError("remote cleanup failed")
                            if sample is None:
                                report["failures"][-1].update(cleanup)
                                atomic_json(args.output, report)
                                print(json.dumps(report["failures"][-1]), flush=True)
                                continue
                            sample.update(cleanup, round=index + 1)
                            report["samples"].append(sample)
                            atomic_json(args.output, report)
                            print(
                                json.dumps({k: sample[k] for k in ("round", "transport", "role", "bytes", "fanout", "messages_per_second", "p99_ms")}),
                                flush=True,
                            )
                        finally:
                            # SSH EOF 也是远端拥有者的停止信号. 45 秒服务期限作为断网后的最终边界.
                            stdin.close()
                            stdout.channel.close()
        report["status"] = "passed" if not report["failures"] else "completed_with_failures"
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        remote.close()
        atomic_json(args.output, report)


if __name__ == "__main__":
    main()
