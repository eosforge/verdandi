#!/usr/bin/env bash

# 只检查已有工具与项目缓存. Linux 默认执行 Go race, --no-race 可显式关闭.
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
    (cd "$astra_root" && env CARGO_HOME="$astra_root/build/deps/cargo" CARGO_TARGET_DIR="$astra_root/build/proto/target" CARGO_NET_OFFLINE=true cargo "$@")
}
bash "$astra_root/scripts/generate-proto.sh" --check
generator_check fmt --manifest-path "$astra_root/proto/generator/Cargo.toml" --all --check
generator_check clippy --manifest-path "$astra_root/proto/generator/Cargo.toml" --frozen --all-targets --jobs "$jobs" -- -D warnings
generator_check test --manifest-path "$astra_root/proto/generator/Cargo.toml" --frozen --jobs "$jobs"
if [[ "$service" == all || "$service" == star ]]; then
    bash "$astra_root/astra/build.sh" test --profile release
    bash "$astra_root/astra/build.sh" check-generated
fi
if [[ "$service" == all || "$service" == supervisor ]]; then
    formatting="$(gofmt -l "$astra_root/supervisor/cmd" "$astra_root/supervisor/internal")"
    if [[ -n "$formatting" ]]; then printf '%s\n' "$formatting" >&2; exit 1; fi
    bash "$astra_root/supervisor/go.sh" mod tidy -diff
    bash "$astra_root/supervisor/go.sh" mod verify
    bash "$astra_root/supervisor/go.sh" vet -p "$jobs" ./...
    bash "$astra_root/supervisor/go.sh" test -p "$jobs" -count=1 -shuffle=on -timeout=60s ./...
    if (( fuzz_seconds > 0 )); then
        bash "$astra_root/supervisor/go.sh" test -run='^$' -fuzz=FuzzAccountFileNeverAcceptsInvalidRoles -fuzztime="${fuzz_seconds}s" -parallel=2 ./internal/admission
        bash "$astra_root/supervisor/go.sh" test -run='^$' -fuzz=FuzzPersistedMemberDecodeRoundTrips -fuzztime="${fuzz_seconds}s" -parallel=2 ./internal/membership
    fi
    if [[ "$race" == true ]]; then
        env CGO_ENABLED=1 bash "$astra_root/supervisor/go.sh" test -race -p "$jobs" -count=1 -shuffle=on -timeout=180s ./...
    fi
    bash "$astra_root/supervisor/go.sh" build -trimpath -p "$jobs" -o ../build/supervisor/supervisor ./cmd/supervisor
    "$astra_root/build/supervisor/supervisor" --version
fi
printf '%s\n' 'Service checks passed.'
