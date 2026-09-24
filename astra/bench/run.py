"""当前原生存储及实际多 Star/Comet 性能入口. 只消费已有 Release 产物, 不下载或构建."""

from __future__ import annotations

import argparse
import base64
from contextlib import ExitStack
import hashlib
from http.cookiejar import CookieJar
import json
import os
from pathlib import Path
import shutil
import signal
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "astra"))
from testkit.support import popen, stop_process
from test_comet_process import port
from test_pulsar import wait, terminate
from build import environment


class ProbeFailure(RuntimeError):
    """探针明确非零退出, 可记录为负载失败; 不等同于环境/清理失败."""


def resources(processes=()):
    """采集实际 Linux 内存/换页及已拥有进程, 不能将动态内存上限当作已分配量."""
    memory = {
        key: int(value.split()[0]) * 1024
        for key, value in (
            line.split(":", 1)
            for line in Path("/proc/meminfo").read_text().splitlines()
        )
    }
    vm = dict(line.split() for line in Path("/proc/vmstat").read_text().splitlines())
    values = {
        "time": time.monotonic(),
        "total": memory["MemTotal"],
        "available": memory["MemAvailable"],
        "swap_used": memory["SwapTotal"] - memory["SwapFree"],
        "swap_in": int(vm["pswpin"]),
        "swap_out": int(vm["pswpout"]),
        "processes": {},
    }
    for name, process in processes:
        try:
            status = dict(
                line.split(":", 1)
                for line in Path(f"/proc/{process.pid}/status").read_text().splitlines()
            )
            stat = (
                Path(f"/proc/{process.pid}/stat").read_text().rsplit(")", 1)[1].split()
            )
            values["processes"][name] = {
                "rss": int(status.get("VmRSS", "0").split()[0]) * 1024,
                "hwm": int(status.get("VmHWM", "0").split()[0]) * 1024,
                "threads": int(status["Threads"]),
                "cpu_seconds": (int(stat[11]) + int(stat[12]))
                / os.sysconf("SC_CLK_TCK"),
            }
        except (FileNotFoundError, ProcessLookupError):
            pass
    return values


def cluster(binaries, case, output, probe=None):
    """每份样本重建独立集群, finally 关闭全部自有进程/数据库, 不触碰其他部署."""
    deadline = time.monotonic() + 180
    traces = []
    fixtures = ROOT / "cluster/tests/fixtures"
    temporary = ROOT / "build/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="measure-", dir=temporary
    ) as directory, ExitStack() as stack:
        directory = Path(directory)
        identity = directory / "identity"
        shutil.copytree(fixtures / "pulsar", identity)
        accounts = json.loads((identity / "accounts.json").read_text())
        for account in accounts:
            if account["username"] == "stars":
                account["roles"] = ["star", "polaris", "astrolabe"]
        (identity / "accounts.json").write_text(json.dumps(accounts))
        owned = []

        def start(name, command):
            log = output / f"{name}.log"
            stream = stack.enter_context(log.open("w", encoding="utf-8"))
            process = popen(
                [str(part) for part in command],
                cwd=ROOT,
                env=environment(jobs=len(os.sched_getaffinity(0))),
                stdout=stream,
                stderr=subprocess.STDOUT,
            )
            stack.callback(stop_process, process)
            owned.append((name, process))
            return process, log

        def ready(process, log, predicate):
            return wait(process, log, predicate, seconds=45, overall_deadline=deadline)

        pulse, pulse_log = start(
            "pulsar",
            [
                binaries / "pulsar",
                "--listen=127.0.0.1:0",
                "--pulse-listen=127.0.0.1:0",
                "--galaxy=alpha",
                f"--identity={identity}",
                f"--state={directory / 'members.db'}",
                "--init=true",
            ],
        )
        admission = ready(
            pulse, pulse_log, lambda item: item.get("event") == "started"
        )["fields"]["admission"]
        ready(
            pulse,
            pulse_log,
            lambda item: item.get("event") == "clock_status"
            and item["fields"].get("synchronized") is True,
        )
        authority, authority_log = start(
            "polaris",
            [
                binaries / "polaris",
                f"--listen=127.0.0.1:{port()}",
                f"--super={admission}",
                "--galaxy=alpha",
                f"--identity={fixtures / 'star-a'}",
                f"--state={directory / 'polaris.db'}",
                "--init=true",
            ],
        )
        ready(authority, authority_log, lambda item: item.get("msg") == "Polaris ready")

        # 认证性能只从真实管理入口安装内部凭据, 不直接篡改 Star 存储或跳过会话.
        credential = None
        if case.get("auth", False):
            credential = directory / "comet.secret"
            secret = os.urandom(32)
            credential.write_bytes(secret)
            credential.chmod(0o600)
            password = os.urandom(16).hex()
            salt = os.urandom(16)
            account = directory / "account.json"
            account.write_text(
                json.dumps(
                    {
                        "username": "admin",
                        "salt": salt.hex(),
                        "hash": hashlib.pbkdf2_hmac(
                            "sha256", password.encode(), salt, 600000, 32
                        ).hex(),
                    }
                )
            )
            account.chmod(0o600)
            origin = f"https://127.0.0.1:{port()}"
            admin, admin_log = start(
                "astrolabe",
                [
                    binaries / "astrolabe",
                    f"--listen={origin.removeprefix('https://')}",
                    f"--super={admission}",
                    "--galaxy=alpha",
                    f"--identity={fixtures / 'star-a'}",
                    f"--account={account}",
                    f"--public={origin}",
                ],
            )
            ready(
                admin,
                admin_log,
                lambda item: item.get("msg") == "Astrolabe management listener ready",
            )
            browser = urllib.request.build_opener(
                urllib.request.ProxyHandler({}),
                urllib.request.HTTPCookieProcessor(CookieJar()),
                urllib.request.HTTPSHandler(
                    context=ssl.create_default_context(
                        cafile=str(fixtures / "star-a/ca.pem")
                    )
                ),
            )

            def request(path, value=None):
                message = urllib.request.Request(
                    origin + path,
                    data=json.dumps(value).encode() if value is not None else None,
                    headers={
                        "Origin": origin,
                        "X-Astra-Request": "1",
                        "Content-Type": "application/json",
                    },
                )
                with browser.open(message, timeout=5) as response:
                    if response.status != 200:
                        raise RuntimeError("Management setup failed")
                    return response.read(2 * 1024 * 1024)

            request("/api/session", {"username": "admin", "password": password})
            until = time.monotonic() + 15
            while True:
                try:
                    request("/api/almanac")
                    break
                except urllib.error.HTTPError as failure:
                    if failure.code not in (502, 503, 504) or time.monotonic() >= until:
                        raise
                    time.sleep(0.1)
            confirmed = json.loads(
                request(
                    "/api/credentials",
                    {
                        "key": "performance",
                        "secret": base64.b64encode(secret).decode(),
                        "version": "1",
                    },
                )
            )
            if confirmed.get("effect") != "committed":
                raise RuntimeError("Credential commit not confirmed")

        endpoints = []
        for index in range(case["stars"]):
            node, log = start(
                f"star-{index}",
                [
                    binaries / "star",
                    f"--listen=127.0.0.1:{port()}",
                    "--comet=127.0.0.1:0",
                    f"--auth={'true' if case.get('auth', False) else 'false'}",
                    f"--tls={'true' if case['tls'] else 'false'}",
                    f"--identity={fixtures / 'star-b'}",
                    *(
                        [f"--comet-identity={fixtures / 'star-b'}"]
                        if case["tls"]
                        else []
                    ),
                    f"--super={admission}",
                    "--galaxy=alpha",
                ],
            )
            endpoints.append(
                ready(node, log, lambda item: item.get("event") == "comet_listening")[
                    "fields"
                ]["endpoint"]
            )
            ready(node, log, lambda item: item.get("event") == "business_ready")

        # 就绪并不代表当前没有历史初始化工作. 排空后启动 SDK, 每例内部仍验证所有目标实际可见.
        time.sleep(0.3)
        command = [
            binaries / "comet_bench",
            ",".join(endpoints),
            fixtures / "star-b/ca.pem" if case["tls"] else "-",
            case["domain"],
            case["records"],
            case["bytes"],
            case["attr"],
            case["watchers"],
            case["writers"],
            case["seconds"],
            case["ttl"],
            case["rate"],
        ]
        if credential is not None:
            command.append(credential)
        if probe is not None:
            command = probe(endpoints)
        probe, log = start("comet", command)
        try:
            while probe.poll() is None:
                sample = resources(owned)
                traces.append(sample)
                if sample["available"] < 128 * 1024 * 1024:
                    raise RuntimeError(
                        "Insufficient actual memory; stopped owned benchmark before OOM"
                    )
                if time.monotonic() >= deadline or any(
                    process.poll() is not None for _, process in owned[:-1]
                ):
                    raise RuntimeError(
                        "Service exited or bounded measurement timed out"
                    )
                time.sleep(0.2)
            if probe.returncode != 0:
                raise ProbeFailure(f"SDK benchmark failed: {log.read_text()[-2000:]}")
            return [
                json.loads(line)
                for line in log.read_text().splitlines()
                if line.startswith('{"metric":')
            ]
        finally:
            (output / "resources.json").write_text(json.dumps(traces, indent=2))


def run(arguments):
    """顺序执行性能场景. 正确性并行策略不应用于共享同一 VM 的性能对比样本."""
    binaries = arguments.binaries.resolve()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    state = resources()
    identity = {
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "cpus": os.cpu_count(),
        "affinity": sorted(os.sched_getaffinity(0)),
        "kernel": os.uname().release,
        "resources": state,
        "binaries": {},
    }
    native = "star_almanac_bench" if arguments.almanac else "star_native_bench"
    names = (
        (native,)
        if arguments.native
        else ("pulsar", "polaris", "star", "comet_bench", "astrolabe")
    )
    for name in names:
        identity["binaries"][name] = hashlib.sha256(
            (binaries / name).read_bytes()
        ).hexdigest()
    (output / "identity.json").write_text(json.dumps(identity, indent=2))
    results = []
    cases = (
        json.loads(arguments.cases.read_text())
        if arguments.cases
        else [
            dict(
                domain="catalog",
                records=100,
                bytes=128,
                attr=256,
                watchers=1,
                writers=1,
                seconds=3,
                ttl=30000,
                rate=0,
                stars=1,
                tls=False,
            )
        ]
    )
    try:
        for number, case in enumerate(cases):
            for repeat in range(arguments.repeat):
                directory = output / f"case-{number}-round-{repeat}"
                directory.mkdir()
                print(
                    json.dumps({"case": number, "round": repeat, "parameters": case}),
                    flush=True,
                )
                before = resources()
                if arguments.native:
                    log = directory / "native.log"
                    traces = []
                    with log.open("w", encoding="utf-8") as stream:
                        process = popen(
                            [
                                binaries / native,
                                str(case["records"]),
                                str(case["bytes"]),
                            ],
                            cwd=ROOT,
                            stdout=stream,
                            stderr=subprocess.STDOUT,
                            env=environment(),
                        )
                        try:
                            until = time.monotonic() + 120
                            while process.poll() is None:
                                sample = resources([("native", process)])
                                traces.append(sample)
                                if (
                                    sample["available"] < 128 * 1024 * 1024
                                    or time.monotonic() >= until
                                ):
                                    raise RuntimeError(
                                        "Native sample exceeded resource/time budget"
                                    )
                                time.sleep(0.02)
                            if process.returncode != 0:
                                raise RuntimeError(
                                    f"Native benchmark failed: {log.read_text()[-2000:]}"
                                )
                        finally:
                            stop_process(process)
                            (directory / "resources.json").write_text(
                                json.dumps(traces, indent=2)
                            )
                    measurements = [
                        json.loads(line) for line in log.read_text().splitlines()
                    ]
                else:
                    measurements = cluster(binaries, case, directory)
                after = resources()
                traces = (
                    json.loads((directory / "resources.json").read_text())
                    if (directory / "resources.json").exists()
                    else []
                )
                totals = {sample["total"] for sample in [before, *traces, after]}
                stable = (
                    len(totals) == 1
                    and before["swap_in"] == after["swap_in"]
                    and before["swap_out"] == after["swap_out"]
                )
                results.append(
                    dict(
                        case=case,
                        round=repeat,
                        stable_memory=stable,
                        before=before,
                        after=after,
                        measurements=measurements,
                    )
                )
                print(
                    json.dumps(
                        {
                            "completed": number,
                            "round": repeat,
                            "stable_memory": stable,
                            "measurements": measurements,
                        }
                    ),
                    flush=True,
                )
    finally:
        (output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", type=Path)
    parser.add_argument("--repeat", type=int, choices=range(1, 6), default=3)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--native", action="store_true")
    mode.add_argument("--almanac", action="store_true")
    for number in (signal.SIGTERM, signal.SIGINT):
        signal.signal(number, terminate)
    arguments = parser.parse_args()
    arguments.native = arguments.native or arguments.almanac
    run(arguments)
