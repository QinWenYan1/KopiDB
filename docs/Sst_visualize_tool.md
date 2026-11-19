# 🔎 普通 SST 可视化

[返回项目 README](../README.zh-CN.md#tools-and-docs)

[`scripts/sst_visualize.py`](../scripts/sst_visualize.py) 将一份普通 SST 文件解析为可离线浏览的 HTML 网页，用来查看文件内部的分块和记录。

## 🕒 什么时候使用

数据刷盘生成 SST 后，需要查看某个 key 的版本、Block 的首尾 key 或文件布局时，手动运行本工具。刚执行 `put()` 时数据可能仍在 MemTable，尚未产生对应 SST。普通构建和数据库运行不会自动调用这个脚本。

**仅用于普通 SST，不支持 WiscKey SST 或 VLog 原值解析。** 脚本按普通 SST 的 24 字节文件尾读取字段，不能用它解释 WiscKey 的 26 字节文件尾。

## 📦 准备条件

- Python 3，仅使用标准库，无需安装第三方 Python 包。
- 一份已生成且写入完成的普通 SST 文件。

## ▶️ 使用方法

在仓库根目录执行，将路径替换为实际 SST 文件：

```bash
python3 scripts/sst_visualize.py /path/to/000123.sst

# macOS：用浏览器打开生成的报告
open sst_html/000123.sst_visualization.html
```

文件不必以 `.sst` 结尾。例如输入为 `example_data/sst_1.0`，输出就是 `sst_html/sst_1.0_visualization.html`。也可以直接用浏览器打开生成的 HTML。

文档位于 `docs/`，但脚本仍在 `scripts/`。命令中的相对路径以终端当前目录为准；从其他目录运行时，可以使用脚本和 SST 的绝对路径。

## 📊 网页能查看什么

| 部分 | 展示内容 |
| --- | --- |
| Block | 每条记录的 key、value 和事务 ID，可切换不同 Block |
| Meta | 各 Block 的偏移、首 key 和尾 key |
| Bloom Filter | 预计元素数量、误判率、位数和哈希数量 |
| 文件尾与布局 | 元数据偏移、事务 ID 范围，以及各部分的大小和占比 |

key/value 按文本展示；二进制内容可能显示为替代字符，不应据此判断原始字节是否一致。这个网页是查看工具，不代替数据库的完整性校验。

## 📂 输出位置

报告固定写入**运行命令时所在目录**的 `sst_html/`：

```text
sst_html/<输入文件名>_visualization.html
```

脚本只读取输入 SST，另外生成 HTML；相同文件名的报告会覆盖，即使两个输入来自不同目录。没有输入参数时会打印用法；脚本没有单独的 `--help` 选项。

## 🧪 工具回归检查

在仓库根目录运行 Python 回归测试，无需重新构建数据库：

```bash
python3 -m unittest discover -s test -p 'test_sst_visualize.py' -v
```

测试使用由实际 SSTBuilder 生成的小型普通 SST 样本，检查关闭 Bloom 时的区域划分、多版本与墓碑展示，以及缺少参数或文件时的提示。这组 Python 测试独立运行，不属于 `test_coverage.sh` 收集的 C++ 测试目标。
