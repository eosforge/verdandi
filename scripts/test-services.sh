#!/usr/bin/env bash
# 一键服务回归/长时测试. 参数仅传给子进程, 不安装依赖或修改调用终端的配置.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
astra_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
skip_checks=false
implementation=cpp
arguments=()
for argument in "$@"; do
    case "$argument" in
        --skip-checks) skip_checks=true ;;
        --implementation=cpp) implementation="${argument#*=}" ;;
        --implementation|--implementation=*) printf '%s\n' 'Use --implementation=cpp; the Rust service is retired.' >&2; exit 2 ;;
        *) arguments+=("$argument") ;;
    esac
done
if [[ "$skip_checks" == false ]]; then bash "$astra_root/scripts/check-services.sh" --implementation="$implementation"; fi
astra_python="$astra_root/build/tools/python-build/bin/python"
if [[ ! -x "$astra_python" ]]; then printf '%s\n' 'Prepare the approved project-local Python environment first.' >&2; exit 1; fi
cd -- "$astra_root"
if [[ "$skip_checks" == false ]]; then "$astra_python" -B -m unittest discover -s testkit/tests -p 'test_*.py'; fi
if [[ "$skip_checks" == false && "$implementation" == cpp ]]; then
    "$astra_python" -B astra/test_processes.py --binaries "$astra_root/build/astra/release"
fi
exec "$astra_python" -B -m testkit.services --implementation="$implementation" "${arguments[@]}"
