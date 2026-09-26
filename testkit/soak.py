"""真实 Pulsar/Polaris/Astrolabe/三 Star 长测, 只使用已有产物, 不构建或下载."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "astra"))
from build import environment
from test_comet_process import run
from test_pulsar import terminate
from testkit.support import binary_digest


class Recorder:
    """单线程采样, 不创建额外调度器; 记录每个 PID 的资源, 不把重启前后 RSS 混为一条曲线."""

    def __init__(self, output, faults, steady, interval, records=16):
        self.output = output
        self.faults = faults
        self.steady = steady
        self.interval = interval
        self.records = records
        self.expected = {}
        self.offsets = {}
        self.last = 0.0
        self.output.mkdir(parents=True, exist_ok=False)

    def record(self, event, **values):
        """JSONL 每行一个观察, 测试通过与主动停止必须分别记录."""
        value = {
            "event": event,
            "utc": time.time(),
            "elapsed": time.monotonic(),
            **values,
        }
        with (self.output / "events.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value) + "\n")
        if event != "sample":
            print(json.dumps(value), flush=True)

    def expect(self, process, name):
        self.expected[process.pid] = (process, name)

    def retire(self, process):
        self.expected.pop(process.pid, None)

    def check(self, owned, *, force=False):
        """最多每五秒读取一次 /proc 和日志新增字节, Sanitizer/意外退出/资源耗尽都不能被下一轮覆盖."""
        now = time.monotonic()
        if not force and now - self.last < 5:
            return
        self.last = now
        for process, name in self.expected.values():
            if process.poll() is not None:
                raise RuntimeError(f"Resident service exited unexpectedly: {name}, code={process.returncode}")
        memory = {key: int(value.split()[0]) * 1024 for key, value in (line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())}
        if memory["MemAvailable"] < 128 * 1024 * 1024:
            raise RuntimeError("Soak stopped below 128 MiB available host memory")
        samples = []
        total = 0
        for process, log in owned:
            size = log.stat().st_size
            total += size
            if size > 128 * 1024 * 1024 or total > 512 * 1024 * 1024:
                raise RuntimeError("Soak log budget exhausted, evidence is incomplete")
            offset = self.offsets.get(log, 0)
            with log.open("rb") as stream:
                stream.seek(max(0, offset - 64))  # 保留跨块诊断前缀, 不漏掉恰好拆开的 Sanitizer 文本.
                added = stream.read(size - stream.tell())
                if any(
                    marker in added
                    for marker in (
                        b"ThreadSanitizer:",
                        b"AddressSanitizer:",
                        b"LeakSanitizer:",
                        b"runtime error:",
                    )
                ):
                    (self.output / log.name).write_bytes(added[-8192:])
                    raise RuntimeError(f"Sanitizer diagnostic in {log.name}")
                if size != offset:
                    stream.seek(max(0, size - 8192))
                    (self.output / log.name).write_bytes(stream.read(8192))
            self.offsets[log] = size
            if process.poll() is not None:
                continue
            try:
                path = Path(f"/proc/{process.pid}")
                status = dict(line.split(":", 1) for line in (path / "status").read_text().splitlines())
                samples.append(
                    {
                        "pid": process.pid,
                        "log": log.name,
                        "rss": int(status.get("VmRSS", "0").split()[0]) * 1024,
                        "peak": int(status.get("VmHWM", "0").split()[0]) * 1024,
                        "threads": int(status["Threads"]),
                        "fds": len(list((path / "fd").iterdir())),
                    }
                )
            except (FileNotFoundError, ProcessLookupError):
                # 在途探针可以正常结束; 常驻服务是否允许结束由 expected 再确认.
                if process.pid in self.expected:
                    raise RuntimeError("Resident service disappeared during resource sampling")
        self.record(
            "sample",
            available=memory["MemAvailable"],
            swap=memory["SwapTotal"] - memory["SwapFree"],
            logs=total,
            processes=samples,
        )


def parse(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="New evidence directory; existing directories are rejected",
    )
    parser.add_argument(
        "--fault-seconds",
        type=int,
        default=7200,
        help="Rotate through all three Stars at least once; finish the active cycle at the time limit",
    )
    parser.add_argument(
        "--steady-seconds",
        type=int,
        default=43200,
        help="One resident three-source SDK workload after fault cycles",
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=60,
        help="Steady writes before each crash, 0..3600 seconds",
    )
    parser.add_argument(
        "--records",
        type=int,
        default=16,
        help="Per-Star Publishers and Beacons, 1..16 each",
    )
    options = parser.parse_args(arguments)
    if (
        not 0 <= options.fault_seconds <= 604800
        or not 0 <= options.steady_seconds <= 604800
        or options.fault_seconds + options.steady_seconds > 604800
        or not 0 <= options.interval <= 3600
        or not 1 <= options.records <= 16
    ):
        parser.error("Total duration must be 0..604800 seconds, interval 0..3600 and records 1..16")
    return options


def main(arguments=None):
    options = parse(arguments)
    if sys.platform != "linux":
        raise RuntimeError("This resource-qualified soak runner requires Linux /proc")
    binaries = options.binaries.resolve()
    names = (
        "star",
        "pulsar",
        "polaris",
        "astrolabe",
        "comet_process_probe",
        "comet_mesh_probe",
    )
    digests = {name: binary_digest(binaries / name) for name in names}
    recorder = Recorder(
        options.output.resolve(),
        options.fault_seconds,
        options.steady_seconds,
        options.interval,
        options.records,
    )
    recorder.record(
        "started",
        faults=options.fault_seconds,
        steady=options.steady_seconds,
        interval=options.interval,
        records=options.records,
        binaries=digests,
    )
    # 只改变本次 Python 进程及其子进程的环境, 不写入用户/系统配置; 沿用明确选定的 Sanitizer 环境.
    previous_environment = os.environ.copy()
    previous_signal = signal.signal(signal.SIGTERM, terminate)
    try:
        os.environ.update(environment(jobs=min(4, len(os.sched_getaffinity(0)))))
        run(binaries, binaries / "polaris", binaries / "astrolabe", recorder)
    except BaseException as failure:
        recorder.record(
            ("interrupted" if isinstance(failure, (KeyboardInterrupt, SystemExit)) else "failed"),
            reason=type(failure).__name__,
        )
        raise
    finally:
        signal.signal(signal.SIGTERM, previous_signal)
        os.environ.clear()
        os.environ.update(previous_environment)


if __name__ == "__main__":
    main()
