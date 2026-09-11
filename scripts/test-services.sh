#!/usr/bin/env bash
# 一键服务回归/长时测试. 参数仅传给子进程, 不安装依赖或修改调用终端的配置.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    printf '%s\n' 'Execute this script; do not source it.' >&2
    return 1
fi
set -euo pipefail
verdandi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
skip_checks=false
arguments=()
for argument in "$@"; do
    case "$argument" in
        --skip-checks) skip_checks=true ;;
        *) arguments+=("$argument") ;;
    esac
done
if [[ "$skip_checks" == false ]]; then bash "$verdandi_root/scripts/check-services.sh"; fi
verdandi_python="$verdandi_root/build/tools/python-build/bin/python"
if [[ ! -x "$verdandi_python" ]]; then printf '%s\n' 'Prepare the approved project-local Python environment first.' >&2; exit 1; fi
cd -- "$verdandi_root"
if [[ "$skip_checks" == false ]]; then "$verdandi_python" -B -m unittest discover -s testkit/tests -p 'test_*.py'; fi
exec "$verdandi_python" -B -m testkit.services "${arguments[@]}"
