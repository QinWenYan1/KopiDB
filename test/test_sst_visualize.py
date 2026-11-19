"""普通 SST 可视化回归；独立运行，无需构建数据库或第三方 Python 包。"""

import html
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "sst_visualize.py"

# 2026-11-19 macOS arm64：由真实 SSTBuilder(100, false) 生成，
# SST::open + BlockIterator 全版本读取已逐条核对下面的输入。
# 491 字节，5 个 Block、9 条记录；Meta 位于 361，Bloom/footer 同在 467。
PLAIN_SST = bytes.fromhex("""
    0500616c706861000009000000000000000500616c70686103006f6c640500000000000000080062
    7261766f3c263e08003c76616c7565263e0b0000000000000000001100250003006e30fc78070063
    6861726c69650c0031323334353637383930313203000000000000000000010058486a0005006465
    6c746150004444444444444444444444444444444444444444444444444444444444444444444444
    44444444444444444444444444444444444444444444444444444444444444444444444444444444
    44444444440d00000000000000000001007a11d87704006563686f00000700000000000000040065
    63686f0d006265666f72652d64656c657465010000000000000005006f6d6567610600e59296e595
    a10400000000000000000010002d000300803fe4860600e59296e595a12200e78988e69cace58fb7
    e8b685e8bf87204a5320e79a84e695b4e695b0e7b2bee5baa6010000000000200000000100ccf182
    0b05000000000000000500616c7068610800627261766f3c263e4d0000000700636861726c696507
    00636861726c696574000000050064656c7461050064656c7461dd00000004006563686f05006f6d
    6567612d0100000600e59296e595a10600e59296e595a174693ff669010000d30100000100000000
    0000000100000000002000
""")
BLOCKS = [
    [("alpha", "", 9), ("alpha", "old", 5), ("bravo<&>", "<value&>", 11)],
    [("charlie", "123456789012", 3)],
    [("delta", "D" * 80, 13)],
    [("echo", "", 7), ("echo", "before-delete", 1), ("omega", "咖啡", 4)],
    [("咖啡", "版本号超过 JS 的整数精度", 9007199254740993)],
]


def table_rows(table):
    body = re.search(r"<tbody>(.*?)</tbody>", table, re.S).group(1)
    return [[html.unescape(cell) for cell in re.findall(r"<td>(.*?)</td>", row, re.S)]
            for row in re.findall(r"<tr>(.*?)</tr>", body, re.S)]


class SstVisualizeTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="kopidb-sst-visualize-test-")
        self.addCleanup(directory.cleanup)
        self.work = Path(directory.name)

    def run_cli(self, *args, seed="0"):
        return subprocess.run(
            [sys.executable, str(SCRIPT), *map(str, args)], cwd=self.work,
            env={**os.environ, "PYTHONHASHSEED": seed}, capture_output=True,
            text=True, encoding="utf-8", timeout=20)

    def test_without_bloom_keeps_footer_and_all_records(self):
        source = self.work / "plain-without-bloom.sst"
        source.write_bytes(PLAIN_SST)
        expected_blocks = [[[str(i), key, value, str(version)]
                            for i, (key, value, version) in enumerate(block)]
                           for block in BLOCKS]
        expected_meta = [[str(i), str(offset), block[0][0], block[-1][0]]
                         for i, (offset, block) in enumerate(
                             zip([0, 77, 116, 221, 301], BLOCKS))]
        expected_footer = [["Meta Section Offset", "361"],
                           ["Bloom Filter Offset", "467"],
                           ["Min Transaction ID", "1"],
                           ["Max Transaction ID", "9007199254740993"]]
        # A19：不同 hash seed 曾决定同址的 Bloom/Extra 谁错误占用文件尾。
        for seed in ("0", "1", "2", "3"):
            with self.subTest(seed=seed):
                result = self.run_cli(source, seed=seed)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertNotIn("警告", result.stdout)
                page = (self.work / "sst_html" / (source.name + "_visualization.html")).read_text("utf-8")
                self.assertEqual(re.findall(r'id="nav-([^"]+)"', page),
                                 ["blocks", "meta", "extra"])
                self.assertRegex(page, r'(?s)id="nav-extra".*?<span>24 bytes</span>')
                self.assertRegex(page, r'(?s)id="detail-extra".*?<div>467 - 491</div>')
                tables = [table_rows(t) for t in re.findall(
                    r"<table class='data-table'>(.*?)</table>", page, re.S)]
                self.assertEqual(tables, expected_blocks + [expected_meta, expected_footer])
                self.assertEqual(source.read_bytes(), PLAIN_SST)

    def test_missing_argument_prints_usage(self):
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("使用方法", result.stdout)

    def test_missing_file_prints_error(self):
        result = self.run_cli(self.work / "missing.sst")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("不存在", result.stdout)
        self.assertFalse((self.work / "sst_html").exists())


if __name__ == "__main__":
    unittest.main()
