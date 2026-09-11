#!/usr/bin/env bash
# 仅选择已有项目 Python, 构建、生成、测试逻辑由同目录 Python 入口持有.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
verdandi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
verdandi_python="$verdandi_root/build/tools/python-build/bin/python"
if [[ ! -x "$verdandi_python" ]]; then verdandi_python=python3; fi
exec "$verdandi_python" -B "$verdandi_root/peer-cpp/build.py" "$@"
