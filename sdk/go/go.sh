#!/usr/bin/env bash

# 独立进程限定缓存设置的作用范围；禁止 source 改变调用者的终端。
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute: bash sdk/go/go.sh [Go arguments]; do not source this script.' >&2
    return 1
fi
set -euo pipefail
verdandi_go_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
verdandi_build_root="$(cd -- "$verdandi_go_root/../.." && pwd -P)/build"
# Git Bash 的本机工具需要 Windows 绝对路径；Linux 保留 POSIX 路径。
if command -v cygpath >/dev/null 2>&1; then
    verdandi_build_root="$(cygpath -m "$verdandi_build_root")"
fi
cd -- "$verdandi_go_root"
exec env GOMODCACHE="$verdandi_build_root/deps/go/pkg/mod" GOCACHE="$verdandi_build_root/cache/go" go "$@"
