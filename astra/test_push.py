"""Compare isolated C++/Rust push dispatch with one shared state-validating Rust receiver."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.support import atomic_json, binary_digest, environment, temporary_directory
from testkit.transport.run import owned, ready, shape_arguments, source_hashes, trial


def cases(smoke):
    """分层选取负载而不展开全部笛卡尔积; 每个失败样本也保留, 不仅统计成功且快的消息."""

    def case(name, **changes):
        return (
            dict(
                name=name,
                role="planet",
                size=256,
                fanout=4,
                rate=2000,
                mode="mixed",
                registries=1000,
                burst=False,
                pause_ms=0,
                hot_keys=0,
                capacity_probe=False,
            )
            | changes
        )

    base = [
        case("mixed"),
        case("hot", registries=10000, hot_keys=16),
        case("slow", pause_ms=200),
    ]
    if smoke:
        return [*base, case("buffered-burst", burst=True)]
    return [
        *base,
        case("fanout1", fanout=1),
        case("fanout16", fanout=16),
        case("burst", burst=True, rate=8000),
        *[case(f"registry{count}", mode="registry", registries=count, rate=8000) for count in (100, 1000, 10000)],
        *[case(f"catalog{size}", mode="catalog", size=size, rate=8000) for size in (64, 256, 1024)],
        *[case(f"capacity{rate}", mode="catalog", rate=rate, capacity_probe=True) for rate in (20000, 60000)],
        case("paused-over-budget", pause_ms=1000, rate=20000, capacity_probe=True),
    ]


def run_case(cpp, rust, variant, case, seconds, allow_capacity=False):
    """复用客户端、采样和进程拥有者; 不让测量失败绕过清理或被成功记录覆盖."""
    push = {key: case[key] for key in ("burst", "pause_ms", "mode", "registries", "hot_keys")}
    load = (case["role"], case["size"], case["fanout"], 64, case["rate"])
    if variant == "rust":
        command = [
            rust,
            "serve",
            "--transport=grpc",
            "--address=127.0.0.1:0",
            "--seconds=60",
            "--scenario=push",
            f"--fanout={case['fanout']}",
            f"--rate={case['rate']}",
            f"--push-seconds={seconds}",
            f"--burst={str(case['burst']).lower()}",
            f"--data-mode={case['mode']}",
            *shape_arguments(push, case["size"]),
        ]
    else:
        command = [
            cpp,
            "--address=127.0.0.1:0",
            f"--milliseconds={round(seconds * 1000)}",
            f"--fanout={case['fanout']}",
            f"--rate={case['rate']}",
            f"--burst={int(case['burst'])}",
            f"--data-mode={case['mode']}",
            f"--fresh={int(variant == 'cpp-fresh')}",
            *shape_arguments(push, case["size"]),
        ]
    row = {
        "variant": variant,
        "case": case,
        "offered_source_updates": int(case["rate"] * seconds),
    }
    with temporary_directory("cpp-push-") as directory:
        directory = Path(directory)
        log = directory / "owned-server.log"
        with owned(command, log) as server:
            address = ready(server, log)
            try:
                row["measurement"] = trial(
                    rust,
                    "grpc",
                    load,
                    seconds,
                    directory,
                    address=address,
                    push=push,
                    server_process=server,
                )
                measured = row["measurement"]
                accepted = measured["publish_replies"] - measured["rejected_publications"]
                if measured["updates_per_recipient"] != row["offered_source_updates"] + accepted:
                    raise RuntimeError("Completed watermark differs from source updates plus accepted publications")
                row["status"] = "pass"
            except (RuntimeError, TimeoutError) as error:
                detail = str(error) + "\n" + log.read_text(encoding="utf-8")[-4000:]
                if not (allow_capacity or case["capacity_probe"]) or not any(marker in detail.lower() for marker in ("lagged", "resynchronization")):
                    raise RuntimeError(f"{variant}/{case['name']}: {detail}") from error
                row.update(
                    status="capacity_rejected",
                    error=detail[-6000:],
                    received_deliveries=None,
                    reason="Incomplete counts remain unknown; this trial contributes a failure, never a successful latency sample",
                )
            finally:
                if variant != "rust" and server.poll() is None:
                    server.terminate()
                    if server.wait(timeout=8) != 0:
                        raise RuntimeError("C++ push fixture did not exit gracefully: " + log.read_text(encoding="utf-8")[-4000:])
            output = log.read_text(encoding="utf-8")
            if any(
                marker in output
                for marker in (
                    "WARNING: ThreadSanitizer:",
                    "FATAL: ThreadSanitizer:",
                    "ERROR: AddressSanitizer:",
                    "ERROR: LeakSanitizer:",
                    "runtime error:",
                )
            ):
                raise RuntimeError("Sanitizer failure in push fixture: " + output[-6000:])
            records = [json.loads(line) for line in output.splitlines() if '"event":"allocation_measure"' in line]
            if records:
                row["allocation_measure"] = records[-1]
        # 调用者只绑定刚刚持有的监听端口; 这里不删除其它测试或服务资源.
        with socket.socket() as check:
            check.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            check.bind(("127.0.0.1", int(address.rsplit(":", 1)[1])))
    row["cleanup"] = "processes joined, listener reusable, owned temporary directory removed"
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-binary", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument(
        "--smoke-rate",
        type=int,
        default=2000,
        help="Explicit correctness load; full TSan uses a lower rate, never performance evidence",
    )
    parser.add_argument("--allocations", action="store_true")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--seconds", type=float, default=2)
    args = parser.parse_args()
    parser.error("Retired v4 Rust comparison; this script is historical and cannot validate the current C++ v5 service.")
    if sys.platform != "linux" or not 1 <= args.rounds <= 10 or not 0.5 <= args.seconds <= 10 or not 100 <= args.smoke_rate <= 2000:
        parser.error("Requires Linux, rounds 1..10 and seconds 0.5..10")
    # 只限制当前测量进程及其子进程的 CPU, 不修改系统或其它工作进程的调度配置.
    affinity = sorted(os.sched_getaffinity(0))[:2]
    os.sched_setaffinity(0, affinity)
    env = environment()
    env["CARGO_TARGET_DIR"] = str(ROOT / "build/transport/target")
    subprocess.run(
        [
            "cargo",
            "build",
            "--manifest-path",
            str(ROOT / "testkit/transport/Cargo.toml"),
            "--release",
            "--frozen",
            "--jobs",
            "1",
        ],
        cwd=ROOT,
        env=env,
        check=True,
        timeout=1800,
    )
    rust = ROOT / "build/transport/target/release/verdandi-transport-probe"
    selected = cases(args.smoke or args.allocations)
    if args.smoke:
        selected = [case | {"rate": args.smoke_rate} for case in selected]
    variants = ["cpp-fresh", "cpp-reuse"] if args.allocations else ["rust", "cpp-fresh", "cpp-reuse"]
    report = {
        "status": "running",
        "mode": ("allocations" if args.allocations else ("smoke" if args.smoke else "comparison")),
        "trials": [],
        "cpu_affinity": affinity,
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "source_hashes": source_hashes()
        | {str(path.relative_to(ROOT)).replace("\\", "/"): binary_digest(path) for path in sorted((ROOT / "astra/bench").rglob("*")) if path.is_file()}
        | {"astra/test_push.py": binary_digest(Path(__file__))},
        "binary_sha256": {
            "cpp": binary_digest(args.cpp_binary),
            "rust": binary_digest(rust),
        },
        "configuration": {
            "tls": "1.3",
            "http2_initial_stream_window": 65535,
            "adaptive_window": False,
            "broadcast_ring": 512,
            "per_connection_output_queue": 16,
            "inflight_write": 1,
            "cpp_write_policy": "buffer at most 16 ready updates; flush last update and every control reply",
            "sparse_progress_every_updates": 128,
        },
        "limits": [
            "Isolated fixture using public signed bearer data; not Catalog/Registry production replication",
            "Shared Rust receiver validates every revision and final per-recipient state hash",
            "Control/producer scheduled latency exposes delayed dispatch; update latency uses sparse round-trip watermarks, not one-way timestamps",
            "Keep failed capacity trials alongside successful quantiles; no latency is fabricated for incomplete streams",
            "C++ fresh/reuse changes only ordinary Protobuf object reuse, not protocol or zero-copy behavior",
            "Allocation and sanitizer results must not be merged into normal Release timing comparisons",
        ],
    }
    output = ROOT / "build/testkit/results" / f"astra-push-{time.time_ns()}.json"
    try:
        if not args.smoke and not args.allocations:
            report["warmup_outcomes"] = []
            for case in selected:
                for variant in variants:
                    warmup = run_case(
                        args.cpp_binary,
                        rust,
                        variant,
                        case | {"pause_ms": 0},
                        0.5,
                        allow_capacity=True,
                    )
                    report["warmup_outcomes"].append(
                        {
                            "case": case["name"],
                            "variant": variant,
                            "status": warmup["status"],
                        }
                    )
            report["warmup"] = "Separate 0.5-second runs per case and variant, excluded from measurements"
        rounds = 1 if args.smoke else args.rounds
        seconds = 0.65 if args.smoke else args.seconds
        for round_number in range(rounds):
            order = variants[round_number % len(variants) :] + variants[: round_number % len(variants)]
            for case in selected:
                for variant in order:
                    row = run_case(
                        args.cpp_binary,
                        rust,
                        variant,
                        case,
                        seconds,
                        allow_capacity=not args.smoke,
                    )
                    row["round"] = round_number + 1
                    report["trials"].append(row)
                    atomic_json(output, report)
                print(
                    f"Push completed: round {round_number + 1}/{rounds}, {case['name']}",
                    flush=True,
                )
        rejected = sum(row["status"] == "capacity_rejected" for row in report["trials"])
        report["status"] = "completed_with_capacity_rejections" if rejected else "pass"
        report["capacity_rejected_trials"] = rejected
    except BaseException as error:
        report["status"], report["error"] = "fail", str(error)
        raise
    finally:
        atomic_json(output, report)
        print("Report: " + str(output), flush=True)


if __name__ == "__main__":
    main()
