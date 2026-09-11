#!/usr/bin/env bash
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
verdandi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
verdandi_python="$verdandi_root/build/tools/python-build/bin/python"
if [[ ! -x "$verdandi_python" ]]; then verdandi_python=python3; fi
exec "$verdandi_python" -B "$verdandi_root/testkit/run.py" "$@"

