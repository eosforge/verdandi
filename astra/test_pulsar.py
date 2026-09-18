"""Pulsar + two real Stars: discovery, clock recovery and owned SIGTERM cleanup."""

from __future__ import annotations

import argparse
from contextlib import ExitStack, contextmanager
from functools import partial
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.support import popen, stop_process


def events(path):
    """Only parse complete local JSON log lines; cap logs so failures cannot exhaust the host."""
    if path.stat().st_size > 8 * 1024 * 1024:
        raise RuntimeError("Pulsar process test log exceeded 8 MiB")
    result = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if any(
            marker in line
            for marker in (
                "ThreadSanitizer:",
                "AddressSanitizer:",
                "LeakSanitizer:",
                "runtime error:",
            )
        ):
            raise RuntimeError(f"Service sanitizer diagnostic: {line[:2000]}")
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            result.append(value)
    return result


def verify_clock_log(path, *, require_holdover=False):
    """A process must preserve its epoch timeline through loss and recovery of the reference."""
    previous = None
    holdover = False
    recovered = False
    for value in events(path):
        if value.get("event") != "clock_status":
            continue
        fields = value["fields"]
        current = fields.get("nanoseconds")
        if current is None:
            if previous is not None:
                raise RuntimeError("Initialized clock lost its epoch anchor")
            continue
        if type(current) is not int or current < 0 or (previous is not None and current < previous):
            raise RuntimeError("Public epoch clock moved backwards or emitted an invalid timestamp")
        if fields.get("ready") is False and previous is not None and current > previous:
            holdover = True
        elif holdover and fields.get("ready") is True:
            recovered = True
        previous = current
    if previous is None or (require_holdover and not (holdover and recovered)):
        raise RuntimeError("Clock log lacks required initialization, holdover or recovery evidence")


def wait(process, log, predicate, seconds=20, *, overall_deadline, after=0):
    """Watch owned process state with a monotonic bound, never start a build or download."""
    deadline = min(time.monotonic() + seconds, overall_deadline)
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Owned service exited unexpectedly: {process.returncode}; log={log}")
        for value in reversed(events(log)[after:]):
            if predicate(value):
                return value
        time.sleep(0.02)
    raise TimeoutError(f"Owned service did not reach expected state; log={log}")


@contextmanager
def failure_logs(directory):
    """Emit bounded diagnostic tails after process cleanup, before temporary logs are removed."""
    try:
        yield
    except BaseException:
        for path in sorted(directory.glob("*.log")):
            try:
                with path.open("rb") as source:
                    source.seek(max(0, path.stat().st_size - 8192))
                    tail = source.read(8192).decode("utf-8", errors="replace")
                print(f"Service log tail ({path.name}):\n{tail}", file=sys.stderr)
            except OSError:
                pass
        raise


def run(binaries):
    """Each process group and temporary file belongs to this test and is released on every exit."""
    root = ROOT / "build/tmp"
    root.mkdir(parents=True, exist_ok=True)
    wait_for = partial(wait, overall_deadline=time.monotonic() + 90)
    with (
        tempfile.TemporaryDirectory(prefix="pulsar-process-", dir=root) as temporary,
        failure_logs(Path(temporary)),
        ExitStack() as stack,
    ):
        directory = Path(temporary)
        sequence = 0

        def start(name, command):
            nonlocal sequence
            sequence += 1
            log = directory / f"{sequence}-{name}.log"
            output = stack.enter_context(log.open("w", encoding="utf-8"))
            process = popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
            stack.callback(stop_process, process)
            return process, log

        def pulsar(admission="127.0.0.1:0", pulse="127.0.0.1:0"):
            process, log = start(
                "pulsar",
                [
                    binaries / "pulsar",
                    f"--listen={admission}",
                    f"--pulse-listen={pulse}",
                    "--galaxy=alpha",
                    f"--identity={ROOT / 'cluster/tests/fixtures/supervisor'}",
                    f"--state={directory / 'membership.journal'}",
                ],
            )
            value = wait_for(process, log, lambda value: value.get("event") == "started")
            # 真实进程用例要求宿主已完成物理对时, 不通过生产开关伪造同步资格.
            wait_for(
                process,
                log,
                lambda value: value.get("event") == "clock_status" and value["fields"].get("ready") is True,
            )
            return process, log, value["fields"]

        authority, authority_log, addresses = pulsar()
        stars = []
        for fixture in ("star-a", "star-b"):
            process, log = start(
                fixture,
                [
                    binaries / "star",
                    "--listen=127.0.0.1:0",
                    f"--super={addresses['admission']}",
                    "--galaxy=alpha",
                    f"--identity={ROOT / 'cluster/tests/fixtures' / fixture}",
                    "--status-interval-seconds=1",
                ],
            )
            wait_for(process, log, lambda value: value.get("event") == "initialized")
            wait_for(
                process,
                log,
                lambda value: value.get("event") == "clock_status" and value["fields"].get("ready") is True,
            )
            stars.append((process, log))
        for process, log in stars:
            wait_for(
                process,
                log,
                lambda value: value.get("event") == "status"
                and value["fields"].get("members") == 2
                and value["fields"].get("inbound") == 1
                and value["fields"].get("outbound") == 1,
            )

        # 失联与恢复都只接受本阶段的新事件, 不复用停机前偶发的质量下降日志.
        outage_marks = [(process, log, len(events(log))) for process, log in stars]
        authority.terminate()
        if authority.wait(timeout=10) != 0:
            raise RuntimeError("Pulsar did not shut down cleanly")
        for process, log, mark in outage_marks:
            wait_for(
                process,
                log,
                lambda value: value.get("event") == "clock_status" and value["fields"].get("ready") is False and "nanoseconds" in value["fields"],
                after=mark,
            )
        # 后续判断只读取本阶段新事件, 不让旧 ready 日志误报重连成功.
        marks = [(process, log, len(events(log))) for process, log in stars]
        authority, authority_log, _ = pulsar(addresses["admission"], addresses["pulse"])
        for process, log, mark in marks:
            wait_for(
                process,
                log,
                lambda value: value.get("event") == "clock_status" and value["fields"].get("ready") is True,
                after=mark,
            )
        for process, log in [*stars, (authority, authority_log)]:
            process.terminate()
            if process.wait(timeout=10) != 0 or not any(value.get("event") == "stopped" for value in events(log)):
                raise RuntimeError("Service shutdown was incomplete")
            verify_clock_log(log, require_holdover=process in [star for star, _ in stars])
        print("PASS Pulsar + two Stars: topology, calibration, outage, fixed-Unix reconnect and clean SIGTERM")


def terminate(signum, _frame):
    """Unwind ExitStack on harness termination so CTest does not leave owned services behind."""
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise SystemExit(128 + signum)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    args = parser.parse_args()
    previous = signal.signal(signal.SIGTERM, terminate)
    try:
        run(args.binaries.resolve())
    finally:
        signal.signal(signal.SIGTERM, previous)
