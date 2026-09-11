#!/usr/bin/env bash

# 只检查已有工具与项目缓存. Linux 默认执行 Go race, --no-race 可显式关闭.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
verdandi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
service=all
jobs=2
race=true
fuzz_seconds=0
case "$(uname -s)" in MINGW*|MSYS*) race=false ;; esac
for argument in "$@"; do
    case "$argument" in
        --service=all|--service=peer|--service=supervisor) service="${argument#*=}" ;;
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
peer_check() { bash "$verdandi_root/peer/cargo.sh" "$@"; }
bash "$verdandi_root/scripts/generate-proto.sh" --check
peer_check fmt --manifest-path ../proto/generator/Cargo.toml --all --check
peer_check clippy --manifest-path ../proto/generator/Cargo.toml --frozen --all-targets --jobs "$jobs" -- -D warnings
peer_check test --manifest-path ../proto/generator/Cargo.toml --frozen --jobs "$jobs"
if [[ "$service" == all || "$service" == peer ]]; then
    peer_check fmt --all --check
    peer_check clippy --workspace --frozen --all-targets --all-features --jobs "$jobs" -- -D warnings
    peer_check test --workspace --frozen --all-features --jobs "$jobs" -- --test-threads="$jobs"
    peer_check doc --workspace --frozen --no-deps --jobs "$jobs"
    peer_check build --workspace --frozen --release --bins --jobs "$jobs"
    "$verdandi_root/build/peer/target/release/peer" --version
    "$verdandi_root/build/peer/target/release/planet" --version
fi
if [[ "$service" == all || "$service" == supervisor ]]; then
    formatting="$(gofmt -l "$verdandi_root/supervisor/cmd" "$verdandi_root/supervisor/internal")"
    if [[ -n "$formatting" ]]; then printf '%s\n' "$formatting" >&2; exit 1; fi
    bash "$verdandi_root/supervisor/go.sh" mod tidy -diff
    bash "$verdandi_root/supervisor/go.sh" mod verify
    bash "$verdandi_root/supervisor/go.sh" vet -p "$jobs" ./...
    bash "$verdandi_root/supervisor/go.sh" test -p "$jobs" -count=1 -shuffle=on -timeout=30s ./...
    if (( fuzz_seconds > 0 )); then
        bash "$verdandi_root/supervisor/go.sh" test -run='^$' -fuzz=FuzzAccountFileNeverAcceptsInvalidRoles -fuzztime="${fuzz_seconds}s" -parallel=2 ./internal/admission
        bash "$verdandi_root/supervisor/go.sh" test -run='^$' -fuzz=FuzzPersistedMemberDecodeRoundTrips -fuzztime="${fuzz_seconds}s" -parallel=2 ./internal/membership
    fi
    if [[ "$race" == true ]]; then
        env CGO_ENABLED=1 bash "$verdandi_root/supervisor/go.sh" test -race -p "$jobs" -count=1 -shuffle=on -timeout=120s ./...
    fi
    bash "$verdandi_root/supervisor/go.sh" build -trimpath -p "$jobs" -o ../build/supervisor/supervisor ./cmd/supervisor
    "$verdandi_root/build/supervisor/supervisor" --version
fi
printf '%s\n' 'Service checks passed.'
