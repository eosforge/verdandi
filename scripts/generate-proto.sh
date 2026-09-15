#!/usr/bin/env bash
# 显式生成协议源码, --check 只比较. 普通服务编译不调用本入口.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
astra_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
if command -v cygpath >/dev/null 2>&1; then astra_root="$(cygpath -m "$astra_root")"; fi
exec env CARGO_HOME="$astra_root/build/deps/cargo" CARGO_TARGET_DIR="$astra_root/build/proto/target" CARGO_NET_OFFLINE=true \
    cargo run --manifest-path "$astra_root/proto/generator/Cargo.toml" --locked --offline --jobs 2 -- "$@"
