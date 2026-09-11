#!/usr/bin/env python3
"""Run a complete regression or a duration-controlled, automatically cleaned soak."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def duration(value):
    match = re.fullmatch(r"([1-9][0-9]*)(s|m|h)?", value)
    if not match:
        raise argparse.ArgumentTypeError("Use a duration such as 210s, 30m or 2h")
    seconds = int(match[1]) * {"s": 1, "m": 60, "h": 3600}[match[2] or "s"]
    if not 210 <= seconds <= 86400:
        raise argparse.ArgumentTypeError("Duration must be 210 seconds through 24 hours per domain to cover expiry cycles and fault recovery")
    return seconds


def arguments(argv=None):
    config_path = ROOT / "build/testkit/config.json"
    config = json.loads(config_path.read_text(encoding="utf-8")) if config_path.exists() else {}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("regression", "soak"))
    parser.add_argument("--duration", type=duration, default=7200, help="Effective load time per domain; preparation/cleanup are additional (default: 2h)")
    parser.add_argument("--targets", default=",".join(config.get("targets", ["local"])), help="local,linux; Linux uses the configured Ubuntu project")
    parser.add_argument("--languages", default="go,rust,cpp,csharp", help="Comma-separated SDK scope; narrowed runs are reported as partial scope")
    parser.add_argument("--host", default=config.get("host", "local" if os.name != "nt" else "192.168.0.119"))
    parser.add_argument("--user", default=config.get("user", "ubuntu"))
    parser.add_argument("--remote-project", default=config.get("remote_project", "/home/ubuntu/verdandi"))
    parser.add_argument("--plan", action="store_true", help="Show selected scope without installing, building or creating resources")
    parser.add_argument("--preflight-only", action="store_true")
    options = vars(parser.parse_args(argv))
    for key, valid in (("targets", {"local", "linux"}), ("languages", {"go", "rust", "cpp", "csharp"})):
        options[key] = options[key].split(",")
        if not options[key] or len(set(options[key])) != len(options[key]) or not set(options[key]) <= valid:
            parser.error(f"Invalid or repeated {key}")
    if os.name != "nt" and "linux" in options["targets"]:
        parser.error("On Linux use --targets local; the linux target is a Windows-to-Ubuntu dispatcher")
    return options


def worker_watchdog():
    last = [time.monotonic()]
    stopped = threading.Event()

    def read():
        for line in sys.stdin:
            if line.strip() == "cancel":
                last[0] = 0
                return
            last[0] = time.monotonic()
        last[0] = 0

    def watch():
        while not stopped.wait(2):
            if time.monotonic() - last[0] > 60:
                os.kill(os.getpid(), signal.SIGINT)
                return

    threading.Thread(target=read, daemon=True).start()
    threading.Thread(target=watch, daemon=True).start()
    return stopped


def cancellation_watchdog():
    """Bridge PowerShell cancellation/parent death into ordinary Python cleanup."""
    value = os.environ.get("VERDANDI_TEST_CANCEL_FILE")
    if not value:
        return None
    path = Path(value)
    if path.parent.resolve() != (ROOT / "build/testkit/cancel").resolve() or not re.fullmatch(r"[a-f0-9]{32}\.request", path.name):
        raise ValueError("Invalid cancellation file")
    stopped = threading.Event()
    parent = None
    if os.name == "nt" and os.environ.get("VERDANDI_TEST_PARENT_PID"):
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        parent = kernel.OpenProcess(0x00100000, False, int(os.environ["VERDANDI_TEST_PARENT_PID"]))
        if not parent:
            raise OSError("Test launcher is no longer available")

    def watch():
        try:
            while not stopped.wait(0.2):
                if path.exists() or (parent and kernel.WaitForSingleObject(parent, 0) == 0):
                    signal.raise_signal(signal.SIGINT)
                    return
        finally:
            if parent:
                kernel.CloseHandle(parent)
            path.unlink(missing_ok=True)

    threading.Thread(target=watch, daemon=True).start()
    return stopped


def execute(options):
    from testkit.campaign import fingerprint, remote_campaign, save_report, source_manifest
    from testkit.resources import Remote, Resources
    from testkit.support import FileLock, available_memory, environment, recover_temporary, temporary_directory
    from testkit.suites import execute as execute_suites, preflight

    run_id = options.setdefault("run_id", secrets.token_hex(8))
    output = Path(options.setdefault("output", str(ROOT / "build/testkit/runs" / run_id)))
    if not re.fullmatch("[a-f0-9]{16}", run_id) or not output.resolve().is_relative_to((ROOT / "build/testkit/runs").resolve()):
        raise ValueError("Invalid run ID or report directory")
    output.mkdir(parents=True, exist_ok=False)
    env = environment()
    env.update(VERDANDI_TEST_LOG_DIR=str(output / "logs"), VERDANDI_TEST_SSH_PASSWORD=options["password"], VERDANDI_TEST_RUN_ID=run_id)
    os.environ.clear()
    os.environ.update(env)  # This standalone Python process owns these settings.
    if options["mode"] == "soak":
        print(f"Two serial domains, each {options['duration']}s; effective load >= {2 * options['duration']}s, plus preparation and cleanup.", flush=True)
    manifest = options.get("source_manifest") or source_manifest()
    report = {
        "mode": options["mode"],
        "preflight_only": options["preflight_only"],
        "run_id": run_id,
        "status": "running",
        "cleanup": "pending",
        "full_protocol_qualification": False,
        "coverage_gaps": [
            {"scenario": "live mutual TLS", "status": "not_implemented", "languages": ["go", "rust", "cpp", "csharp"]},
            {"scenario": "dedicated continuous endurance", "status": "not_implemented", "languages": ["rust", "cpp", "csharp"]},
            {"scenario": "direct C++ two-promotion peer", "status": "not_implemented", "languages": ["cpp"]},
        ],
        "source_sha256": fingerprint(manifest),
        "requested_targets": options["targets"],
        "requested_languages": options["languages"],
        "stages": [],
        "coverage_notes": [
            "This report qualifies its listed source/platform/scenario rows. Live mTLS and dedicated C++/CSharp continuous endurance are separate coverage gaps."
        ],
    }
    save = lambda: save_report(report, output)
    started = time.monotonic()
    remote = None
    lock = None
    temporary = ExitStack()
    try:
        lock = FileLock(ROOT / "build/testkit/run.lock")
        if lock is not None:
            save()
            recover_temporary()
            scratch = temporary.enter_context(temporary_directory("verdandi-campaign-"))
            os.environ.update(TEMP=scratch, TMP=scratch, TMPDIR=scratch, GOTMPDIR=scratch)
            if available_memory() < 1024 * 1024 * 1024:
                raise RuntimeError("Preflight requires at least 1 GiB available memory")
            if "local" in options["targets"]:
                if options["preflight_only"]:
                    report["environment"] = preflight(options["languages"])
                else:
                    remote = Remote(options["host"], options["user"], options["password"], project=options["remote_project"])
                    execute_suites(options, remote, report, save)
            if "linux" in options["targets"]:
                if remote is not None:
                    remote.close()
                remote = Remote(options["host"], options["user"], options["password"], project=options["remote_project"], sudo=False)
                report["linux_cleanup"] = "pending"
                save()
                child = remote_campaign(remote, options, manifest)
                report["stages"].extend(child["stages"])
                report["linux_status"] = child["status"]
                report["linux_cleanup"] = child["cleanup"]
                report["coverage_notes"].extend(child.get("coverage_notes", []))
            current = (
                {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in manifest}
                if options.get("source_manifest") is not None
                else source_manifest()
            )
            if current != manifest:
                raise RuntimeError("Source changed during the campaign; results do not qualify one frozen snapshot")
            report["status"] = (
                "failed"
                if any(s["status"] not in ("pass", "not_applicable") for s in report["stages"]) or report.get("linux_status", "pass") != "pass"
                else "pass"
            )
    except (KeyboardInterrupt, BrokenPipeError):
        report.update(status="interrupted", error="Cancelled; partial evidence retained")
    except Exception as error:
        report.update(status="failed" if report["stages"] else "blocked", error=str(error))
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        try:
            with ExitStack() as cleanup:
                if lock is not None:
                    cleanup.callback(recover_temporary)
                cleanup.callback(temporary.close)
                if remote is not None and not options["preflight_only"]:
                    remote.sudo = True
                    cleanup.callback(Resources.recover, remote, run_id)
            report["cleanup"] = "cleaned" if report.get("linux_cleanup", "cleaned") == "cleaned" else "pending"
        except Exception as error:
            report["cleanup"] = "failed"
            report["cleanup_error"] = str(error)
            if report["status"] == "pass":
                report["status"] = "failed"
        finally:
            if remote is not None:
                remote.close()
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        try:
            save()
        finally:
            if lock is not None:
                lock.close()
        try:
            print(f"RESULT {report['status']} cleanup={report['cleanup']} report={output / 'report.md'}", flush=True)
        except OSError:
            # A terminated Windows launcher can report EINVAL rather than EPIPE.
            with open(os.devnull, "w", encoding="utf-8") as sink:
                os.dup2(sink.fileno(), sys.stdout.fileno())
    return 0 if report["status"] == "pass" else 130 if report["status"] == "interrupted" else 1


def main():
    worker = sys.argv[1:] == ["--worker"]
    if worker:
        payload = sys.stdin.readline(4 * 1024 * 1024)
        if not payload.endswith("\n"):
            raise ValueError("Oversized worker payload")
        options = json.loads(payload)
        watchdog = worker_watchdog()
    else:
        options = arguments()
        if options["plan"]:
            print(
                json.dumps(
                    {
                        **options,
                        "soak_effective_load_seconds": 2 * options["duration"] if options["mode"] == "soak" else None,
                        "cleanup": "owned resources on success/failure/timeout/cancellation; stale-run recovery on next run",
                    },
                    indent=2,
                )
            )
            return 0
        venv = ROOT / "build/tools/python-build"
        python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        if python.is_file() and Path(sys.prefix).resolve() != venv.resolve():
            os.execv(str(python), [str(python), "-B", str(Path(__file__).resolve()), *sys.argv[1:]])
        options["password"] = os.environ.get("VERDANDI_TEST_SSH_PASSWORD") or (
            "" if options["preflight_only"] else getpass.getpass("Ubuntu SSH/sudo password (not saved): ")
        )
        watchdog = cancellation_watchdog()

    def interrupt(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    signal.signal(signal.SIGINT, interrupt)
    try:
        return execute(options)
    finally:
        if watchdog is not None:
            watchdog.set()


if __name__ == "__main__":
    raise SystemExit(main())
