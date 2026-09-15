"""扩大真实连接拓扑, 先反复故障恢复, 再持续运行到手动停止. 不构建或下载依赖."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.services import Host
from testkit.support import FileLock, atomic_json, available_memory
from test_processes import binary_digest, endpoint
from test_scale import physical_connections, sample


class RequestedStop(Exception):
    """手动停止与测试失败分别记录, 两者都走相同的资源回收路径."""


class Journal:
    """滚动保留详细采样, 总计最多 8 个 8 MiB 文件, 汇总报告另行原子更新."""

    def __init__(self, directory, limit=8 * 1024 * 1024, slots=8):
        self.directory, self.limit, self.slots = directory, limit, slots
        self.index = 0
        self.path = directory / "samples-0.jsonl"

    def write(self, value):
        if self.path.exists() and self.path.stat().st_size >= self.limit:
            self.index += 1
            self.path = self.directory / f"samples-{self.index % self.slots}.jsonl"
            self.path.write_text("", encoding="utf-8")
        with self.path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"journal_sequence": self.index, **value}, ensure_ascii=False) + "\n")


def topology_ready(states, stars, planets, members):
    """同时核对 Star 双向流和每个 Planet 的有效上游, 再交叉检查 Star 入站总量."""
    star_states = {name: value for name, value in states.items() if name.startswith("star-")}
    planet_states = {name: value for name, value in states.items() if name.startswith("planet-")}
    if len(star_states) != stars or len(planet_states) != planets:
        return False
    ids = {value.get("id") for value in star_states.values()}
    if None in ids or len(ids) != stars:
        return False
    if not all(
        value.get("initialized") and value.get("members") == members and value.get("inbound") == stars - 1 and value.get("outbound") == stars - 1
        for value in star_states.values()
    ):
        return False
    # 已准入节点换绑时仍会输出 upstream: null. 缺少活动上游只表示尚未收敛.
    if not all(value.get("initialized") and (value.get("upstream") or {}).get("id") in ids for value in planet_states.values()):
        return False
    assigned = Counter(value["upstream"]["id"] for value in planet_states.values())
    return all(value.get("planet_inbound", 0) == assigned[value["id"]] for value in star_states.values())


class Endurance:
    """仅持有本次 Host 的进程. 停止标记和信号只发出请求, 清理不受第二次信号打断."""

    def __init__(self, options):
        self.options = options
        self.directory = options.output
        self.journal = Journal(self.directory)
        self.ports = []
        self.stopping = False
        self.fresh = {}
        self.next_sample = 0.0
        self.started = time.monotonic()
        self.report = {
            "status": "running",
            "phase": "starting",
            "started_utc": self.utc(),
            "controller_pid": os.getpid(),
            "stars": options.stars,
            "planets": options.planets,
            "fault_seconds_requested": options.fault_seconds,
            "steady_seconds_requested": options.steady_seconds,
            "fault_cycles": 0,
            "minimum_available_mib": available_memory() // 1024**2,
            "binary_sha256": {name: binary_digest(path) for name, path in self.binary_paths().items()},
            "source_sha256": {
                str(path.relative_to(ROOT)): binary_digest(path)
                for path in (Path(__file__), ROOT / "testkit/services.py", ROOT / "testkit/support.py", ROOT / "astra/test_scale.py")
            },
            "limits": [
                "Single Ubuntu VM, loopback connections, real C++ Release processes and Go Supervisor",
                "Connection/heartbeat endurance only; no business Registry/Catalog load",
                "Heartbeat 200 ms, Pong deadline 1500 ms; topology observations use 1-second service status",
                "0 steady seconds means run until STOP file or SIGINT/SIGTERM; failure also stops and cleans up",
                "Samples rotate at 8 x 8 MiB; reports retain aggregate peaks, last sample and recovery counters",
            ],
        }
        # 先确认产物可读再取得进程/临时目录所有权, 初始化失败也不会遗留测试资源.
        self.host = Host(options.binaries)

    @staticmethod
    def utc():
        return datetime.now(timezone.utc).isoformat()

    def binary_paths(self):
        return {**{name: self.options.binaries / name for name in ("star", "planet")}, "supervisor": ROOT / "build/supervisor/supervisor"}

    def check_stop(self):
        if self.stopping or (self.directory / "STOP").exists():
            raise RequestedStop("Manual stop requested")

    def reserve(self):
        address = endpoint(self.host)
        while address in self.ports:
            address = endpoint(self.host)
        self.ports.append(address)
        return address

    def snapshot(self):
        self.check_stop()
        remaining = available_memory() // 1024**2
        self.report["minimum_available_mib"] = min(self.report["minimum_available_mib"], remaining)
        if remaining < 384:
            raise RuntimeError(f"Available memory below 384 MiB: {remaining}")
        if os.statvfs(self.directory).f_bavail * os.statvfs(self.directory).f_frsize < 256 * 1024**2:
            raise RuntimeError("Available disk below 256 MiB")
        now = time.monotonic()
        states = {}
        for name, process in self.host.processes.items():
            value = process.snapshot()
            if not value["alive"]:
                raise RuntimeError(f"Unexpected exit: {name}: {value}")
            if name == "supervisor":
                continue
            previous = self.fresh.setdefault(name, (value["sequence"], now))
            if value["sequence"] != previous[0]:
                self.fresh[name] = (value["sequence"], now)
            elif now - previous[1] > 10:
                raise RuntimeError(f"No fresh status for 10 seconds: {name}")
            states[name] = value["status"]
        if now >= self.next_sample:
            self.record(states)
            self.next_sample = now + self.options.sample_seconds
        return states

    def record(self, states=None, event="sample", **details):
        """保留 PID 和阶段, 使重启后的资源值不会误算为同一进程的下降趋势."""
        resources = sample(self.host)
        for name, values in resources.items():
            values["pid"] = self.host.processes[name].process.pid
            for key in ("rss_kib", "threads", "fds"):
                peaks = self.report.setdefault("resource_peaks", {}).setdefault(name, {})
                peaks[key] = max(peaks.get(key, 0), values[key])
        now = time.monotonic()
        self.report.update(
            updated_utc=self.utc(),
            elapsed_seconds=round(now - self.started, 3),
            resources=resources,
            tcp_established=physical_connections(self.host),
            topology=states if states is not None else self.report.get("topology", {}),
        )
        if self.report["phase"] == "fault":
            self.report["fault_elapsed_seconds"] = round(now - self.fault_started, 3)
        elif self.report["phase"] == "steady":
            self.report["steady_elapsed_seconds"] = round(now - self.steady_started, 3)
        self.journal.write({"utc": self.report["updated_utc"], "event": event, "phase": self.report["phase"], "resources": resources, **details})
        atomic_json(self.directory / "status.json", self.report)

    def observe(self, predicate, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            states = self.snapshot()
            if predicate(states):
                return states
            if time.monotonic() >= deadline:
                raise TimeoutError("Topology did not converge: " + json.dumps(states))
            time.sleep(0.2)

    def pause(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.snapshot()
            time.sleep(min(0.2, max(0, deadline - time.monotonic())))

    def ready(self, states, missing_stars=0, missing_planets=0):
        return topology_ready(states, self.options.stars - missing_stars, self.options.planets - missing_planets, self.options.stars)

    def start_supervisor(self):
        self.check_stop()
        self.host.call("start", name="supervisor", kind="supervisor", address=self.supervisor, management=self.management, max_members=64, max_connections=128)
        self.observe(lambda _: any("supervisor_registration_started" in line for line in self.host.call("snapshot", name="supervisor")["tail"]))

    def start_node(self, name):
        self.snapshot()
        self.fresh.pop(name, None)
        planet = name.startswith("planet-")
        self.host.call(
            "start",
            name=name,
            kind="planet" if planet else "star",
            role="planet-a" if planet else "star-a",
            address=self.addresses[name],
            supervisor=self.supervisor,
            group="west" if int(name.rsplit("-", 1)[1]) % 2 else "east",
            max_members=64,
        )

    def stop_node(self, name):
        self.host.call("stop", name=name, graceful=False)
        self.fresh.pop(name, None)

    def recover(self, name, action):
        begun = time.monotonic()
        action()
        elapsed = round(time.monotonic() - begun, 3)
        values = self.report.setdefault("recovery", {}).setdefault(name, {"count": 0, "total_seconds": 0, "max_seconds": 0})
        values.update(count=values["count"] + 1, total_seconds=round(values["total_seconds"] + elapsed, 3), max_seconds=max(values["max_seconds"], elapsed))
        self.record(event="recovery", operation=name, seconds=elapsed)

    def fault_cycle(self):
        states = self.observe(self.ready)
        planet = f"planet-{self.report['fault_cycles'] % self.options.planets}"
        upstream = states[planet]["upstream"]["id"]
        failed = next(name for name, value in states.items() if name.startswith("star-") and value["id"] == upstream)
        # Supervisor 离线时强杀真实上游, 剩余节点必须使用已有授权恢复, 不依赖新的登录.
        self.stop_node("supervisor")
        self.stop_node(failed)
        self.recover("upstream_failover_supervisor_offline", lambda: self.observe(lambda values: self.ready(values, missing_stars=1)))
        self.start_supervisor()
        self.start_node(failed)
        self.recover("star_restart", lambda: self.observe(lambda values: self.ready(values) and values[failed].get("id") != upstream))
        # Planet 进程也轮换重启, 检查旧入站会话被清除和新进程身份重新准入.
        old_id = self.host.call("snapshot", name=planet)["status"]["id"]
        self.stop_node(planet)
        self.observe(lambda values: self.ready(values, missing_planets=1))
        self.start_node(planet)
        self.recover("planet_restart", lambda: self.observe(lambda values: self.ready(values) and values[planet].get("id") != old_id))
        self.report["fault_cycles"] += 1
        self.record(event="cycle_complete", cycle=self.report["fault_cycles"])
        self.pause(2)

    def run(self):
        self.supervisor, self.management = self.reserve(), self.reserve()
        names = [*(f"star-{i}" for i in range(self.options.stars)), *(f"planet-{i}" for i in range(self.options.planets))]
        self.addresses = {name: self.reserve() for name in names}
        self.start_supervisor()
        # 逐台启动, 让动态内存有机会增长, 每一步都检查余量和已启动进程状态.
        for name in names:
            self.start_node(name)
            self.observe(lambda states: states.get(name, {}).get("initialized"))
        self.observe(self.ready)
        self.fault_started = time.monotonic()
        self.report.update(phase="fault", fault_started_utc=self.utc())
        self.record(event="fault_started")
        while time.monotonic() - self.fault_started < self.options.fault_seconds:
            self.fault_cycle()
        self.report["fault_elapsed_seconds"] = round(time.monotonic() - self.fault_started, 3)
        self.steady_started = time.monotonic()
        self.report.update(phase="steady", steady_started_utc=self.utc())
        self.report["steady_baseline"] = sample(self.host)
        self.report["steady_pids"] = {name: process.process.pid for name, process in self.host.processes.items()}
        self.record(event="steady_started")
        # 此阶段不主动重启任何节点. 状态停更, 退出或持续无法收敛都会失败, 不自动重跑掩盖结果.
        while not self.options.steady_seconds or time.monotonic() - self.steady_started < self.options.steady_seconds:
            self.observe(self.ready, timeout=10)
            self.pause(1)
        self.report["status"] = "pass"

    def cleanup(self):
        errors = []
        self.report["last_live_resources"] = self.report.get("resources", {})
        # 先保留最终诊断, 再逐个正常退出. Host.close 负责异常时兜底释放剩余自有进程.
        self.report["final_states"] = {}
        for name in reversed(list(self.host.processes)):
            try:
                self.report["final_states"][name] = self.host.call("snapshot", name=name)
            except BaseException as error:
                errors.append(f"{name} snapshot: {error}")
            try:
                self.host.call("stop", name=name, graceful=True)
            except BaseException as error:
                errors.append(f"{name} stop: {error}")
        try:
            self.host.close()
        except BaseException as error:
            errors.append(f"Host cleanup: {error}")
        for address in self.ports:
            try:
                self.host.call("rebind", address="127.0.0.1", port=int(address.rsplit(":", 1)[1]))
            except BaseException as error:
                errors.append(f"Port cleanup: {error}")
        if self.host.directory.exists():
            errors.append("Owned temporary directory survived cleanup")
        self.report["cleanup"] = {"status": "fail" if errors else "pass", "errors": errors, "directory": str(self.host.directory)}
        if errors:
            self.report["status"] = "fail"


def parse_options(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, default=ROOT / "build/astra/release")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stars", type=int, default=8)
    parser.add_argument("--planets", type=int, default=16)
    parser.add_argument("--fault-seconds", type=int, default=7200)
    parser.add_argument("--steady-seconds", type=int, default=0)
    parser.add_argument("--sample-seconds", type=int, default=10)
    parser.add_argument("--stop", action="store_true")
    options = parser.parse_args(argv)
    options.output = options.output.resolve()
    options.binaries = options.binaries.resolve()
    if not options.output.is_relative_to((ROOT / "build").resolve()) or options.output == (ROOT / "build").resolve():
        parser.error("output must be a dedicated directory inside project build")
    if not 2 <= options.stars <= 16 or not 1 <= options.planets <= 32:
        parser.error("stars must be 2..16, planets must be 1..32")
    if not 0 <= options.fault_seconds <= 604800 or not 0 <= options.steady_seconds <= 604800 or not 1 <= options.sample_seconds <= 60:
        parser.error("invalid duration or sample interval")
    return options


def main():
    options = parse_options()
    if options.stop:
        if not (options.output / "status.json").is_file():
            raise SystemExit("No endurance run exists at output")
        (options.output / "STOP").touch()
        print("Stop requested; wait for status.json cleanup.status=pass")
        return
    if sys.platform != "linux":
        raise SystemExit("Linux /proc is required")
    options.output.mkdir(parents=True, exist_ok=True)
    with FileLock(ROOT / "build/testkit/endurance.lock"):
        if (options.output / "status.json").exists() or (options.output / "STOP").exists():
            raise SystemExit("Use a new output directory; previous evidence is never overwritten")
        run = Endurance(options)
        previous = {key: signal.getsignal(key) for key in (signal.SIGINT, signal.SIGTERM)}
        for key in previous:
            signal.signal(key, lambda *_: setattr(run, "stopping", True))
        try:
            run.run()
        except RequestedStop:
            run.report["status"] = "stopped"
            run.report["stop_reason"] = "manual"
        except BaseException as error:
            run.report.update(status="fail", error=f"{type(error).__name__}: {error}", traceback=traceback.format_exc())
        finally:
            run.cleanup()
            run.report["ended_utc"] = run.utc()
            run.record(event="finished")
            for key, handler in previous.items():
                signal.signal(key, handler)
            print(json.dumps({"status": run.report["status"], "report": str(options.output / "status.json")}), flush=True)
        if run.report["status"] == "fail":
            raise SystemExit(1)


if __name__ == "__main__":
    main()
