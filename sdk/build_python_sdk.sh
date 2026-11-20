#!/bin/bash
set -euo pipefail

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    echo "用法: [PYTHON=/path/to/python3.13] sdk/build_python_sdk.sh [输出目录]"
    echo "构建 macOS arm64 / CPython 3.13 的 Release wheel，默认输出至 dist/。"
    exit 0
fi
[[ $# -le 1 ]] || { echo "只接受一个输出目录" >&2; exit 1; }
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
python=${PYTHON:-python3.13}
# 固定解释器的绝对路径，切换工作目录后仍使用原先选定的环境。
python=$("$python" -c 'import sys; print(sys.executable)')
"$python" - <<'PY'
import platform, sys, sysconfig
if not (sys.platform == "darwin" and platform.machine() == "arm64" and
        sys.implementation.name == "cpython" and sys.version_info[:2] == (3, 13) and
        not sysconfig.get_config_var("Py_GIL_DISABLED") and not sysconfig.get_config_var("Py_DEBUG")):
    sys.exit("当前 wheel 需要 macOS arm64 和标准 CPython 3.13。")
PY

out=${1:-"$repo/dist"}
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/kopidb-wheel.XXXXXX")
trap 'rm -rf "$work"' EXIT

# 复用 C++ 发布构建，取得同一份引擎、安装用头文件和依赖许可。
bash "$repo/scripts/package_cpp.sh" "$work/cpp"
archives=("$work"/cpp/*.tar.gz)
tar -xzf "${archives[0]}" -C "$work"
sdk_dirs=("$work"/kopidb-*-macos-arm64)
cpp=${sdk_dirs[0]}
stage="$work/python"
mkdir -p "$stage/kopidb/core/lib"
cp "$repo/sdk/kopidb/"{pyproject.toml,setup.py} "$repo/sdk/kopidb_pybind.cpp" "$stage/"
cp "$repo/sdk/kopidb/kopidb/"{*.py,py.typed} "$stage/kopidb/"
cp "$repo/sdk/kopidb/kopidb/core/"{*.py,*.pyi} "$stage/kopidb/core/"
touch "$stage/kopidb/core/lib/__init__.py"
cp "$cpp/lib/libkopidb.so" "$stage/kopidb/core/lib/"
cp -R "$cpp/include" "$cpp/LICENSE" "$cpp/licenses" "$stage/"
cp "$repo/packaging/python/pybind11-LICENSE" "$stage/licenses/"

# 两个二进制以同一最低系统版本构建，wheel 标签必须与实际产物匹配。
minimum=$(awk '/^Library minimum macOS:/ {print $NF}' "$cpp/BUILD-INFO.txt")
[[ -n "$minimum" ]] || { echo "C++ SDK 未报告最低系统版本" >&2; exit 1; }
export MACOSX_DEPLOYMENT_TARGET="$minimum" ARCHFLAGS="-arch arm64"
"$python" -m pip wheel --no-deps --wheel-dir "$work/wheels" "$stage"
wheels=("$work"/wheels/*.whl)
wheel=${wheels[0]}
"$python" "$repo/packaging/python/check_wheel.py" "$wheel" "$minimum"
mv "$wheel" "$out/"
echo "已生成 $out/$(basename "$wheel")"
