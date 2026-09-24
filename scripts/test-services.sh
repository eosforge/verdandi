#!/usr/bin/env bash
# 当前服务回归入口. 旧 soak/scale 尚未迁移, 明确拒绝, 不将未执行报告为成功.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
astra_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
astra_command=regression
astra_arguments=()
for argument in "$@"; do
    case "$argument" in
        --mode=regression|--implementation=cpp) ;;
        --skip-checks) astra_command=test ;;
        --jobs=*|--test-jobs=*) astra_arguments+=("$argument") ;;
        --mode=soak|--mode=scale) printf '%s\n' 'Legacy soak/scale is retired; no tests were executed.' >&2; exit 2 ;;
        *) printf 'Unsupported service test option: %s\n' "$argument" >&2; exit 2 ;;
    esac
done
# skip-checks 仅省略协议生成比较, 实际 C++/Go 构建与回归不可跳过. 子入口负责资源预算和失败退出码.
exec bash "$astra_root/astra/build.sh" "$astra_command" --profile release "${astra_arguments[@]}"
