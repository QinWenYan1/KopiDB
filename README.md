# KopiDB

<p align="center">
  <img src="assets/kopidb-logo.png" alt="KopiDB pixel-art coffee logo" width="720">
</p>

**English** | [简体中文](README.zh-CN.md)

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/wiki/faq/cpp20) [![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)](https://github.com/QinWenYan1/KopiDB) [![Build](https://img.shields.io/badge/build-Xmake-green.svg)](https://xmake.io) [![RocksDB DeepWiki](https://img.shields.io/badge/RocksDB-DeepWiki-purple.svg)](https://deepwiki.com/facebook/rocksdb)

KopiDB is an embedded LSM-tree key-value engine written in C++20 for learning how databases are implemented. It supports ordered reads and writes, SST storage, multi-version reads, and transaction APIs. Applications access the database directly through its library interface, without running a separate server.

---

## 🧭 Contents

- [📖 What Is an LSM-tree?](#what-is-lsm)
- [💡 Why Use an LSM-tree?](#why-lsm)
- [🎯 Use Cases](#use-cases)
- [📚 Learning Notes (Chinese)](#learning-docs)
- [🛠️ Build and Test](#build-and-test)
- [🧵 Concurrency and Transactions](#concurrency-and-transactions)
- [💻 C++ Usage Example](#cpp-example)
- [🧭 Choosing a Read/Write Mode](#choosing-read-write-mode)

---

<a id="what-is-lsm"></a>

## 📖 What Is an LSM-tree?

The core idea of an LSM-tree (Log-Structured Merge-Tree, O'Neil et al., 1996) is to **turn random writes into sequential writes**: writes first enter an ordered in-memory structure (MemTable) and are appended to a write-ahead log (WAL) for durability. Once the MemTable is full, its contents are flushed in batches to immutable SST files. Background compaction merges and cleans up data across levels. RocksDB (Meta) is a production-grade implementation of this structure and serves as a reference for this project.

Further reading:

- [The Log-Structured Merge-Tree (LSM-tree), Acta Informatica 1996](https://doi.org/10.1007/s002360050048) — The original LSM-tree paper.
- [The RocksDB Experience, FAST 2021](https://www.usenix.org/conference/fast21/presentation/dong) — A paper on the RocksDB team's production experience.
- [WiscKey, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/lu) — The inspiration for this project's vlog module (key-value separation).

---

<a id="why-lsm"></a>

## 💡 Why Use an LSM-tree?

Compared with B+ trees, which are read-optimized structures that update data in place:

1. **Significantly higher write throughput**: B+ tree writes involve substantial random I/O and page splits. LSM-trees write to memory first and then flush sequentially, making the write path almost entirely sequential I/O.
2. **SSD-friendly writes**: Batched sequential writes reduce flash write/erase amplification and extend SSD lifespan.
3. **Stable write latency**: With no in-place updates, the foreground write path stays short.

The trade-offs are read amplification (queries may need to search multiple levels) and write amplification (compaction rewrites data repeatedly). Production systems use Bloom filters, block caches, and level-based compaction strategies to mitigate these costs.

---

<a id="use-cases"></a>

## 🎯 Use Cases

LSM-based architectures are a common choice for write-intensive workloads:

- Logging and messaging systems, and time-series data.
- Metadata storage and blockchain client storage.
- State backends for stream processing (Flink and Kafka Streams embed RocksDB).
- Storage engines for higher-level databases: TiKV, MyRocks, Cassandra, and HBase are based on or influenced by LSM designs.

---

<a id="learning-docs"></a>

## 📚 Learning Notes (Chinese)

The `doc/` directory contains **RocksDB source code study notes**, developed alongside this implementation. They cover workflows, call chains, and key mechanisms, with an emphasis on interview preparation. These notes are currently available in Chinese.

- [doc/README](doc/README.md) — Notes index and reading guide (one note per sub-lab).
- [01-skiplist-basic](doc/01-skiplist-basic.md) — Lab 1.1: RocksDB implementations corresponding to skiplist put/get/remove.
- [02-skiplist-iter](doc/02-skiplist-iter.md) — Lab 1.2: Skiplist iterators.
- [03-skiplist-scan](doc/03-skiplist-scan.md) — Lab 1.3: Prefix and bounded scans.
- [04-data-path](doc/04-data-path.md) — Data paths: Put/Get workflows and the SuperVersion model (prerequisite for Lab 2).
- [05-memtable-rw](doc/05-memtable-rw.md) — Lab 2.1: MemTable reads and writes, freezing, and tombstones.
- [06-memtable-iter](doc/06-memtable-iter.md) — Lab 2.2: MemTable iterators and merging.
- [07-memtable-scan](doc/07-memtable-scan.md) — Lab 2.3: MemTable prefix scans.

---

<a id="build-and-test"></a>

## 🛠️ Build and Test

A C++20 compiler and [Xmake](https://xmake.io) are required. See [config.toml](config.toml) for configuration.

```bash
xmake build lsm             # Build the engine
xmake build test_skiplist
xmake run test_skiplist     # Run one module's tests
```

For environment setup instructions, see [Lab 0](https://vanilla-beauty.github.io/tiny-lsm/book/lab0-env.html) in the tutorial (Chinese).

---

<a id="concurrency-and-transactions"></a>

## 🧵 Concurrency and Transactions

KopiDB's concurrency design aims to let **multiple threads in the same process share one database instance**. Full concurrency support is still being developed.

Each call to `begin_tran()` creates an independent `TranContext` representing one transaction. Each thread should use its own transaction context. A thread may also create and operate on multiple contexts; threads and contexts do not need a one-to-one relationship.

**A single `TranContext` must be used serially: only one thread may operate on it at a time.** Contexts are not automatically bound to a thread. If multiple threads share a context or hand it off, the caller must coordinate access using locks or other synchronization mechanisms. A single iterator must also be used serially.

Ordinary `put/get/remove` calls do not require an explicit transaction. Use `begin_tran()` and `commit/abort` when multiple operations need to form one transaction.

`clear()` removes all database data and is an exclusive maintenance operation. Before calling it, finish transactions, destroy iterators, and pause other threads' operations on the instance. Reads and writes can resume after it returns successfully. A failed clear may already have deleted some data; handle the error before retrying or destroying the instance.

---

<a id="cpp-example"></a>

## 💻 C++ Usage Example

The following example demonstrates five usage patterns on a single database instance, separated by comments. Commit-result handling is omitted to keep the API flow clear.

```cpp
#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include <iostream>

using namespace tiny_lsm;

int main() {
  LSM db("api-demo");

  // ---------- 1. Basic CRUD: no explicit transaction required ----------
  db.put("name", "Alice");  // Insert
  if (auto value = db.get("name")) {
    std::cout << *value << '\n';  // Read: Alice
  }
  db.put("name", "Bob");  // Update the same key
  db.remove("name");      // Delete
  if (!db.get("name")) {
    std::cout << "name does not exist\n";
  }

  // ---------- 2. Explicit transaction: begin, read/write, commit ----------
  auto tx = db.begin_tran(IsolationLevel::REPEATABLE_READ);
  tx->put("user:1", "Alice");
  tx->put("user:2", "Bob");
  if (auto value = tx->get("user:1")) {
    std::cout << *value << '\n';  // Read this transaction's own uncommitted changes
  }
  tx->commit();  // Default test_fail=false: normal commit; do not use tx for further reads/writes

  // ---------- 3. Transaction rollback: discard this transaction's changes ----------
  auto cancelled = db.begin_tran(IsolationLevel::REPEATABLE_READ);
  cancelled->put("user:1", "Temporary");
  cancelled->abort();  // Discard changes; do not use cancelled for further reads/writes
  if (auto value = db.get("user:1")) {
    std::cout << *value << '\n';  // Still Alice if the previous transaction committed successfully
  }

  // ---------- 4. Batch operations: pass multiple keys at once ----------
  db.put_batch({{"batch:a", "A"}, {"batch:b", "B"}, {"batch:c", "C"}});
  for (const auto &[key, value] :
       db.get_batch({"batch:a", "batch:b", "missing"})) {
    // Each result retains its key; missing keys return std::nullopt.
    std::cout << key << " = " << value.value_or("<not found>") << '\n';
  }
  db.remove_batch({"batch:a", "batch:b"});

  // ---------- 5. Ordered iteration: read keys in ascending order ----------
  // The 0 in begin(0) means there is no upper bound on the read version.
  for (auto it = db.begin(0); it != db.end(); ++it) {
    std::cout << it->first << " = " << it->second << '\n';
  }

  return 0;
}
```

**The test parameter of `commit()`**: For normal use, call `tx->commit()`; `test_fail` defaults to `false`. `commit(true)` is only for recovery testing. For transactions that buffer their changes, it confirms that the commit log has been written, then deliberately skips applying the changes while still registering the transaction as committed and returning `true`. The test then closes and reopens the database to verify log replay. This `true` result does not mean the current instance's data has been updated. The flag does not terminate the process, and read-uncommitted transactions ignore it and commit normally.

Public write APIs reject empty keys, and `put`/`put_batch` also reject empty values, throwing `std::invalid_argument` for these invalid inputs. Use `remove`/`remove_batch` for deletion. If any entry in a batch is invalid, the entire batch is rejected before writing. This is a learning project with WAL-based log replay and recovery implemented; complete recovery guarantees for power loss and failures partway through persistence are still being developed.

---

<a id="choosing-read-write-mode"></a>

## 🧭 Choosing a Read/Write Mode

For independent reads and writes, call `db.put/get/remove` directly. To group multiple operations into one transaction and commit or roll them back together, use `begin_tran()` and select an isolation level. The table below describes the behavior of KopiDB's current implementation.

| Mode | Read behavior | Writes and transaction completion | Transaction version assignment | Read version upper bound | Can read other transactions' uncommitted changes? | Rolled-back records |
| --- | --- | --- | --- | --- | --- | --- |
| **Direct operations** | Each query is independent; results may change | Each operation executes independently; no manual commit or shared rollback across calls | Each `put()` gets a new version; queries do not allocate a transaction ID | `0`: no version upper bound | Currently yes | Excluded |
| **Read uncommitted** `READ_UNOP_COMMITTED` | Each query reads again; results may change, and observed changes may later be rolled back | Writes enter the database immediately; `commit()` ends the transaction, or `abort()` discards its changes | All writes in a transaction share one version | `0`: no version upper bound | Yes | Excluded |
| **Read committed** `READ_OP_COMMITTED` | Own changes take priority; otherwise reads only committed data and can see later commits | Changes are buffered and applied on successful commit; rollback is supported | All writes in a transaction share one version | `0`: no version upper bound | No | Excluded |
| **Repeatable read** `REPEATABLE_READ` | Preserves the first valid result for each key, including "not found"; own changes take priority | Changes are buffered; write conflicts are checked at commit; rollback is supported | All writes in a transaction share one version | The first query uses the transaction's version; subsequent reads reuse the cache | No | Excluded |
| **Serializable** `SERIALIZABLE` **(experimental, incomplete)** | Currently uses the same per-key read behavior as repeatable read | Changes are buffered; commit additionally checks whether previously read data has changed; rollback is supported | All writes in a transaction share one version | The first query uses the transaction's version; subsequent reads reuse the cache | No | Excluded |

Additional notes:

- **Version numbers and read bounds are different**: An explicit transaction receives its version when it is created, so the version does not indicate commit order. Passing `0` for a query only removes the version upper bound; actual record versions must still be compared, and rollback status must still be checked. The bounds in the table apply to queries against the storage layer; a transaction's own buffered changes and existing read cache take priority.
- **Rollback checks always apply**: Internally, `is_aborted(id)` checks whether the transaction that owns a record has been rolled back. `require_committed = true` additionally excludes records from active transactions; `false` does not disable rollback checks. "Uncommitted" here refers to other transactions; transactions that buffer changes can still read their own modifications.
- **Batch APIs**: An ordinary `put_batch()` assigns one new version to the entire batch. An ordinary `get_batch()` does not allocate a transaction ID; it preserves input order, duplicate keys, and missing entries, and excludes rolled-back versions, but does not guarantee a consistent snapshot across the whole batch.
- **Scope of guarantees**: The current repeatable-read implementation does not guarantee a consistent snapshot across all keys. Full serializability requires the read/write effects of successfully committed transactions to be equivalent to some serial execution order; it does not require threads to run one at a time. KopiDB does not yet provide this complete guarantee; a full implementation is left for future work.
