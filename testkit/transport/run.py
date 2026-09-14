"""Run an offline, bounded Star/Planet transport comparison with owned-process cleanup."""

from __future__ import annotations

import argparse
import ctypes
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import time

from testkit.support import ROOT, atomic_json, available_memory, environment, popen, stop_process, temporary_directory


def process_sample(process):
    """Read only the owned process; CPU includes setup/warmup, RSS is sampled every 20 ms."""
    if os.name == "nt":
        from ctypes import wintypes

        class Memory(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ("peak_rss", "rss", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")
            ]

        memory = Memory()
        memory.cb = ctypes.sizeof(memory)
        handle = wintypes.HANDLE(int(process._handle))
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        if not kernel.K32GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        stamps = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(stamp) for stamp in stamps)):
            raise ctypes.WinError(ctypes.get_last_error())
        cpu = sum((stamp.dwHighDateTime << 32) | stamp.dwLowDateTime for stamp in stamps[2:]) / 10000000
        return {"cpu_seconds": cpu, "rss_bytes": memory.rss}
    try:
        fields = Path(f"/proc/{process.pid}/stat").read_text().rpartition(")")[2].split()
        return {"cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"), "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGE_SIZE")}
    except FileNotFoundError:
        return None


@contextmanager
def owned(command, log):
    with log.open("w", encoding="utf-8") as output:
        process = popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        try:
            yield process
        finally:
            stop_process(process)
            if process.poll() is None:
                raise RuntimeError("owned process survived cleanup")


def ready(process, log):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        text = log.read_text(encoding="utf-8")
        if text:
            result = json.loads(text.splitlines()[0])
            if result.get("ready"):
                return result["address"]
        if process.poll() is not None:
            raise RuntimeError("probe server exited: " + text[-4096:])
        time.sleep(0.02)
    raise TimeoutError("probe readiness timeout")


def trial(binary, transport, case, seconds, directory, address=None, push=None, server_process=None):
    role, size, fanout, window, rate = case
    command = [
        binary,
        "run",
        f"--transport={transport}",
        f"--role={role}",
        f"--bytes={size}",
        f"--fanout={fanout}",
        f"--window={window}",
        f"--rate={rate}",
        f"--seconds={seconds}",
    ]
    if push is not None:
        command = [binary, "push", f"--transport={transport}", f"--role={role}", f"--fanout={fanout}", f"--seconds={seconds}", f"--pause-ms={push['pause_ms']}"]
        command += shape_arguments(push, size)
    client_log, server_log = directory / "client.log", directory / "server.log"
    from contextlib import ExitStack

    owns_server = address is None
    with ExitStack() as stack:
        server = server_process
        if address is None:
            server_command = [binary, "serve", f"--transport={transport}", "--address=127.0.0.1:0", "--seconds=60"]
            if push is not None:
                server_command += [
                    "--scenario=push",
                    f"--fanout={fanout}",
                    f"--rate={rate}",
                    f"--push-seconds={seconds}",
                    f"--burst={str(push['burst']).lower()}",
                    f"--data-mode={push.get('mode', 'mixed')}",
                    *shape_arguments(push, size),
                ]
            server = stack.enter_context(owned(server_command, server_log))
            address = ready(server, server_log)
        before = process_sample(server) if server else None
        started = time.monotonic()
        client = stack.enter_context(owned([*command, f"--address={address}"], client_log))
        peaks = {"client": 0, "server": 0}
        cpu = {"client": 0.0, "server": 0.0}
        minimum = available_memory()
        while True:
            for name, process in (("client", client), ("server", server)):
                if process is not None and (sample := process_sample(process)):
                    peaks[name] = max(peaks[name], sample["rss_bytes"])
                    cpu[name] = max(cpu[name], sample["cpu_seconds"])
            minimum = min(minimum, available_memory())
            if minimum < 256 * 1024 * 1024:
                raise RuntimeError("available memory below 256 MiB guard")
            if client.poll() is not None:
                break
            if time.monotonic() - started > seconds + 35:
                raise TimeoutError("bounded measurement timeout")
            time.sleep(0.02)
        text = client_log.read_text(encoding="utf-8")
        if client.returncode:
            raise RuntimeError("probe client failed: " + text[-4096:])
        report = json.loads(text)
        report.update(
            role=role,
            process_cpu_seconds={"client": cpu["client"], "server": cpu["server"] - before["cpu_seconds"] if before else None},
            sampled_peak_rss_bytes=peaks,
            minimum_available_memory_bytes=minimum,
            resource_interval_seconds=time.monotonic() - started,
        )
        if push is not None:
            report.update(bytes=size, source_rate=rate, burst=push["burst"], pause_ms=push["pause_ms"], data_mode=push.get("mode", "mixed"))
            report["offered_deliveries_per_second"] = rate * fanout
            report["delivery_rate_ratio"] = report["messages_per_second"] / (rate * fanout)
    # 本地服务器由当前上下文独占. 退出后检查监听释放, 不清理其他进程或端口.
    if server and owns_server:
        host, port = address.rsplit(":", 1)
        cleanup_deadline = time.monotonic() + 3
        while True:
            with socket.socket() as check:
                check.settimeout(0.2)
                if check.connect_ex((host, int(port))) != 0:
                    break
            if time.monotonic() >= cleanup_deadline:
                raise RuntimeError("probe listener survived cleanup")
            time.sleep(0.02)
    report["cleanup"] = "passed"
    return report


def shape_arguments(push, size):
    return [
        f"--registries={push.get('registries', 1000)}",
        f"--catalogs={push.get('catalogs', 1000)}",
        f"--catalog-bytes={size}",
        f"--hot-keys={push.get('hot_keys', 0)}",
    ]


def source_hashes():
    paths = sorted(p for p in (ROOT / "testkit/transport").rglob("*") if p.is_file() and p.suffix in (".rs", ".toml", ".lock", ".proto", ".py"))
    paths += sorted((ROOT / "peer/common/src").rglob("*.rs"))
    paths += [ROOT / "peer/Cargo.toml", ROOT / "peer/common/Cargo.toml", ROOT / "testkit/support.py", ROOT / "testkit/resources.py"]
    return {str(p.relative_to(ROOT)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


def source_digest():
    return hashlib.sha256(json.dumps(source_hashes(), sort_keys=True).encode()).hexdigest()


def push_cases(suite="all", rates=(2000, 8000, 20000, 60000)):
    cases = []
    if suite in ("all", "standard"):
        cases += [
            ((role, 256, fanout, 64, rate), {"burst": burst, "pause_ms": pause, "mode": "mixed"})
            for role, fanout, rate, burst, pause in (
                ("star", 1, 2000, False, 0),
                ("star", 4, 2000, False, 0),
                ("star", 16, 2000, False, 0),
                ("planet", 4, 2000, False, 0),
                ("star", 4, 8000, True, 0),
                ("planet", 4, 2000, False, 200),
            )
        ]
    if suite in ("all", "cardinality"):
        cases += [
            (("star", 256, 4, 64, 8000), {"burst": False, "pause_ms": 0, "mode": "registry", "registries": count}) for count in (100, 1000, 10000, 100000)
        ]
    if suite in ("all", "catalog"):
        # 按发布速率逐级增加压力, 每一级覆盖三种小消息大小. 不把低负载达标当作最大吞吐.
        cases += [(("planet", size, 4, 64, rate), {"burst": False, "pause_ms": 0, "mode": "catalog"}) for rate in rates for size in (64, 256, 1024)]
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--workload", choices=("push", "echo"), default="push")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--suite", choices=("all", "standard", "cardinality", "catalog"), default="all")
    parser.add_argument("--rates", default="2000,8000,20000,60000")
    parser.add_argument("--output", type=Path, default=ROOT / "build/transport/benchmark.json")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 20 or not 0.1 <= args.seconds <= 30:
        parser.error("rounds must be 1..20 and seconds 0.1..30")
    rates = tuple(int(rate) for rate in args.rates.split(","))
    if not rates or any(not 1 <= rate <= 200000 for rate in rates) or list(rates) != sorted(set(rates)):
        parser.error("rates must increase within 1..200000")
    binary = ROOT / "build/transport/target/release" / ("verdandi-transport-probe.exe" if os.name == "nt" else "verdandi-transport-probe")
    env = environment()
    env["CARGO_TARGET_DIR"] = str(ROOT / "build/transport/target")
    if args.build:
        subprocess.run(
            ["cargo", "build", "--manifest-path", "testkit/transport/Cargo.toml", "--release", "--frozen", "--jobs", "1"],
            cwd=ROOT,
            env=env,
            check=True,
            timeout=1800,
        )
    report = {
        "schema": "verdandi.transport.v3",
        "status": "running",
        "platform": platform.platform(),
        "logical_cpus": os.cpu_count(),
        "initial_available_memory_bytes": available_memory(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256": source_hashes(),
        "rounds": args.rounds,
        "workload": args.workload,
        "warmup_seconds": 0.25 if args.workload == "echo" else 0,
        "duration_seconds": args.seconds,
        "samples": [],
        "failures": [],
        "limits": {
            "application_queue_messages": 16,
            "message_bytes": 32768,
            "payload_bytes": 16384,
            "runtime_workers_per_process": 2,
            "grpc_stream_and_connection_window_bytes": 65535,
            "grpc_adaptive_window": False,
            "tcp_flush_policy": "ready_batch_up_to_16" if args.workload == "push" else "each_echo_frame",
        },
        "notes": [
            "Synthetic bidirectional echo; fanout is simultaneous sessions, not a implemented broadcast store.",
            "CPU includes connection setup and warmup; RSS is sampled; allocator counts and wire bytes are not measured.",
            "TLS/Hello uses public fixture admissions, not live Supervisor CAS/candidate selection.",
            "Window is in-flight messages, not a forced equal TLS/HTTP2 batching policy.",
        ],
    }
    cases = [("star", size, 1, 64, 0) for size in (256, 1024, 16384)]
    if not args.quick:
        cases += [("star", 1024, n, 64, 0) for n in (4, 16, 64)]
        cases += [("star", 1024, 1, 1, 0), ("star", 1024, 4, 64, 4000), ("planet", 1024, 1, 64, 0), ("planet", 1024, 4, 64, 0), ("planet", 1024, 4, 64, 4000)]
    cases = [(case, None) for case in cases]
    if args.workload == "push":
        cases = push_cases(args.suite, rates)
        if args.quick:
            cases = cases[:2]
        report["notes"] = [
            "Shared HashMap string keys: 100..100000 independent 128 B registry records and 1000 catalog records, preloaded before Join.",
            "Warm incremental cache; mixed source is 25% registry / 75% catalog with snapshot fragments. Dedicated catalog excludes snapshots.",
            "Each recipient sends 20 control probes/s and 5 small publications/s; control generation stops before completion.",
            "P50/P95/P99 are control enqueue-to-reply RTT, including transport queues and receiver pauses. Scheduled latency is reported separately.",
            "Recipients verify every revision and matching final state hashes; ring overflow explicitly fails the session.",
            "Update latency: sampled cumulative progress every 128 updates, from scheduled publication to application confirmation on server clock.",
            "Update P50/P95/P99 are the worst recipient percentile, not averaged percentiles; progress skips and expired stamps are reported.",
            "CPU/RSS include process setup; allocation counts, wire bytes and real durable replication are not measured.",
        ]
        report["limits"].update(shared_broadcast_ring_messages=512, control_queue_messages=8, latency_timestamp_ring=16384)
    try:
        with temporary_directory("transport-measure-") as directory:
            for round_index in range(args.rounds):
                for case, push in cases:
                    for transport in (("tcp", "grpc") if round_index % 2 == 0 else ("grpc", "tcp")):
                        try:
                            sample = trial(binary, transport, case, args.seconds, Path(directory), push=push)
                        except (RuntimeError, TimeoutError) as error:
                            # 饱和与丢水位是结果, 必须保留失败样本, 不能只统计跑通的传输.
                            failure = {"round": round_index + 1, "transport": transport, "case": case, "push": push, "error": str(error)}
                            failure["logs"] = {
                                name: (Path(directory) / name).read_text(encoding="utf-8")[-4096:]
                                for name in ("client.log", "server.log")
                                if (Path(directory) / name).is_file()
                            }
                            report["failures"].append(failure)
                            atomic_json(args.output, report)
                            print(json.dumps({k: v for k, v in failure.items() if k != "logs"}), flush=True)
                            if "memory" in str(error) or "cleanup" in str(error):
                                raise
                            continue
                        sample["round"] = round_index + 1
                        report["samples"].append(sample)
                        atomic_json(args.output, report)
                        print(
                            json.dumps({k: sample[k] for k in ("round", "transport", "role", "bytes", "fanout", "messages_per_second", "p99_ms")}), flush=True
                        )
        report["status"] = "passed" if not report["failures"] else "completed_with_failures"
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        atomic_json(args.output, report)


if __name__ == "__main__":
    main()
