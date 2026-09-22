"""真实基础设施链路: Almanac 持久恢复、多 Star 动态复制与 Comet 故障切换."""

from __future__ import annotations

import argparse
import base64
from contextlib import ExitStack
from functools import partial
import hashlib
from http.cookiejar import CookieJar
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.support import popen, stop_process
from test_pulsar import events, failure_logs, terminate, wait


def port():
    """选择测试固定部署端口, 真正启动争用则失败, 不悄悄重写已准入部署地址."""
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def run(binaries: Path, polaris: Path, astrolabe: Path):
    """只消费已显式构建的文件, 不下载/构建, 每个进程和数据库都属于本次夹具."""
    for executable in (
        binaries / "pulsar",
        binaries / "star",
        binaries / "comet_process_probe",
        binaries / "comet_mesh_probe",
        polaris,
        astrolabe,
    ):
        if not executable.is_file():
            raise RuntimeError(
                f"Missing explicitly built process executable: {executable.name}"
            )
    root = ROOT / "build/tmp"
    root.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + 390
    wait_for = partial(wait, overall_deadline=deadline)
    fixtures = ROOT / "cluster/tests/fixtures"
    with tempfile.TemporaryDirectory(
        prefix="comet-process-", dir=root
    ) as temporary, failure_logs(Path(temporary)), ExitStack() as stack:
        directory = Path(temporary)
        identity = directory / "pulsar-identity"
        shutil.copytree(fixtures / "supervisor", identity)
        accounts = json.loads((identity / "accounts.json").read_text(encoding="utf-8"))
        for account in accounts:
            if account["username"] == "stars":
                account["roles"] = ["star", "polaris", "astrolabe"]
        (identity / "accounts.json").write_text(json.dumps(accounts), encoding="utf-8")
        password = "integration-admin-password"
        salt = os.urandom(16)
        account_file = directory / "account.json"
        account_file.write_text(
            json.dumps(
                {
                    "username": "admin",
                    "salt": salt.hex(),
                    "hash": hashlib.pbkdf2_hmac(
                        "sha256", password.encode(), salt, 600000, 32
                    ).hex(),
                }
            ),
            encoding="utf-8",
        )
        account_file.chmod(0o600)
        secret = os.urandom(32)
        secret_file = directory / "comet.secret"
        secret_file.write_bytes(secret)
        secret_file.chmod(0o600)
        owned = []

        def start(name, command):
            log = directory / f"{len(owned)}-{name}.log"
            output = stack.enter_context(log.open("w", encoding="utf-8"))
            process = popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
            stack.callback(stop_process, process)
            owned.append((process, log))
            return process, log

        def stop(process):
            process.terminate()
            if process.wait(timeout=10) != 0:
                raise RuntimeError("Owned process did not exit cleanly")

        pulse, pulse_log = start(
            "pulsar",
            [
                binaries / "pulsar",
                "--listen=127.0.0.1:0",
                "--pulse-listen=127.0.0.1:0",
                "--galaxy=alpha",
                f"--identity={identity}",
                f"--state={directory / 'membership.db'}",
                "--init=true",
            ],
        )
        addresses = wait_for(
            pulse, pulse_log, lambda value: value.get("event") == "started"
        )["fields"]
        wait_for(
            pulse,
            pulse_log,
            lambda value: value.get("event") == "clock_status"
            and value["fields"].get("synchronized") is True,
        )
        authority_port = port()

        def publisher(initialize):
            process, log = start(
                "polaris",
                [
                    polaris,
                    f"--listen=127.0.0.1:{authority_port}",
                    f"--super={addresses['admission']}",
                    "--galaxy=alpha",
                    f"--identity={fixtures / 'star-a'}",
                    f"--state={directory / 'polaris.db'}",
                    f"--init={'true' if initialize else 'false'}",
                ],
            )
            wait_for(process, log, lambda value: value.get("msg") == "Polaris ready")
            return process

        authority = publisher(True)
        star_port, comet_port, metrics_port = port(), port(), port()
        if len({star_port, comet_port, metrics_port}) != 3:
            raise RuntimeError("Fixture selected duplicate Star ports")
        metrics_file = directory / "metrics.json"
        metrics_file.write_text(
            json.dumps(
                {f"127.0.0.1:{star_port}": f"http://127.0.0.1:{metrics_port}/metrics"}
            ),
            encoding="utf-8",
        )
        admin_port = port()
        origin = f"https://127.0.0.1:{admin_port}"
        management, management_log = start(
            "astrolabe",
            [
                astrolabe,
                f"--listen=127.0.0.1:{admin_port}",
                f"--super={addresses['admission']}",
                "--galaxy=alpha",
                f"--identity={fixtures / 'star-a'}",
                f"--account={account_file}",
                f"--public={origin}",
                f"--metrics={metrics_file}",
            ],
        )
        wait_for(
            management,
            management_log,
            lambda value: value.get("msg") == "Astrolabe management listener ready",
        )
        context = ssl.create_default_context(cafile=str(fixtures / "star-a/ca.pem"))
        browser = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(CookieJar()),
            urllib.request.HTTPSHandler(context=context),
        )

        def request(method, path, value=None):
            body = None if value is None else json.dumps(value).encode()
            message = urllib.request.Request(
                origin + path,
                data=body,
                method=method,
                headers={
                    "Origin": origin,
                    "X-Astra-Request": "1",
                    "Content-Type": "application/json",
                },
            )
            try:
                with browser.open(message, timeout=5) as response:
                    data = response.read(2 * 1024 * 1024 + 1)
                    if len(data) > 2 * 1024 * 1024:
                        raise RuntimeError("Management fixture response exceeded bound")
                    return response.status, data
            except urllib.error.HTTPError as failure:
                return failure.code, failure.read(4096)

        status, _ = request(
            "POST", "/api/session", {"username": "admin", "password": password}
        )
        if status != 200:
            raise RuntimeError("Management login failed")

        def available(path, check):
            until = min(deadline, time.monotonic() + 50)
            while time.monotonic() < until:
                status, body = request("GET", path)
                if status == 200 and check(body):
                    return body
                if status not in (200, 502, 503, 504):
                    raise RuntimeError(f"Unexpected management read status: {status}")
                if any(
                    process.poll() is not None
                    for process in (management, pulse, authority)
                ):
                    raise RuntimeError(
                        "Control process exited during availability wait"
                    )
                time.sleep(0.1)
            raise TimeoutError("Management authority did not recover")

        available("/api/almanac", lambda _: True)

        def write(path, value):
            # 写入恰好一次. 失败即终止用例, 不用重试掩盖 COMMIT 不确定或控制面错误.
            status, body = request("POST", path, value)
            if status != 200 or json.loads(body).get("effect") != "committed":
                raise RuntimeError(
                    f"Management commit did not confirm persistence: HTTP {status}"
                )

        write(
            "/api/credentials",
            {
                "key": "integration",
                "secret": base64.b64encode(secret).decode(),
                "version": "1",
            },
        )
        redacted = available(
            "/api/credentials", lambda data: b'"complete":true' in data
        )
        if (
            b'"redacted":true' not in redacted
            or base64.b64encode(secret) in redacted
            or secret in redacted
        ):
            raise RuntimeError("Credential management did not preserve redaction")

        def update(version, value):
            data = {
                "sector": "integration",
                "spectrum": "main",
                "key": "key",
                "version": str(version),
            }
            data.update(
                {"erase": True}
                if value is None
                else {"value": base64.b64encode(value).decode()}
            )
            write("/api/almanac", data)

        update(1, b"one")

        def node(name, internal, external):
            process, log = start(
                name,
                [
                    binaries / "star",
                    f"--listen=127.0.0.1:{internal}",
                    f"--comet=127.0.0.1:{external}",
                    f"--metrics=127.0.0.1:{metrics_port if internal == star_port else 0}",
                    "--auth=true",
                    "--tls=true",
                    f"--comet-identity={fixtures / 'star-b'}",
                    f"--identity={fixtures / 'star-b'}",
                    f"--super={addresses['admission']}",
                    "--galaxy=alpha",
                    "--status-interval-seconds=1",
                ],
            )
            endpoint = wait_for(
                process, log, lambda value: value.get("event") == "comet_listening"
            )["fields"]["endpoint"]
            return process, log, endpoint

        star, star_log, endpoint = node("star-a", star_port, comet_port)

        def probe(target):
            return start(
                "comet",
                [
                    binaries / "comet_process_probe",
                    endpoint,
                    fixtures / "star-b/ca.pem",
                    secret_file,
                    "integration",
                    "main",
                    str(target),
                ],
            )

        reader, reader_log = probe(4)

        def view(version, value):
            wait_for(
                reader,
                reader_log,
                lambda event: event.get("event") == "view"
                and event.get("version") == version
                and event.get("present") == (value is not None)
                and event.get("value") == ("" if value is None else value.hex()),
                seconds=45,
            )

        view(1, b"one")
        observed = available(
            "/api/metrics",
            lambda data: any(
                not sample["stale"]
                and sample.get("values", {}).get("astra_ready") == "1"
                for sample in json.loads(data)["samples"]
            ),
        )
        if secret in observed or base64.b64encode(secret) in observed:
            raise RuntimeError("Metrics exposed credential material")
        update(2, b"two")
        view(2, b"two")
        stop(authority)
        cached, cached_log = probe(2)
        if cached.wait(timeout=15) != 0 or not any(
            event.get("event") == "view"
            and event.get("version") == 2
            and event.get("value") == b"two".hex()
            for event in events(cached_log)
        ):
            raise RuntimeError(
                "Star could not serve its complete cache during authority outage"
            )
        authority = publisher(False)

        def persisted(data):
            rows = [json.loads(line) for line in data.splitlines()]
            return (
                bool(rows)
                and rows[-1].get("complete") is True
                and rows[-1].get("position", {}).get("version") == "2"
                and any(
                    row.get("key") == "key"
                    and row.get("value") == base64.b64encode(b"two").decode()
                    for row in rows
                )
            )

        available("/api/almanac?sector=integration&spectrum=main", persisted)
        update(3, None)
        view(3, None)
        update(4, b"")
        # 最后一帧可以先被进程写出再正常退出, 不用只允许存活进程的 wait 错判成功退出.
        if reader.wait(timeout=20) != 0 or not any(
            event.get("event") == "view"
            and event.get("version") == 4
            and event.get("present") is True
            and event.get("value") == ""
            for event in events(reader_log)
        ):
            raise RuntimeError(
                "Native Reader did not install empty-value final commit or clean up"
            )
        # 两台 Star 都是真实进程和独立端口. 原生探针只访问公共 TLS, 不使用内部 Stub 代替 SDK.
        second_port, second_comet_port = port(), port()
        if (
            len({star_port, comet_port, metrics_port, second_port, second_comet_port})
            != 5
        ):
            raise RuntimeError("Fixture selected duplicate mesh ports")
        second, second_log, second_endpoint = node(
            "star-b", second_port, second_comet_port
        )
        wait_for(
            second,
            second_log,
            lambda value: value.get("event") == "business_ready",
            seconds=45,
        )
        mesh, mesh_log = start(
            "mesh",
            [
                binaries / "comet_mesh_probe",
                endpoint,
                second_endpoint,
                fixtures / "star-b/ca.pem",
                secret_file,
            ],
        )
        wait_for(
            mesh, mesh_log, lambda value: value.get("event") == "mesh_ready", seconds=60
        )
        star.kill()  # 此项特意测试权威突然消失, 不经过正常注销/服务关闭补偿.
        if star.wait(timeout=10) == 0:
            raise RuntimeError("Crash fixture unexpectedly reported clean shutdown")
        wait_for(
            mesh,
            mesh_log,
            lambda value: value.get("event") == "mesh_failover",
            seconds=80,
        )
        star, star_log, recovered_endpoint = node(
            "star-a-restarted", star_port, comet_port
        )
        if recovered_endpoint != endpoint:
            raise RuntimeError("Restart did not preserve the deployment endpoint")
        if mesh.wait(
            timeout=min(100, max(1, deadline - time.monotonic()))
        ) != 0 or not any(
            event.get("event") == "mesh_recovered" for event in events(mesh_log)
        ):
            raise RuntimeError("Multi-Star native recovery or resource cleanup failed")

        for process in (star, second, management, authority, pulse):
            stop(process)
        for _, log in owned:
            events(log)  # 同时检查所有完整进程日志中的 Sanitizer 诊断, 不只读成功标记.
        print(
            "PASS real Almanac persistence and native multi-Star replication, crash, failover, trusted restart, TTL and owned cleanup"
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--polaris", type=Path, required=True)
    parser.add_argument("--astrolabe", type=Path, required=True)
    arguments = parser.parse_args()
    previous = signal.signal(signal.SIGTERM, terminate)
    try:
        run(
            arguments.binaries.resolve(),
            arguments.polaris.resolve(),
            arguments.astrolabe.resolve(),
        )
    finally:
        signal.signal(signal.SIGTERM, previous)
