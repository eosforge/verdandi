#!/usr/bin/env bash

# 独立进程限定缓存与离线设置, 禁止 source 改变调用终端.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute: bash supervisor/go.sh [Go arguments]; do not source this script.' >&2
    return 1
fi
set -euo pipefail
astra_supervisor_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
astra_build_root="$(cd -- "$astra_supervisor_root/.." && pwd -P)/build"
# Git Bash 调用 Windows Go 时转换路径, Linux 保留 POSIX 路径.
if command -v cygpath >/dev/null 2>&1; then
    astra_build_root="$(cygpath -m "$astra_build_root")"
fi
mkdir -p -- "$astra_build_root/tmp/supervisor"
cd -- "$astra_supervisor_root"
exec env GOMODCACHE="$astra_build_root/deps/go/pkg/mod" GOCACHE="$astra_build_root/cache/go" \
    TMPDIR="$astra_build_root/tmp/supervisor" GOTMPDIR="$astra_build_root/tmp/supervisor" GOMAXPROCS=2 \
    GOTOOLCHAIN=local GOPROXY=off GOSUMDB=off GOFLAGS=-mod=readonly GOWORK=off go "$@"
