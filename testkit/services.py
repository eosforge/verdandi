"""Star/Supervisor 真实进程回归与长时测试, 只使用已经构建的二进制和公开测试凭据."""

from __future__ import annotations

import argparse
from collections import deque
from contextlib import ExitStack
import json
import os
from pathlib import Path
import signal
import select
import socket
import shutil
import subprocess
import sys
import threading
import time

from testkit.support import (
    ROOT,
    atomic_json,
    available_memory,
    environment,
    stop_process,
    temporary_directory,
)


class ServiceProcess:
    """唯一持有子进程, 有界日志与最后一份真实状态, 停止后 join 输出读取线程."""

    def __init__(self, command, log):
        self.last = {}
        self.sequence = 0
        self.tail = deque(maxlen=32)
        self.error = None
        self.lock = threading.Lock()
        self.job = None
        kwargs = {}
        if os.name == "nt":
            from testkit.windows_job import Job

            self.job = Job()
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            # 独占且隐藏的控制台允许向本进程发送 Ctrl+Break, 不触碰用户终端.
            kwargs = {
                "creationflags": subprocess.CREATE_NEW_CONSOLE | 4,
                "startupinfo": startup,
            }
        else:
            kwargs = {"start_new_session": True}
        self.process = None
        try:
            self.process = subprocess.Popen(
                list(map(str, command)),
                cwd=ROOT,
                env=environment(),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                **kwargs,
            )
            if self.job:
                self.process._verdandi_job = self.job
                self.job.attach(self.process)
            self.reader = threading.Thread(target=self._read, args=(log,), daemon=True)
            self.reader.start()
        except BaseException:
            if self.job:
                self.job.stop()
            if self.process is not None:
                stop_process(self.process)
            raise

    def _read(self, log):
        try:
            written = 0
            with log.open("w", encoding="utf-8") as target:
                while line := self.process.stdout.readline(65536):
                    if len(line) >= 65536:
                        raise RuntimeError("Service emitted an oversized log line")
                    # 预期拒绝与强制重启也不能吞掉 sanitizer 失败; 继续排空输出, 避免诊断本身阻塞子进程.
                    if any(
                        marker in line
                        for marker in (
                            "WARNING: ThreadSanitizer:",
                            "FATAL: ThreadSanitizer:",
                            "ERROR: AddressSanitizer:",
                            "ERROR: LeakSanitizer:",
                            "runtime error:",
                        )
                    ):
                        self.error = "Service sanitizer diagnostic: " + line.rstrip()
                    with self.lock:
                        self.tail.append(line.rstrip())
                        try:
                            record = json.loads(line)
                        except ValueError:
                            record = {}
                        if record.get("event") == "status":
                            self.sequence += 1
                            self.last = record["fields"]
                    # 长时运行持续消费日志但只保留前 4 MiB 和最近 32 行, 内存/磁盘均有界.
                    if written < 4 * 1024 * 1024:
                        target.write(line)
                        target.flush()
                        written += len(line.encode("utf-8"))
        except BaseException as error:
            self.error = str(error)

    def snapshot(self):
        if self.error:
            with self.lock:
                raise RuntimeError(self.error + "\n" + "\n".join(self.tail))
        with self.lock:
            return {
                "alive": self.process.poll() is None,
                "exit": self.process.poll(),
                "sequence": self.sequence,
                "status": dict(self.last),
                "tail": list(self.tail),
                "pid": self.process.pid,
            }

    def stop(self, graceful):
        try:
            if graceful and self.process.poll() is None:
                if os.name == "nt":
                    # 临时助手仅附着这个自有子进程的独占控制台, 不在测试主进程切换控制台.
                    subprocess.run(
                        [
                            getattr(sys, "_base_executable", sys.executable),
                            "-B",
                            "-m",
                            "testkit.services",
                            "--signal-pid",
                            str(self.process.pid),
                        ],
                        cwd=ROOT,
                        env=environment(),
                        creationflags=subprocess.CREATE_NO_WINDOW,
                        capture_output=True,
                        check=True,
                        timeout=5,
                    )
                else:
                    os.killpg(self.process.pid, signal.SIGTERM)
            if graceful:
                # 在发送信号前已经异常退出也必须失败, 不能因 poll() 非空而漏过退出码校验.
                code = self.process.wait(timeout=8)
                if code != 0:
                    raise RuntimeError(
                        f"Graceful service stop returned {code}: {self.snapshot()['tail']}"
                    )
        finally:
            stop_process(self.process)
            self.reader.join(timeout=5)
            self.process.stdout.close()
            if self.reader.is_alive():
                raise RuntimeError("Service output reader did not stop")
            if self.error:
                with self.lock:
                    raise RuntimeError(self.error + "\n" + "\n".join(self.tail))


def star_binary_directory(implementation):
    """显式选择独立产物目录, 不因某个程序缺失而静默切换实现."""
    if implementation != "cpp":
        raise ValueError("Unknown Star implementation: " + str(implementation))
    return ROOT / "build/cluster-cpp/release"


class Host:
    """本机进程集合; SSH 模式复用同一实现, 断开控制连接后自动清理全部子进程."""

    def __init__(self, star_binaries=None):
        self.stack = ExitStack()
        self.directory = Path(
            self.stack.enter_context(temporary_directory("services-"))
        )
        self.processes = {}
        self.counter = 0
        # 默认采用已验收的 C++ 入口. Rust 比较必须显式选择, 不覆盖或伪装旧二进制.
        self.star_binaries = (
            Path(star_binaries) if star_binaries else star_binary_directory("cpp")
        )

    def call(self, action, **args):
        if action == "verify_cli":
            suffix = ".exe" if os.name == "nt" else ""
            for kind in ("star", "planet", "supervisor"):
                binary = (
                    ROOT / f"build/supervisor/{kind}{suffix}"
                    if kind == "supervisor"
                    else self.star_binaries / f"{kind}{suffix}"
                )
                for option, expected in [
                    ("--version", 0),
                    ("--help", 0),
                    ("--unknown-test-option", 2),
                ]:
                    result = subprocess.run(
                        [str(binary), option],
                        cwd=ROOT,
                        env=environment(),
                        stdin=subprocess.DEVNULL,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=10,
                        creationflags=(
                            subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
                        ),
                    )
                    if (
                        result.returncode != expected
                        or not result.stdout + result.stderr
                    ):
                        raise AssertionError(
                            f"{kind} {option}: unexpected CLI result {result.returncode}"
                        )
                    if (
                        option == "--help"
                        and kind != "supervisor"
                        and (
                            "login.json" not in result.stdout
                            or "process key" in result.stdout
                        )
                    ):
                        raise AssertionError(f"{kind}: stale identity instructions")
            return True
        if action == "reserve":
            with socket.socket() as listener:
                listener.bind((args["address"], 0))
                return listener.getsockname()[1]
        if action == "start":
            name, kind = args["name"], args["kind"]
            if name in self.processes:
                raise ValueError("Process name already owned")
            suffix = ".exe" if os.name == "nt" else ""
            if kind == "supervisor":
                command = [
                    ROOT / f"build/supervisor/supervisor{suffix}",
                    f"--listen={args['management']}",
                    f"--star-listen={args['address']}",
                    "--cluster=alpha",
                    f"--max-members={args.get('max_members', 4)}",
                    f"--max-connections={args.get('max_connections', 32)}",
                    f"--identity={ROOT / 'cluster/tests/fixtures/supervisor'}",
                    f"--members={self.directory / 'members.db'}",
                ]
            elif kind in {"star", "planet"}:
                role = args["role"]
                if role not in {
                    "star-a",
                    "star-b",
                    "star-c",
                    "star-d",
                    "planet-a",
                    "planet-b",
                    "wrong-cluster",
                    "expired",
                    "rogue",
                }:
                    raise ValueError("Unknown test identity")
                identity = ROOT / "cluster/tests/fixtures" / role
                if login_case := args.get("login_case"):
                    if login_case not in {"wrong-password", "malformed"}:
                        raise ValueError("Unknown login test case")
                    # 只改本次测试拥有的副本, 公开夹具与用户部署材料保持独立.
                    copied = self.directory / f"identity-{self.counter + 1}"
                    copied.mkdir()
                    for filename in (
                        "ca.pem",
                        "cert.pem",
                        "key.pem",
                        "admission.pub",
                        "login.json",
                    ):
                        shutil.copyfile(identity / filename, copied / filename)
                    login = {
                        "username": "stars",
                        "password": "wrong-public-test-password",
                    }
                    if login_case == "malformed":
                        login["unexpected"] = True
                    (copied / "login.json").write_text(
                        json.dumps(login), encoding="utf-8"
                    )
                    identity = copied
                command = [
                    self.star_binaries / f"{kind}{suffix}",
                    f"--listen={args['address']}",
                    f"--super={args['supervisor']}",
                    "--cluster=alpha",
                    f"--group={args.get('group', 'default')}",
                    f"--max-members={args.get('max_members', 4)}",
                    f"--identity={identity}",
                    "--status-interval-seconds=1",
                    "--heartbeat-interval-ms=200",
                    "--pong-timeout-ms=1500",
                ]
            else:
                raise ValueError("Unknown service")
            self.counter += 1
            self.processes[name] = ServiceProcess(
                command, self.directory / f"{self.counter}.log"
            )
            return self.processes[name].process.pid
        if action == "snapshot":
            return self.processes[args["name"]].snapshot()
        if action == "stop":
            process = self.processes.pop(args["name"])
            process.stop(args.get("graceful", True))
            return True
        if action == "rebind":
            with socket.socket() as listener:
                if os.name != "nt":
                    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind((args["address"], args["port"]))
            return True
        if action == "memory":
            return available_memory()
        if action == "resources":
            if sys.platform != "linux":
                return None
            samples = {}
            for name, owned in self.processes.items():
                if owned.process.poll() is None:
                    directory = Path(f"/proc/{owned.process.pid}")
                    try:
                        fields = dict(
                            line.split(":", 1)
                            for line in (directory / "status").read_text().splitlines()
                        )
                        samples[name] = {
                            "rss_kib": int(fields["VmRSS"].split()[0]),
                            "threads": int(fields["Threads"]),
                            "fds": len(list((directory / "fd").iterdir())),
                        }
                    except FileNotFoundError:
                        # 进程恰好退出时不伪造零资源记录, 存活与退出码仍由组网断言检查.
                        continue
            return samples
        raise ValueError("Unknown host action")

    def close(self):
        errors = []
        for process in list(self.processes.values()):
            try:
                process.stop(False)
            except BaseException as error:
                errors.append(error)
        self.processes.clear()
        # 即使某个进程退出检查失败, 也继续释放本次持有的临时目录.
        try:
            self.stack.close()
        except BaseException as error:
            errors.append(error)
        if errors:
            raise ExceptionGroup("Service cleanup failed", errors)


class RemoteHost:
    """SSH 控制通道只承载有限 JSON 操作, 校验已知主机密钥, 不安装软件或执行任意远程命令."""

    def __init__(self, config):
        import paramiko
        import shlex

        implementation = config.get("star_implementation", "cpp")
        star_binary_directory(implementation)
        self.client = paramiko.SSHClient()
        self.client.load_system_host_keys()
        known = ROOT / "build/testkit/known_hosts"
        if known.exists():
            self.client.load_host_keys(str(known))
        self.client.connect(
            config["host"],
            username=config["username"],
            password=config.get("password"),
            timeout=5,
            banner_timeout=5,
            auth_timeout=5,
            look_for_keys=False,
            allow_agent=False,
        )
        project = config.get("project", "/home/ubuntu/verdandi")
        command = f"cd {shlex.quote(project)} && exec {shlex.quote(project + '/build/tools/python-build/bin/python')} -B -m testkit.services --agent --implementation={implementation}"
        self.stdin, self.stdout, self.stderr = self.client.exec_command(
            command, timeout=30
        )

    def call(self, action, **args):
        self.stdin.write(json.dumps({"action": action, **args}) + "\n")
        self.stdin.flush()
        line = self.stdout.readline(65536)
        if not line or len(line) >= 65536:
            raise RuntimeError("Remote service control stream closed or oversized")
        value = json.loads(line)
        if not value["ok"]:
            raise RuntimeError(value["error"])
        return value["result"]

    def close(self):
        try:
            self.call("close")
        finally:
            self.client.close()


def wait_for(host, name, predicate, timeout=20):
    deadline = time.monotonic() + timeout
    latest = {}
    while time.monotonic() < deadline:
        latest = host.call("snapshot", name=name)
        if predicate(latest):
            return latest
        if not latest["alive"]:
            raise RuntimeError(f"{name} exited: {latest}")
        time.sleep(0.1)
    raise TimeoutError(f"{name} did not reach expected state: {latest}")


def converge(nodes, members, active=None):
    active = members if active is None else active
    baselines = [host.call("snapshot", name=name)["sequence"] for host, name in nodes]
    deadline = time.monotonic() + 20
    latest = []
    while time.monotonic() < deadline:
        latest = [host.call("snapshot", name=name) for host, name in nodes]
        complete = True
        for baseline, value in zip(baselines, latest):
            state = value["status"]
            complete &= (
                value["alive"]
                and value["sequence"] > baseline
                and state.get("initialized")
                and state.get("members") == members
            )
            complete &= (
                state.get("inbound") == active - 1
                and state.get("outbound") == active - 1
            )
        if complete:
            return
        if not all(value["alive"] for value in latest):
            break
        time.sleep(0.1)
    # 保留每个节点的有限诊断, 避免只报告最先等待者而遗漏实际失败的另一端.
    evidence = [
        {
            "name": name,
            "alive": value["alive"],
            "status": value["status"],
            "tail": value["tail"][-6:],
        }
        for (_, name), value in zip(nodes, latest)
    ]
    raise TimeoutError(f"Mesh did not converge: {evidence}")


def campaign(options, report):
    with ExitStack() as stack:
        local = Host(getattr(options, "star_binaries", None))
        stack.callback(local.close)
        other = local
        remote_address = options.address
        if options.remote_config:
            config = json.loads(Path(options.remote_config).read_text(encoding="utf-8"))
            other = RemoteHost(config)
            stack.callback(other.close)
            remote_address = config["host"]
        endpoints = [
            (local, options.address),
            (other, remote_address),
            (other, remote_address),
            (local, options.address),
        ]
        ports = [host.call("reserve", address=address) for host, address in endpoints]
        super_port = local.call("reserve", address=options.address)
        management = local.call("reserve", address=options.address)
        supervisor = f"{options.address}:{super_port}"

        local.call("verify_cli")
        if other is not local:
            other.call("verify_cli")
        report["cases"].append("service_cli_help_version_and_invalid_options")

        def start_super():
            local.call(
                "start",
                name="supervisor",
                kind="supervisor",
                address=supervisor,
                management=f"{options.address}:{management}",
            )
            wait_for(
                local,
                "supervisor",
                lambda value: any(
                    "supervisor_registration_started" in line for line in value["tail"]
                ),
            )

        def start_star(index):
            host, address = endpoints[index]
            host.call(
                "start",
                name=f"star-{index}",
                kind="star",
                role=f"star-{chr(97 + index)}",
                address=f"{address}:{ports[index]}",
                supervisor=supervisor,
                group="west" if index == 2 else "east",
            )
            return host, f"star-{index}"

        start_super()
        nodes = [start_star(index) for index in range(3)]
        converge(nodes, 3)
        report["cases"].append("concurrent_three_star_full_mesh")
        local.call("stop", name="supervisor", graceful=True)
        fourth = start_star(3)
        wait_for(
            *fourth,
            lambda value: value["sequence"] >= 2
            and not value["status"].get("initialized"),
        )
        converge(nodes, 3)
        report["cases"].append("supervisor_offline_existing_mesh_and_new_star_wait")
        start_super()
        nodes.append(fourth)
        converge(nodes, 4)
        report["cases"].append("persistent_supervisor_restart_and_waiting_star_join")
        # 每次重启必须获取新 id, 其他节点随后只保留当前实例的两条连接.
        for graceful in [True, False]:
            host, name = nodes[2]
            old_id = host.call("snapshot", name=name)["status"]["id"]
            host.call("stop", name=name, graceful=graceful)
            converge([node for index, node in enumerate(nodes) if index != 2], 4, 3)
            start_star(2)
            value = wait_for(
                host, name, lambda value: value["status"].get("initialized")
            )
            if value["status"]["id"] == old_id:
                raise AssertionError("Star restart reused issued process id")
            converge(nodes, 4)
        report["cases"].append("graceful_and_forced_star_restart")
        # 未知账号和无效本地 TLS 证书均不能进入 initialized 状态.
        for role in ["wrong-cluster", "expired", "rogue"]:
            port = local.call("reserve", address=options.address)
            local.call(
                "start",
                name=role,
                kind="star",
                role=role,
                address=f"{options.address}:{port}",
                supervisor=supervisor,
            )
            value = wait_for(local, role, lambda value: not value["alive"])
            if value["exit"] != 1 or value["status"].get("initialized"):
                raise AssertionError(f"Invalid identity accepted: {role}")
            local.call("stop", name=role, graceful=False)
            local.call("rebind", address=options.address, port=port)
        report["cases"].append("invalid_identities_fail_closed")

        for login_case in ("wrong-password", "malformed"):
            port = local.call("reserve", address=options.address)
            local.call(
                "start",
                name="bad-login",
                kind="star",
                role="star-a",
                login_case=login_case,
                address=f"{options.address}:{port}",
                supervisor=supervisor,
            )
            value = wait_for(local, "bad-login", lambda value: not value["alive"])
            if value["exit"] != 1 or value["status"].get("initialized"):
                raise AssertionError("Invalid account login entered initialized state")
            if "wrong-public-test-password" in "\n".join(value["tail"]):
                raise AssertionError("Service logs leaked password")
            local.call("stop", name="bad-login", graceful=False)
            local.call("rebind", address=options.address, port=port)
        report["cases"].append("invalid_login_configuration_and_password_fail_closed")

        # 使用真实 Go 准入服务检查账号角色, 不能通过请求字段冒充另一种部署.
        for kind, role in [("planet", "star-a"), ("star", "planet-a")]:
            port = local.call("reserve", address=options.address)
            local.call(
                "start",
                name="wrong-role",
                kind=kind,
                role=role,
                address=f"{options.address}:{port}",
                supervisor=supervisor,
            )
            value = wait_for(local, "wrong-role", lambda value: not value["alive"])
            if value["exit"] != 1 or value["status"].get("initialized"):
                raise AssertionError("Account role escalation accepted")
            local.call("stop", name="wrong-role", graceful=False)
            local.call("rebind", address=options.address, port=port)
        report["cases"].append("star_planet_account_roles_fail_closed")

        planet_endpoints = [(local, options.address), (other, remote_address)]
        planet_ports = [
            host.call("reserve", address=address) for host, address in planet_endpoints
        ]

        def start_planet(index):
            host, address = planet_endpoints[index]
            name = f"planet-{index}"
            host.call(
                "start",
                name=name,
                kind="planet",
                role=f"planet-{chr(97 + index)}",
                address=f"{address}:{planet_ports[index]}",
                supervisor=supervisor,
                group="west",
            )
            return host, name

        def planet_connected(node, *, group=None, excluded=None):
            baseline = node[0].call("snapshot", name=node[1])["sequence"]

            def ready(value):
                state = value["status"]
                upstream = state.get("upstream") or {}
                return (
                    value["sequence"] > baseline
                    and state.get("initialized")
                    and upstream.get("id")
                    and (group is None or upstream.get("group") == group)
                    and upstream.get("id") != excluded
                )

            return wait_for(*node, ready)

        planets = [start_planet(0)]
        initial = planet_connected(planets[0], group="west")
        planet_id = initial["status"]["id"]
        old_upstream = initial["status"]["upstream"]["id"]
        converge(nodes, 4)
        report["cases"].append("planet_prefers_local_group_without_joining_star_mesh")

        # 断开管理端和唯一同组 Star 后, 已准入 Planet 必须靠原有候选切换到跨组入口.
        local.call("stop", name="supervisor", graceful=True)
        nodes[2][0].call("stop", name=nodes[2][1], graceful=False)
        switched = planet_connected(planets[0], group="east", excluded=old_upstream)
        if switched["status"]["id"] != planet_id:
            raise AssertionError("Planet failover changed its process identity")
        report["cases"].append("planet_cross_group_failover_while_supervisor_offline")
        planets.append(start_planet(1))
        wait_for(
            *planets[1],
            lambda value: value["sequence"] >= 2
            and not value["status"].get("initialized"),
        )
        start_super()
        start_star(2)
        converge(nodes, 4)
        planet_connected(planets[1])
        planet_connected(planets[0], group="east")
        report["cases"].append(
            "new_planet_waits_for_supervisor_and_healthy_upstream_stays"
        )

        # 单上游不是仅检查 Planet 的一个字段, 还从所有 Star 的实际入站索引交叉验证.
        deadline = time.monotonic() + 20
        while (
            sum(
                host.call("snapshot", name=name)["status"].get("planet_inbound", 0)
                for host, name in nodes
            )
            != 2
        ):
            if time.monotonic() >= deadline:
                raise AssertionError("Stars do not own exactly two Planet sessions")
            time.sleep(0.1)
        report["cases"].append("one_active_upstream_per_planet")
        if options.mode == "soak":
            started = time.monotonic()
            restarts = 0
            minimum_available = available_memory()
            next_progress = started + 60
            resource_hosts = [("local", local)] + (
                [("remote", other)] if other is not local else []
            )

            def sample_resources():
                samples = {
                    label: host.call("resources") for label, host in resource_hosts
                }
                report.setdefault("resources_initial", samples)
                report["resources_final"] = samples
                peaks = report.setdefault("resources_peak", {})
                for label, values in samples.items():
                    for name, metrics in (values or {}).items():
                        peak = peaks.setdefault(f"{label}/{name}", {})
                        for metric, value in metrics.items():
                            peak[metric] = max(peak.get(metric, 0), value)

            def record_soak():
                # 失败或 Ctrl+C 也保留已完成循环和实际时长, 不等到正常结束才填入报告.
                report["soak"] = {
                    "requested_seconds": options.duration,
                    "elapsed_seconds": round(time.monotonic() - started, 3),
                    "restart_cycles": restarts,
                    "minimum_available_mib": minimum_available // 1024**2,
                    "stars": 4,
                    "planets": 2,
                }

            while time.monotonic() - started < options.duration:
                sample_resources()
                minimum_available = min(
                    minimum_available, local.call("memory"), other.call("memory")
                )
                record_soak()
                if minimum_available < 384 * 1024**2:
                    raise RuntimeError("Available memory fell below 384 MiB")
                upstream_id = planet_connected(planets[0])["status"]["upstream"]["id"]
                index = next(
                    index
                    for index, (host, name) in enumerate(nodes)
                    if host.call("snapshot", name=name)["status"]["id"] == upstream_id
                )
                host, name = nodes[index]
                host.call("stop", name=name, graceful=False)
                planet_connected(planets[0], excluded=upstream_id)
                start_star(index)
                converge(nodes, 4)
                local.call("stop", name="supervisor", graceful=False)
                converge(nodes, 4)
                start_super()
                restarts += 1
                record_soak()
                if time.monotonic() >= next_progress:
                    print(
                        json.dumps({"event": "soak_progress", **report["soak"]}),
                        flush=True,
                    )
                    next_progress = time.monotonic() + 60
                time.sleep(
                    min(2, max(0, options.duration - (time.monotonic() - started)))
                )
            sample_resources()
            record_soak()
        for host, name in reversed(planets):
            host.call("stop", name=name, graceful=True)
        for host, name in reversed(nodes):
            host.call("stop", name=name, graceful=True)
        local.call("stop", name="supervisor", graceful=True)
        for (host, address), port in zip(endpoints, ports):
            host.call("rebind", address=address, port=port)
        for (host, address), port in zip(planet_endpoints, planet_ports):
            host.call("rebind", address=address, port=port)
        local.call("rebind", address=options.address, port=super_port)
        local.call("rebind", address=options.address, port=management)
        report["cases"].append("signals_process_exit_and_port_reuse")


def signal_windows_child(pid):
    import ctypes

    api = ctypes.WinDLL("kernel32", use_last_error=True)
    from ctypes import wintypes

    handler_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)
    api.AttachConsole.argtypes, api.AttachConsole.restype = [
        wintypes.DWORD
    ], wintypes.BOOL
    api.SetConsoleCtrlHandler.argtypes, api.SetConsoleCtrlHandler.restype = [
        handler_type,
        wintypes.BOOL,
    ], wintypes.BOOL
    api.GenerateConsoleCtrlEvent.argtypes, api.GenerateConsoleCtrlEvent.restype = [
        wintypes.DWORD,
        wintypes.DWORD,
    ], wintypes.BOOL
    handler = handler_type(lambda event: True)
    # 只从专用助手的控制台脱离, 不能在调用者或用户终端中执行此步骤.
    api.FreeConsole()
    if not api.AttachConsole(pid):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not api.SetConsoleCtrlHandler(
            handler, True
        ) or not api.GenerateConsoleCtrlEvent(1, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        time.sleep(0.1)
    finally:
        api.FreeConsole()


def agent_lines():
    if os.name == "nt":
        raise RuntimeError("SSH service agent currently requires Linux")
    pending = bytearray()
    deadline = time.monotonic() + 60
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([sys.stdin], [], [], remaining)[0]:
            raise TimeoutError("Service control lease expired")
        chunk = os.read(sys.stdin.fileno(), 65536)
        if not chunk:
            return
        pending.extend(chunk)
        if len(pending) >= 65536:
            raise RuntimeError("Oversized service control command")
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            yield line.decode("utf-8")
            deadline = time.monotonic() + 60


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=["regression", "soak"], default="regression")
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--address", default="127.0.0.1")
    parser.add_argument("--remote-config")
    parser.add_argument(
        "--implementation",
        choices=("cpp",),
        default="cpp",
        help="Service implementation; never falls back to another binary",
    )
    parser.add_argument(
        "--star-binaries",
        type=Path,
        help="Explicit local Star/Planet binary directory, overriding implementation output path",
    )
    parser.add_argument("--agent", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--signal-pid", type=int, help=argparse.SUPPRESS)
    options = parser.parse_args()
    if options.signal_pid:
        signal_windows_child(options.signal_pid)
        return
    if (
        options.implementation == "cpp"
        and sys.platform != "linux"
        and not options.star_binaries
    ):
        parser.error("C++26 services require Linux; the Rust service is retired")
    options.star_binaries = options.star_binaries or star_binary_directory(
        options.implementation
    )
    if options.agent:
        host = Host(options.star_binaries)
        try:
            # SSH 断网未及时产生 EOF 时也清理. 控制协议必须在 60 秒内完成一条命令.
            for line in agent_lines():
                try:
                    request = json.loads(line)
                    action = request.pop("action")
                    if action == "close":
                        host.close()
                        print(json.dumps({"ok": True, "result": True}), flush=True)
                        return
                    result = host.call(action, **request)
                    print(json.dumps({"ok": True, "result": result}), flush=True)
                except Exception as error:
                    print(json.dumps({"ok": False, "error": str(error)}), flush=True)
        finally:
            host.close()
        return
    if not 1 <= options.duration <= 604800:
        parser.error("duration must be between 1 and 604800 seconds")
    report = {
        "status": "running",
        "platform": sys.platform,
        "mode": options.mode,
        "mixed_hosts": bool(options.remote_config),
        "star_binaries": str(options.star_binaries.resolve()),
        "cases": [],
    }
    result = ROOT / "build/testkit/results" / f"services-{time.time_ns()}.json"
    started = time.monotonic()
    try:
        campaign(options, report)
        report["status"] = "pass"
        report["cleanup"] = (
            "processes joined, ports reusable, owned temporary directories removed"
        )
    except BaseException as error:
        report["status"] = "fail"
        report["error"] = str(error)
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        atomic_json(result, report)
        print(
            json.dumps({"result": str(result), **report}, ensure_ascii=False),
            flush=True,
        )


if __name__ == "__main__":
    main()
