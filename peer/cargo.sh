#!/usr/bin/env bash

# 独立进程限定 Peer Cargo 缓存设置的作用范围, 禁止 source 改变调用终端.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute: bash peer/cargo.sh [Cargo arguments]; do not source this script.' >&2
    return 1
fi
set -euo pipefail
verdandi_peer_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
verdandi_root="$(cd -- "$verdandi_peer_root/.." && pwd -P)"
# Git Bash 中的本机 Cargo 需要 Windows 路径, Linux 则保留 POSIX 路径.
if command -v cygpath >/dev/null 2>&1; then
    verdandi_root="$(cygpath -m "$verdandi_root")"
fi

# 脚本自身进入 crate 目录, 让 Cargo 的子命令和参数可以原样转发.
# exec 让 Cargo 接管脚本进程, 保留退出状态和信号传递路径.
cd -- "$verdandi_peer_root"
exec env CARGO_HOME="$verdandi_root/build/deps/cargo" CARGO_TARGET_DIR="$verdandi_root/build/peer/target" CARGO_NET_OFFLINE=true cargo "$@"
