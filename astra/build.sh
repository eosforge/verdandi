#!/usr/bin/env bash
# 仅选择已有项目 Python, 构建、生成、测试逻辑由同目录 Python 入口持有.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
astra_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
astra_python="$astra_root/build/tools/python-build/bin/python"
if [[ ! -x "$astra_python" ]]; then astra_python=python3; fi
exec "$astra_python" -B "$astra_root/astra/build.py" "$@"
