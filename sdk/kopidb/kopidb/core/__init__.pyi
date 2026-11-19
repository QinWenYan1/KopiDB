from typing import Iterator, List, Optional, Tuple
from enum import Enum


class IsolationLevel(Enum):
    READ_UNCOMMITTED = 0
    READ_COMMITTED = 1
    REPEATABLE_READ = 2
    SERIALIZABLE = 3  # 实验性级别，完整串行化尚未实现。
    READ_UNOP_COMMITTED = READ_UNCOMMITTED
    READ_OP_COMMITTED = READ_COMMITTED


class TwoMergeIterator:

    def __iter__(self) -> Iterator[Tuple[bytes, bytes]]:
        ...

    def __next__(self) -> Tuple[bytes, bytes]:
        ...

class Level_Iterator:

    def __iter__(self) -> Iterator[Tuple[bytes, bytes]]:
        ...

    def __next__(self) -> Tuple[bytes, bytes]:
        ...


class TranContext:
    def get_isolation_level(self) -> IsolationLevel:
        ...

    # 事务操作
    def commit(self) -> bool:
        ...

    def abort(self) -> bool:
        ...

    # 事务内操作
    def put(self, key: bytes, value: bytes) -> None:
        ...

    def remove(self, key: bytes) -> None:
        ...

    def get(self, key: bytes) -> Optional[bytes]:
        ...


class LSM:

    def __init__(self, path: str) -> None:
        ...

    # 基础操作
    def put(self, key: bytes, value: bytes) -> None:
        ...

    def get(self, key: bytes) -> Optional[bytes]:
        ...

    def remove(self, key: bytes) -> None:
        ...

    # 批量操作
    def put_batch(self, kvs: List[Tuple[bytes, bytes]]) -> None:
        ...

    def get_batch(self, keys: List[bytes]) -> List[Tuple[bytes, Optional[bytes]]]:
        ...

    def remove_batch(self, keys: List[bytes]) -> None:
        ...

    # 迭代器
    def begin(self, tranc_id: int) -> Level_Iterator:
        ...

    def end(self) -> Level_Iterator:
        ...

    # 事务管理
    def begin_tran(self, isolation_level: IsolationLevel) -> TranContext:
        ...

    # 维护操作
    def clear(self) -> None:
        ...  # ! Fixme

    def flush(self) -> None:
        ...

    def flush_all(self) -> None:
        ...
