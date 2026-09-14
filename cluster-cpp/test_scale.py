"""Measure bounded native connection scaling using the existing owned-process harness."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.services import Host, wait_for
from testkit.support import atomic_json, available_memory
from test_processes import binary_digest, endpoint


def physical_connections(host):
    """从所持有进程的 FD 找到实际 TCP, 将同一 socket 两端去重, 不用 Channel 数代替连接数."""
    owned = set()
    for process in host.processes.values():
        for descriptor in Path(f"/proc/{process.process.pid}/fd").iterdir():
            try:
                target = descriptor.readlink().as_posix()
            except FileNotFoundError:
                continue
            if target.startswith("socket:["):
                owned.add(target[8:-1])
    connections = set()
    for family in ("tcp", "tcp6"):
        for line in Path("/proc/net/" + family).read_text().splitlines()[1:]:
            fields = line.split()
            if fields[3] == "01" and fields[9] in owned:
                connections.add(tuple(sorted(fields[1:3])))
    return len(connections)


def sample(host):
    result = host.call("resources")
    for name, values in result.items():
        fields = Path(f"/proc/{host.processes[name].process.pid}/stat").read_text().rpartition(")")[2].split()
        values["cpu_seconds"] = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    return result


def trial(binaries, stars, planets, allocations):
    """先组网并采样稳态, 再做一次上游强制退出/重启, 最后逐个正常停止并验证端口与目录释放."""
    host = Host(binaries)
    ports = []
    row = {"stars": stars, "planets": planets, "allocation_instrumentation": allocations}
    begun = {}
    joined = {}
    minimum = available_memory()
    peaks = {}

    def reserve():
        address = endpoint(host)
        ports.append(int(address.rsplit(":", 1)[1]))
        return address

    def observe(predicate, timeout=60):
        nonlocal minimum
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            minimum = min(minimum, available_memory())
            if minimum < 384 * 1024 * 1024:
                raise RuntimeError("Available memory below 384 MiB; scale expansion stopped")
            states = {name: process.snapshot() for name, process in host.processes.items() if name != "supervisor"}
            for name, value in states.items():
                if not value["alive"]:
                    raise RuntimeError(f"Node exited before scale cleanup: {name}: {value['tail']}")
                if value["status"].get("initialized") and name not in joined:
                    joined[name] = time.monotonic() - begun[name]
            for name, values in sample(host).items():
                for key in ("rss_kib", "threads", "fds"):
                    peaks.setdefault(name, {})[key] = max(peaks.get(name, {}).get(key, 0), values[key])
            if predicate({name: value["status"] for name, value in states.items()}):
                return
            time.sleep(0.1)
        raise TimeoutError("Scale convergence timed out: " + json.dumps(states))

    def mesh(states, count, active):
        return (
            all(
                value.get("initialized") and value.get("members") == count and value.get("inbound") == active - 1 and value.get("outbound") == active - 1
                for name, value in states.items()
                if name.startswith("star-")
            )
            and all(value.get("upstream") for name, value in states.items() if name.startswith("planet-"))
            and sum(value.get("planet_inbound", 0) for name, value in states.items() if name.startswith("star-")) == planets
        )

    try:
        super_address, management = reserve(), reserve()
        host.call("start", name="supervisor", kind="supervisor", address=super_address, management=management, max_members=64, max_connections=128)
        wait_for(host, "supervisor", lambda value: any("supervisor_registration_started" in line for line in value["tail"]))
        addresses = {f"star-{index}": reserve() for index in range(stars)}
        addresses.update({f"planet-{index}": reserve() for index in range(planets)})

        def start(name):
            if available_memory() < 384 * 1024 * 1024:
                raise RuntimeError("Available memory below 384 MiB before node start")
            begun[name] = time.monotonic()
            host.call(
                "start",
                name=name,
                kind="planet" if name.startswith("planet") else "star",
                role="planet-a" if name.startswith("planet") else "star-a",
                address=addresses[name],
                supervisor=super_address,
                max_members=64,
            )

        started = time.monotonic()
        for name in addresses:
            start(name)
        observe(lambda states: mesh(states, stars, stars))
        row["observed_full_topology_seconds"] = time.monotonic() - started
        row["observed_admission_seconds_by_node"] = dict(joined)
        row["logical_star_streams"] = stars * (stars - 1)
        row["logical_planet_streams"] = planets
        row["observed_tcp_established_including_supervisor"] = physical_connections(host)
        row["resources_idle"] = sample(host)
        started = time.monotonic()
        observe(lambda _: time.monotonic() - started >= 3)
        row["steady_sample_seconds"] = time.monotonic() - started
        steady = sample(host)
        row["steady_cpu_seconds"] = {name: values["cpu_seconds"] - row["resources_idle"][name]["cpu_seconds"] for name, values in steady.items()}

        # 选择真实 Planet 上游做故障对象. 没有 Planet 的阶梯仍验证 Star 退出后的剩余 mesh.
        failed = "star-0"
        if planets:
            upstream = host.call("snapshot", name="planet-0")["status"]["upstream"]["id"]
            failed = next(name for name in addresses if name.startswith("star") and host.call("snapshot", name=name)["status"]["id"] == upstream)
        old_id = host.call("snapshot", name=failed)["status"]["id"]
        started = time.monotonic()
        host.call("stop", name=failed, graceful=False)
        observe(
            lambda states: mesh(states, stars, stars - 1)
            and all((value.get("upstream") or {}).get("id") != old_id for name, value in states.items() if name.startswith("planet"))
        )
        row["observed_failure_recovery_seconds"] = time.monotonic() - started
        started = time.monotonic()
        start(failed)
        observe(lambda states: mesh(states, stars, stars) and states[failed].get("id") != old_id)
        row["observed_restart_convergence_seconds"] = time.monotonic() - started
        row["resources_after_recovery"] = sample(host)
        row["observed_tcp_after_recovery"] = physical_connections(host)
        row["sampled_resource_peaks"] = peaks
        row["minimum_available_memory_bytes"] = minimum
        row["shutdown_seconds_by_node"] = {}
        row["allocations_at_exit"] = {}
        for name in list(host.processes):
            process = host.processes[name]
            started = time.monotonic()
            host.call("stop", name=name)
            row["shutdown_seconds_by_node"][name] = time.monotonic() - started
            if allocations and name != "supervisor":
                records = [json.loads(line) for line in process.tail if '"event":"allocation_measure"' in line]
                if len(records) != 1:
                    raise RuntimeError("Missing C++ allocation report: " + name)
                row["allocations_at_exit"][name] = records[0]
        for port in ports:
            host.call("rebind", address="127.0.0.1", port=port)
        row["status"] = "pass"
    finally:
        host.close()
    if host.directory.exists():
        raise RuntimeError("Owned scale directory survived cleanup")
    row["cleanup"] = "processes joined, ports reusable, owned temporary directory removed"
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--allocations", action="store_true")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Native Linux /proc measurements are required")
    report = {
        "status": "running",
        "cases": [],
        "binary_sha256": {name: binary_digest(args.binaries / name) for name in ("star", "planet")},
        "limits": [
            "Observed timings include 1-second status publication and 100-ms polling",
            "Allocation mode is not used for timing comparisons",
            "C++ allocation counters include static libraries' new; exclude direct malloc, placement new and post-report exit destructors",
            "Loopback topology uses shared public certificates; credentials still come from real Go Supervisor login",
            "Resource peaks are samples, not hard process limits; heartbeat 200 ms and Pong deadline 1500 ms match existing fault fixtures",
        ],
    }
    destination = ROOT / "build/testkit/results" / f"cluster-cpp-scale-{time.time_ns()}.json"
    try:
        for stars, planets in ((2, 0), (4, 1), (8, 8), (16, 32)):
            report["cases"].append(trial(args.binaries, stars, planets, args.allocations))
            atomic_json(destination, report)
            print(f"Scale passed: {stars} Stars / {planets} Planets", flush=True)
        report["status"] = "pass"
    except BaseException as error:
        report["status"], report["error"] = "fail", str(error)
        raise
    finally:
        atomic_json(destination, report)
        print("Report: " + str(destination), flush=True)


if __name__ == "__main__":
    main()
