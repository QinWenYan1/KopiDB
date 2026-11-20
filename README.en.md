# KopiDB

<p align="center">
  <img src="assets/kopidb-logo.png" alt="KopiDB pixel-art coffee logo" width="720">
</p>

[简体中文](README.md) | **English**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/wiki/faq/cpp20) [![SDK](https://img.shields.io/badge/SDK-macOS_arm64-lightgrey.svg)](https://github.com/QinWenYan1/KopiDB/releases) [![Build](https://img.shields.io/badge/build-Xmake-green.svg)](https://xmake.io)

KopiDB is an **embedded LSM-tree key-value database in C++20**, built for learning and experimentation on the [tiny-lsm](https://github.com/Vanilla-Beauty/tiny-lsm) code framework and labs. Applications access data through its library interface, without running a separate server.

- **Data access**: CRUD, batch operations, ordered iteration, and range queries.
- **Transactions and storage**: MVCC reads, transaction commit and rollback, SST files, and WAL replay and recovery.
- **Read and compaction optimizations**: Bloom filters, a block cache, WiscKey value separation, and reuse of existing VLog references during compaction.

## 🧭 Contents

- [🐍 Python Quick Start](#python-sdk)
- [📦 C++ Quick Start](#download-and-use)
- [💻 Minimal Example](#cpp-example)
- [📚 Documentation](#tools-and-docs)
- [⚡ Optimization Notes](#optimizations)
- [📖 LSM and Learning Resources](#learning-docs)
- [🙏 Origins and Acknowledgements](#acknowledgements)

<a id="python-sdk"></a>

## 🐍 Python Quick Start

The v0.1.0 Python SDK supports **Apple Silicon, macOS 18.0 or later, and standard arm64 CPython 3.13**. Download the wheel named below from [GitHub Releases](https://github.com/QinWenYan1/KopiDB/releases) and run these commands in the download directory.

```bash
python3.13 -m venv .venv
.venv/bin/python -m pip install ./kopidb-0.1.0-cp313-cp313-macosx_18_0_arm64.whl
```

Run this example with the Python interpreter from the same virtual environment. The [usage guide](docs/usage.md#python-sdk) covers text, bytes, batches, and transactions (in Chinese).

```python
import kopidb

db = kopidb.LSM("my-data")
db.put("name", "Alice")
print(db.get("name"))  # Alice
db.remove("name")
```

<a id="build-and-test"></a>
<a id="download-and-use"></a>

## 📦 C++ Quick Start

The prebuilt C++ SDK targets **macOS arm64**. Use a C++20-capable compiler and either Xmake or CMake. The package's `BUILD-INFO.txt` records its build environment and minimum OS version.

**1. Download the SDK.** Get `kopidb-<version>-macos-arm64.tar.gz` from [C++ SDK v0.0.1](https://github.com/QinWenYan1/KopiDB/releases/tag/v0.0.1). Rename the extracted directory to `kopidb` and place it in your project's `third_party/` directory:

```text
your-project/
├── main.cpp
├── xmake.lua
└── third_party/kopidb/
    ├── include/
    ├── lib/
    ├── xmake.lua
    └── KopiDBConfig.cmake
```

**2. Add the dependency.** Save the next section's example as `main.cpp`, and put this in your project's `xmake.lua`:

```lua
includes("third_party/kopidb")

target("my_app")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("kopidb")
```

**3. Build and run your application.**

```bash
xmake
xmake run my_app
```

The database is already compiled in the SDK. Your program needs one KopiDB entry header: `<lsm/engine.h>`. Keep the shared library available at runtime; reconfigure and rebuild your application after moving the SDK.

For CMake, see the [integration instructions](docs/usage.md#cmake); for building and packaging from source, see the [packaging guide](docs/packaging.md). These detailed guides are currently in Chinese.

<a id="cpp-example"></a>

## 💻 Minimal Example

```cpp
#include <lsm/engine.h>
#include <iostream>

int main() {
  kopidb::LSM db("my-data");

  // Basic CRUD: each operation can be called independently.
  db.put("name", "Alice");
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';
  }
  db.put("name", "Bob");
  db.remove("name");

  // Explicit transaction: commit multiple changes together.
  auto tx = db.begin_tran(kopidb::IsolationLevel::REPEATABLE_READ);
  tx->put("user:1", "Alice");
  tx->put("user:2", "Bob");
  tx->commit();  // Result handling omitted here; check it in application code.
}
```

Data is stored in `my-data/` under the application's working directory. No configuration file is needed for the defaults. For custom settings, put [config.toml](config.toml) in the application's working directory; WiscKey is disabled by default.

<a id="concurrency-and-transactions"></a>
<a id="choosing-read-write-mode"></a>

**Usage constraints:** Public writes reject empty keys, and `put`/`put_batch` reject empty values; use `remove` for deletion. Use each transaction context or iterator serially. `SERIALIZABLE` is incomplete. Ordinary reads currently permit observing other read-uncommitted transactions' changes; see the [usage manual](docs/usage.md) for isolation, concurrency, and persistence boundaries.

The [complete API example](docs/usage.md#cpp-example) also demonstrates rollback, batch operations, and ordered iteration.

<a id="tools-and-docs"></a>

## 📚 Documentation

Detailed guides and study notes are currently in Chinese.

| What you want to learn | Guide |
| --- | --- |
| Integration, configuration, complete API examples, and transactions | [Usage manual](docs/usage.md) |
| LSM principles, trade-offs, use cases, and papers | [LSM overview](docs/lsm-overview.md) |
| Implemented optimizations and how their gains were measured | [Optimization case studies](docs/optimizations.md) |
| Which source lines the tests execute | [Coverage tool](docs/coverage.md) |
| Records, versions, and blocks inside an ordinary SST | [SST visualization](docs/Sst_visualize_tool.md) |
| SDK builds, package layout, and releases | [Build and packaging](docs/packaging.md) |
| Reading RocksDB source code | [Study notes index](docs/rocksdb/README.md) |

<a id="optimizations"></a>

## ⚡ Optimization Notes

- **Flush completion notification order**: Use the existing order of IDs within one batch to reduce repeated scans in the transaction manager.
- **Reuse VLog references during compaction**: Copy existing references directly, avoiding value reconstruction and repeated VLog appends.

The [case studies](docs/optimizations.md) explain the scenarios, architectural changes, code locations, test conditions, and before/after measurements. Results apply to the workloads described there.

<a id="what-is-lsm"></a>
<a id="why-lsm"></a>
<a id="use-cases"></a>
<a id="learning-docs"></a>

## 📖 LSM and Learning Resources

An LSM-tree first organizes writes in memory, then produces sorted SST files in batches and manages its levels through compaction. This supports write-throughput-oriented designs while introducing read amplification, write amplification, and background compaction costs.

The [LSM overview](docs/lsm-overview.md) covers the principles, trade-offs, use cases, and related papers. The [RocksDB study notes](docs/rocksdb/README.md) provide a chapter index and recommended reading order.

<a id="acknowledgements"></a>

## 🙏 Origins and Acknowledgements

KopiDB is based on the code framework and [labs](https://vanilla-beauty.github.io/tiny-lsm/book/) provided by [Vanilla-Beauty/tiny-lsm](https://github.com/Vanilla-Beauty/tiny-lsm). The project builds on that foundation with lab implementations, fixes, optimizations, and SDK packaging. Thank you to the original author for sharing the code and teaching materials.

Thanks also to RocksDB and the LSM-tree and WiscKey research and open-source resources that support this learning work. See [LICENSE](LICENSE) for the project license.
