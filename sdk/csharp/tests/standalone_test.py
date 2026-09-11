#!/usr/bin/env python3
"""Compatibility entry for the independently selected CSharp regression."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host")
    parser.add_argument("--ssh-user", default="ubuntu")
    options = parser.parse_args()
    args = [sys.executable, "-B", str(ROOT / "testkit/run.py"), "regression", "--targets", "local", "--languages", "csharp", "--user", options.ssh_user]
    if options.host:
        args += ["--host", options.host]
    return subprocess.call(args, cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
