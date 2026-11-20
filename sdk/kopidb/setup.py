from pathlib import Path

from pybind11.setup_helpers import Pybind11Extension
from setuptools import setup

# 构建脚本把当前版本的 C++ SDK 和绑定源码放在临时工程中。
# 使用构建 wheel 的解释器提供的 Python.h 和 ABI 编译扩展。
engine = Path("kopidb/core/lib/libkopidb.so")
if not engine.is_file():
    raise RuntimeError("请从仓库运行 sdk/build_python_sdk.sh，以准备 C++ SDK")

setup(ext_modules=[Pybind11Extension(
    "kopidb.core.lib.kopidb_pybind",
    ["kopidb_pybind.cpp"],
    include_dirs=["include/kopidb"],
    extra_objects=[str(engine)],
    extra_link_args=["-Wl,-rpath,@loader_path"],
    cxx_std=20,
)])
