#!/usr/bin/env bash

# 只检查已有工具与项目缓存. Linux 默认执行 Go race, --no-race 可显式关闭.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
verdandi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
# 仅在这个脚本进程内优先选取已获准的项目工具, 不修改用户 PATH 或安装缺失工具.
for verdandi_tool in "$verdandi_root/build/tools/rust-1.98.1/bin" "$verdandi_root/build/tools/go-1.27.1/bin"; do
    if [[ -d "$verdandi_tool" ]]; then export PATH="$verdandi_tool:$PATH"; fi
done
service=all
jobs=2
race=true
fuzz_seconds=0
case "$(uname -s)" in MINGW*|MSYS*) race=false ;; esac
for argument in "$@"; do
    case "$argument" in
        --service=all|--service=star|--service=supervisor) service="${argument#*=}" ;;
        --implementation=cpp) ;;
        --jobs=[1-8]) jobs="${argument#*=}" ;;
        --no-race) race=false ;;
        --race) race=true ;;
        --fuzz-seconds=*)
            fuzz_seconds="${argument#*=}"
            if [[ ! "$fuzz_seconds" =~ ^(0|[1-9][0-9]{0,2})$ ]] || (( fuzz_seconds > 300 )); then
                printf '%s\n' 'Fuzz seconds must be between 0 and 300.' >&2; exit 2
            fi ;;
        *) printf 'Unknown check option: %s\n' "$argument" >&2; exit 2 ;;
    esac
done
# 原生服务编译不需要 protoc; 一键质量检查单独核对全部已提交生成源码.
generator_check() {
    (cd "$verdandi_root" && env CARGO_HOME="$verdandi_root/build/deps/cargo" CARGO_TARGET_DIR="$verdandi_root/build/proto/target" CARGO_NET_OFFLINE=true cargo "$@")
}
bash "$verdandi_root/scripts/generate-proto.sh" --check
generator_check fmt --manifest-path "$verdandi_root/proto/generator/Cargo.toml" --all --check
generator_check clippy --manifest-path "$verdandi_root/proto/generator/Cargo.toml" --frozen --all-targets --jobs "$jobs" -- -D warnings
generator_check test --manifest-path "$verdandi_root/proto/generator/Cargo.toml" --frozen --jobs "$jobs"
if [[ "$service" == all || "$service" == star ]]; then
    bash "$verdandi_root/cluster-cpp/build.sh" test --profile release
    bash "$verdandi_root/cluster-cpp/build.sh" check-generated
fi
if [[ "$service" == all || "$service" == supervisor ]]; then
    formatting="$(gofmt -l "$verdandi_root/supervisor/cmd" "$verdandi_root/supervisor/internal")"
    if [[ -n "$formatting" ]]; then printf '%s\n' "$formatting" >&2; exit 1; fi
    bash "$verdandi_root/supervisor/go.sh" mod tidy -diff
    bash "$verdandi_root/supervisor/go.sh" mod verify
    bash "$verdandi_root/supervisor/go.sh" vet -p "$jobs" ./...
    bash "$verdandi_root/supervisor/go.sh" test -p "$jobs" -count=1 -shuffle=on -timeout=60s ./...
    if (( fuzz_seconds > 0 )); then
        bash "$verdandi_root/supervisor/go.sh" test -run='^$' -fuzz=FuzzAccountFileNeverAcceptsInvalidRoles -fuzztime="${fuzz_seconds}s" -parallel=2 ./internal/admission
        bash "$verdandi_root/supervisor/go.sh" test -run='^$' -fuzz=FuzzPersistedMemberDecodeRoundTrips -fuzztime="${fuzz_seconds}s" -parallel=2 ./internal/membership
    fi
    if [[ "$race" == true ]]; then
        env CGO_ENABLED=1 bash "$verdandi_root/supervisor/go.sh" test -race -p "$jobs" -count=1 -shuffle=on -timeout=180s ./...
    fi
    bash "$verdandi_root/supervisor/go.sh" build -trimpath -p "$jobs" -o ../build/supervisor/supervisor ./cmd/supervisor
    "$verdandi_root/build/supervisor/supervisor" --version
fi
printf '%s\n' 'Service checks passed.'
