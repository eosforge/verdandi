#!/usr/bin/env bash
# Linux 兼容入口；所有构建策略由同目录的标准库 Python 实现负责。
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
python=python3
if [[ "${1:-}" == --python ]]; then
    if (($# < 2)) || [[ -z "$2" ]]; then
        printf '%s\n' '--python requires an existing interpreter path.' >&2
        exit 2
    fi
    python=$2
    shift 2
fi
"$python" -I -S -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' || {
    printf '%s\n' 'Python 3.10+ is required; select it with build.sh --python PATH [arguments].' >&2
    exit 1
}
exec "$python" -B "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/build.py" "$@"
