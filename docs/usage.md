# KopiDB 使用指南

[← 返回项目主页](../README.md#tools-and-docs)

本页说明Python 和 C++ SDK 的接入方式、公开 API 和当前实现的使用边界。KopiDB 是用于学习数据库实现的嵌入式 KV 引擎，应用通过库接口直接使用，无需启动独立服务。

## 🧭 目录

- [🐍 Python 使用方法](#python-sdk)
- [📦 C++ 下载与接入](#download-and-use)
- [🛠️ 使用 Xmake](#xmake)
- [🔧 使用 CMake](#cmake)
- [⚙️ 运行配置](#configuration)
- [💻 C++ 使用示例](#cpp-example)
- [🧵 并发与事务](#concurrency-and-transactions)
- [🧭 如何选择读写方式](#choosing-read-write-mode)
- [🧪 进阶：源码构建与恢复测试](#advanced-testing)

<a id="python-sdk"></a>

## 🐍 Python 使用方法

按[首页的安装步骤](../README.md#python-sdk)下载 wheel。如果同时下载了同名的 `.sha256`，可在安装前运行 `shasum -a 256 -c kopidb-0.1.0-cp313-cp313-macosx_18_0_arm64.whl.sha256`，应显示 `OK`。

`put`、`get`、`remove` 和对应的批量方法都接受文本或字节。文本按 UTF-8 存储，因此 `"name"` 和 `b"name"` 是同一个 key。读取默认返回文本；读图片等任意字节时加 `as_bytes=True`。这也适用于事务和遍历。

```python
import kopidb

db = kopidb.LSM("python-demo")
db.put_batch([("name", "小林"), ("avatar", b"\xff\x00")])
assert db.get("name") == "小林"
assert db.get("avatar", as_bytes=True) == b"\xff\x00"

# 批量结果保留输入顺序、重复 key 和缺失项。
assert db.get_batch(["name", "missing", "name"]) == [
    ("name", "小林"), ("missing", None), ("name", "小林"),
]

tx = db.begin_tran(kopidb.IsolationLevel.READ_COMMITTED)
tx.put("name", "小陈")
if tx.commit():
    print(db.get("name"))

for key, value in db.begin(as_bytes=True):
    print(key, value)
```

- **返回值**：查不到返回 `None`。批量读取和遍历用同一模式返回 key 和 value。如果数据不是有效 UTF-8，默认读取会抛 `UnicodeDecodeError`。
- **写入**：不支持的参数类型抛 `TypeError`；写入空 key 或空 value 抛 `ValueError`。批量参数有错时整批都不会写入。
- **事务**：`tx.get_isolation_level()` 返回创建时选择的级别。`commit()` 遇到写冲突返回 `False`，主动取消用 `abort()`。同一事务对象应串行使用，`SERIALIZABLE` 仍是实验性级别。[隔离级别对照](#choosing-read-write-mode)
- **维护**：`flush()` 刷一张内存表，`flush_all()` 刷完所有待刷表。`clear()` 清空整库；调用前结束事务、释放迭代器，并暂停其他线程对该库的访问。

<a id="download-and-use"></a>

## 📦 C++ 下载与接入

预编译 SDK 面向 **macOS arm64**。使用时需要支持 C++20 的编译器，以及 **Xmake 或 CMake，任选一种**。具体构建环境与库的最低系统版本见包内 `BUILD-INFO.txt`。

在 [GitHub Releases](https://github.com/QinWenYan1/KopiDB/releases) 中选择 `kopidb-<版本>-macos-arm64.tar.gz` 附件。若尚无对应附件，可按下方[从源码构建](#advanced-testing)的步骤生成同样的 SDK。

解压后，将文件夹命名为 `kopidb`，放入你自己项目的 `third_party/`：

```text
你的项目/
├── main.cpp
├── xmake.lua 或 CMakeLists.txt
└── third_party/kopidb/
    ├── include/kopidb/
    ├── lib/libkopidb.so
    ├── xmake.lua
    └── KopiDBConfig.cmake
```

将[下方的 C++ 使用示例](#cpp-example)保存为自己的 `main.cpp`。下面以 `my_app` 为程序目标；已有项目只需添加对应的包引用和目标依赖，**不需要修改 SDK 内的配置文件**。

<a id="xmake"></a>

## 🛠️ 使用 Xmake

在你自己项目的 `xmake.lua` 中：

```lua
includes("third_party/kopidb")

target("my_app")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("kopidb")
```

在自己的项目目录执行：

```bash
xmake
xmake run my_app
```

此接入配置会将依赖 KopiDB 的目标设置为 C++20；不影响没有该依赖的目标。

<a id="cmake"></a>

## 🔧 使用 CMake

在你自己项目的 `CMakeLists.txt` 中：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

find_package(KopiDB CONFIG REQUIRED
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kopidb"
    NO_DEFAULT_PATH)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE KopiDB::KopiDB)
```

在自己的项目目录执行：

```bash
cmake -S . -B build
cmake --build build
./build/my_app
```

两种方式都只编译你的程序，链接 SDK 中已经编译好的数据库。源码只需 `#include <lsm/engine.h>`。运行时仍需要 SDK 中的共享库；移动 SDK 后请重新配置并构建程序。下方示例的数据保存在运行目录的 `api-demo/` 中。

<a id="configuration"></a>

## ⚙️ 运行配置

无需准备配置文件即可使用默认设置，首次运行可能打印默认配置提示；WiscKey 默认关闭。如需自定义参数，将 [config.toml](../config.toml) 放到程序运行时的工作目录，再修改对应配置。

<a id="cpp-example"></a>

## 💻 C++ 使用示例

下面在同一个数据库实例中演示五类用法，用注释区分；为突出调用流程，省略提交结果处理。

```cpp
#include <lsm/engine.h>
#include <iostream>

using namespace kopidb;

int main() {
  LSM db("api-demo");

  // ---------- 1. 普通 CRUD：无需显式开启事务 ----------
  db.put("name", "Alice");  // 新增
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';  // 查询：Alice
  }
  db.put("name", "Bob");  // 更新同一个 key
  db.remove("name");      // 删除
  if (!db.get("name")) {
    std::cout << "name 不存在\n";
  }

  // ---------- 2. 显式事务：开启、读写、提交 ----------
  auto tx = db.begin_tran(IsolationLevel::REPEATABLE_READ);
  tx->put("user:1", "Alice");
  tx->put("user:2", "Bob");
  if (auto value = tx->get("user:1")) {
    std::cout << *value << '\n';  // 能读到本事务尚未提交的修改
  }
  tx->commit();  // 结束后不再用 tx 读写

  // ---------- 3. 事务回滚：放弃本事务的修改 ----------
  auto cancelled = db.begin_tran(IsolationLevel::REPEATABLE_READ);
  cancelled->put("user:1", "Temporary");
  cancelled->abort();  // 放弃修改；结束后不再用 cancelled 读写
  if (auto value = db.get("user:1")) {
    std::cout << *value << '\n';  // 前一笔事务提交成功时，仍为 Alice
  }

  // ---------- 4. 批量操作：一次传入多个 key ----------
  db.put_batch({{"batch:a", "A"}, {"batch:b", "B"}, {"batch:c", "C"}});
  for (const auto &[key, value] :
       db.get_batch({"batch:a", "batch:b", "missing"})) {
    // 每个结果保留对应 key；不存在的 key 返回 std::nullopt。
    std::cout << key << " = " << value.value_or("<不存在>") << '\n';
  }
  db.remove_batch({"batch:a", "batch:b"});

  // ---------- 5. 有序遍历：按 key 升序读取 ----------
  // begin(0) 的 0 表示不限制读取版本号。
  for (auto it = db.begin(0); it != db.end(); ++it) {
    std::cout << it->first << " = " << it->second << '\n';
  }

  return 0;
}
```

公开写入接口拒绝空 key，`put`／`put_batch` 也拒绝空 value，空参数会抛出 `std::invalid_argument`；删除请使用 `remove`／`remove_batch`。批量参数有一项不合法时，整批在写入前被拒绝。项目目前用于学习，已实现基于 WAL 的日志重放与恢复；断电及持久化中途失败时的完整恢复保障尚未完善。

<a id="concurrency-and-transactions"></a>

## 🧵 并发与事务

KopiDB 的并发设计目标是：**同一进程内，多个线程共享同一个数据库实例**，完整并发支持仍在完善中。

在当前 C++ 源码中，通过 `db.begin_tran()` 开启显式事务，每个线程同时最多一笔。由创建线程调用该事务的读写接口以及 `commit()`、`abort()`，结束后该线程可以开始下一笔。多个线程可以共享同一个 DB，各自创建和使用自己的事务。

`begin_tran()` 返回 `std::shared_ptr<TranContext>`。同一线程可以用 `auto alias = tx;` 复制句柄：通过 `alias->put()` 暂存的修改可用 `tx->get()` 读到，任一句柄结束事务后，另一个也不能继续读写。直接复制或移动 `TranContext` 对象会在编译时被拒绝。

现有 SDK 尚未强制上述约束。同一个迭代器也应由调用方串行使用。

每次调用 `begin_tran()` 后，调用方负责显式结束事务：确认修改时调用 `commit()`，放弃修改时调用 `abort()`。只读事务也需要结束。仅让句柄离开作用域不会自动提交或回滚。

普通 `put/get/remove` 无需显式开启事务；需要将多次操作组成一笔事务时，再使用 `begin_tran()` 和 `commit/abort`。

`clear()` 会清空数据库数据，是独占维护操作：调用前先结束事务、销毁迭代器，并暂停其他线程对该实例的操作；成功返回后可以继续读写。清空失败可能已经删除部分数据，应处理错误后重试或销毁实例。


<a id="choosing-read-write-mode"></a>

## 🧭 如何选择读写方式

独立读写可以直接调用 `db.put/get/remove`。需要把多次操作组成一笔事务，统一提交或回滚时，使用 `begin_tran()` 并选择隔离级别。下表描述 KopiDB 当前实现的行为。

| 方式 | 读取表现 | 写入与结束方式 | 事务版本号如何分配 | 查询版本上限 | 允许读取其他事务未提交的修改吗 | 已回滚记录 |
| --- | --- | --- | --- | --- | --- | --- |
| **直接操作** | 每次独立查询，结果可能变化 | 每次操作独立执行，无需手动提交，没有跨调用的统一回滚 | 每次 `put()` 分配新的版本号；查询不分配事务 ID | `0`，不限制版本号大小 | 当前允许 | 排除 |
| **读未提交** `READ_UNOP_COMMITTED` | 每次重新查询，结果可能变化；读到的修改之后可能被回滚 | 写入立即进入数据库；通过 `commit()` 结束事务，或通过 `abort()` 取消修改 | 同一事务的写入共用一个版本号 | `0`，不限制版本号大小 | 允许 | 排除 |
| **读已提交** `READ_OP_COMMITTED` | 自身修改优先；其余只读已提交数据，可以看到后来提交的更新 | 修改先暂存，提交成功后应用；支持回滚 | 同一事务的写入共用一个版本号 | `0`，不限制版本号大小 | 不允许 | 排除 |
| **可重复读** `REPEATABLE_READ` | 同一个 key 保持首次合法结果，包括“不存在”；自身修改优先 | 修改先暂存，提交时检查写入冲突；支持回滚 | 同一事务的写入共用一个版本号 | 首次查询使用本事务的版本号，之后复用缓存 | 不允许 | 排除 |
| **可串行化** `SERIALIZABLE` **（实验性，未完整实现）** | 当前沿用可重复读的单键读取行为 | 修改先暂存；提交时额外验证此前读过的数据是否变化；支持回滚 | 同一事务的写入共用一个版本号 | 首次查询使用本事务的版本号，之后复用缓存 | 不允许 | 排除 |

补充说明：

- **版本号与读取上限不同**：显式事务的版本号在创建时分配，不代表提交顺序。查询传 `0` 只是取消版本上限，仍需比较记录的真实版本并检查回滚状态。表中的版本上限用于查询存储层；自身暂存修改和已有读缓存优先处理。
- **回滚检查始终生效**：内部的 `is_aborted(id)` 判断记录所属事务是否已经回滚。`require_committed = true` 会进一步排除活跃事务的记录；`false` 不表示关闭回滚检查。这里的“未提交”指其他事务，暂存型事务仍可以读取自己的修改。
- **批量接口**：普通 `put_batch()` 整批共用一个新版本号；普通 `get_batch()` 不分配事务 ID，保留输入顺序、重复 key 和缺失项，排除已回滚版本，但不承诺整批一致快照。
- **保证范围**：当前可重复读不承诺跨所有 key 的一致快照。完整可串行化要求成功提交事务的读写效果等价于某种逐笔执行的顺序，不要求线程实际排队；KopiDB 当前尚不提供这一完整保证，完整实现留作后续扩展。


<a id="advanced-testing"></a>

## 🧪 进阶：源码构建与恢复测试

### 🧰 从源码构建 SDK（可选）

在 KopiDB 源码仓库根目录执行，需要 C++20 编译器和 [Xmake](https://xmake.io)：

```bash
./scripts/package_cpp.sh
```

脚本在临时源码副本中构建，压缩包输出到 `dist/`，不改变原工作区的构建模式。也可只构建、安装共享库及接入配置：

```bash
KOPIDB_PREFIX="$HOME/.local/kopidb"

xmake f -m release
xmake build kopidb_shared
xmake install -o "$KOPIDB_PREFIX" kopidb_shared
```

环境安装步骤见教程的 [Lab 0](https://vanilla-beauty.github.io/tiny-lsm/book/lab0-env.html)。构建与打包的职责、设计取舍和发布附件准备见[打包说明](packaging.md)。

### 🧪 运行模块测试

在 KopiDB 源码仓库根目录执行：

```bash
xmake build test_skiplist
xmake run test_skiplist
```

手动选择测试与查看源码覆盖率的方法见[测试覆盖率说明](coverage.md)。

### 🔁 恢复测试开关

**`commit()` 的测试参数**：正常业务使用 `tx->commit()`，`test_fail` 默认为 `false`。`commit(true)` 仅供恢复测试：暂存型事务确认提交日志写出后，故意跳过修改的应用，仍登记提交并返回 `true`，由测试关闭重启来验证日志重放。这个 `true` 不表示当前实例的数据已经更新；开关不会主动终止进程，读未提交事务会忽略它并正常提交。
