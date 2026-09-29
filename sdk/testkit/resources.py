"""Exact resource ownership, recoverable manifests and bounded Docker/SSH commands."""

from __future__ import annotations

import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import time

from testkit.support import ROOT, FileLock, atomic_json, environment, redact


class Remote:
    def __init__(self, host, username, password, *, project="/home/ubuntu/verdandi", sudo=True):
        self.host, self.username, self.password = host, username, password
        self.project, self.sudo = project, sudo
        self._client = None
        if host == "local":
            if os.name == "nt":
                raise RuntimeError("Local Docker fixtures require Linux; configure the Ubuntu host on Windows")
            self.host = "127.0.0.1"
            self.project = str(ROOT)
            return
        import paramiko

        self._client = paramiko.SSHClient()
        keys = ROOT / "build/testkit/known_hosts"
        keys.parent.mkdir(parents=True, exist_ok=True)
        self._client.load_system_host_keys()
        if keys.exists():
            self._client.load_host_keys(str(keys))
        else:
            keys.touch()
        self._client.load_host_keys(str(keys))
        self._client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            self._client.connect(host, username=username, password=password, timeout=10, banner_timeout=10, auth_timeout=10)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self._client is not None:
            self._client.close()

    def run(self, command, *, check=True, timeout=60):
        invocation = ["sh", "-c", command]
        if self.sudo:
            invocation = ["sudo", "-S", "-p", "", "--", *invocation]
        if self._client is None:
            completed = subprocess.run(invocation, input=self.password + "\n" if self.sudo else "", capture_output=True, text=True, timeout=timeout)
            status, output, diagnostics = completed.returncode, completed.stdout, completed.stderr
        else:
            stdin, stdout, stderr = self._client.exec_command(shlex.join(invocation), timeout=timeout)
            channel = stdout.channel
            stdin.write(self.password + "\n" if self.sudo else "")
            stdin.flush()
            channel.shutdown_write()
            started = time.monotonic()
            output, diagnostics = bytearray(), bytearray()
            try:
                while True:
                    for ready, read, target in ((channel.recv_ready, channel.recv, output), (channel.recv_stderr_ready, channel.recv_stderr, diagnostics)):
                        while ready():
                            target.extend(read(65536))
                            if len(target) > 4 * 1024 * 1024:
                                raise RuntimeError("Remote command output exceeded 4 MiB")
                    if channel.exit_status_ready() and not channel.recv_ready() and not channel.recv_stderr_ready():
                        break
                    if time.monotonic() - started > timeout:
                        raise TimeoutError("Remote command timed out")
                    time.sleep(0.02)
                status = channel.recv_exit_status()
            finally:
                channel.close()
            output = output.decode("utf-8", errors="replace")
            diagnostics = diagnostics.decode("utf-8", errors="replace")
        if check and status:
            raise RuntimeError(f"Remote command exited {status}: {redact((output + diagnostics)[-4096:])}")
        return output

    def write(self, path, content, *, mode=0o644):
        if self._client is None:
            target = Path(path)
            target.write_text(content, encoding="utf-8")
            target.chmod(mode)
            return
        with self._client.open_sftp() as sftp:
            with sftp.file(path, "w") as target:
                target.write(content)
            sftp.chmod(path, mode)


class Resources:
    """Register intent before creation; cleanup verifies immutable IDs and ownership."""

    def __init__(self, remote, run_id, *, existing=None):
        if not re.fullmatch("[a-f0-9]{8}", run_id):
            raise ValueError("Invalid resource run ID")
        self.remote, self.run_id = remote, run_id
        self.path = ROOT / "build/testkit/resources" / f"{run_id}.json"
        self.lock = FileLock(self.path.with_suffix(".lock"))
        self.data = existing or {
            "run_id": run_id,
            "campaign_id": os.environ.get("VERDANDI_TEST_RUN_ID"),
            "host": remote.host,
            "user": remote.username,
            "project": remote.project,
            "resources": [],
            "status": "active",
        }
        if existing is None and self.path.exists():
            self.lock.close()
            raise RuntimeError(f"Resource manifest collision: {self.path}")
        self.save()

    def save(self):
        atomic_json(self.path, self.data)

    def container(self, name):
        if not re.fullmatch(r"verdandi-[a-z0-9-]+", name) or self.run_id not in name:
            raise ValueError("Invalid owned container name")
        self.data["resources"].append({"kind": "container", "name": name})
        self.save()

    def directory(self, path):
        self._check_path(path)
        self.remote.run(f"test ! -e {shlex.quote(path)} && test ! -L {shlex.quote(path)}")
        self.data["resources"].append({"kind": "directory", "name": path})
        self.save()
        parent, owner = str(PurePosixPath(path).parent), path + "/owner"
        self.remote.run(
            f"set -eu; mkdir -p -- {shlex.quote(parent)}; "
            f"mkdir -m 0777 -- {shlex.quote(path)}; "
            f"printf %s {shlex.quote(self.run_id)} > {shlex.quote(owner)}; chmod 644 -- {shlex.quote(owner)}"
        )

    def _check_path(self, path):
        expected = PurePosixPath(self.remote.project) / "build/testkit/fixtures"
        target = PurePosixPath(path)
        if not expected.is_absolute() or target.parent != expected or not re.fullmatch(r"[a-z-]+-" + self.run_id, target.name):
            raise ValueError(f"Refusing resource path outside the fixture directory: {path}")

    def _remove(self, item):
        name = item["name"]
        if item["kind"] == "container":
            if not re.fullmatch(r"verdandi-[a-z0-9-]+", name) or self.run_id not in name:
                raise ValueError("Invalid container in resource manifest")
            names = self.remote.run("docker ps -a --format '{{.Names}}'").splitlines()
            if name not in names:
                return
            data = json.loads(self.remote.run("docker inspect " + shlex.quote(name)))[0]
            if data["Config"].get("Labels", {}).get("verdandi.test") != self.run_id:
                raise RuntimeError(f"Refusing to remove foreign container {name}")
            identifier = data["Id"]
            if not re.fullmatch("[a-f0-9]{64}", identifier):
                raise RuntimeError("Invalid Docker container ID")
            self.remote.run("docker rm -f " + identifier)
        elif item["kind"] == "directory":
            self._check_path(name)
            path, owner = shlex.quote(name), shlex.quote(name + "/owner")
            # Do not follow symlinks, recurse through another shell, or remove the
            # owner marker before all other entries have been successfully removed.
            self.remote.run(
                f"set -eu; if test -e {path} || test -L {path}; then "
                f'test ! -L {path}; test "$(realpath -- {path})" = {path}; '
                f'test ! -L {owner}; test "$(cat -- {owner})" = {shlex.quote(self.run_id)}; '
                f"find {path} -mindepth 1 ! -path {owner} -type f -delete; "
                f"find {path} -mindepth 1 -type l -delete; "
                f"find {path} -depth -mindepth 1 -type d -empty -delete; "
                f'test "$(find {path} -mindepth 1 -maxdepth 1 | wc -l)" -eq 1; '
                f"rm -- {owner}; rmdir -- {path}; fi"
            )
        else:
            raise ValueError("Unknown resource kind")

    def cleanup(self):
        errors = []
        try:
            for item in list(reversed(self.data["resources"])):
                try:
                    self._remove(item)
                    self.data["resources"].remove(item)
                    self.save()
                except Exception as error:
                    errors.append(f"{item['kind']} {item['name']}: {error}")
            self.data["status"] = "cleanup_failed" if errors else "cleaned"
            self.data["cleanup_errors"] = errors
            self.save()
        finally:
            self.lock.close()
        if errors:
            raise RuntimeError("Cleanup incomplete: " + "; ".join(errors))

    @classmethod
    def recover(cls, remote, campaign_id=None):
        for path in sorted((ROOT / "build/testkit/resources").glob("*.json")):
            value = json.loads(path.read_text(encoding="utf-8"))
            if value.get("host") != remote.host or value.get("user") != remote.username or not value.get("resources"):
                continue
            if value.get("project") != remote.project or value.get("run_id") != path.stem:
                raise RuntimeError(f"Resource manifest identity mismatch: {path}")
            try:
                resources = cls(remote, path.stem, existing=value)
            except RuntimeError as error:
                if str(error).startswith("Another test owns"):
                    if campaign_id is not None and value.get("campaign_id") == campaign_id:
                        raise RuntimeError(f"Current campaign still owns active resources: {path}") from error
                    continue
                raise
            resources.cleanup()
