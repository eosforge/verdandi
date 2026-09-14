"""Native regression stages; Python owns fixtures, each SDK owns its tests."""

from __future__ import annotations

from contextlib import ExitStack
import importlib.util
import importlib.metadata
import json
import os
from pathlib import Path
import secrets
import shutil
import sys
import time

from testkit.support import ROOT, atomic_json, environment, run_command, temporary_directory
from testkit.resources import Resources


def native_environment():
    report = json.loads((ROOT / "build/environment.json").read_text(encoding="utf-8"))
    library = Path(report["paths"]["native_runtime"])
    if not library.is_file():
        raise RuntimeError(f"Missing native runtime {library}")
    env = environment()
    env["VERDANDI_NATIVE_LIBRARY"] = str(library)
    key = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
    env[key] = os.pathsep.join(path for path in (str(library.parent), env.get(key, "")) if path)
    return env, report


def managed_build():
    # An empty local source makes a missing package an explicit offline failure.
    source = ROOT / "build/deps/nuget-offline"
    source.mkdir(parents=True, exist_ok=True)
    run_command("CSharp offline restore", ["dotnet", "restore", "Verdandi.slnx", "--source", str(source), "-p:NuGetAudit=false"], ROOT / "sdk/csharp")
    run_command("CSharp format", ["dotnet", "format", "Verdandi.slnx", "--verify-no-changes", "--no-restore"], ROOT / "sdk/csharp")
    return run_command("CSharp build", ["dotnet", "build", "Verdandi.slnx", "-c", "Release", "--no-restore", "-m:1"], ROOT / "sdk/csharp")


def managed_test(fixture, output):
    env, _ = native_environment()
    config = {
        "version": "v1",
        "redis": {"mode": "standalone", "addresses": [f"{fixture.remote.host}:{fixture.port}"], "auth": {"username": "default", "password": fixture.password}},
        "registration": {"zone": "Reg" + "".join(chr(65 + value % 26) for value in secrets.token_bytes(12))},
        "catalog": {"zone": "Cat" + "".join(chr(65 + value % 26) for value in secrets.token_bytes(12)), "max_record_bytes": 4 * 1024 * 1024},
    }
    with temporary_directory(prefix="verdandi-csharp-") as temporary:
        path = Path(temporary) / "configuration.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        for framework in ("net8.0", "net10.0"):
            run_command(
                f"CSharp {framework} Redis",
                [
                    "dotnet",
                    "run",
                    "--project",
                    "tests/Verdandi.Tests",
                    "-c",
                    "Release",
                    "-f",
                    framework,
                    "--no-build",
                    "--no-restore",
                    "--",
                    "--configuration-file",
                    str(path),
                ],
                ROOT / "sdk/csharp",
                env,
                "Verdandi C# offline and Redis tests passed.",
            )


def preflight(languages):
    env = environment()
    missing = [name for name in ("redis", "msgpack", "paramiko", "cryptography") if importlib.util.find_spec(name) is None]
    required = [tool for language, tool in (("go", "go"), ("rust", "cargo"), ("cpp", "cmake"), ("csharp", "dotnet")) if language in languages]
    missing.extend(name for name in required if shutil.which(name, path=env["PATH"]) is None)
    if missing:
        raise RuntimeError(
            "Missing prerequisites: " + ", ".join(missing) + ". Prepare approved tools/dependencies externally in this project's build directory."
        )
    versions = {}
    for tool in required:
        directory = ROOT / "sdk" / {"cargo": "rust", "cmake": "cpp", "dotnet": "csharp"}.get(tool, tool)
        result = run_command(f"Preflight {tool}", [tool, "version" if tool == "go" else "--version"], directory, timeout=30)
        versions[tool] = {"path": shutil.which(tool, path=env["PATH"]), "version": result["output"].strip()}
    return {
        "python": sys.version.split()[0],
        "tools": versions,
        "packages": {name: importlib.metadata.version(name) for name in ("redis", "msgpack", "paramiko", "cryptography")},
    }


def execute(options, remote, report, save):
    """Run independent stages serially; fixture cleanup has its own result."""
    from testkit.standalone.standalone_test import Fixture

    languages = set(options["languages"])
    output = Path(options["output"])
    runtime = "win-x64" if os.name == "nt" else "linux-x64"
    report["platform"] = runtime
    report["environment"] = preflight(languages)
    image = remote.run("docker image inspect redis:8.8.0 --format '{{.Id}}'")
    report["redis_image"] = image.strip()
    Resources.recover(remote)
    tasks = report["stages"]
    ready = set()

    def stage(name, language, action):
        record = {"name": name, "language": language, "platform": runtime, "status": "running"}
        tasks.append(record)
        save()
        start = time.monotonic()
        try:
            value = action()
            record.update(status="pass")
            if isinstance(value, dict):
                record["details"] = {k: v for k, v in value.items() if k != "output"}
            return True
        except KeyboardInterrupt:
            record["status"] = "interrupted"
            raise
        except Exception as error:
            record.update(status="failed", error=str(error))
            return False
        finally:
            record["elapsed_seconds"] = round(time.monotonic() - start, 3)
            save()

    def command(name, args, directory=ROOT, env=None, required=None):
        return run_command(name, args, directory, env, required)

    if options["mode"] == "regression":
        stage(
            "Harness ownership regression",
            "python",
            lambda: command("Harness tests", [sys.executable, "-B", "-m", "unittest", "discover", "-s", "testkit/tests", "-v"]),
        )
    for domain in ("registration", "catalog"):
        stage(
            f"Generated {domain} Lua freshness",
            "lua",
            lambda domain=domain: command(f"{domain} Lua freshness", [sys.executable, "-B", f"testkit/lua/generate_{domain}.py", "--check"]),
        )
    if "go" in languages:
        if stage(
            "Go unit and vet",
            "go",
            lambda: (
                command("Go unit", ["go", "test", "-count=1", "./..."], ROOT / "sdk/go"),
                command("Go vet", ["go", "vet", "-tags=integration", "./..."], ROOT / "sdk/go"),
            ),
        ):
            ready.add("go")
    if "rust" in languages:
        if stage("Rust workspace", "rust", lambda: command("Rust workspace", ["cargo", "test", "--workspace", "--locked", "--offline"], ROOT / "sdk/rust")):
            ready.add("rust")
    if languages & {"cpp", "csharp"}:
        if stage(
            "C++ shared build",
            "cpp",
            lambda: command(
                "C++ shared build", [sys.executable, "-B", "sdk/cpp/build.py", "build", "--profile", "dev", "--linkage", "shared", "--offline", "--jobs", "1"]
            ),
        ):
            ready.add("cpp")
    if "csharp" in languages and "cpp" in ready:
        if stage("CSharp build and analyzers", "csharp", managed_build):
            ready.add("csharp")

    fixture = Fixture(remote, secrets.token_hex(4), 36380)
    try:
        fixture.deploy()
        env = environment()
        env.update(
            VERDANDI_REDIS_URL=fixture.url,
            VERDANDI_REDIS_ADDRESS=f"{remote.host}:{fixture.port}",
            VERDANDI_REDIS_USERNAME="default",
            VERDANDI_REDIS_PASSWORD=fixture.password,
            VERDANDI_REDIS_CONFIGURATION_JSON=json.dumps(
                {"mode": "standalone", "addresses": [f"{remote.host}:{fixture.port}"], "auth": {"username": "default", "password": fixture.password}}
            ),
        )
        for domain in ("registration", "catalog"):
            stage(
                f"{domain} Lua contract",
                "lua",
                lambda domain=domain: command(
                    f"{domain} Lua contract", [sys.executable, "-B", f"testkit/lua/{domain}_test.py", "--redis-url", fixture.url], env=env
                ),
            )
        if "go" in ready:
            args = ["go", "test", "-tags=integration", "-count=1", "-timeout=15m", "./..."]
            if os.name != "nt":
                args.insert(2, "-race")
            stage("Go Redis integration" + (" with race" if os.name != "nt" else ""), "go", lambda: command("Go Redis", args, ROOT / "sdk/go", env))
        if "rust" in ready:
            for target in ("integration", "root_redis", "catalog_v2"):
                args = ["cargo", "test", "--locked", "--offline", "--test", target, "--"]
                if target != "catalog_v2":
                    args += ["--ignored", "--skip", "sentinel"]
                args += ["--test-threads=1", "--nocapture"]
                stage(f"Rust {target} Redis", "rust", lambda args=args, target=target: command(f"Rust {target} Redis", args, ROOT / "sdk/rust", env))
        if "cpp" in ready:

            def native_tests():
                value = command(
                    "C++ CTest with Redis",
                    [sys.executable, "-B", "sdk/cpp/build.py", "test", "--profile", "dev", "--linkage", "shared", "--offline", "--jobs", "1"],
                    env=env,
                    required="100% tests passed",
                )
                if "Skipped" in value["output"]:
                    raise RuntimeError("C++ Redis regression skipped tests")
                return value

            stage("C++ / C ABI / Legacy Redis", "cpp", native_tests)
        if "csharp" in ready:
            stage("CSharp net8/net10 Redis", "csharp", lambda: managed_test(fixture, output))
        if {"go", "rust"} <= ready:
            for script in ("interop/interop_test.py", "catalog/interop_test.py"):
                stage(
                    f"Interop {script}",
                    "go,rust",
                    lambda script=script: command(f"Interop {script}", [sys.executable, "-B", f"testkit/{script}", "--redis-url", fixture.url], env=env),
                )
        import redis

        with redis.Redis.from_url(fixture.url, socket_timeout=5) as client:
            stage("Standalone owned keys released", "all", lambda: assert_empty(client))
    finally:
        stage("Standalone resource cleanup", "python", fixture.cleanup)

    if options["mode"] == "regression":
        common = ["--host", options["host"], "--ssh-user", options["user"]]
        env = environment()
        if "cpp" in ready:
            env, native = native_environment()
        for tls in (False, True):
            suffix = " TLS" if tls else ""
            flags = [*common, *(["--tls"] if tls else [])]
            if {"go", "rust"} <= ready:
                stage(
                    "Registration two-promotion Sentinel" + suffix,
                    "go,rust",
                    lambda flags=flags: command(
                        "Registration Sentinel" + (" TLS" if "--tls" in flags else ""),
                        [sys.executable, "-B", "testkit/sentinel/sentinel_test.py", "--runtime", runtime, *flags],
                        env=env,
                    ),
                )
            if not tls and {"go", "rust"} <= ready:
                stage(
                    "Catalog two-promotion Sentinel",
                    "go,rust",
                    lambda: command("Catalog Sentinel", [sys.executable, "-B", "testkit/catalog/sentinel_test.py", *common], env=env),
                )
            if "cpp" in ready:
                stage(
                    "C++ Sentinel domains" + suffix,
                    "cpp",
                    lambda flags=flags: command(
                        "C++ Sentinel" + (" TLS" if "--tls" in flags else ""),
                        [sys.executable, "-B", "testkit/cpp/sentinel_smoke.py", "--runtime", runtime, "--build", native["paths"]["cpp"], *flags],
                        env=env,
                    ),
                )
            if "csharp" in ready:
                stage(
                    "CSharp two-promotion Sentinel" + suffix,
                    "csharp",
                    lambda flags=flags: command(
                        "CSharp Sentinel" + (" TLS" if "--tls" in flags else ""),
                        [sys.executable, "-B", "sdk/csharp/tests/sentinel_test.py", "--runtime", runtime, *flags],
                        env=env,
                    ),
                )
    else:
        for domain, script, port in (("registration", "soak/soak_test.py", 36390), ("catalog", "catalog/soak_test.py", 36391)):
            if not {"go", "rust"} <= ready:
                tasks.append(
                    {"name": f"{domain} continuous soak", "status": "blocked", "language": "go", "reason": "Requires Go workload and Rust post-checks"}
                )
                continue
            args = [
                sys.executable,
                "-B",
                f"testkit/{script}",
                "--host",
                options["host"],
                "--ssh-user",
                options["user"],
                "--port",
                str(port),
                "--duration-seconds",
                str(options["duration"]),
                "--minimum-redis-seconds",
                str(options["duration"]),
                "--sample-seconds",
                str(max(5, (options["duration"] + 3999) // 4000)),
                "--result-file",
                str(output / f"{domain}-soak.json"),
            ]
            if domain == "registration":
                args += ["--lifecycle-interval", "10s" if options["duration"] < 600 else "5m"]
            stage(
                f"{domain} continuous fault soak",
                "go",
                lambda args=args, domain=domain: run_command(f"{domain} soak", args, env=environment(), timeout=options["duration"] + 1800),
            )
        report["coverage_notes"] = [
            "Continuous workload qualification currently exercises Go; Rust/C++/C# have separate regression rows, not continuous-soak certification.",
            "Duration is per domain; two serial domains require at least twice the requested load duration.",
        ]
    Resources.recover(remote)
    failed_languages = languages - ready
    if failed_languages:
        tasks.append({"name": "Requested SDK readiness", "status": "blocked", "languages": sorted(failed_languages)})
    save()


def assert_empty(client):
    count = client.dbsize()
    if count:
        raise RuntimeError(f"Successful suites left {count} Redis keys")
    return {"remaining_keys": 0}
