"""严格配置、Linux 本地原生采集与义务门禁; 不安装依赖, 校验和判定不启动测试."""

import argparse
import ctypes
import datetime as dt
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import selectors
import shutil
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SCHEMAS = Path(__file__).with_name("schemas")
LIMIT = 8 * 1024 * 1024
TRUST = {"local": 0, "ci": 1, "release": 2}


class Invalid(ValueError):
    """带稳定原因码的输入/证据拒绝, 不等同于测试失败."""


def reject(code, detail=""):
    raise Invalid(f"{code}: {detail}")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def decode(data):
    if len(data) > LIMIT:
        reject("json-size")

    def pairs(entries):
        result = {}
        for key, value in entries:
            if key in result:
                reject("duplicate-key", key)
            result[key] = value
        return result

    def walk(value, depth=0):
        if depth > 32:
            reject("json-depth")
        if isinstance(value, float) and not math.isfinite(value):
            reject("non-finite")
        if isinstance(value, dict):
            for child in value.values():
                walk(child, depth + 1)
        elif isinstance(value, list):
            for child in value:
                walk(child, depth + 1)

    try:
        result = json.loads(data, object_pairs_hook=pairs, parse_constant=lambda value: reject("non-finite", value))
        walk(result)
        return result
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        reject("json-invalid", type(error).__name__)


def read(path, maximum=LIMIT):
    with Path(path).open("rb") as stream:
        data = stream.read(maximum + 1)
    if len(data) > maximum:
        reject("file-size", str(path))
    return data


def stamp(value):
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?Z", value):
        reject("timestamp", value)
    try:
        return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        reject("timestamp", value)


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def inside(root, name):
    path = PurePosixPath(name)
    if not name or "\\" in name or ":" in name or path.is_absolute() or any(part in ("", ".", "..") for part in name.split("/")):
        reject("path-invalid", name)
    root = Path(root).resolve()
    target = root.joinpath(*path.parts)
    current = root
    for part in path.parts:
        current /= part
        if current.is_symlink():
            reject("path-symlink", name)
    if not target.resolve().is_relative_to(root):
        reject("path-escape", name)
    return target


def write_new(path, value):
    data = value if isinstance(value, bytes) else encode(value)
    with Path(path).open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    return {"path": Path(path).name, "bytes": len(data), "sha256": digest(data)}


def schema(value, name):
    try:
        from jsonschema import Draft202012Validator
        from referencing import Registry
    except ImportError:
        reject("schema-engine-unavailable", "requires existing jsonschema with Draft202012Validator; no automatic installation")
    definition = decode(read(SCHEMAS / f"{name}.schema.json"))
    # 本地 $defs 足够, 空注册表加拒绝回调杜绝隐式网络解析.
    registry = Registry(retrieve=lambda uri: reject("schema-remote-reference", uri))
    Draft202012Validator.check_schema(definition)
    validator = Draft202012Validator(definition, registry=registry)
    error = next(validator.iter_errors(value), None)
    if error is not None:
        reject("schema-invalid", f"{name}: /{'/'.join(map(str, error.absolute_path))}: {error.validator}")


def unique(items, field="id"):
    result = {}
    for item in items:
        key = item[field]
        if key in result:
            reject("duplicate-id", key)
        result[key] = item
    return result


def closure(registry, requested):
    rules = unique(registry["rules"])
    visited, active = set(), set()

    def visit(name):
        if name not in rules:
            reject("unknown-rule", name)
        if name in active:
            reject("rule-cycle", name)
        if name in visited:
            return
        active.add(name)
        for dependency in rules[name]["depends_on"]:
            visit(dependency)
        active.remove(name)
        visited.add(name)

    for name in requested:
        visit(name)
    return sorted(visited)


def expand(config):
    result = {}
    for component in config["components"]:
        for requirement in component["requirements"]:
            for target in component["platforms"]:
                key = "/".join((component["id"], requirement["rule"], requirement["contract"], target, requirement["scenario"]))
                if key in result:
                    reject("duplicate-obligation", key)
                result[key] = {**requirement, "component": component["id"], "platform": target}
    return result


def configuration(path, root=ROOT):
    raw = read(path)
    config = decode(raw)
    schema(config, "development")
    rules_raw = read(inside(root, config["rules"]))
    registry = decode(rules_raw)
    schema(registry, "rules")
    rules = unique(registry["rules"])
    closure(registry, rules)
    platforms = unique(config["platforms"])
    checks = unique(config["checks"])
    components = unique(config["components"])
    for component in components.values():
        if component["role"] == "storage-core" and component["level"] != "L3":
            reject("minimum-level", component["id"])
        if set(component["applicability"]) != set(rules):
            reject("rule-mapping-incomplete", component["id"])
        applicable = {key for key, value in component["applicability"].items() if value["status"] == "applicable"}
        if not set(registry["global"]) <= applicable:
            reject("global-rule-excluded", component["id"])
        closure(registry, applicable | set(registry["global"]))
        if not set(component["platforms"]) <= platforms.keys():
            reject("unknown-platform", component["id"])
        contracts = unique(component["contracts"])
        mapped = set()
        for requirement in component["requirements"]:
            if requirement["rule"] not in rules:
                reject("unknown-rule", requirement["rule"])
            if requirement["rule"] not in applicable or requirement["contract"] not in contracts:
                reject("invalid-requirement", component["id"])
            mapped.add(requirement["rule"])
            for key in requirement["checks"]:
                if key not in checks or checks[key]["component"] != component["id"]:
                    reject("unknown-check", key)
                check = checks[key]
                if check["adapter"] == "go-test-json" and requirement["contract"] not in {probe["contract"] for probe in check["probes"]}:
                    reject("unobservable-contract", requirement["contract"])
        if component["level"] == "L3":
            needed = {"structure", "mcdc", "mutation", "finite-model", "fault-matrix", "delivery-matrix"}
            if not needed <= {item["scenario"] for item in component["requirements"]}:
                reject("l3-capability-missing", component["id"])
        if mapped != applicable or {item["contract"] for item in component["requirements"]} != set(contracts):
            reject("unmapped-obligation", component["id"])
        for name in component["paths"]:
            inside(root, name)
        convergence = component["convergence"]
        if convergence["deadline"] is not None:
            stamp(convergence["deadline"])
    for check in checks.values():
        if check["component"] not in components:
            reject("unknown-component", check["id"])
        if check["adapter"] == "go-test-json":
            unique(check["probes"], "contract")
            if not {probe["test"] for probe in check["probes"]} <= set(check["expected_tests"]):
                reject("probe-test-missing", check["id"])
            if check["argv"][:3] != ["go", "test", "-json"] or "-count=1" not in check["argv"]:
                reject("unsupported-go-command", check["id"])
            for key, value in {"GOPROXY": "off", "GOSUMDB": "off", "GOTOOLCHAIN": "local", "GOWORK": "off", "GOFLAGS": "-mod=readonly"}.items():
                if check["environment"].get(key) != value:
                    reject("offline-environment", key)
    for name in config["inputs"] + [config["design"], config["standard"]]:
        inside(root, name)
    expand(config)
    return config, registry, {"policy": digest(raw), "rules": digest(rules_raw)}


def source(root):
    # 绑定整个 Astra 自有工作区, 包含未跟踪、删除和 index 状态; 忽略缓存不进入源码清单.
    def git(*args):
        return subprocess.check_output(["git", "-C", str(root), *args], timeout=30)

    names = git("ls-files", "-z", "--cached", "--others", "--exclude-standard").decode("utf-8").split("\0")
    files = []
    for name in sorted(set(filter(None, names))):
        path = inside(root, name)
        files.append({"path": name, "sha256": file_digest(path) if path.is_file() else None})
    return {"commit": git("rev-parse", "HEAD").decode().strip(), "status": git("status", "--porcelain=v1", "-z", "--", ".").decode(), "files": files}


def file_digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while block := stream.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def authority(path, expected, pins, root, config):
    raw = read(path)
    if digest(raw) != expected:
        reject("authority-hash")
    value = decode(raw)
    schema(value, "authority")
    if stamp(value["expires_at"]) <= dt.datetime.now(dt.timezone.utc) or value["revoked"]:
        reject("authority-expired-or-revoked")
    actual = identities(pins, root, config)
    if actual != value["pins"]:
        reject("authority-pins")
    for exception in value["exceptions"]:
        stamp(exception["expires_at"])
    unique(value["exceptions"])
    return value


def identities(pins, root, config):
    actual = {
        **pins,
        "collector": file_digest(Path(__file__)),
        "design": file_digest(inside(root, config["design"])),
        "standard": file_digest(inside(root, config["standard"])),
    }
    actual["schemas"] = digest(encode({p.name: file_digest(p) for p in sorted(SCHEMAS.glob("*.schema.json"))}))
    return actual


def go_observations(data, check):
    tests, probes = {}, {}
    package_started = False
    package_passed = False
    failures = []
    for line in data.splitlines():
        event = decode(line)
        if not isinstance(event, dict) or event.get("Package") != check["package"]:
            reject("go-package")
        action, test = event.get("Action"), event.get("Test")
        if not isinstance(action, str) or (test is not None and not isinstance(test, str)):
            reject("go-event-shape")
        if action not in {"start", "run", "pause", "cont", "pass", "fail", "skip", "output", "bench"}:
            reject("go-action", action)
        if package_passed:
            reject("go-event-after-terminal")
        if action in ("fail", "skip"):
            failures.append(f"{action}:{test or check['package']}")
        if not test:
            if action == "start":
                if package_started:
                    reject("go-duplicate-package")
                package_started = True
            if action == "pass":
                if not package_started or package_passed:
                    reject("go-terminal-order")
                package_passed = True
            continue
        if action == "run":
            if test in tests:
                reject("go-duplicate-test", test)
            tests[test] = "running"
        elif action in ("pass", "fail", "skip"):
            if tests.get(test) != "running":
                reject("go-test-order", test)
            tests[test] = action
        elif action == "output":
            output = event.get("Output", "")
            if not isinstance(output, str):
                reject("go-output-shape")
            marker = re.fullmatch(r"\s*(?:[^\n]+\.go:\d+: )?ASTRA-CONTRACT (\{[^\n]*\})\s*", output)
            if marker:
                probe = decode(marker.group(1).encode())
                if not isinstance(probe, dict) or set(probe) != {"contract", "hits"}:
                    reject("probe-shape")
                if not isinstance(probe["contract"], str):
                    reject("probe-contract")
                if type(probe["hits"]) is not int or probe["hits"] < 1:
                    reject("probe-empty")
                key = (test, probe["contract"])
                if key in probes or tests.get(test) != "running":
                    reject("probe-duplicate-or-late")
                probes[key] = probe["hits"]
    if not package_passed:
        failures.append("go-package-incomplete")
    if any(state != "pass" for state in tests.values()):
        failures.append("go-test-incomplete")
    for test in check["expected_tests"]:
        if tests.get(test) != "pass":
            failures.append(f"go-required-test:{test}")
    observed = {}
    for probe in check["probes"]:
        count = probes.get((probe["test"], probe["contract"]), 0)
        if count < probe["minimum_hits"]:
            failures.append(f"probe-budget:{probe['contract']}")
        else:
            observed[probe["contract"]] = count
    if set(probes) - {(probe["test"], probe["contract"]) for probe in check["probes"]}:
        failures.append("undeclared-probe")
    return observed, failures


def reference(directory, item, maximum=LIMIT):
    data = read(inside(directory, item["path"]), maximum)
    if len(data) != item["bytes"] or digest(data) != item["sha256"]:
        reject("artifact-integrity", item["path"])
    return data


def decide(config, observations, hard, exceptions, now):
    obligations = expand(config)
    passed, exempt, details = [], [], {}
    for key, item in obligations.items():
        states = [observations.get((check, item["platform"], item["contract"]), "not-run") for check in item["checks"]]
        if states and all(state == "passed" for state in states):
            passed.append(key)
        else:
            details[key] = states
    for exception in exceptions:
        if exception["obligation"] not in obligations:
            hard.append("exception-out-of-scope:" + exception["id"])
        elif not exception["revoked"] and stamp(exception["expires_at"]) > now and exception["obligation"] not in passed:
            exempt.append(exception["obligation"])
    missing = sorted(set(obligations) - set(passed) - set(exempt))
    state = "blocked" if missing or hard else "eligible-with-exceptions" if exempt else "eligible"
    return {
        "status": state,
        "required": sorted(obligations),
        "passed": sorted(passed),
        "exceptions": sorted(set(exempt)),
        "missing": missing,
        "hard": sorted(set(hard)),
        "details": details,
    }


def gate(config, pins, approved, directory, root=ROOT):
    evidence = decode(read(Path(directory) / "evidence.json"))
    schema(evidence, "evidence")
    hard = []
    if not evidence["checks"]:
        hard.append("no-native-executions")
    if evidence["policy"] != pins["policy"] or evidence["rules"] != pins["rules"]:
        reject("evidence-policy")
    if evidence["collector"] != approved["pins"]["collector"]:
        reject("evidence-collector")
    if evidence["kind"] != "native-local" or evidence["trust"] != "local":
        reject("unsupported-provenance")
    before = decode(reference(directory, evidence["source_before"]))
    after = decode(reference(directory, evidence["source_after"]))
    if before != after or before != source(root):
        hard.append("source-changed")
    if stamp(evidence["finished_at"]) < stamp(evidence["started_at"]):
        hard.append("time-order")
    checks = unique(config["checks"])
    records = {}
    observations = {}
    if len(evidence["records"]) != len(evidence["checks"]):
        reject("native-record-count")
    for record, native in zip(evidence["checks"], evidence["records"]):
        if decode(reference(directory, native)) != record:
            reject("native-record-mismatch")
        key = (record["id"], record["platform"])
        if key in records or record["id"] not in checks:
            reject("duplicate-or-unknown-check", record["id"])
        records[key] = record
        check = checks[record["id"]]
        if check["adapter"] != "go-test-json" or record["platform"] != evidence["platform"]:
            reject("record-mapping")
        if record["argv"] != check["argv"] or record["environment"] != check["environment"]:
            reject("command-mismatch")
        if stamp(record["finished_at"]) < stamp(record["started_at"]):
            hard.append("check-time-order:" + record["id"])
        out = reference(directory, record["stdout"], check["log_bytes"])
        reference(directory, record["stderr"], check["log_bytes"])
        failures = []
        if record["exit_code"] != 0 or record["termination"] != "exited":
            failures.append("execution-failed")
        if not record["complete"] or record["duration_ms"] > check["timeout_s"] * 1000:
            failures.append("execution-incomplete")
            hard.append("evidence-incomplete:" + record["id"])
        if not record["cleanup"]:
            hard.append("cleanup-unverified:" + record["id"])
        try:
            observed, parser_failures = go_observations(out, check)
            failures.extend(parser_failures)
        except Invalid as error:
            observed = {}
            failures.append(str(error))
            hard.append("native-format:" + record["id"])
        if "go-package-incomplete" in failures or "go-test-incomplete" in failures:
            hard.append("native-stream-incomplete:" + record["id"])
        for probe in check["probes"]:
            observations[(record["id"], record["platform"], probe["contract"])] = (
                "passed" if not failures and probe["contract"] in observed else ";".join(failures)
            )
    platforms = unique(config["platforms"])
    target = platforms.get(evidence["platform"])
    if target is None or evidence["system"] != target["system"] or evidence["machine"] != target["machine"]:
        hard.append("platform-mismatch")
    if evidence["authority_sha256"] != approved["artifact_sha256"]:
        hard.append("authority-mismatch")
    if evidence["resources"]["build_jobs"] != 4 or evidence["resources"]["test_jobs"] != 4:
        hard.append("pilot-parallel-budget")
    now = dt.datetime.now(dt.timezone.utc)
    for component in config["components"]:
        if TRUST[evidence["trust"]] < TRUST[component["evidence_trust"]]:
            hard.append("trust-gap:" + component["id"])
        convergence = component["convergence"]
        if convergence["baseline"] is None or convergence["deadline"] is None or convergence["approval_ref"] is None:
            hard.append("convergence-unconfirmed:" + component["id"])
        elif stamp(convergence["deadline"]) <= now:
            hard.append("convergence-expired:" + component["id"])
    for check in checks.values():
        if check["adapter"] == "unavailable":
            for item in expand(config).values():
                if check["id"] in item["checks"]:
                    observations[(check["id"], item["platform"], item["contract"])] = "unavailable:" + check["reason"]
    return decide(config, observations, hard, approved["exceptions"], now)


def proc_identity(pid):
    path = Path("/proc") / str(pid)
    try:
        parts = (path / "stat").read_text().rsplit(") ", 1)[1].split()
        return {"pid": pid, "start_ticks": parts[19], "parent": int(parts[1]), "session": int(parts[3]), "state": parts[0]}
    except FileNotFoundError:
        return None


def owned_members():
    # collect 单线程串行派生; subreaper 接住双重 fork/setsid 后失去父进程的后代.
    identities = {}
    for entry in Path("/proc").iterdir():
        if entry.name.isdecimal():
            identity = proc_identity(int(entry.name))
            if identity:
                identities[identity["pid"]] = identity
    owned = {os.getpid()}
    while True:
        added = {pid for pid, identity in identities.items() if identity["parent"] in owned} - owned
        if not added:
            break
        owned.update(added)
    return [identity for pid, identity in identities.items() if pid in owned and pid != os.getpid()]


def drain(process):
    # 只向本次新会话中身份复核的进程发信号; pidfd 防止核验与发送之间 PID 复用.
    for signum, budget in ((signal.SIGTERM, 2), (signal.SIGKILL, 3)):
        deadline = time.monotonic() + budget
        while True:
            process.poll()
            members = owned_members()
            if not members:
                process.wait(timeout=1)
                return True
            for member in members:
                if member["state"] == "Z":
                    if member["parent"] == os.getpid() and member["pid"] != process.pid:
                        try:
                            os.waitpid(member["pid"], os.WNOHANG)
                        except ChildProcessError:
                            pass
                    continue
                try:
                    handle = os.pidfd_open(member["pid"])
                    try:
                        current = proc_identity(member["pid"])
                        if current and current["start_ticks"] == member["start_ticks"]:
                            signal.pidfd_send_signal(handle, signum)
                    finally:
                        os.close(handle)
                except ProcessLookupError:
                    pass
            process.poll()
            if time.monotonic() >= deadline:
                break
            time.sleep(0.02)
    process.poll()
    return not owned_members()


def execute(check, root, directory, target, cancelled=lambda: False):
    environment = {"PATH": os.environ.get("PATH", os.defpath), "HOME": os.environ.get("HOME", ""), "LANG": "C.UTF-8", **check["environment"]}
    # 只展开受控项目/缓存/本轮目录, 不经 shell, 不继承任意 GOFLAGS 或下载代理.
    cache = str(Path(os.environ.get("ASTRA_CACHE_ROOT", root / "build")).resolve())
    environment = {key: value.replace("{root}", str(root)).replace("{cache}", cache).replace("{run}", str(directory)) for key, value in environment.items()}
    environment.update({"GOPROXY": "off", "GOSUMDB": "off", "GOTOOLCHAIN": "local", "GOWORK": "off", "GOFLAGS": "-mod=readonly"})
    for key in ("GOCACHE", "TMPDIR"):
        if key in environment:
            path = Path(environment[key]).resolve()
            if not path.is_relative_to(root / "build"):
                reject("cache-path", key)
            if key == "TMPDIR" and path != directory / "tmp":
                reject("temporary-ownership")
            path.mkdir(parents=True, exist_ok=True)
    executable = shutil.which(check["argv"][0], path=environment["PATH"])
    if not executable:
        reject("executable-missing", check["argv"][0])
    toolchain = {"command": {"path": executable, "sha256": file_digest(executable)}}
    for name in ("CC", "CXX"):
        if name in environment:
            tool = shutil.which(environment[name], path=environment["PATH"])
            if not tool:
                reject("executable-missing", name)
            toolchain[name] = {"path": tool, "sha256": file_digest(tool)}
    paths = {name: directory / f"{check['id']}.{name}.log" for name in ("stdout", "stderr")}
    outputs = {name: path.open("xb") for name, path in paths.items()}
    started = time.monotonic()
    started_at = utc()
    process = None
    reason, complete, cleanup = "exited", True, False
    selected = selectors.DefaultSelector()
    sizes = {name: 0 for name in paths}
    try:
        process = subprocess.Popen(
            check["argv"], cwd=root, env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True
        )
        identity = proc_identity(process.pid)
        write_new(
            directory / f"{check['id']}.process.json",
            {"identity": identity, "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(), "argv": check["argv"], "cwd": str(root)},
        )
        for name in paths:
            pipe = getattr(process, name)
            os.set_blocking(pipe.fileno(), False)
            selected.register(pipe, selectors.EVENT_READ, name)
        while selected.get_map() or process.poll() is None:
            if cancelled():
                reason, complete = "interrupted", False
                break
            if time.monotonic() - started > check["timeout_s"]:
                reason, complete = "timeout", False
                break
            for key, _ in selected.select(0.1):
                block = os.read(key.fd, 65536)
                if not block:
                    selected.unregister(key.fileobj)
                    continue
                remaining = check["log_bytes"] - sizes[key.data]
                outputs[key.data].write(block[:remaining])
                sizes[key.data] += min(remaining, len(block))
                if len(block) > remaining:
                    reason, complete = "log-limit", False
                    break
            if not complete:
                break
    except KeyboardInterrupt:
        reason, complete = "interrupted", False
    finally:
        if process is not None:
            cleanup = drain(process)
            process.stdout.close()
            process.stderr.close()
        if cleanup and "TMPDIR" in environment:
            temporary = Path(environment["TMPDIR"])
            try:
                if temporary.is_symlink() or temporary.resolve() != directory / "tmp" or not shutil.rmtree.avoids_symlink_attacks:
                    cleanup = False
                else:
                    shutil.rmtree(temporary)
            except OSError:
                cleanup = False
        selected.close()
        for stream in outputs.values():
            stream.flush()
            os.fsync(stream.fileno())
            stream.close()
    record = {
        "id": check["id"],
        "platform": target,
        "argv": check["argv"],
        "environment": check["environment"],
        "resolved_environment": environment,
        "executable": executable,
        "executable_sha256": file_digest(executable),
        "toolchain": toolchain,
        "started_at": started_at,
        "finished_at": utc(),
        "duration_ms": round((time.monotonic() - started) * 1000),
        "exit_code": process.returncode,
        "termination": reason,
        "complete": complete,
        "cleanup": cleanup,
    }
    for name, path in paths.items():
        record[name] = {"path": path.name, "bytes": path.stat().st_size, "sha256": file_digest(path)}
    return record


def collect(config, pins, approved, root, output, target, chosen, authorization):
    if platform.system() != "Linux" or not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        reject("collector-platform", "Linux with pidfd required; other platforms not silently substituted")
    library = ctypes.CDLL(None, use_errno=True)
    if library.prctl(36, 1, 0, 0, 0) != 0:
        reject("collector-subreaper", str(ctypes.get_errno()))
    targets = unique(config["platforms"])
    if target not in targets or (platform.system(), platform.machine()) != (targets[target]["system"], targets[target]["machine"]):
        reject("collector-platform")
    checks = unique(config["checks"])
    if len(chosen) != len(set(chosen)) or not chosen:
        reject("check-selection")
    for name in chosen:
        if name not in checks or checks[name]["adapter"] != "go-test-json":
            reject("unsupported-check", name)
    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    from tools.build import parallel, resources

    cpus, memory = resources()
    builds, cases = parallel(cpus, memory, "release", 4, 4)
    capacity = {"cpus": cpus, "available_bytes": memory, "build_jobs": builds, "test_jobs": cases, "disk_free_bytes": shutil.disk_usage(root).free}
    if memory is None or builds < 4 or cases < 4 or capacity["disk_free_bytes"] < 1024**3:
        reject("resource-preflight", "fixed jobs4/test-jobs4 pilot budget unavailable; reapprove changed policy, no implicit override")
    run_id = str(uuid.uuid4())
    directory = Path(output) / run_id
    directory.mkdir(parents=True, exist_ok=False)
    evidence = {
        "schema_version": 1,
        "run_id": run_id,
        "kind": "native-local",
        "trust": "local",
        "platform": target,
        "system": platform.system(),
        "machine": platform.machine(),
        "policy": pins["policy"],
        "rules": pins["rules"],
        "collector": approved["pins"]["collector"],
        "authority_sha256": approved["artifact_sha256"],
        "authorization_ref": authorization,
        "started_at": utc(),
        "checks": [],
        "records": [],
        "resources": capacity,
        "engine": {"python": platform.python_version(), "jsonschema": importlib.metadata.version("jsonschema")},
    }
    evidence["source_before"] = write_new(directory / "source-before.json", source(root))
    stopped = False

    def stop(*_):
        nonlocal stopped
        stopped = True

    previous = {signum: signal.getsignal(signum) for signum in (signal.SIGTERM, signal.SIGINT)}
    for signum in previous:
        signal.signal(signum, stop)
    try:
        for name in chosen:
            if stopped:
                break
            record = execute(checks[name], root, directory, target, lambda: stopped)
            evidence["checks"].append(record)
            evidence["records"].append(write_new(directory / f"{name}.record.json", record))
            if not record["complete"] or record["exit_code"] != 0 or not record["cleanup"]:
                break
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)
        evidence["finished_at"] = utc()
        evidence["source_after"] = write_new(directory / "source-after.json", source(root))
        write_new(directory / "evidence.json", evidence)
    return directory


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--config", default="development.json")
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("validate")
    for action in ("collect", "gate"):
        command = sub.add_parser(action)
        command.add_argument("--authority", type=Path, required=True)
        command.add_argument("--authority-sha256", required=True)
        if action == "collect":
            command.add_argument("--platform", required=True)
            command.add_argument("--check", action="append", required=True)
            command.add_argument("--authorization", required=True)
        else:
            command.add_argument("directory", type=Path)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        config, registry, pins = configuration(inside(root, args.config), root)
        if args.action == "validate":
            print(
                encode(
                    {
                        "status": "valid",
                        "pins": identities(pins, root, config),
                        "obligations": list(expand(config)),
                        "loaded_rules": closure(registry, [r["id"] for r in registry["rules"]]),
                    }
                ).decode()
            )
            return 0
        approved = authority(args.authority, args.authority_sha256, pins, root, config)
        approved["artifact_sha256"] = args.authority_sha256
        output = inside(root, "build/results/verification")
        if args.authority.resolve().is_relative_to(output.resolve()):
            reject("authority-in-evidence")
        if args.action == "collect":
            directory = collect(config, pins, approved, root, output, args.platform, args.check, args.authorization)
            print(directory)
            records = decode(read(directory / "evidence.json"))["checks"]
            return 0 if len(records) == len(args.check) and all(c["complete"] and c["cleanup"] and c["exit_code"] == 0 for c in records) else 2
        directory = args.directory.resolve()
        result = gate(config, pins, approved, directory, root)
        report = {
            "evaluated_at": utc(),
            "policy_sha256": pins["policy"],
            "authority_sha256": args.authority_sha256,
            "evidence_sha256": file_digest(directory / "evidence.json"),
            "gate": result,
        }
        write_new(directory / f"gate-{uuid.uuid4()}.json", report)
        print(encode(report).decode())
        return {"eligible": 0, "eligible-with-exceptions": 10, "blocked": 2}[result["status"]]
    except (Invalid, OSError, subprocess.SubprocessError) as error:
        print(encode({"status": "invalid", "reason": str(error)}).decode(), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
