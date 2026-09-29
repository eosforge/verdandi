"""Snapshot synchronization and durable campaign reports."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time
import zipfile

from testkit.support import ROOT, atomic_json, redact


def source_manifest():
    result = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT, capture_output=True, check=True)
    manifest = {}
    for name in sorted(set(result.stdout.decode("utf-8").split("\0"))):
        path = ROOT / name
        if not name or name.startswith(("build/", "testkit/results/")) or path.suffix == ".md" or not path.is_file():
            continue
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT):
            raise RuntimeError(f"Source snapshot refuses symlink: {name}")
        manifest[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return manifest


def fingerprint(manifest):
    return hashlib.sha256(json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def save_report(report, output):
    output = Path(output)
    atomic_json(output / "report.json", report)
    lines = [
        f"# Verdandi {'preflight' if report.get('preflight_only') else report['mode']} — {report['status']}",
        "",
        f"Run: {report['run_id']}",
        f"Source: {report.get('source_sha256', 'unavailable')}",
        f"Targets: {', '.join(report.get('requested_targets', []))}; SDKs: {', '.join(report.get('requested_languages', []))}",
        "",
        "| Platform | Stage | Language | Result | Seconds |",
        "| --- | --- | --- | --- | --- |",
    ]
    for stage in report.get("stages", []):
        lines.append(
            f"| {stage.get('platform', report.get('platform',''))} | {stage['name']} | {stage.get('language','')} | {stage['status']} | {stage.get('elapsed_seconds','')} |"
        )
        if stage.get("error"):
            lines.extend(["", f"Failure — {stage['name']}: {stage['error']}", ""])
    lines.extend(["", f"Cleanup: {report.get('cleanup', 'pending')}", ""])
    lines.extend(report.get("coverage_notes", []))
    lines.extend(["", "Coverage gaps:", ""])
    for gap in report.get("coverage_gaps", []):
        lines.append(f"- {gap['scenario']}: {gap['status']} ({', '.join(gap['languages'])})")
    if report.get("error"):
        lines.extend(["", report["error"]])
    temporary = output / "report.md.tmp"
    temporary.write_text(redact("\n".join(lines) + "\n"), encoding="utf-8")
    temporary.replace(output / "report.md")


def synchronize(remote, manifest, run_id):
    """Send only changed source bytes; validate every old hash before any write."""
    state = ROOT / "build/testkit/sync" / f"{remote.host}.json"
    if state.exists():
        previous = json.loads(state.read_text(encoding="utf-8"))
    else:
        baseline = ROOT / "build/reaudit-20260907/source-current-manifest.json"
        previous = {item["path"]: item["linux_sha256"] for item in json.loads(baseline.read_text(encoding="utf-8"))} if baseline.exists() else {}
    previous = {name: digest for name, digest in previous.items() if not name.startswith(("build/", "testkit/results/")) and not name.endswith(".md")}
    changed = {name: digest for name, digest in manifest.items() if previous.get(name) != digest}
    deleted = sorted(set(previous) - set(manifest))
    local = ROOT / "build/testkit/sync" / f"{run_id}.zip"
    local.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(local, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps({"files": manifest, "previous": previous, "changed": list(changed), "deleted": deleted}))
        for name in changed:
            archive.write(ROOT / name, "source/" + name)
    remote.run("mkdir -p -- " + shlex.quote(remote.project + "/build/testkit/sync"))
    destination = remote.project + f"/build/testkit/sync/{run_id}.zip"
    with remote._client.open_sftp() as sftp:
        sftp.put(str(local), destination)
    archive_hash = hashlib.sha256(local.read_bytes()).hexdigest()
    script = r"""
import fcntl, hashlib, json, os, pathlib, sys, zipfile
root=pathlib.Path(sys.argv[1]).resolve(); archive_path=pathlib.Path(sys.argv[2]); expected=sys.argv[3]
lock_path=root/'build/testkit/run.lock'; lock_path.parent.mkdir(parents=True,exist_ok=True)
with lock_path.open('a+b') as lock:
 fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
 if hashlib.sha256(archive_path.read_bytes()).hexdigest()!=expected: raise RuntimeError('archive hash mismatch')
 with zipfile.ZipFile(archive_path) as archive:
  data=json.loads(archive.read('manifest.json')); files=data['files']; previous=data['previous']
  def target(name):
   relative=pathlib.PurePosixPath(name)
   if relative.is_absolute() or '..' in relative.parts or relative.parts[0] in ('.git','build'): raise RuntimeError('unsafe source path')
   p=root/name
   if p.is_symlink() or not p.resolve().is_relative_to(root): raise RuntimeError('source symlink')
   return p
  def digest(p):
   return hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None
  for name in set(files)|set(data['deleted']):
   current=digest(target(name)); desired=files.get(name)
   if current!=previous.get(name) and current!=desired: raise RuntimeError('source conflict: '+name)
  for name in data['changed']:
   if hashlib.sha256(archive.read('source/'+name)).hexdigest()!=files[name]: raise RuntimeError('file hash mismatch: '+name)
  history=root/'build/testkit/sync-history'/archive_path.stem
  for name in data['changed']+data['deleted']:
   p=target(name)
   if p.exists() and digest(p)!=files.get(name):
    backup=history/name; backup.parent.mkdir(parents=True,exist_ok=True); p.replace(backup)
   if name in files:
    p.parent.mkdir(parents=True,exist_ok=True)
    temporary=p.with_name(p.name+'.verdandi-sync-tmp'); temporary.write_bytes(archive.read('source/'+name)); temporary.replace(p)
  for name,expected_hash in files.items():
   if digest(target(name))!=expected_hash: raise RuntimeError('final source mismatch: '+name)
  (root/'build/testkit/source-manifest.json').write_text(json.dumps(files,sort_keys=True))
 print('SOURCE_VERIFIED',len(files),len(data['changed']),len(data['deleted']),flush=True)
"""
    command = shlex.join(["python3", "-c", script, remote.project, destination, archive_hash])
    print(remote.run(command, timeout=60).strip(), flush=True)
    atomic_json(state, manifest)


def remote_campaign(remote, options, manifest):
    synchronize(remote, manifest, options["run_id"])
    payload = {**options, "targets": ["local"], "host": "local", "password": remote.password, "source_manifest": manifest}
    payload["output"] = remote.project + f"/build/testkit/runs/{options['run_id']}-linux"
    python = remote.project + "/build/tools/python-build/bin/python"
    invocation = shlex.join(["setsid", "--wait", python, "-u", "-B", remote.project + "/testkit/run.py", "--worker"])
    stdin, stdout, stderr = remote._client.exec_command(invocation)
    channel = stdout.channel
    remote._client.get_transport().set_keepalive(10)
    stdin.write(json.dumps(payload) + "\n")
    stdin.flush()
    log = Path(options["output"]) / "linux-controller.log"
    last_heartbeat = time.monotonic()
    started = last_heartbeat
    maximum = 7200 + (options["duration"] * 2 if options["mode"] == "soak" else 0)
    written = 0
    try:
        with log.open("w", encoding="utf-8") as output:
            while True:
                for ready, read in ((channel.recv_ready, channel.recv), (channel.recv_stderr_ready, channel.recv_stderr)):
                    while ready():
                        text = redact(read(65536).decode("utf-8", errors="replace"))
                        written += len(text.encode())
                        if written > 256 * 1024 * 1024:
                            raise RuntimeError("Remote campaign log exceeded 256 MiB")
                        output.write(text)
                        output.flush()
                        print(text, end="", flush=True)
                if channel.exit_status_ready():
                    if not channel.recv_ready() and not channel.recv_stderr_ready():
                        break
                now = time.monotonic()
                if now - last_heartbeat > 10:
                    stdin.write("heartbeat\n")
                    stdin.flush()
                    last_heartbeat = now
                if now - started > maximum:
                    raise TimeoutError("Remote campaign exceeded its outer deadline")
                time.sleep(0.1)
        status = channel.recv_exit_status()
    except BaseException:
        try:
            stdin.write("cancel\n")
            stdin.flush()
        except OSError:
            pass
        raise
    finally:
        channel.close()
    with remote._client.open_sftp() as sftp:
        with sftp.file(payload["output"] + "/report.json") as result:
            report = json.loads(result.read())
        sftp.get(payload["output"] + "/report.json", str(Path(options["output"]) / "linux-report.json"))
        sftp.get(payload["output"] + "/report.md", str(Path(options["output"]) / "linux-report.md"))
    if (status == 0) != (report["status"] == "pass") or report["status"] == "running":
        raise RuntimeError("Remote exit/report status disagree")
    return report
