"""KopiDB 的文本与字节接口。文本统一使用 UTF-8，数据库保存原始字节。"""
from __future__ import annotations

from collections.abc import Iterable, Iterator
from typing import Generic, Literal, TypeVar, overload

from . import core


def _encode(item: str | bytes) -> bytes:
    if isinstance(item, bytes):
        return item
    if isinstance(item, str):
        return item.encode("utf-8")
    raise TypeError("key and value must be str or bytes")


def _check_mode(as_bytes: bool) -> None:
    if not isinstance(as_bytes, bool):
        raise TypeError("as_bytes must be a bool")


def _batch(items):
    # 拒绝把单个 key "abc" 意外当作一批 key "a", "b", "c"。
    if isinstance(items, (str, bytes, bytearray, memoryview)):
        raise TypeError("expected a collection of batch items")
    return iter(items)


_Store = TypeVar("_Store", core.LSM, core.TranContext)


class _KeyValue(Generic[_Store]):
    _raw: _Store

    def put(self, key: str | bytes, value: str | bytes) -> None:
        self._raw.put(_encode(key), _encode(value))

    @overload
    def get(self, key: str | bytes, *, as_bytes: Literal[False] = False) -> str | None: ...
    @overload
    def get(self, key: str | bytes, *, as_bytes: Literal[True]) -> bytes | None: ...
    @overload
    def get(self, key: str | bytes, *, as_bytes: bool) -> str | bytes | None: ...
    def get(self, key: str | bytes, *, as_bytes: bool = False) -> str | bytes | None:
        _check_mode(as_bytes)
        value = self._raw.get(_encode(key))
        if value is None or as_bytes:
            return value
        return value.decode("utf-8")

    def remove(self, key: str | bytes) -> None:
        self._raw.remove(_encode(key))


class TranContext(_KeyValue[core.TranContext]):
    def __init__(self, db: LSM, isolation_level: core.IsolationLevel):
        self._db = db  # 事务使用期间，所属数据库也必须保持打开。
        self._raw = db._raw.begin_tran(isolation_level)

    def get_isolation_level(self) -> core.IsolationLevel:
        return self._raw.get_isolation_level()

    def commit(self) -> bool:
        return self._raw.commit()

    def abort(self) -> bool:
        return self._raw.abort()


class LSM(_KeyValue[core.LSM]):
    def __init__(self, path: str):
        self._raw = core.LSM(path)

    def put_batch(self, kvs: Iterable[tuple[str | bytes, str | bytes]]) -> None:
        """批量写入 (key, value)。同一批可混用 str 和 bytes。

        每个 str 按 UTF-8 编码，每个 bytes 原样保存。任何一项类型错误
        或 key/value 为空时，整批都不会写入。
        """
        entries = []
        for pair in _batch(kvs):
            if not isinstance(pair, (tuple, list)) or len(pair) != 2:
                raise TypeError("each batch entry must be a (key, value) pair")
            entries.append((_encode(pair[0]), _encode(pair[1])))
        # 整批转换完成后才进入引擎，后面的参数有错时不会写入前面的项。
        self._raw.put_batch(entries)

    @overload
    def get_batch(self, keys: Iterable[str | bytes], *, as_bytes: Literal[False] = False) -> list[tuple[str, str | None]]: ...
    @overload
    def get_batch(self, keys: Iterable[str | bytes], *, as_bytes: Literal[True]) -> list[tuple[bytes, bytes | None]]: ...
    @overload
    def get_batch(self, keys: Iterable[str | bytes], *, as_bytes: bool) -> list[tuple[str, str | None]] | list[tuple[bytes, bytes | None]]: ...
    def get_batch(self, keys: Iterable[str | bytes], *, as_bytes: bool = False):
        _check_mode(as_bytes)
        rows = self._raw.get_batch([_encode(key) for key in _batch(keys)])
        if as_bytes:
            return rows
        return [(key.decode("utf-8"), value.decode("utf-8") if value is not None else None)
                for key, value in rows]

    def remove_batch(self, keys: Iterable[str | bytes]) -> None:
        self._raw.remove_batch([_encode(key) for key in _batch(keys)])

    @overload
    def begin(self, tranc_id: int = 0, *, as_bytes: Literal[False] = False) -> Iterator[tuple[str, str]]: ...
    @overload
    def begin(self, tranc_id: int = 0, *, as_bytes: Literal[True]) -> Iterator[tuple[bytes, bytes]]: ...
    @overload
    def begin(self, tranc_id: int = 0, *, as_bytes: bool) -> Iterator[tuple[str, str]] | Iterator[tuple[bytes, bytes]]: ...
    def begin(self, tranc_id: int = 0, *, as_bytes: bool = False):
        _check_mode(as_bytes)
        rows = self._raw.begin(tranc_id)
        if as_bytes:
            return rows
        return ((key.decode("utf-8"), value.decode("utf-8")) for key, value in rows)

    def end(self) -> Iterator[tuple[bytes, bytes]]:
        return self._raw.end()

    def begin_tran(self, isolation_level: core.IsolationLevel) -> TranContext:
        return TranContext(self, isolation_level)

    def flush(self) -> None:
        self._raw.flush()

    def flush_all(self) -> None:
        self._raw.flush_all()

    def clear(self) -> None:
        self._raw.clear()
