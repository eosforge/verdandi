#!/usr/bin/env bash

# 只使用已有工具与项目缓存. 不隐式启动 race、Sanitizer 或 fuzz.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
astra_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
# 仅在这个脚本进程内优先选取已获准的项目工具, 不修改用户 PATH 或安装缺失工具.
for astra_tool in "$astra_root/build/tools/rust-1.98.1/bin" "$astra_root/build/tools/go-1.27.1/bin"; do
    if [[ -d "$astra_tool" ]]; then export PATH="$astra_tool:$PATH"; fi
done
service=all
jobs=0
for argument in "$@"; do
    case "$argument" in
        --service=all|--service=star) service="${argument#*=}" ;;
        --implementation=cpp) ;;
        --jobs=*)
            jobs="${argument#*=}"
            if [[ ! "$jobs" =~ ^(0|[1-9][0-9]{0,2})$ ]] || (( jobs > 256 )); then
                printf '%s\n' 'Jobs must be between 0 and 256.' >&2; exit 2
            fi ;;
        *) printf 'Unknown check option: %s\n' "$argument" >&2; exit 2 ;;
    esac
done
# Rust 生成检查体量小, 单独使用最多两个任务; 原生构建按当前内存预算自适应.
astra_generator_jobs=2
if (( jobs > 0 && jobs < 2 )); then astra_generator_jobs="$jobs"; fi
# 原生服务编译不需要 protoc; 一键质量检查单独核对全部已提交生成源码.
generator_check() {
    (cd "$astra_root" && env CARGO_HOME="$astra_root/build/deps/cargo" CARGO_TARGET_DIR="$astra_root/build/proto/target" CARGO_NET_OFFLINE=true cargo "$@")
}
bash "$astra_root/scripts/generate-proto.sh" --check
generator_check fmt --manifest-path "$astra_root/proto/generator/Cargo.toml" --all --check
generator_check clippy --manifest-path "$astra_root/proto/generator/Cargo.toml" --frozen --all-targets --jobs "${astra_generator_jobs}" -- -D warnings
generator_check test --manifest-path "$astra_root/proto/generator/Cargo.toml" --frozen --jobs "${astra_generator_jobs}"
if [[ "$service" == all || "$service" == star ]]; then
    bash "$astra_root/astra/build.sh" regression --profile release --jobs="$jobs"
fi
printf '%s\n' 'Service checks passed.'
