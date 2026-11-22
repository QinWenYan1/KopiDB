# KopiDB

<p align="center">
  <img src="assets/kopidb-logo.png" alt="KopiDB 像素咖啡标志" width="720">
</p>

**简体中文** | [English](README.en.md)

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/wiki/faq/cpp20) [![SDK](https://img.shields.io/badge/SDK-macOS_arm64-lightgrey.svg)](https://github.com/QinWenYan1/KopiDB/releases) [![Build](https://img.shields.io/badge/build-Xmake-green.svg)](https://xmake.io)

KopiDB 是一个用于学习与实验的 **C++20 嵌入式 LSM-tree KV 数据库**，基于 [tiny-lsm](https://github.com/Vanilla-Beauty/tiny-lsm) 的代码框架与配套 Lab 开发。应用程序通过库接口访问数据，无需启动独立服务器。

- **读写**：CRUD、批量操作、有序遍历和范围查询。
- **事务与存储**：MVCC 多版本读取、事务提交与回滚、SST、WAL 日志重放与恢复。
- **读取与合并优化**：Bloom Filter、Block Cache、WiscKey 键值分离，以及合并时复用已有 VLog 引用。

## 🧭 目录

- [🐍 Python 快速开始](#python-sdk)
- [📦 C++ 快速开始](#download-and-use)
- [💻 最小使用示例](#cpp-example)
- [📚 文档导航](#tools-and-docs)
- [⚡ 优化记录](#optimizations)
- [📖 LSM 与学习资料](#learning-docs)
- [🙏 来源与鸣谢](#acknowledgements)

<a id="python-sdk"></a>

## 🐍 Python 快速开始

Python SDK v0.1.0 适用于 **Apple Silicon、macOS 18.0 或更高版本、标准 CPython 3.13（arm64）**。从 [GitHub Releases](https://github.com/QinWenYan1/KopiDB/releases) 下载下方命令中的 wheel，在下载目录运行：

```bash
python3.13 -m venv .venv
.venv/bin/python -m pip install ./kopidb-0.1.0-cp313-cp313-macosx_18_0_arm64.whl
```

用同一个虚拟环境运行以下 Python 示例。完整用法见[文本与二进制、批量和事务](docs/usage.md#python-sdk)。

```python
import kopidb

db = kopidb.LSM("my-data")
db.put("name", "Alice")
print(db.get("name"))  # Alice
db.remove("name")
```

<a id="build-and-test"></a>
<a id="download-and-use"></a>

## 📦 C++ 快速开始

预编译 C++ SDK 面向 **macOS arm64**。准备支持 C++20 的编译器，以及 Xmake 或 CMake；包内 `BUILD-INFO.txt` 记录该产物的构建环境和最低系统版本。

**1. 下载 SDK。** 在 [C++ SDK v0.0.1](https://github.com/QinWenYan1/KopiDB/releases/tag/v0.0.1) 下载 `kopidb-<版本>-macos-arm64.tar.gz`，解压后将目录命名为 `kopidb`，放入自己项目的 `third_party/`：

```text
你的项目/
├── main.cpp
├── xmake.lua
└── third_party/kopidb/
    ├── include/
    ├── lib/
    ├── xmake.lua
    └── KopiDBConfig.cmake
```

**2. 添加依赖。** 将下一节示例保存为 `main.cpp`，在自己项目的 `xmake.lua` 中写入：

```lua
includes("third_party/kopidb")

target("my_app")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("kopidb")
```

**3. 编译并运行自己的程序。**

```bash
xmake
xmake run my_app
```

数据库已经编译在 SDK 中；程序只需包含一个 KopiDB 入口头文件：`<lsm/engine.h>`。运行时保留 SDK 共享库，移动 SDK 后重新配置并构建程序。

使用 CMake 请看[对应接入步骤](docs/usage.md#cmake)；从源码构建和打包请看[打包说明](docs/packaging.md)。

<a id="cpp-example"></a>

## 💻 最小使用示例

```cpp
#include <lsm/engine.h>
#include <iostream>

int main() {
  kopidb::LSM db("my-data");

  // 普通 CRUD：独立调用即可。
  db.put("name", "Alice");
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';
  }
  db.put("name", "Bob");
  db.remove("name");

  // 显式事务：将多次修改一起提交。
  auto tx = db.begin_tran(kopidb::IsolationLevel::REPEATABLE_READ);
  tx->put("user:1", "Alice");
  tx->put("user:2", "Bob");
  tx->commit();  // 示例省略结果处理；实际使用需检查提交结果。
}
```

数据保存在程序工作目录的 `my-data/`。无需配置文件即可使用默认设置；自定义参数时，将 [config.toml](config.toml) 放在工作目录，WiscKey 默认关闭。

<a id="concurrency-and-transactions"></a>
<a id="choosing-read-write-mode"></a>

**事务与线程** 每个线程同时最多开启一笔显式事务。事务由创建它的线程读写，并在该线程上调用 `commit()` 或 `abort()` 结束。这两条约定适用于所有隔离级别，已发布的 SDK 尚未强制检查。多个线程可以共享同一个 DB。

公开写入接口拒绝空 key，`put`/`put_batch` 拒绝空 value，删除请用 `remove`。同一迭代器应串行使用。`SERIALIZABLE` 尚未完整实现。普通查询允许看到其他读未提交事务的修改。更多事务和持久性说明见[使用手册](docs/usage.md#concurrency-and-transactions)。

完整的事务回滚、批量操作和有序遍历示例见[五类 API 用法](docs/usage.md#cpp-example)。

<a id="tools-and-docs"></a>

## 📚 文档导航

| 想了解什么 | 阅读入口 |
| --- | --- |
| 接入、配置、完整 API 示例与事务语义 | [使用手册](docs/usage.md) |
| LSM 原理、设计取舍、应用场景与论文 | [LSM 概览](docs/lsm-overview.md) |
| 做过哪些优化，收益如何测量 | [优化案例与实测](docs/optimizations.md) |
| 哪些代码被测试执行过 | [覆盖率工具](docs/coverage.md) |
| 查看普通 SST 的记录、版本和分块 | [SST 可视化](docs/Sst_visualize_tool.md) |
| 构建 SDK、理解包结构与发布流程 | [构建与打包](docs/packaging.md) |
| 深入阅读 RocksDB 源码 | [学习笔记索引](docs/rocksdb/README.md) |

<a id="optimizations"></a>

## ⚡ 优化记录

- **刷盘完成通知顺序**：利用同一批 ID 的已有次序，减少事务管理器的重复扫描。
- **合并复用 VLog 引用**：合并已有引用时直接搬运引用，省去还原原值和再次追加 VLog 的工作。
- **回滚版本清理**：首次刷盘和后续合并时跳过已回滚的版本。[刷盘、合并与点查实测](docs/optimizations.md#aborted-versions)

[优化文档](docs/optimizations.md) 包含具体场景、架构调整、代码位置、测试条件和前后数据；测量结论只针对对应负载。

<a id="what-is-lsm"></a>
<a id="why-lsm"></a>
<a id="use-cases"></a>
<a id="learning-docs"></a>

## 📖 LSM 与学习资料

LSM-tree 先将写入组织在内存中，再批量生成有序 SST，通过合并管理多个层级。它适合以写入吞吐为重点的设计，但也带来读放大、写放大和后台合并成本。

LSM 原理、设计取舍、应用场景和相关论文见 [LSM 概览](docs/lsm-overview.md)。[RocksDB 源码笔记](docs/rocksdb/README.md) 保留逐篇索引和推荐阅读顺序。

<a id="acknowledgements"></a>

## 🙏 来源与鸣谢

KopiDB 基于 [Vanilla-Beauty/tiny-lsm](https://github.com/Vanilla-Beauty/tiny-lsm) 提供的代码框架与[配套教程](https://vanilla-beauty.github.io/tiny-lsm/book/)开发，并在此基础上完成实验实现、修复、优化与 SDK 整理。感谢原作者提供开源代码和教学资料。

同时感谢 RocksDB、LSM-tree 与 WiscKey 的研究和开源资料为学习提供参考。项目许可见 [LICENSE](LICENSE)。
