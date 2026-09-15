"""C++ gRPC 边界和 Go Supervisor 组网测试; 仅运行已有产物, 所有服务由本次测试持有并清理."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from testkit.services import Host, converge, wait_for
from testkit.support import atomic_json, environment


def binary_digest(path):
    """按块记录实际执行产物的摘要, 不用源码 commit 代替二进制身份."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def endpoint(host):
    return f"127.0.0.1:{host.call('reserve', address='127.0.0.1')}"


def supervisor(host):
    address, management = endpoint(host), endpoint(host)
    host.call(
        "start",
        name="supervisor",
        kind="supervisor",
        address=address,
        management=management,
    )
    wait_for(
        host,
        "supervisor",
        lambda value: any("supervisor_registration_started" in line for line in value["tail"]),
    )
    return address


def node(host, name, kind, role, address, super_address, group="default"):
    host.call(
        "start",
        name=name,
        kind=kind,
        role=role,
        address=address,
        supervisor=super_address,
        group=group,
    )
    return wait_for(host, name, lambda value: value["status"].get("initialized"))


def rpc_boundaries(binaries, report):
    with ExitStack() as stack:
        host = Host(binaries)
        stack.callback(host.close)
        super_address = supervisor(host)
        address = endpoint(host)
        state = node(host, "star", "star", "star-a", address, super_address)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        tls.load_verify_locations(ROOT / "cluster/tests/fixtures/star-a/ca.pem")
        tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_3
        tls.set_alpn_protocols(["h2"])
        port = int(address.rsplit(":", 1)[1])
        with socket.create_connection(("127.0.0.1", port), timeout=3) as raw:
            with tls.wrap_socket(raw, server_hostname="127.0.0.1") as secured:
                if secured.version() != "TLSv1.3" or secured.selected_alpn_protocol() != "h2":
                    raise AssertionError("TLS version or HTTP/2 ALPN mismatch")
        tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_2
        with socket.create_connection(("127.0.0.1", port), timeout=3) as raw:
            try:
                with tls.wrap_socket(raw, server_hostname="127.0.0.1"):
                    raise AssertionError("Server accepted TLS 1.2")
            except ssl.SSLError:
                pass
        report["cases"].append("tls13_only_and_h2_negotiation")
        tls.minimum_version = tls.maximum_version = ssl.TLSVersion.TLSv1_3
        idle_started = time.monotonic()
        idle = []
        # 完成 TLS/h2 协商但不发送 HTTP2 preface, 在运行其余探针期间持续占用这两条测试连接.
        for _ in range(2):
            raw = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            idle.append(stack.enter_context(tls.wrap_socket(raw, server_hostname="127.0.0.1")))
        descriptor_path = Path(f"/proc/{state['pid']}/fd")
        baseline = len(list(descriptor_path.iterdir()))
        # 半个 TLS 记录不进入业务 RPC. 等待公共握手期限后观察 FD 是否回收.
        with ExitStack() as sockets:
            for _ in range(24):
                connection = sockets.enter_context(socket.create_connection(("127.0.0.1", int(address.rsplit(":", 1)[1])), timeout=2))
                connection.sendall(b"\x16")
            peak = len(list(descriptor_path.iterdir()))
            time.sleep(6.5)
            after = len(list(descriptor_path.iterdir()))
            if not host.call("snapshot", name="star")["alive"] or after > baseline + 8:
                raise AssertionError(f"TLS handshake descriptors not reclaimed: {baseline}, {peak}, {after}")
        report["tls_descriptors"] = {
            "baseline": baseline,
            "peak": peak,
            "after_deadline": after,
            "connections": 24,
        }
        report["cases"].append("partial_tls_handshake_deadline_and_fd_cleanup")
        # 同一 gRPC Channel 上先后建立逻辑流, 并检查并发重复、非法 Hello 和存活流隔离.
        result = subprocess.run(
            [
                str(binaries / "star_rpc_probe"),
                super_address,
                address,
                str(ROOT / "cluster/tests/fixtures/star-b"),
                endpoint(host),
            ],
            cwd=ROOT,
            env=environment(),
            capture_output=True,
            text=True,
            timeout=90,
        )
        if result.returncode:
            raise AssertionError(f"RPC probe failed: {result.stdout[-4000:]} {result.stderr[-4000:]} {host.call('snapshot', name='star')}")
        report["rpc_probe"] = result.stdout.strip()
        for line in result.stdout.splitlines():
            if line.startswith("{"):
                report.update(json.loads(line))
        report["rpc_server_resources"] = host.call("resources")
        report["cases"].append("channel_reuse_duplicate_fencing_and_protocol_rejection")
        for connection in idle:
            connection.settimeout(1)
            while time.monotonic() - idle_started < 65:
                try:
                    if not connection.recv(4096):
                        break
                except socket.timeout:
                    continue
                except (ssl.SSLError, ConnectionError):
                    break
            else:
                raise AssertionError("TLS connection without HTTP/2 requests exceeded idle deadline")
        report["idle_http2_connections"] = {
            "count": len(idle),
            "observed_closed_ms": round((time.monotonic() - idle_started) * 1000),
        }
        report["cases"].append("tls_without_http2_request_idle_cleanup")
        # 停机还要取消此刻刚进入 TLS 的连接, 不能依赖前面已经自然到期的套接字证明清理有效.
        for _ in range(8):
            connection = stack.enter_context(socket.create_connection(("127.0.0.1", port), timeout=3))
            connection.sendall(b"\x16")
        host.call("stop", name="star", graceful=True)
        host.call("rebind", address="127.0.0.1", port=int(address.rsplit(":", 1)[1]))
        host.call("stop", name="supervisor", graceful=True)
        report["cases"].append("rpc_fixture_graceful_stop_and_listener_release")


def planet_before_stars(binaries, report):
    with ExitStack() as stack:
        host = Host(binaries)
        stack.callback(host.close)
        super_address = supervisor(host)
        initial = node(host, "planet", "planet", "planet-a", endpoint(host), super_address)
        if initial["status"].get("upstream") or initial["status"].get("candidates") != 0:
            raise AssertionError("Empty Galaxy unexpectedly supplied an upstream")
        # Planet 已获准但暂时没有候选时, 必须主动刷新并发现随后启动的 Star.
        star = node(host, "star", "star", "star-a", endpoint(host), super_address)
        wait_for(
            host,
            "planet",
            lambda value: (value["status"].get("upstream") or {}).get("id") == star["status"]["id"],
        )
        report["cases"].append("empty_candidates_refresh_when_star_joins")
        host.call("stop", name="planet", graceful=True)
        host.call("stop", name="star", graceful=True)
        host.call("stop", name="supervisor", graceful=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    options = parser.parse_args()
    report = {
        "scope": "C++26 logical gRPC sessions with Go Supervisor",
        "cases": [],
        "passed": False,
    }
    destination = ROOT / f"build/testkit/results/astra-rpc-{time.time_ns()}.json"
    try:
        binaries = [options.binaries / name for name in ("star", "planet", "star_rpc_probe")]
        binaries.append(ROOT / "build/supervisor/supervisor")
        report["binary_sha256"] = {str(path.resolve()): binary_digest(path) for path in binaries}
        rpc_boundaries(options.binaries.resolve(), report)
        planet_before_stars(options.binaries.resolve(), report)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        atomic_json(destination, report)
        print(json.dumps(report, indent=2, ensure_ascii=False), flush=True)
        print(f"Report: {destination}", flush=True)


if __name__ == "__main__":
    main()
