import os
import sys
import ctypes
from pathlib import Path


def _load_core_libs():
    lib_dir = Path(__file__).parent / "lib"

    # 动态库加载顺序优化
    if sys.platform.startswith("linux"):
        # 修正拼写错误：llib_dir → lib_dir
        os.environ[
            'LD_LIBRARY_PATH'] = f"{str(lib_dir)}:{os.environ.get('LD_LIBRARY_PATH', '')}"
        ctypes.CDLL(str(lib_dir / "liblsm_shared.so"), mode=ctypes.RTLD_GLOBAL)
    elif sys.platform == "darwin":
        os.environ[
            'DYLD_LIBRARY_PATH'] = f"{str(lib_dir)}:{os.environ.get('DYLD_LIBRARY_PATH', '')}"
        ctypes.CDLL(str(lib_dir / "liblsm_shared.dylib"),
                    mode=ctypes.RTLD_GLOBAL)

    # 显式导入绑定模块（优化路径处理）
    sys.path.insert(0, str(lib_dir))
    try:
        from lsm_pybind import LSM, IsolationLevel  # 注意这里需要导入 IsolationLevel
        globals()["LSM"] = LSM
        globals()["IsolationLevel"] = IsolationLevel
    except ImportError as e:
        raise RuntimeError(f"Failed to import LSM binding: {e}") from e
    finally:
        sys.path.pop(0)


_load_core_libs()
