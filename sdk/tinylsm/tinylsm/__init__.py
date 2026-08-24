# tinylsm/__init__.py
from .core import LSM, IsolationLevel  # 从 core 子模块导出 LSM 类

__all__ = ["LSM", "IsolationLevel"]
