"""Shared project-only environment, bounded command output and peer ownership."""

from __future__ import annotations

from collections import deque
from contextlib import contextmanager
import json
import os
from pathlib import Path
import queue
import re
import secrets
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def environment(overrides=None):
    """Return child settings; never activate caches in the caller's terminal."""
    env = dict(os.environ if overrides is None else overrides)
    if overrides is None:
        for name in tuple(env):
            if name.startswith(("VERDANDI_REDIS_", "VERDANDI_SENTINEL_", "VERDANDI_TLS_", "VERDANDI_CATALOG_")):
                env.pop(name)
    build = ROOT / "build"
    env.update(
        GOMODCACHE=str(build / "deps/go/pkg/mod"),
        GOCACHE=str(build / "cache/go"),
        GOPATH=str(build / "deps/go"),
        GOPROXY="off",
        GOTOOLCHAIN="local",
        GOSUMDB="off",
        GOMAXPROCS=env.get("GOMAXPROCS", "2"),
        GOFLAGS="-p=1",
        CARGO_HOME=str(build / "deps/cargo"),
        CARGO_TARGET_DIR=str(build / "rust/target"),
        CARGO_NET_OFFLINE="true",
        CARGO_BUILD_JOBS="1",
        RUSTUP_AUTO_INSTALL="0",
        NUGET_PACKAGES=str(build / "deps/nuget"),
        DOTNET_CLI_HOME=str(build / "tools/dotnet-home"),
        DOTNET_CLI_TELEMETRY_OPTOUT="1",
        DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
        PYTHONDONTWRITEBYTECODE="1",
        PYTHONIOENCODING="utf-8",
        PIP_CACHE_DIR=str(build / "deps/pip"),
        BLACK_CACHE_DIR=str(build / "cache/black"),
        CMAKE_BUILD_PARALLEL_LEVEL="1",
    )
    local_bins = [
        *sorted((build / "tools").glob("go-*/bin"), reverse=True),
        *sorted((build / "tools").glob("rust-*/bin"), reverse=True),
        build / "tools/go/bin",
        build / "tools/rust/cargo/bin",
        build / "tools/cargo/bin",
        build / "tools/dotnet",
    ]
    env["PATH"] = os.pathsep.join([*(str(p) for p in local_bins if p.is_dir()), env.get("PATH", "")])
    rustup = build / "tools/rust/rustup"
    if rustup.is_dir():
        env["RUSTUP_HOME"] = str(rustup)
    return env


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    # Windows 读取者可能短暂持有不共享删除的句柄. 只重试替换, 保留旧报告的原子可见性.
    for attempt in range(41):
        try:
            temporary.replace(path)
            return
        except PermissionError as error:
            if os.name != "nt" or error.winerror not in (5, 32) or attempt == 40:
                raise
            time.sleep(0.05)


def redact(text):
    text = re.sub(r"(rediss?://[^:/\s]+:)[^@\s]+@", r"\1<redacted>@", text)
    return re.sub(r'("(?:password|ssh_password)"\s*:\s*")[^"]*"', r'\1<redacted>"', text)


def available_memory():
    if os.name == "nt":
        import ctypes

        class Memory(ctypes.Structure):
            _fields_ = [
                ("length", ctypes.c_uint32),
                ("load", ctypes.c_uint32),
                *[(name, ctypes.c_uint64) for name in ("total", "available", "page", "free_page", "virtual", "free_virtual", "extended")],
            ]

        state = Memory()
        state.length = ctypes.sizeof(state)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(state)):
            raise OSError("GlobalMemoryStatusEx failed")
        return state.available
    values = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
    return int(values["MemAvailable"].split()[0]) * 1024


class FileLock:
    """An OS-held lock survives as a file, but never as a stale PID lock."""

    def __init__(self, path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.file = self.path.open("a+b")
        try:
            if os.name == "nt":
                import msvcrt

                self.file.seek(0)
                if self.file.read(1) == b"":
                    self.file.write(b"\0")
                    self.file.flush()
                self.file.seek(0)
                msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.file.close()
            raise RuntimeError(f"Another test owns {self.path}") from None

    def close(self):
        if not self.file.closed:
            if os.name == "nt":
                import msvcrt

                self.file.seek(0)
                msvcrt.locking(self.file.fileno(), msvcrt.LK_UNLCK, 1)
            self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def _remove_temporary(path):
    parent = (ROOT / "build/testkit/tmp").resolve()
    if path.parent.resolve() != parent or path.is_symlink() or not re.fullmatch(r"[a-z-]+[a-f0-9]{16}", path.name):
        raise ValueError("Refusing an unowned temporary directory")
    if not path.exists():
        return
    owner = path / ".owner"
    if owner.is_symlink() or owner.read_text(encoding="utf-8") != path.name:
        raise ValueError("Temporary directory ownership mismatch")
    for current, directories, files in os.walk(path, topdown=False, followlinks=False):
        for name in files:
            entry = Path(current) / name
            if entry != owner:
                entry.unlink()
        for name in directories:
            entry = Path(current) / name
            entry.unlink() if entry.is_symlink() else entry.rmdir()
    owner.unlink()
    path.rmdir()


@contextmanager
def temporary_directory(prefix="verdandi-"):
    if not re.fullmatch(r"[a-z-]+", prefix):
        raise ValueError("Invalid temporary directory prefix")
    path = ROOT / "build/testkit/tmp" / (prefix + secrets.token_hex(8))
    with FileLock(path.with_suffix(".lock")):
        path.mkdir()
        (path / ".owner").write_text(path.name, encoding="utf-8")
        try:
            yield str(path)
        finally:
            _remove_temporary(path)


def recover_temporary():
    for path in (ROOT / "build/testkit/tmp").glob("*"):
        if not path.is_dir():
            continue
        try:
            lock = FileLock(path.with_suffix(".lock"))
        except RuntimeError as error:
            if str(error).startswith("Another test owns"):
                continue
            raise
        with lock:
            _remove_temporary(path)


def stop_process(process):
    """Kill only this command's process tree, including a surviving POSIX group."""
    if job := getattr(process, "_verdandi_job", None):
        job.stop()
    elif os.name == "nt":
        subprocess.run(
            [str(Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"), "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.kill()
    process.wait(timeout=10)


def popen(command, cwd=ROOT, env=None, **kwargs):
    job = None
    process = None
    if os.name == "nt":
        from testkit.windows_job import Job

        job = Job()
    try:
        process = subprocess.Popen(
            list(map(str, command)),
            cwd=cwd,
            env=environment(env),
            start_new_session=os.name != "nt",
            creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP | 4) if os.name == "nt" else 0,
            text=True,
            encoding="utf-8",
            errors="replace",
            **kwargs,
        )
        if job is not None:
            process._verdandi_job = job
            job.attach(process)
        return process
    except BaseException:
        if job is not None:
            job.stop()
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
        raise


def run_command(name, command, directory=ROOT, env=None, required_output=None, timeout=1800, *, on_output=None):
    """Stream a bounded log and retain only its tail; own cleanup on every exit."""
    started = time.monotonic()
    log_root = Path(os.environ.get("VERDANDI_TEST_LOG_DIR", ROOT / "build/testkit/logs"))
    log_root.mkdir(parents=True, exist_ok=True)
    log = log_root / (str(time.time_ns()) + "-" + re.sub(r"[^a-zA-Z0-9_.-]", "-", name)[:100] + ".log")
    print(f"RUN {name}", flush=True)
    output = deque(maxlen=256)
    lines = queue.Queue(maxsize=128)
    stopped = threading.Event()
    seen = required_output is None
    written = 0
    minimum = int(os.environ.get("VERDANDI_TEST_MIN_FREE_MIB", "384")) * 1024 * 1024
    process = popen(command, directory, env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def read():
        try:
            while text := process.stdout.readline(65_536):
                while not stopped.is_set():
                    try:
                        lines.put(text, timeout=0.1)
                        break
                    except queue.Full:
                        pass
                if stopped.is_set():
                    break
        finally:
            while not stopped.is_set():
                try:
                    lines.put(None, timeout=0.1)
                    break
                except queue.Full:
                    pass

    reader = threading.Thread(target=read, daemon=True)
    try:
        reader.start()
        with log.open("w", encoding="utf-8") as target:
            last_check = 0.0
            descendants_stopped = False
            while True:
                now = time.monotonic()
                if not descendants_stopped and process.poll() is not None:
                    stop_process(process)
                    descendants_stopped = True
                if now - started > timeout:
                    raise TimeoutError(f"{name} exceeded {timeout}s")
                if now - last_check > 2:
                    last_check = now
                    if available_memory() < minimum:
                        raise RuntimeError(f"{name}: available memory below {minimum // 1024**2} MiB")
                try:
                    line = lines.get(timeout=0.2)
                except queue.Empty:
                    continue
                if line is None:
                    break
                line = redact(line)
                written += len(line.encode("utf-8"))
                if written > 128 * 1024 * 1024:
                    raise RuntimeError(f"{name}: log exceeded 128 MiB")
                target.write(line)
                target.flush()
                print(line, end="", flush=True)
                output.append(line)
                if on_output is not None:
                    on_output(line)
                seen = seen or required_output in line
        status = process.wait(timeout=max(1, timeout - (time.monotonic() - started)))
        if status or not seen:
            raise RuntimeError(f"{name}: exit={status}, required output present={seen}; log={log}\n{''.join(output)[-4096:]}")
        return {"name": name, "status": "pass", "elapsed_seconds": round(time.monotonic() - started, 3), "log": str(log), "output": "".join(output)}
    finally:
        stopped.set()
        stop_process(process)
        if reader.ident is not None:
            reader.join(timeout=5)
        process.stdout.close()


def build_peers(kind):
    """Finish both native builds before the short peer readiness deadline starts."""
    if kind not in ("catalog", "interop"):
        raise ValueError("Unknown peer group")
    suffix = ".exe" if os.name == "nt" else ""
    go = ROOT / "build/testkit/bin" / (kind + "-go" + suffix)
    go.parent.mkdir(parents=True, exist_ok=True)
    run_command(f"{kind} Go peer build", ["go", "build", "-o", str(go), "."], ROOT / f"testkit/{kind}/go-peer")
    run_command(f"{kind} Rust peer build", ["cargo", "build", "--locked", "--offline", "--manifest-path", str(ROOT / f"testkit/{kind}/rust-peer/Cargo.toml")])
    rust = ROOT / "build/rust/target/debug" / (f"verdandi-{kind}-rust-peer" + suffix)
    return str(go), str(rust)


class Peer:
    """Bounded line protocol; constructor and readiness failures retain an owner."""

    def __init__(self, command, cwd, env=None):
        self.stderr = deque(maxlen=64)
        self.lines = queue.Queue(maxsize=256)
        self.stopped = threading.Event()
        self.overflow = False
        self.process = popen(command, cwd, env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.threads = [
            threading.Thread(target=self._read, args=(stream, diagnostic), daemon=True)
            for stream, diagnostic in ((self.process.stdout, False), (self.process.stderr, True))
        ]
        try:
            for thread in self.threads:
                thread.start()
        except BaseException:
            self.stop()
            raise

    def _read(self, stream, diagnostic):
        while not self.stopped.is_set() and (line := stream.readline(65_536)):
            if len(line) >= 65_536:
                self.overflow = True
                return
            line = redact(line.rstrip("\r\n"))
            if diagnostic:
                self.stderr.append(line)
            else:
                try:
                    self.lines.put_nowait(line)
                except queue.Full:
                    self.overflow = True
                    return

    def wait_line(self, prefix, timeout=90):
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            if self.overflow:
                raise RuntimeError("Peer output exceeded its bounded protocol buffer")
            if self.process.poll() is not None and self.lines.empty():
                break
            try:
                line = self.lines.get(timeout=min(0.2, remaining))
            except queue.Empty:
                continue
            if line.startswith(prefix):
                return line
            raise RuntimeError(f"Unexpected peer output: {line!r}")
        raise TimeoutError(f"Peer did not emit {prefix!r}; exit={self.process.poll()}; {' | '.join(self.stderr)}")

    def send(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def command(self, command, prefix, timeout=90):
        self.send(command)
        return self.wait_line(prefix, timeout)

    def stop(self):
        if self.stopped.is_set():
            return
        self.stopped.set()
        stop_process(self.process)
        for thread in self.threads:
            if thread.ident is not None:
                thread.join(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.stop()
