import tempfile
import unittest
from pathlib import Path

import kopidb


class DatabaseTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "db"
        self.db = kopidb.LSM(path=str(self.path))
        # 先关闭引擎，再让 TemporaryDirectory 删除它正在使用的数据目录。
        self.addCleanup(delattr, self, "db")

    def reopen(self):
        del self.db
        self.db = kopidb.LSM(str(self.path))

    def test_flush_and_reopen(self):
        self.db.put("名字", "小林")
        self.db.flush()
        # 关闭数据库也会自动刷盘，因此先验证显式调用已经生成 SST。
        first_files = set(self.path.glob("sst_*"))
        self.assertTrue(first_files)
        self.db.remove("名字")
        self.db.put("头像", b"\xff\x00")
        self.db.flush_all()
        self.assertTrue(set(self.path.glob("sst_*")) - first_files)

        self.reopen()
        self.assertIsNone(self.db.get("名字"))
        self.assertEqual(self.db.get("头像", as_bytes=True), b"\xff\x00")
        self.assertEqual(list(self.db.begin(as_bytes=True)), [("头像".encode(), b"\xff\x00")])

    def test_clear_and_reuse(self):
        self.db.put("disk", "old")
        self.db.flush_all()
        self.db.put("memory", "old")
        self.assertEqual(list(self.db.begin()), list(self.db.begin(0)))
        self.db.clear()
        self.assertEqual(list(self.db.begin()), [])
        self.assertEqual(list(self.db.end()), [])
        self.db.put("fresh", "新值")
        self.reopen()
        self.assertEqual(list(self.db.begin()), [("fresh", "新值")])

    def test_clear_rejects_active_transaction(self):
        self.db.put("key", "old")
        tx = self.db.begin_tran(kopidb.IsolationLevel.READ_COMMITTED)
        tx.put("key", "new")
        with self.assertRaises(RuntimeError):
            self.db.clear()
        self.assertEqual(self.db.get("key"), "old")
        self.assertTrue(tx.commit())
        self.assertEqual(self.db.get("key"), "new")
        self.db.clear()
        self.assertIsNone(self.db.get("key"))

    def test_binary_crud(self):
        # 相同前缀的两个 key 用来检测零字节处是否发生了截断。
        key = b"k\x00\xff"
        self.db.put(b"k", b"prefix")
        self.db.put(key, b"\xff\x00")
        self.assertEqual(self.db.get(key, as_bytes=True), b"\xff\x00")
        self.db.put(key, b"\x00new\xff")
        self.assertEqual(self.db.get(key, as_bytes=True), b"\x00new\xff")
        self.db.remove(key)
        self.assertIsNone(self.db.get(key, as_bytes=True))
        self.assertEqual(self.db.get(b"k", as_bytes=True), b"prefix")

    def test_text_and_bytes_share_keys_across_reads(self):
        key, value = "名字", "小林"
        raw_key, raw_value = key.encode(), value.encode()
        self.db.put(key, value)
        self.assertEqual(self.db.get(raw_key), value)
        self.assertEqual(self.db.get(key, as_bytes=True), raw_value)
        self.assertIsNone(self.db.get("missing"))
        self.assertEqual(self.db.get_batch([key, "missing", raw_key]),
                         [(key, value), ("missing", None), (key, value)])
        self.assertEqual(self.db.get_batch([key, "missing"], as_bytes=True),
                         [(raw_key, raw_value), (b"missing", None)])
        self.assertEqual(list(self.db.begin()), [(key, value)])
        self.assertEqual(list(self.db.begin(0, as_bytes=True)), [(raw_key, raw_value)])
        self.db.put_batch([(raw_key, "更新"), ("new", b"ok")])
        self.assertEqual(self.db.get_batch([key, "new"]), [(key, "更新"), ("new", "ok")])
        self.db.remove_batch([key, b"new"])
        self.assertEqual(list(self.db.begin()), [])

    def test_binary_read_mode_and_decode_errors(self):
        self.db.put_batch([("name", "小林"), ("头像", b"\xff\x00")])
        self.assertEqual(self.db.get("name"), "小林")
        self.assertEqual(self.db.get_batch(["name", "头像"], as_bytes=True),
                         [(b"name", "小林".encode()), ("头像".encode(), b"\xff\x00")])
        with self.assertRaises(UnicodeDecodeError):
            self.db.get("头像")
        with self.assertRaises(UnicodeDecodeError):
            self.db.get_batch(["头像"])
        with self.assertRaises(UnicodeDecodeError):
            list(self.db.begin())
        self.db.put(b"\xffkey", "ok")
        self.assertEqual(dict(self.db.begin(as_bytes=True)),
                         {b"name": "小林".encode(), "头像".encode(): b"\xff\x00", b"\xffkey": b"ok"})

    def test_transaction_uses_same_text_mode(self):
        tx = self.db.begin_tran(kopidb.IsolationLevel.READ_COMMITTED)
        tx.put("名字", "小林")
        self.assertEqual(tx.get("名字"), "小林")
        self.assertEqual(tx.get("名字".encode(), as_bytes=True), "小林".encode())
        tx.put("头像", b"\xff")
        with self.assertRaises(UnicodeDecodeError):
            tx.get("头像")
        self.assertEqual(tx.get("头像", as_bytes=True), b"\xff")
        tx.remove("名字")
        self.assertIsNone(tx.get("名字"))
        self.assertTrue(tx.commit())
        self.assertEqual(self.db.get("头像", as_bytes=True), b"\xff")

    def test_scalar_is_not_a_batch(self):
        self.db.put("a", "original")
        for call in (self.db.remove_batch, self.db.get_batch, self.db.put_batch):
            with self.assertRaises(TypeError):
                call("ab")
        with self.assertRaises(TypeError):
            self.db.put_batch(["ab"])
        self.assertEqual(self.db.get("a"), "original")

    def test_point_input_errors(self):
        for bad in (123, None, bytearray(b"k")):
            with self.subTest(input=bad):
                with self.assertRaises(TypeError):
                    self.db.put(b"key", bad)
                with self.assertRaises(TypeError):
                    self.db.put(bad, b"value")
                with self.assertRaises(TypeError):
                    self.db.get(bad)
                with self.assertRaises(TypeError):
                    self.db.remove(bad)
        for key, value in [(b"", b"value"), (b"key", b"")]:
            with self.assertRaises(ValueError):
                self.db.put(key, value)
        with self.assertRaises(ValueError):
            self.db.remove(b"")

    def test_batch_results_preserve_keys_order_and_misses(self):
        key, value = b"\xff\x00k", b"\xff\x00"
        self.db.put_batch([(b"other", b"second"), (key, value)])
        self.assertEqual(
            self.db.get_batch([key, b"missing", key, b"other"], as_bytes=True),
            [(key, value), (b"missing", None), (key, value), (b"other", b"second")],
        )
        self.db.remove_batch([key, b"other"])
        self.assertEqual(self.db.get_batch([key, b"other"], as_bytes=True), [(key, None), (b"other", None)])
        self.db.put_batch([])
        self.db.remove_batch([])
        self.assertEqual(self.db.get_batch([]), [])

    def test_invalid_batch_never_applies_earlier_items(self):
        self.db.put(b"existing", b"old")
        for bad_entry, error in [
            ((b"bad", 123), TypeError),
            ((None, b"value"), TypeError),
            ((b"bad",), TypeError),
            ((b"bad", b""), ValueError),
            ((b"", b"value"), ValueError),
        ]:
            with self.subTest(entry=bad_entry):
                with self.assertRaises(error):
                    self.db.put_batch([(b"existing", b"new"), (b"fresh", b"new"), bad_entry])
                self.assertEqual(self.db.get(b"existing", as_bytes=True), b"old")
                self.assertIsNone(self.db.get(b"fresh", as_bytes=True))
        for bad_key, error in [(123, TypeError), (b"", ValueError)]:
            with self.assertRaises(error):
                self.db.remove_batch([b"existing", bad_key])
            self.assertEqual(self.db.get(b"existing", as_bytes=True), b"old")
        with self.assertRaises(TypeError):
            self.db.get_batch([b"existing", 123])

    def test_transaction_commit_reads_own_binary_changes(self):
        key = b"k\x00\xff"
        self.db.put(key, b"\xfeold")
        tx = self.db.begin_tran(kopidb.IsolationLevel.READ_COMMITTED)
        self.assertEqual(tx.get(key, as_bytes=True), b"\xfeold")
        tx.remove(key)
        self.assertIsNone(tx.get(key, as_bytes=True))
        tx.put(key, b"\xff\x00new")
        self.assertEqual(tx.get(key, as_bytes=True), b"\xff\x00new")
        self.assertEqual(self.db.get(key, as_bytes=True), b"\xfeold")
        self.assertTrue(tx.commit())
        self.assertEqual(self.db.get(key, as_bytes=True), b"\xff\x00new")

    def test_transaction_abort_preserves_previous_value(self):
        levels = kopidb.IsolationLevel
        # RU 修改已经进入引擎，RC 的修改仍在事务里；两条路径都应恢复旧值。
        for level in (levels.READ_UNCOMMITTED, levels.READ_COMMITTED):
            with self.subTest(level=level):
                self.db.put(b"k", b"\xffold")
                tx = self.db.begin_tran(level)
                tx.remove(b"k")
                tx.put(b"fresh", b"\xfe")
                self.assertIsNone(tx.get(b"k", as_bytes=True))
                self.assertEqual(tx.get(b"fresh", as_bytes=True), b"\xfe")
                self.assertTrue(tx.abort())
                self.assertEqual(self.db.get(b"k", as_bytes=True), b"\xffold")
                self.assertIsNone(self.db.get(b"fresh", as_bytes=True))

    def test_transaction_write_conflict(self):
        level = kopidb.IsolationLevel.READ_COMMITTED
        older = self.db.begin_tran(level)
        newer = self.db.begin_tran(level)
        older.put(b"k", b"old")
        older.put(b"other", b"should not be written")
        newer.put(b"k", b"new")
        self.assertTrue(newer.commit())
        self.assertFalse(older.commit())
        self.assertEqual(self.db.get(b"k", as_bytes=True), b"new")
        self.assertIsNone(self.db.get(b"other", as_bytes=True))

    def test_transaction_input_errors_leave_transaction_usable(self):
        tx = self.db.begin_tran(kopidb.IsolationLevel.REPEATABLE_READ)
        tx.put(b"valid", b"\x00")
        with self.assertRaises(TypeError):
            tx.put(b"k", 123)
        with self.assertRaises(TypeError):
            tx.put(None, b"value")
        with self.assertRaises(TypeError):
            tx.get(None)
        with self.assertRaises(TypeError):
            tx.remove(None)
        with self.assertRaises(ValueError):
            tx.put(b"k", b"")
        with self.assertRaises(ValueError):
            tx.put(b"", b"value")
        with self.assertRaises(ValueError):
            tx.remove(b"")
        with self.assertRaises(TypeError):
            tx.commit(True)
        with self.assertRaises(TypeError):
            tx.commit(test_fail=True)
        self.assertTrue(tx.commit())
        self.assertEqual(self.db.get(b"valid", as_bytes=True), b"\x00")

    def test_transaction_isolation_levels_and_legacy_names(self):
        levels = kopidb.IsolationLevel
        for spelling, expected in [
            (levels.READ_UNCOMMITTED, levels.READ_UNCOMMITTED),
            (levels.READ_UNOP_COMMITTED, levels.READ_UNCOMMITTED),
            (levels.READ_COMMITTED, levels.READ_COMMITTED),
            (levels.READ_OP_COMMITTED, levels.READ_COMMITTED),
            (levels.REPEATABLE_READ, levels.REPEATABLE_READ),
            (levels.SERIALIZABLE, levels.SERIALIZABLE),
        ]:
            with self.subTest(level=spelling):
                tx = self.db.begin_tran(isolation_level=spelling)
                self.assertEqual(tx.get_isolation_level(), expected)
                self.assertTrue(tx.abort())


if __name__ == "__main__":
    unittest.main()
