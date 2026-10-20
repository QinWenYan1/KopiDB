#include "config/config.h"
#include "logger/logger.h"
#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include "sst/concact_iterator.h"
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <latch>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace ::tiny_lsm;

class LSMTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a temporary test directory
    test_dir = "test_lsm_data";
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
    std::filesystem::create_directory(test_dir);
  }

  void TearDown() override {
    if (no_clear) {
      return;
    }
    // Clean up test directory
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
  }
  void setNoClear() { no_clear = true; }

  std::string test_dir;
  bool no_clear = false;
};

namespace {
using MvccRecord = std::tuple<std::string, std::string, uint64_t>;

// 用有限的期望序列检查内容、真实版本及结束状态，避免错误实现导致测试无限遍历。
void expect_mvcc_records(BaseIterator &it,
                         const std::vector<MvccRecord> &expected) {
  for (const auto &[key, value, id] : expected) {
    ASSERT_TRUE(it.is_valid()) << key;
    EXPECT_EQ((*it).first, key);
    EXPECT_EQ((*it).second, value);
    EXPECT_EQ(it.get_cur_tranc_id(), id);
    ++it;
  }
  EXPECT_TRUE(it.is_end());
}

std::shared_ptr<HeapIterator> make_mvcc_heap(
    const std::vector<MvccRecord> &records, uint64_t read_id, bool keep_all) {
  std::vector<SearchItem> items;
  for (const auto &[key, value, id] : records)
    items.emplace_back(key, value, 0, 0, id);
  return std::make_shared<HeapIterator>(std::move(items), read_id, false,
                                        keep_all);
}
} // namespace

// 目的：先判断版本可见性，再处理墓碑；普通模式去重，全版本模式保留历史。
// 场景：读上限 8 看不到 a@12 的删除，却看得到 b@8 的删除。
TEST(IteratorMvccTest, HeapFiltersVersionsBeforeTombstones) {
  const std::vector<SearchItem> items = {
      {"a", "", 0, 0, 12}, {"a", "a5", 0, 0, 5},
      {"b", "", 0, 0, 8}, {"b", "b3", 0, 0, 3},
      {"c", "c20", 0, 0, 20}, {"d", "d0", 0, 0, 0}};

  HeapIterator user_view(items, 8, true, false);
  expect_mvcc_records(user_view, {{"a", "a5", 5}, {"d", "d0", 0}});
  HeapIterator merge_view(items, 8, false, false);
  expect_mvcc_records(merge_view,
                      {{"a", "a5", 5}, {"b", "", 8}, {"d", "d0", 0}});
  HeapIterator all_versions(items, 8, false, true);
  expect_mvcc_records(all_versions, {{"a", "a5", 5}, {"b", "", 8},
                                     {"b", "b3", 3}, {"d", "d0", 0}});
  HeapIterator latest(items, 0, true, false);
  expect_mvcc_records(latest, {{"c", "c20", 20}, {"d", "d0", 0}});
}

// 目的：验证当前 compaction 的标准调用方式：两路均保留全部版本，读上限为 0。
// 场景：同 key 的版本分布在两路中，输出必须按真实版本降序，且保留墓碑。
TEST(IteratorMvccTest, TwoMergePreservesAllVersionsInDescendingOrder) {
  auto a = make_mvcc_heap({{"k", "k9", 9}, {"k", "k3", 3}}, 0, true);
  auto b = make_mvcc_heap({{"k", "", 7}, {"z", "z2", 2}}, 0, true);
  TwoMergeIterator it(a, b, 0, true);
  expect_mvcc_records(it, {{"k", "k9", 9}, {"k", "", 7},
                           {"k", "k3", 3}, {"z", "z2", 2}});
}

// 目的：compaction 使用的全版本归并必须保留两路中的事务完成标记。
// 场景：空 key、空 value 的标记分别属于事务 7 和 5，应按版本降序输出，
//       然后继续输出普通数据；对用户隐藏标记由查询层负责。
TEST(IteratorMvccTest, TwoMergeCompactionPreservesTransactionMarkers) {
  auto a = make_mvcc_heap({{"", "", 7}, {"a", "a7", 7}}, 0, true);
  auto b = make_mvcc_heap({{"", "", 5}, {"b", "b5", 5}}, 0, true);
  TwoMergeIterator it(a, b, 0, true);
  expect_mvcc_records(it, {{"", "", 7}, {"", "", 5},
                           {"a", "a7", 7}, {"b", "b5", 5}});
}

// 目的：外层给出读上限时，不能把子迭代器的读上限 0 当成记录版本。
// 场景：两路各只有一条记录，不涉及子迭代器提前去重造成的历史版本丢失。
//       k@12 对读上限 8 不可见，最终只能返回 z@5。
TEST(IteratorMvccTest, TwoMergeFiltersByActualRecordVersion) {
  auto a = make_mvcc_heap({{"k", "future", 12}}, 0, false);
  auto b = make_mvcc_heap({{"z", "visible", 5}}, 0, false);
  TwoMergeIterator it(a, b, 8, false);
  expect_mvcc_records(it, {{"z", "visible", 5}});
}

// 目的：同 key 在两路中出现时，查询应选最新可见版本。
// 场景：两路已经按同一个快照 8 过滤，但 A 为 k@5，B 为 k@7。
//       不能仅凭来源 A 优先而丢掉 B 的更新版本。
TEST(IteratorMvccTest, TwoMergeQueryChoosesNewestVisibleVersion) {
  auto a = make_mvcc_heap({{"k", "old", 5}}, 8, false);
  auto b = make_mvcc_heap({{"k", "new", 7}}, 8, false);
  TwoMergeIterator it(a, b, 8, false);
  expect_mvcc_records(it, {{"k", "new", 7}});
}

// 目的：普通模式合并全版本输入时，每个 key 只输出最大可见版本。
// 场景：两路都有历史版本，B 的 a@7 是墓碑；去重后还会遇到不可见的
//       k@9 和整个不可见的 m，必须继续过滤，不能重复输出旧版本。
TEST(IteratorMvccTest, TwoMergeQuerySkipsHistoryAndFiltersNextKeys) {
  auto a = make_mvcc_heap({{"a", "a12", 12}, {"a", "a5", 5},
                           {"a", "a3", 3}, {"k", "k9", 9},
                           {"k", "k5", 5}, {"k", "k1", 1},
                           {"m", "m11", 11}, {"t", "t4", 4}},
                          0, true);
  auto b = make_mvcc_heap({{"a", "", 7}, {"a", "a2", 2},
                           {"b", "b6", 6}, {"k", "k7", 7},
                           {"k", "k2", 2}, {"n", "n6", 6}},
                          0, true);
  TwoMergeIterator it(a, b, 8, false);
  expect_mvcc_records(it, {{"a", "", 7}, {"b", "b6", 6},
                           {"k", "k7", 7}, {"n", "n6", 6},
                           {"t", "t4", 4}});
}

// 目的：普通模式下，同 key、同版本仍优先 A，而不是固定优先墓碑。
// 场景：分别检查 A 为较新的删除、A 为删除后重写的新值，B 都不应再输出。
TEST(IteratorMvccTest, TwoMergeQueryPrefersAOnEqualVersions) {
  for (bool a_is_tombstone : {false, true}) {
    SCOPED_TRACE(a_is_tombstone);
    const std::string a_value = a_is_tombstone ? "" : "new";
    const std::string b_value = a_is_tombstone ? "old" : "";
    auto a = make_mvcc_heap({{"k", a_value, 5}}, 8, false);
    auto b = make_mvcc_heap({{"k", b_value, 5}}, 8, false);
    TwoMergeIterator it(a, b, 8, false);
    expect_mvcc_records(it, {{"k", a_value, 5}});
  }
}

// 目的：一路为空时，普通模式仍能对另一条全版本流过滤并去重。
// 场景：分别让 A/B 为空；有记录的一路含不可见 k@12 和可见 k@5、k@3。
TEST(IteratorMvccTest, TwoMergeQueryHandlesEitherEmptyInput) {
  for (bool a_is_empty : {false, true}) {
    SCOPED_TRACE(a_is_empty);
    auto records = make_mvcc_heap({{"k", "k12", 12}, {"k", "k5", 5},
                                   {"k", "k3", 3}, {"z", "z2", 2}},
                                  0, true);
    auto empty = make_mvcc_heap({}, 0, true);
    TwoMergeIterator it(a_is_empty ? empty : records,
                        a_is_empty ? records : empty, 8, false);
    expect_mvcc_records(it, {{"k", "k5", 5}, {"z", "z2", 2}});
  }
}

// 目的：排序使用真实版本号，而不依赖子迭代器 keep_all_versions 的设置。
// 场景：每路只有一条记录，真实版本分别为 9、5，读上限同为 10。
TEST(IteratorMvccTest, TwoMergeOrdersByActualVersionNotReadBound) {
  // 两个方向都验证，避免恰好固定选 A 或 B 也能通过测试。
  for (bool larger_version_in_a : {false, true}) {
    SCOPED_TRACE(larger_version_in_a);
    const std::vector<MvccRecord> newer = {{"k", "new", 9}};
    const std::vector<MvccRecord> older = {{"k", "old", 5}};
    auto a = make_mvcc_heap(larger_version_in_a ? newer : older, 10, false);
    auto b = make_mvcc_heap(larger_version_in_a ? older : newer, 10, false);
    TwoMergeIterator it(a, b, 0, true);
    expect_mvcc_records(it, {{"k", "new", 9}, {"k", "old", 5}});
  }
}

// 目的：Concact 的构造和 ++ 都能跨过整张不可见 SST，并保持版本模式。
// 场景：a/c/e 所在表不可见，b 有历史版本，d 的最新可见版本是墓碑。
TEST_F(LSMTest, MvccConcactSkipsInvisibleSsts) {
  auto cache = std::make_shared<BlockCache>(8, 2);
  const std::vector<std::vector<MvccRecord>> groups = {
      {{"a", "a20", 20}}, {{"b", "b12", 12}, {"b", "b7", 7}, {"b", "b3", 3}},
      {{"c", "c20", 20}}, {{"d", "", 8}, {"d", "d2", 2}}, {{"e", "e20", 20}}};
  std::vector<std::shared_ptr<SST>> ssts;
  for (size_t i = 0; i < groups.size(); ++i) {
    SSTBuilder builder(32, false);
    for (const auto &[key, value, id] : groups[i])
      builder.add(key, value, id);
    ssts.push_back(builder.build(i, test_dir + "/concat" + std::to_string(i) + ".sst", cache));
  }
  ConcactIterator visible(ssts, 8, false);
  expect_mvcc_records(visible, {{"b", "b7", 7}, {"d", "", 8}});
  ConcactIterator history(ssts, 8, true);
  expect_mvcc_records(history, {{"b", "b7", 7}, {"b", "b3", 3},
                                {"d", "", 8}, {"d", "d2", 2}});
  ConcactIterator invisible(ssts, 1, false);
  EXPECT_TRUE(invisible.is_end());
}

// 目的：Level 从 MemTable/L0/L1 中选择真实的最新可见版本，并最后过滤墓碑。
// 场景：MemTable 的 k@4 比磁盘旧；L0 的 k@9 是墓碑，L1 的 k@7 仍应被快照 8 看见。
TEST_F(LSMTest, MvccLevelMergesSnapshotsAcrossSources) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto install = [&](size_t level, const std::vector<MvccRecord> &records) {
    SSTBuilder builder(32, false);
    for (const auto &[key, value, id] : records)
      builder.add(key, value, id);
    const size_t sst_id = engine->next_sst_id++;
    engine->ssts[sst_id] = builder.build(sst_id, engine->get_sst_path(sst_id, level),
                                         engine->block_cache);
    engine->level_sst_ids[level].push_back(sst_id);
  };
  install(0, {{"k", "", 9}, {"k", "l0-old", 5}});
  install(1, {{"k", "l1-newer", 7}, {"z", "z2", 2}});
  engine->cur_max_level = 1;
  engine->memtable.put("", "", 4);
  engine->memtable.put("a", "future", 12);
  engine->memtable.put("a", "a3", 3);
  engine->memtable.put("k", "memory-old", 4);

  {
    auto it = engine->begin(8);
    expect_mvcc_records(it, {{"a", "a3", 3}, {"k", "l1-newer", 7}, {"z", "z2", 2}});
  }
  {
    auto it = engine->begin(0);
    expect_mvcc_records(it, {{"a", "future", 12}, {"z", "z2", 2}});
  }
}

// 目的：compaction 中同 key、同版本冲突时，应延续较新来源 A 的优先级。
// 场景：A 中 k 是墓碑，B 中 k 是旧值；若 B 先写入新 SST，点查询会复活旧值。
//       读上限 0、两路及外层全部 keep_all=true，与当前 compaction 调用一致。
TEST_F(LSMTest, MvccTwoMergeCompactionPreservesEqualVersionTombstone) {
  for (uint64_t id : {uint64_t{0}, uint64_t{5}}) {
    SCOPED_TRACE(id);
    auto a = make_mvcc_heap({{"k", "", id}}, 0, true);
    auto b = make_mvcc_heap({{"k", "old", id}}, 0, true);
    TwoMergeIterator it(a, b, 0, true);
    ASSERT_TRUE(it.is_valid());
    EXPECT_TRUE(it->second.empty());

    SSTBuilder builder(128, false);
    size_t count = 0;
    while (it.is_valid()) {
      ASSERT_LT(count++, 3u);
      builder.add((*it).first, (*it).second, it.get_cur_tranc_id());
      ++it;
    }
    auto sst = builder.build(id, test_dir + "/tie" + std::to_string(id) + ".sst",
                             std::make_shared<BlockCache>(2, 2));
    auto found = sst->get("k", 0);
    ASSERT_TRUE(found.is_valid());
    EXPECT_TRUE(found->second.empty());
  }
}

// 目的：实际读未提交事务的删除，不能仅因后台 compaction 而失效。
// 场景：同一事务先写 k，再在旧值进入 L1 后删除 k；两次操作都使用事务 ID 5。
//       使用正常 put/flush 触发合并，不手工构造 SST，也不调用尚未完成的提交/WAL。
TEST_F(LSMTest, MvccReadUncommittedDeleteSurvivesActualCompaction) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  TranContext transaction(5, engine, nullptr,
                          IsolationLevel::READ_UNOP_COMMITTED);
  const int ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  ASSERT_GE(ratio, 2);

  transaction.put("k", "old");
  engine->flush();

  // 填充其他 key 并逐个刷盘，使旧值通过正常 compaction 进入 L1。
  for (int i = 0; i < ratio; ++i) {
    engine->put("filler-before-" + std::to_string(i), "v", 100 + i);
    engine->flush();
  }
  ASSERT_FALSE(engine->level_sst_ids[1].empty());
  auto old_value = transaction.get("k");
  ASSERT_TRUE(old_value.has_value());
  ASSERT_EQ(*old_value, "old");

  transaction.remove("k");
  engine->flush();
  // 此时 L0 墓碑先于 L1 旧值被查询到，删除正常生效。
  ASSERT_FALSE(transaction.get("k").has_value());

  // 再次触发正常合并，把 L0 的墓碑与 L1 的同版本旧值合到一起。
  for (int i = 0; i < ratio; ++i) {
    engine->put("filler-after-" + std::to_string(i), "v", 200 + i);
    engine->flush();
  }
  auto after_compaction = transaction.get("k");
  EXPECT_FALSE(after_compaction.has_value())
      << "Compaction resurrected value: " << after_compaction.value_or("");
}

// 目的：同版本冲突也要支持先删除、后重新写入，不能固定让墓碑优先。
// 场景：同一个读未提交事务先删除 k，使墓碑进入 L1，再把新值写到 L0。
//       实际 compaction 后，仍应读到最后写入的新值。
TEST_F(LSMTest, MvccReadUncommittedRewriteSurvivesActualCompaction) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  TranContext transaction(5, engine, nullptr,
                          IsolationLevel::READ_UNOP_COMMITTED);
  const int ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  ASSERT_GE(ratio, 2);

  transaction.remove("k");
  engine->flush();
  for (int i = 0; i < ratio; ++i) {
    engine->put("filler-before-" + std::to_string(i), "v", 100 + i);
    engine->flush();
  }
  ASSERT_FALSE(engine->level_sst_ids[1].empty());
  ASSERT_FALSE(transaction.get("k").has_value());

  transaction.put("k", "new");
  engine->flush();
  auto before_compaction = transaction.get("k");
  ASSERT_TRUE(before_compaction.has_value());
  ASSERT_EQ(*before_compaction, "new");

  for (int i = 0; i < ratio; ++i) {
    engine->put("filler-after-" + std::to_string(i), "v", 200 + i);
    engine->flush();
  }
  auto after_compaction = transaction.get("k");
  ASSERT_TRUE(after_compaction.has_value());
  EXPECT_EQ(*after_compaction, "new");
}

// 目的：Engine 点查应和范围遍历一样，跨 MemTable/SST 选择最大可见版本。
// 场景：k@7 已刷入 L0，随后较早事务的 k@5 才写入内存；再验证新墓碑
//       不能被内存中的旧值覆盖，以及旧墓碑不能遮住 SST 中的新值。
TEST_F(LSMTest, EngineMvccPointReadComparesMemoryAndSstVersions) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("k", "disk7", 7);
  engine->remove("deleted", 7);
  engine->put("restored", "disk7", 7);
  engine->flush();
  engine->put("k", "memory5", 5);
  engine->put("deleted", "memory5", 5);
  engine->remove("restored", 5);

  // 范围遍历已经按真实版本比较，作为同一状态下的一致性检查。
  {
    auto it = engine->begin(8);
    expect_mvcc_records(it, {{"k", "disk7", 7}, {"restored", "disk7", 7}});
  }
  for (uint64_t read_id : {uint64_t{0}, uint64_t{8}}) {
    SCOPED_TRACE(read_id);
    for (const std::string key : {"k", "restored"}) {
      auto result = engine->get(key, read_id);
      EXPECT_EQ(result, std::make_optional(
                            std::make_pair(std::string("disk7"), uint64_t{7})));
    }
    EXPECT_FALSE(engine->get("deleted", read_id).has_value());
  }
  EXPECT_EQ(engine->get("k", 6), std::make_optional(
                                   std::make_pair(std::string("memory5"), uint64_t{5})));
  EXPECT_FALSE(engine->get("k", 4).has_value());
}

// 目的：批量查询也必须跨来源比较版本，不能只对 MemTable 未命中的 key 查 SST。
// 场景：分别覆盖所有 key 都在内存命中、有缺失 key 触发 SST 查询两条路径；
//       同时验证墓碑、重复 key 和返回顺序。
TEST_F(LSMTest, EngineMvccBatchReadComparesMemoryAndSstVersions) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("k", "disk7", 7);
  engine->remove("deleted", 7);
  engine->flush();
  engine->put("k", "memory5", 5);
  engine->put("deleted", "memory5", 5);

  for (bool include_missing : {false, true}) {
    SCOPED_TRACE(include_missing);
    std::vector<std::string> keys = {"k", "deleted", "k"};
    if (include_missing) {
      keys.push_back("missing");
    }
    auto results = engine->get_batch(keys, 8);
    ASSERT_EQ(results.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
      EXPECT_EQ(results[i].first, keys[i]);
      if (keys[i] == "k") {
        EXPECT_EQ(results[i].second, std::make_optional(
                      std::make_pair(std::string("disk7"), uint64_t{7})));
      } else {
        EXPECT_FALSE(results[i].second.has_value());
      }
    }
  }
}

// 目的：SST 内部点查不能把文件刷新顺序或层号当成事务版本顺序。
// 场景：先刷 k@7，再刷 k@5；分别检查同在 L0，以及将最先刷出的文件
//       作为 L1 来源的状态。内部查询必须保留胜出墓碑及其真实版本号。
TEST_F(LSMTest, EngineMvccSstReadComparesFilesAndLevels) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("k", "v7", 7);
  engine->remove("deleted", 7);
  engine->flush();
  const size_t first_sst_id = engine->level_sst_ids.at(0).front();
  engine->put("k", "v5", 5);
  engine->put("deleted", "v5", 5);
  engine->flush();

  for (bool first_sst_in_l1 : {false, true}) {
    SCOPED_TRACE(first_sst_in_l1);
    if (first_sst_in_l1) {
      // 构造跨层查询状态；此测试不重开引擎，不验证 compaction 文件重命名。
      engine->level_sst_ids.at(0).pop_back();
      engine->level_sst_ids[1].push_back(first_sst_id);
      engine->cur_max_level = 1;
    }
    std::shared_lock<std::shared_mutex> lock(engine->ssts_mtx);
    EXPECT_EQ(engine->sst_get_("k", 8), std::make_optional(
                  std::make_pair(std::string("v7"), uint64_t{7})));
    EXPECT_EQ(engine->sst_get_("deleted", 8), std::make_optional(
                  std::make_pair(std::string(""), uint64_t{7})));
    EXPECT_EQ(engine->sst_get_("k", 6), std::make_optional(
                  std::make_pair(std::string("v5"), uint64_t{5})));
  }
}

// 目的：保留 16 位偏移和同 key 不跨块的约定，超出编码范围时明确拒绝刷盘。
// 场景：k 有 80 个版本，每个 value 仅 1 KiB；单条长度合法，但版本组超过
//       uint16_t 偏移范围。SSTBuilder 会把相同 key 强制留在同一个 Block。
// 预期：flush 抛出 length_error，不登记 SST、不移除内存数据，旧版本仍可查询。
TEST_F(LSMTest, EngineMvccOversizedVersionGroupFlushPreservesMemory) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  const std::string value(1024, 'x');
  for (uint64_t version = 1; version <= 80; ++version) {
    engine->put("k", value, version);
  }

  const auto oldest = std::make_optional(std::make_pair(value, uint64_t{1}));
  ASSERT_EQ(engine->get("k", 1), oldest);
  const auto size_before = engine->memtable.get_total_size();
  ASSERT_THROW(engine->flush(), std::length_error);
  EXPECT_TRUE(engine->ssts.empty());
  EXPECT_EQ(engine->memtable.get_total_size(), size_before);
  EXPECT_EQ(engine->get("k", 1), oldest);
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(value, uint64_t{80})));
}

// Test basic operations: put, get, remove
TEST_F(LSMTest, BasicOperations) {
  LSM lsm(test_dir);

  // Test put and get
  lsm.put("key1", "value1");
  EXPECT_EQ(lsm.get("key1").value(), "value1");

  // Test update
  lsm.put("key1", "new_value");
  EXPECT_EQ(lsm.get("key1").value(), "new_value");

  // Test remove
  lsm.remove("key1");
  EXPECT_FALSE(lsm.get("key1").has_value());

  // Test non-existent key
  EXPECT_FALSE(lsm.get("nonexistent").has_value());
}

// Test persistence across restarts
TEST_F(LSMTest, Persistence) {
  std::unordered_map<std::string, std::string> kvs;
  int num = 100000;
  {
    LSM lsm(test_dir);
    for (int i = 0; i < num; ++i) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      lsm.put(key, value);
      kvs[key] = value;

      // 删除之前被10整除的key
      if (i % 10 == 0 && i != 0) {
        std::string del_key = "key" + std::to_string(i - 10);
        lsm.remove(del_key);
        kvs.erase(del_key);
      }
    }
  } // LSM destructor called here

  // Create new LSM instance
  LSM lsm(test_dir);
  for (int i = 0; i < num; ++i) {
    std::string key = "key" + std::to_string(i);
    if (kvs.find(key) != kvs.end()) {
      EXPECT_EQ(lsm.get(key).value(), kvs[key]);
    } else {
      if (key == "key4410") {
        // debug
        auto res = lsm.get("key4410");
      }
      if (lsm.get(key).has_value()) {
        std::cout << "key" << i << " not exist but found" << std::endl;
        exit(-1);
      }
      // EXPECT_FALSE(lsm.get(key).has_value());
    }
  }

  // Query a not exist key
  EXPECT_FALSE(lsm.get("nonexistent").has_value());
}

// Test large scale operations
TEST_F(LSMTest, LargeScaleOperations) {
  LSM lsm(test_dir);
  std::vector<std::pair<std::string, std::string>> data;

  // Insert enough data to trigger multiple flushes
  for (int i = 0; i < 1000; i++) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    lsm.put(key, value);
    data.emplace_back(key, value);
  }

  // Verify all data
  for (const auto &[key, value] : data) {
    EXPECT_EQ(lsm.get(key).value(), value);
  }
}

// Test iterator functionality
TEST_F(LSMTest, IteratorOperations) {
  LSM lsm(test_dir);
  std::map<std::string, std::string> reference;

  // Insert data
  for (int i = 0; i < 100; i++) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    lsm.put(key, value);
    reference[key] = value;
  }

  // Test iterator
  auto it = lsm.begin(0);
  auto ref_it = reference.begin();

  while (it != lsm.end() && ref_it != reference.end()) {
    EXPECT_EQ(it->first, ref_it->first);
    EXPECT_EQ(it->second, ref_it->second);
    ++it;
    ++ref_it;
  }

  EXPECT_EQ(it == lsm.end(), ref_it == reference.end());
}

// 目的：验证内部事务完成标记不会影响最终用户遍历的 key 顺序。
// 场景：MemTable 含空 key 标记和 A，L0 SST 含 Z；归并时必须先处理标记，
//       才能露出被它挡住的 A。直接构造提交后的数据状态，不依赖尚未完成的 WAL。
// 预期：最终只输出 A、Z，顺序正确；不限定标记必须在哪一层过滤。
TEST_F(LSMTest, TransactionMarkerDoesNotReorderMergedIteration) {
  auto engine = std::make_shared<LSMEngine>(test_dir);

  SSTBuilder builder(256, false);
  builder.add("Z", "disk-value", 3);
  const size_t sst_id = engine->next_sst_id++;
  engine->ssts[sst_id] = builder.build(
      sst_id, engine->get_sst_path(sst_id, 0), engine->block_cache);
  engine->level_sst_ids[0].push_front(sst_id);

  engine->memtable.put("A", "memory-value", 5);
  engine->memtable.put("", "", 5);

  auto it = engine->begin(0);
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->first, "A");
  EXPECT_EQ(it->second, "memory-value");
  ++it;
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->first, "Z");
  EXPECT_EQ(it->second, "disk-value");
  ++it;
  EXPECT_TRUE(it.is_end());
}

// 目的：内部事务完成标记既不出现在查询结果中，也不能传给用户的谓词。
// 场景：MemTable 与 SST 都包含标记和普通数据；用户谓词接受所有用户 key。
//       即使最终 HeapIterator 去掉了空值，也不能掩盖此前回调收到空 key 的问题。
// 回归：Engine 在查询入口包装谓词后，所有来源都应拦截空 key。
TEST_F(LSMTest, PredicateDoesNotExposeTransactionMarkersToCallback) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  SSTBuilder builder(256, false);
  builder.add("", "", 3);
  builder.add("Z", "disk-value", 3);
  const size_t sst_id = engine->next_sst_id++;
  engine->ssts[sst_id] = builder.build(
      sst_id, engine->get_sst_path(sst_id, 0), engine->block_cache);
  engine->level_sst_ids[0].push_front(sst_id);
  engine->memtable.put("", "", 5);
  engine->memtable.put("A", "memory-value", 5);

  bool saw_internal_key = false;
  auto range = engine->lsm_iters_monotony_predicate(
      0, [&saw_internal_key](const std::string &key) {
        if (key.empty()) {
          saw_internal_key = true;
        }
        return 0;
      });
  EXPECT_FALSE(saw_internal_key)
      << "Internal transaction marker was passed to the user predicate";
  ASSERT_TRUE(range.has_value());
  expect_mvcc_records(range->first,
                      {{"A", "memory-value", 5}, {"Z", "disk-value", 3}});
  EXPECT_TRUE(range->first == range->second);
}

// 目的：验证查询跳过事务标记后，刷盘仍能取得标记对应的事务 ID。
// 场景：MemTable 含 A 和事务 5 的完成标记，先遍历用户数据，再刷出该表。
// 预期：查询只返回 A；flush_last 仍输出事务 ID 5，说明推进查询迭代器
//       不会删除底层记录。直接构造提交后的状态，不依赖 WAL 的实现。
TEST_F(LSMTest, QuerySkippingTransactionMarkerPreservesFlushMetadata) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->memtable.put("A", "value", 5);
  engine->memtable.put("", "", 5);

  {
    auto it = engine->begin(0);
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it->first, "A");
    EXPECT_EQ(it->second, "value");
    ++it;
    EXPECT_TRUE(it.is_end());
  } // 查询结束，释放 Level_Iterator 持有的 SST 读锁。

  SSTBuilder builder(256, false);
  const size_t sst_id = engine->next_sst_id++;
  auto path = engine->get_sst_path(sst_id, 0);
  std::vector<uint64_t> flushed_ids;
  auto sst = engine->memtable.flush_last(
      builder, path, sst_id, flushed_ids, engine->block_cache);
  ASSERT_NE(sst, nullptr);
  ASSERT_EQ(flushed_ids.size(), 1u);
  EXPECT_EQ(flushed_ids.front(), 5u);
}

// 目的：验证引擎刷盘正确处理空表，并且不会在重复刷盘时登记空 SST。
// 场景：先刷空表，再写 K@5 刷盘，最后再次刷已清空的 MemTable。
// 预期：返回值依次为 0、5、0，仅登记一张有效 SST，数据仍然可读。
TEST_F(LSMTest, EngineFlushEmptyAndNonemptyTables) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  EXPECT_EQ(engine->flush(), 0u);
  EXPECT_TRUE(engine->ssts.empty());
  EXPECT_TRUE(engine->level_sst_ids.empty());

  engine->memtable.put("K", "value", 5);
  EXPECT_EQ(engine->flush(), 5u);
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  EXPECT_EQ(engine->flush(), 0u);

  ASSERT_EQ(engine->ssts.size(), 1u);
  const auto level = engine->level_sst_ids.find(0);
  ASSERT_NE(level, engine->level_sst_ids.end());
  ASSERT_EQ(level->second.size(), 1u);
  ASSERT_NE(engine->ssts.at(level->second.front()), nullptr);
  auto result = engine->get("K", 0);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->first, "value");
  EXPECT_EQ(result->second, 5u);
}

// 目的：验证并发刷盘不会重复登记同一张表或留下空 SST。
// 场景：多个线程一起请求刷出唯一一张 MemTable，只有一个线程应生成 SST。
// 预期：一个调用返回 5，其余返回 0；最终仅有一张有效 SST，数据可读。
//       此用例检查并发结果，不保证每次调度都进入加锁前检查的竞争窗口。
TEST_F(LSMTest, ConcurrentFlushRegistersOnlyOneSst) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->memtable.put("K", "value", 5);

  constexpr size_t worker_count = 8;
  std::latch start(1);
  std::vector<uint64_t> results(worker_count, 0);
  std::vector<std::exception_ptr> errors(worker_count);
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    workers.emplace_back([&, i] {
      start.wait();
      try {
        results[i] = engine->flush();
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  start.count_down();
  for (auto &worker : workers)
    worker.join();

  size_t successful_flushes = 0;
  for (size_t i = 0; i < worker_count; ++i) {
    ASSERT_FALSE(static_cast<bool>(errors[i]));
    EXPECT_TRUE(results[i] == 0 || results[i] == 5);
    successful_flushes += results[i] == 5;
  }
  EXPECT_EQ(successful_flushes, 1u);
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  ASSERT_EQ(engine->ssts.size(), 1u);
  const auto level = engine->level_sst_ids.find(0);
  ASSERT_NE(level, engine->level_sst_ids.end());
  ASSERT_EQ(level->second.size(), 1u);
  ASSERT_NE(engine->ssts.at(level->second.front()), nullptr);
  auto result = engine->get("K", 0);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->first, "value");
  EXPECT_EQ(result->second, 5u);
}

// Test mixed operations
TEST_F(LSMTest, MixedOperations) {
  LSM lsm(test_dir);
  std::map<std::string, std::string> reference;

  // Perform mixed operations
  lsm.put("key1", "value1");
  reference["key1"] = "value1";

  lsm.put("key2", "value2");
  reference["key2"] = "value2";

  lsm.remove("key1");
  reference.erase("key1");

  lsm.put("key3", "value3");
  reference["key3"] = "value3";

  // Verify final state
  for (const auto &[key, value] : reference) {
    EXPECT_EQ(lsm.get(key).value(), value);
  }
  EXPECT_FALSE(lsm.get("key1").has_value());
}

TEST_F(LSMTest, MonotonyPredicate) {
  LSM lsm(test_dir);

  // Insert data
  for (int i = 0; i < 100; i++) {
    std::ostringstream oss_key;
    std::ostringstream oss_value;
    oss_key << "key" << std::setw(2) << std::setfill('0') << i;
    oss_value << "value" << std::setw(2) << std::setfill('0') << i;
    std::string key = oss_key.str();
    std::string value = oss_value.str();
    lsm.put(key, value);
    if (i == 50) {
      // 主动刷一次盘
      lsm.flush();
    }
  }

  // Define a predicate function
  auto predicate = [](const std::string &key) -> int {
    // Extract the number from the key
    int key_num = std::stoi(key.substr(3));
    if (key_num < 20) {
      return 1;
    }
    if (key_num > 60) {
      return -1;
    }
    return 0;
  };

  // Call the method under test
  auto result = lsm.lsm_iters_monotony_predicate(0, predicate);

  // Check if the result is not empty
  ASSERT_TRUE(result.has_value());

  // Extract the iterators from the result
  auto [start, end] = result.value();

  // Verify the range of keys returned by the iterators
  std::set<std::string> expected_keys;
  for (int i = 20; i <= 60; i++) {
    std::ostringstream oss;
    oss << "key" << std::setw(2) << std::setfill('0') << i;
    expected_keys.insert(oss.str());
  }

  std::set<std::string> actual_keys;
  for (auto it = start; it != end; ++it) {
    actual_keys.insert(it->first);
  }

  EXPECT_EQ(actual_keys, expected_keys);
}

// 目的：新事务应处于未结束状态，并为后续操作保留正确的事务身份和开始记录。
// 场景：分别创建四种隔离级别的上下文；构造本身不执行读写、提交或 WAL 持久化。
TEST_F(LSMTest, TranContextConstructorInitializesTransaction) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = std::make_shared<TranManager>(test_dir);
  uint64_t id = 7;
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    TranContext context(id, engine, manager, level);

    EXPECT_EQ(context.tranc_id_, id);
    EXPECT_EQ(context.engine_, engine);
    EXPECT_EQ(context.tranManager_.lock(), manager);
    EXPECT_EQ(context.isolation_level_, level);
    EXPECT_FALSE(context.isCommited);
    EXPECT_FALSE(context.isAborted);
    EXPECT_TRUE(context.temp_map_.empty());
    ASSERT_EQ(context.operations.size(), 1u);
    EXPECT_EQ(context.operations.front().getOperationType(),
              OperationType::OP_CREATE);
    EXPECT_EQ(context.operations.front().getTrancId(), id);
    ++id;
  }
}

// 目的：上下文保持引擎存活，但不通过强引用延长管理器生命周期，避免双方互持。
// 场景：释放外部引用后，引擎随上下文存活，管理器可以先销毁；不调用提交或回滚。
TEST_F(LSMTest, TranContextConstructorPreservesOwnershipRules) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = std::make_shared<TranManager>(test_dir);
  std::weak_ptr<LSMEngine> engine_observer = engine;
  std::weak_ptr<TranManager> manager_observer = manager;
  auto context = std::make_shared<TranContext>(
      7, engine, manager, IsolationLevel::REPEATABLE_READ);

  engine.reset();
  manager.reset();
  EXPECT_FALSE(engine_observer.expired());
  EXPECT_TRUE(manager_observer.expired());
  EXPECT_TRUE(context->tranManager_.expired());

  context.reset();
  EXPECT_TRUE(engine_observer.expired());
}

// 目的：new_tranc 创建具有独立 ID 的上下文，并将它们登记为活跃事务。
// 场景：不同隔离级别各创建一个事务；调用者释放句柄后，管理器仍持有上下文。
//       管理器销毁后上下文也能释放，避免双方互持形成循环引用。
//       查询未登记 ID 不消耗事务 ID；活跃事务也不应被误判为已回滚。
TEST_F(LSMTest, NewTransactionRegistersContexts) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = std::make_shared<TranManager>(test_dir);
  manager->set_engine(engine);

  const TranManager &read_only_manager = *manager;
  EXPECT_FALSE(read_only_manager.is_aborted(1));

  auto first = manager->new_tranc(IsolationLevel::READ_OP_COMMITTED);
  auto second = manager->new_tranc(IsolationLevel::REPEATABLE_READ);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(first->tranc_id_, 1u);
  EXPECT_EQ(second->tranc_id_, 2u);
  EXPECT_FALSE(read_only_manager.is_aborted(first->tranc_id_));
  EXPECT_FALSE(read_only_manager.is_aborted(second->tranc_id_));
  EXPECT_EQ(first->engine_, engine);
  EXPECT_EQ(second->engine_, engine);
  EXPECT_EQ(first->tranManager_.lock(), manager);
  EXPECT_EQ(second->tranManager_.lock(), manager);
  EXPECT_EQ(first->isolation_level_, IsolationLevel::READ_OP_COMMITTED);
  EXPECT_EQ(second->isolation_level_, IsolationLevel::REPEATABLE_READ);
  ASSERT_EQ(first->operations.size(), 1u);
  EXPECT_EQ(first->operations.front().getTrancId(), first->tranc_id_);

  std::weak_ptr<TranContext> first_observer = first;
  std::weak_ptr<TranContext> second_observer = second;
  first.reset();
  second.reset();
  EXPECT_FALSE(first_observer.expired());
  EXPECT_FALSE(second_observer.expired());
  manager.reset();
  EXPECT_TRUE(first_observer.expired());
  EXPECT_TRUE(second_observer.expired());
}

// 目的：缺少引擎时立即报告初始化错误，不返回无法使用的事务句柄。
// 场景：首次创建失败，绑定引擎后重试；失败的调用不应提前分配 ID。
TEST_F(LSMTest, NewTransactionRequiresEngine) {
  auto manager = std::make_shared<TranManager>(test_dir);
  EXPECT_THROW(manager->new_tranc(IsolationLevel::REPEATABLE_READ),
               std::logic_error);

  auto engine = std::make_shared<LSMEngine>(test_dir);
  manager->set_engine(engine);
  auto context = manager->new_tranc(IsolationLevel::REPEATABLE_READ);
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(context->tranc_id_, 1u);
}

// 目的：并发创建事务时，ID 不重复，且每个上下文都成功登记。
// 场景：4 个线程同时各创建 64 个事务，结束后逐个检查句柄和管理器持有关系。
TEST_F(LSMTest, NewTransactionConcurrentCreation) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = std::make_shared<TranManager>(test_dir);
  manager->set_engine(engine);

  constexpr size_t worker_count = 4;
  constexpr size_t per_worker = 64;
  std::vector<std::vector<std::shared_ptr<TranContext>>> contexts(
      worker_count, std::vector<std::shared_ptr<TranContext>>(per_worker));
  std::vector<std::exception_ptr> errors(worker_count);
  std::vector<std::thread> workers;
  std::latch start(1);
  for (size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      start.wait();
      try {
        for (auto &context : contexts[worker]) {
          context = manager->new_tranc(IsolationLevel::REPEATABLE_READ);
        }
      } catch (...) {
        errors[worker] = std::current_exception();
      }
    });
  }
  start.count_down();
  for (auto &worker : workers) {
    worker.join();
  }

  for (const auto &error : errors) {
    ASSERT_FALSE(static_cast<bool>(error));
  }
  std::set<uint64_t> ids;
  std::vector<std::weak_ptr<TranContext>> observers;
  for (const auto &batch : contexts) {
    for (const auto &context : batch) {
      ASSERT_NE(context, nullptr);
      EXPECT_TRUE(ids.insert(context->tranc_id_).second);
      EXPECT_EQ(context->engine_, engine);
      EXPECT_EQ(context->tranManager_.lock(), manager);
      observers.emplace_back(context);
    }
  }
  ASSERT_EQ(ids.size(), worker_count * per_worker);
  EXPECT_EQ(*ids.begin(), 1u);
  EXPECT_EQ(*ids.rbegin(), worker_count * per_worker);

  contexts.clear();
  for (const auto &observer : observers) {
    EXPECT_FALSE(observer.expired());
  }
  manager.reset();
  for (const auto &observer : observers) {
    EXPECT_TRUE(observer.expired());
  }
}

// 目的：结束登记区分提交与回滚，重复登记回滚不会影响其他事务。
// 场景：5 取消、6 提交、7 未登记；再次取消 5 后，只有 5 属于回滚名单。
TEST_F(LSMTest, TransactionEndRegistrationTracksOnlyAbortedIds) {
  TranManager manager(test_dir);
  EXPECT_FALSE(manager.is_aborted(5));
  EXPECT_FALSE(manager.is_aborted(6));

  manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  manager.add_ready_to_flush_tranc_id(6, TransactionState::OP_COMMITTED);
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_FALSE(manager.is_aborted(6));
  EXPECT_FALSE(manager.is_aborted(7));

  manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_FALSE(manager.is_aborted(6));
  EXPECT_FALSE(manager.is_aborted(7));
}

// 目的：待刷盘登记清理后，回滚名单仍有效，不能让 SST 中的取消版本重新可见。
// 场景：5 已取消，6 已提交；登记 6 刷盘时会一并清理较小的回滚 ID 5。
//       再对 5 重复登记、清理，仍不得丢失其回滚身份。
TEST_F(LSMTest, TransactionEndRegistrationSurvivesFlushCleanup) {
  TranManager manager(test_dir);
  manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  manager.add_ready_to_flush_tranc_id(6, TransactionState::OP_COMMITTED);
  manager.add_flushed_tranc_id(6);

  EXPECT_TRUE(manager.get_flushed_tranc_ids().contains(5));
  EXPECT_TRUE(manager.get_flushed_tranc_ids().contains(6));
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_FALSE(manager.is_aborted(6));

  manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  manager.add_flushed_tranc_id(5);
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_FALSE(manager.is_aborted(6));
}

// 目的：多个线程登记结束状态，同时查询回滚名单，不能丢登记或误判提交事务。
// 场景：4 个线程各登记 64 个不同 ID，奇数取消、偶数提交；每次登记后立即查询，
//       全部线程结束后再逐一核对。并发用例之外，仍需检查所有集合访问均持锁。
TEST_F(LSMTest, TransactionEndRegistrationSupportsConcurrentQueries) {
  TranManager manager(test_dir);
  constexpr size_t worker_count = 4;
  constexpr size_t per_worker = 64;
  std::vector<std::exception_ptr> errors(worker_count);
  std::vector<int> queries_correct(worker_count, 1);
  std::vector<std::thread> workers;
  std::latch start(1);

  for (size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&, worker] {
      start.wait();
      try {
        for (size_t offset = 0; offset < per_worker; ++offset) {
          const uint64_t id = worker * per_worker + offset + 1;
          const bool aborted = (id % 2 != 0);
          manager.add_ready_to_flush_tranc_id(
              id, aborted ? TransactionState::ABORTED
                          : TransactionState::OP_COMMITTED);
          if (manager.is_aborted(id) != aborted || manager.is_aborted(0)) {
            queries_correct[worker] = 0;
          }
        }
      } catch (...) {
        errors[worker] = std::current_exception();
      }
    });
  }
  start.count_down();
  for (auto &worker : workers) {
    worker.join();
  }

  for (size_t worker = 0; worker < worker_count; ++worker) {
    ASSERT_FALSE(static_cast<bool>(errors[worker])) << worker;
    EXPECT_EQ(queries_correct[worker], 1) << worker;
  }
  for (uint64_t id = 1; id <= worker_count * per_worker; ++id) {
    EXPECT_EQ(manager.is_aborted(id), id % 2 != 0) << id;
  }
}

// 目的：暂存型事务的 put 不提前修改引擎，重复写同一 key 时保留最后一次值。
// 场景：读已提交、可重复读及串行化分支分别修改已有 key 并新增 key；
//       只检查写入暂存行为，不用尚未完成的 commit 验证隔离级别整体语义。
TEST_F(LSMTest, TranContextPutStagesWritesAndRecordsOperations) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("existing", "old", 1);

  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    TranContext context(7, engine, nullptr, level);
    context.put("existing", "first");
    context.put("existing", "last");
    context.put("new", "new-value");

    ASSERT_EQ(context.temp_map_.size(), 2u);
    EXPECT_EQ(context.temp_map_.at("existing"), "last");
    EXPECT_EQ(context.temp_map_.at("new"), "new-value");
    EXPECT_EQ(engine->get("existing", 0),
              std::make_optional(std::make_pair(std::string("old"), uint64_t{1})));
    EXPECT_FALSE(engine->get("new", 0).has_value());

    ASSERT_EQ(context.operations.size(), 4u);
    EXPECT_EQ(context.operations.front().getOperationType(), OperationType::OP_CREATE);
    for (size_t i = 1; i < context.operations.size(); ++i) {
      EXPECT_EQ(context.operations[i].getOperationType(), OperationType::OP_PUT);
      EXPECT_EQ(context.operations[i].getTrancId(), 7u);
    }
    EXPECT_EQ(context.operations[1].getKey(), "existing");
    EXPECT_EQ(context.operations[1].getValue(), "first");
    EXPECT_EQ(context.operations[2].getKey(), "existing");
    EXPECT_EQ(context.operations[2].getValue(), "last");
    EXPECT_EQ(context.operations[3].getKey(), "new");
    EXPECT_EQ(context.operations[3].getValue(), "new-value");
  }
}

// 目的：读未提交的 put 立即写入引擎，使用本事务 ID，并保留更老版本。
// 场景：k@3 原值 old，事务 7 连续两次修改 k；读上限 3 仍应读到 old，
//       最新查询读到事务 7 的最后一次修改。此测试不调用待实现的 abort。
TEST_F(LSMTest, TranContextPutReadUncommittedWritesOwnVersion) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("k", "old", 3);
  TranContext context(7, engine, nullptr, IsolationLevel::READ_UNOP_COMMITTED);

  context.put("k", "first");
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("first"), uint64_t{7})));
  context.put("k", "last");
  context.put("new", "created");
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("last"), uint64_t{7})));
  EXPECT_EQ(engine->get("k", 3),
            std::make_optional(std::make_pair(std::string("old"), uint64_t{3})));
  EXPECT_EQ(engine->get("new", 0),
            std::make_optional(std::make_pair(std::string("created"), uint64_t{7})));
  EXPECT_FALSE(engine->get("new", 3).has_value());
  EXPECT_TRUE(context.temp_map_.empty());
  ASSERT_EQ(context.operations.size(), 4u);
  EXPECT_EQ(context.operations.back().getOperationType(), OperationType::OP_PUT);
  EXPECT_EQ(context.operations.back().getTrancId(), 7u);
}

// 目的：事务结束后拒绝继续 put，且不追加操作记录、不修改暂存表或引擎。
// 场景：直接设置公开的结束标记，单独验证 put 的状态检查，避免依赖未完成的提交/回滚。
TEST_F(LSMTest, TranContextPutRejectsFinishedTransactions) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    for (bool committed : {false, true}) {
      SCOPED_TRACE(static_cast<int>(level));
      SCOPED_TRACE(committed);
      TranContext context(7, engine, nullptr, level);
      context.isCommited = committed;
      context.isAborted = !committed;

      EXPECT_THROW(context.put("blocked", "value"), std::logic_error);
      EXPECT_EQ(context.operations.size(), 1u);
      EXPECT_TRUE(context.temp_map_.empty());
      EXPECT_FALSE(engine->get("blocked", 0).has_value());
    }
  }
}

// 目的：本事务自己的修改优先于旧读缓存，暂存墓碑应返回“不存在”。
// 场景：先读取引擎旧值，再暂存新值、空值墓碑、重写值，依次观察 get 的结果。
TEST_F(LSMTest, TranContextGetOwnWritesOverrideReadCache) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("k", "old", 3);
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    TranContext context(10, engine, nullptr, level);
    EXPECT_EQ(context.get("k"), std::make_optional(std::string("old")));
    context.put("k", "own");
    EXPECT_EQ(context.get("k"), std::make_optional(std::string("own")));
    context.put("k", "");
    EXPECT_FALSE(context.get("k").has_value());
    context.put("k", "rewritten");
    EXPECT_EQ(context.get("k"), std::make_optional(std::string("rewritten")));
    EXPECT_EQ(engine->get("k", 0),
              std::make_optional(std::make_pair(std::string("old"), uint64_t{3})));
  }
}

// 目的：已缓存的结果保持稳定，每个事务各自维护读缓存。
// 场景：事务 10 先读到 k@3，之后引擎出现同样满足 <=10 的 k@7；
//       旧事务仍读 k@3，新事务 11 首次读取则选 k@7。
//       SERIALIZABLE 这里只验证单键缓存规则，不代表验证了完整串行化。
TEST_F(LSMTest, TranContextGetReadCacheBelongsToEachTransaction) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  for (auto level : {IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    const auto key = "k" + std::to_string(static_cast<int>(level));
    SCOPED_TRACE(key);
    engine->put(key, "old", 3);
    TranContext first(10, engine, nullptr, level);
    EXPECT_EQ(first.get(key), std::make_optional(std::string("old")));
    engine->put(key, "new", 7);
    EXPECT_EQ(first.get(key), std::make_optional(std::string("old")));
    TranContext second(11, engine, nullptr, level);
    EXPECT_EQ(second.get(key), std::make_optional(std::string("new")));
  }
}

// 目的：首次未命中和首次读到墓碑，都应缓存为 nullopt。
// 场景：事务 10 查询无记录/已删除的 key 后，版本 8 的新值进入引擎；
//       原事务保持“不存在”，新事务可以读到新值，避免只缓存有值的查询。
TEST_F(LSMTest, TranContextGetReadCachePreservesMissingResults) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  for (auto level : {IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto missing = "missing" + suffix;
    const auto deleted = "deleted" + suffix;
    SCOPED_TRACE(suffix);
    engine->put(deleted, "old", 3);
    engine->remove(deleted, 7);
    TranContext first(10, engine, nullptr, level);
    EXPECT_FALSE(first.get(missing).has_value());
    EXPECT_FALSE(first.get(deleted).has_value());
    engine->put(missing, "created", 8);
    engine->put(deleted, "restored", 8);
    EXPECT_FALSE(first.get(missing).has_value());
    EXPECT_FALSE(first.get(deleted).has_value());
    TranContext second(11, engine, nullptr, level);
    EXPECT_EQ(second.get(missing), std::make_optional(std::string("created")));
    EXPECT_EQ(second.get(deleted), std::make_optional(std::string("restored")));
  }
}

// 目的：不缓存查询的分支，每次都能看到引擎当前版本，不固定使用事务开始 ID。
// 场景：事务 5 查询后，引擎出现版本 7 的更新和新增记录；再次查询看到新值。
//       这里只验证刷新行为，不验证混合隔离级别下尚未完成的提交状态过滤。
TEST_F(LSMTest, TranContextGetRefreshesCurrentVersions) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED}) {
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto key = "k" + suffix;
    const auto missing = "missing" + suffix;
    SCOPED_TRACE(suffix);
    engine->put(key, "old", 3);
    TranContext context(5, engine, nullptr, level);
    EXPECT_EQ(context.get(key), std::make_optional(std::string("old")));
    EXPECT_FALSE(context.get(missing).has_value());
    engine->put(key, "new", 7);
    engine->put(missing, "created", 7);
    EXPECT_EQ(context.get(key), std::make_optional(std::string("new")));
    EXPECT_EQ(context.get(missing), std::make_optional(std::string("created")));
    engine->remove(key, 8);
    EXPECT_FALSE(context.get(key).has_value());
  }
}

// 目的：首次读取按版本上限选择候选，能跨内存和 SST 获取正确的历史值。
// 场景：k@3、k@7 已刷盘，k@12 在内存；事务 10、4、2 首次查询应分别得到
//       版本 7、版本 3 和“不存在”，不能直接读取不受限的最新版本 12。
TEST_F(LSMTest, TranContextGetFirstReadRespectsVersionBound) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  engine->put("k", "v3", 3);
  engine->flush();
  engine->put("k", "v7", 7);
  engine->flush();
  engine->put("k", "v12", 12);
  for (auto level : {IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    TranContext at10(10, engine, nullptr, level);
    TranContext at4(4, engine, nullptr, level);
    TranContext at2(2, engine, nullptr, level);
    EXPECT_EQ(at10.get("k"), std::make_optional(std::string("v7")));
    EXPECT_EQ(at4.get("k"), std::make_optional(std::string("v3")));
    EXPECT_FALSE(at2.get("k").has_value());
  }
}

// 目的：结束后的事务不能继续 get，即使自己的暂存表里有命中项也不能绕过检查。
// 场景：先 put，再设置公开的提交/回滚标记，单独验证 get 的状态检查。
TEST_F(LSMTest, TranContextGetRejectsFinishedTransactions) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    for (bool committed : {false, true}) {
      SCOPED_TRACE(static_cast<int>(level));
      SCOPED_TRACE(committed);
      TranContext context(7, engine, nullptr, level);
      context.put("k", "own");
      context.isCommited = committed;
      context.isAborted = !committed;
      EXPECT_THROW(context.get("k"), std::logic_error);
    }
  }
}

// commit 测试单独管理容量配置；测试结束后恢复，避免影响其他用例。
class TranCommitTest : public LSMTest {
protected:
  void SetUp() override {
    LSMTest::SetUp();
    config = &const_cast<TomlConfig &>(TomlConfig::getInstance());
    saved_per_limit = config->getLsmPerMemSizeLimit();
    saved_total_limit = config->getLsmTolMemSizeLimit();
    config->modify_lsm_per_mem_size_limit(1024 * 1024);
    config->modify_lsm_tol_mem_size_limit(4 * 1024 * 1024);
    engine = std::make_shared<LSMEngine>(test_dir);
    manager = std::make_shared<TranManager>(test_dir);
    manager->set_engine(engine);
    engine->set_tran_manager(manager);
  }

  void TearDown() override {
    manager.reset();
    engine.reset();
    config->modify_lsm_per_mem_size_limit(saved_per_limit);
    config->modify_lsm_tol_mem_size_limit(saved_total_limit);
    LSMTest::TearDown();
  }

  TomlConfig *config = nullptr;
  long long saved_per_limit = 0;
  long long saved_total_limit = 0;
  std::shared_ptr<LSMEngine> engine;
  std::shared_ptr<TranManager> manager;
};

// 复用事务测试的引擎、管理器绑定和容量恢复，避免测试依赖尚未完成的 LSM 入口。
class TranAbortTest : public TranCommitTest {};

// 目的：暂存型事务主动取消后，不把修改写入数据库，也不能再提交。
// 场景：修改和删除的原值保持不变，新增 key 不出现；同时检查取消登记、
//       暂存清理、重复取消以及结束后的 put/get/commit。remove 的结束检查待其实现。
TEST_F(TranAbortTest, CancelsStagedWritesWithoutChangingDatabase) {
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto existing = "existing-" + suffix;
    const auto added = "added-" + suffix;
    const auto removed = "removed-" + suffix;
    engine->put(existing, "old", 3);
    engine->put(removed, "keep", 3);
    TranContext context(10 + static_cast<int>(level), engine, manager, level);
    EXPECT_EQ(context.get(existing), std::make_optional(std::string("old")));
    context.put(existing, "pending");
    context.put(added, "pending");
    context.remove(removed);
    ASSERT_TRUE(context.abort());
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(context.isCommited);
    EXPECT_TRUE(manager->is_aborted(context.tranc_id_));
    EXPECT_TRUE(context.temp_map_.empty());
    EXPECT_TRUE(context.operations.empty());
    EXPECT_EQ(engine->get(existing, 0),
              std::make_optional(std::make_pair(std::string("old"), uint64_t{3})));
    EXPECT_EQ(engine->get(removed, 0),
              std::make_optional(std::make_pair(std::string("keep"), uint64_t{3})));
    EXPECT_FALSE(engine->get(added, 0));
    EXPECT_FALSE(context.abort());
    EXPECT_THROW(context.put(added, "too-late"), std::logic_error);
    EXPECT_THROW(context.get(existing), std::logic_error);
    EXPECT_FALSE(context.commit());
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(context.isCommited);
    EXPECT_TRUE(context.temp_map_.empty());
    EXPECT_TRUE(context.operations.empty());
  }
}

// 目的：没有执行过写入的事务也能正常取消，不能要求必须先产生数据记录。
// 场景：四种隔离级别分别创建空事务，首次取消成功，再次取消返回 false。
TEST_F(TranAbortTest, CancelsEmptyTransactionsOnlyOnce) {
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    TranContext context(10 + static_cast<int>(level), engine, manager, level);
    ASSERT_TRUE(context.abort());
    EXPECT_TRUE(manager->is_aborted(context.tranc_id_));
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(context.isCommited);
    EXPECT_TRUE(context.temp_map_.empty());
    EXPECT_TRUE(context.operations.empty());
    EXPECT_FALSE(context.abort());
    EXPECT_FALSE(context.commit());
  }
}

// 目的：无法登记回滚时必须报错，不能提前结束事务或丢弃仍在暂存的修改。
// 场景：事务暂存了写入，但管理器已销毁；abort 抛异常，状态和暂存内容保持原样。
TEST_F(TranAbortTest, ExpiredManagerPreservesUnfinishedContext) {
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  context.put("k", "pending");
  const auto pending = context.temp_map_;
  const auto operation_count = context.operations.size();
  manager.reset();
  ASSERT_TRUE(context.tranManager_.expired());

  EXPECT_THROW(context.abort(), std::logic_error);
  EXPECT_FALSE(context.isAborted);
  EXPECT_FALSE(context.isCommited);
  EXPECT_EQ(context.temp_map_, pending);
  EXPECT_EQ(context.operations.size(), operation_count);
  EXPECT_EQ(context.get("k"), std::make_optional(std::string("pending")));
  EXPECT_FALSE(engine->get("k", 0));
}

// 目的：读未提交的取消只登记 ID，不通过改写原节点或补写墓碑来模拟撤销。
// 场景：修改、新增、删除后保留原节点的迭代器，取消后检查物理记录和内存大小不变。
//       这里检查原节点，不用用户查询结果判断回滚；查询过滤由后续阶段验收。
TEST_F(TranAbortTest, ReadUncommittedRegistersWithoutRewritingRecords) {
  engine->put("existing", "old", 3);
  engine->put("removed", "keep", 3);
  TranContext context(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  context.put("existing", "first");
  context.put("existing", "last");
  context.put("added", "new");
  context.remove("removed");

  auto updated = engine->memtable.get("existing", 0);
  auto original = engine->memtable.get("existing", 3);
  auto added = engine->memtable.get("added", 0);
  auto removed = engine->memtable.get("removed", 0);
  ASSERT_TRUE(updated.is_valid());
  ASSERT_TRUE(original.is_valid());
  ASSERT_TRUE(added.is_valid());
  ASSERT_TRUE(removed.is_valid());
  const auto bytes = engine->memtable.get_total_size();

  ASSERT_TRUE(context.abort());
  EXPECT_TRUE(context.isAborted);
  EXPECT_FALSE(context.isCommited);
  EXPECT_TRUE(manager->is_aborted(5));
  EXPECT_FALSE(manager->is_aborted(3));
  EXPECT_TRUE(context.operations.empty());
  EXPECT_EQ(engine->memtable.get_total_size(), bytes);
  EXPECT_EQ(updated.get_value(), "last");
  EXPECT_EQ(updated.get_cur_tranc_id(), 5u);
  EXPECT_EQ(original.get_value(), "old");
  EXPECT_EQ(original.get_cur_tranc_id(), 3u);
  EXPECT_EQ(added.get_value(), "new");
  EXPECT_EQ(added.get_cur_tranc_id(), 5u);
  EXPECT_EQ(removed.get_value(), "");
  EXPECT_EQ(removed.get_cur_tranc_id(), 5u);
}

// 目的：撤销读未提交事务的更新或删除后，应重新读到原记录及其真实版本号。
// 场景：k@3=old，事务写入 k@10=new 或墓碑，再 abort；分别覆盖内存与 SST。
//       只重写 k@3 无法遮住 k@10；写一个 k@10 的墓碑也会错误遮住原值。
TEST_F(TranAbortTest, ReadUncommittedRestoresPreviousVersionInMemoryAndSst) {
  uint64_t id = 10;
  for (bool flushed : {false, true}) {
    for (bool deleted : {false, true}) {
      SCOPED_TRACE(flushed);
      SCOPED_TRACE(deleted);
      const auto key = "k-" + std::to_string(id);
      engine->put(key, "old", 3);
      TranContext context(id, engine, manager,
                          IsolationLevel::READ_UNOP_COMMITTED);
      if (deleted) {
        context.remove(key);
      } else {
        context.put(key, "first");
        context.put(key, "last");
      }
      if (flushed)
        engine->flush();

      ASSERT_TRUE(context.abort());
      const auto original =
          std::make_optional(std::make_pair(std::string("old"), uint64_t{3}));
      EXPECT_EQ(engine->get(key, 0), original);
      EXPECT_EQ(engine->get(key, id), original);
      EXPECT_EQ(engine->get(key, 3), original);
      ++id;
    }
  }
}

// 目的：取消事务 5 不能改掉事务 6 的提交结果，也不能让旧快照继续看到已取消的 5。
// 场景：k@3=old -> 未提交 k@5=dirty -> 已提交 k@6=newest -> abort(5)。
//       最新查询应得到 6，读上限 5 应退回 3；只检查最新值会漏掉后一种错误。
TEST_F(TranAbortTest, KeepsNewerCommitButHidesAbortedVersionFromOlderSnapshot) {
  engine->put("k", "old", 3);
  TranContext older(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  older.put("k", "dirty");
  engine->flush();
  TranContext newer(6, engine, manager, IsolationLevel::READ_OP_COMMITTED);
  newer.put("k", "newest");
  ASSERT_TRUE(newer.commit());

  ASSERT_TRUE(older.abort());
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("newest"), uint64_t{6})));
  EXPECT_EQ(engine->get("k", 5),
            std::make_optional(std::make_pair(std::string("old"), uint64_t{3})));
}

// 目的：已提交的事务不能再被改成已取消，也不能撤销已经提交的数据。
// 场景：读未提交事务新增 k 并 commit，随后误调用 abort，应返回 false 且无副作用。
TEST_F(TranAbortTest, CannotAbortAnAlreadyCommittedTransaction) {
  TranContext context(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  context.put("k", "committed");
  ASSERT_TRUE(context.commit());
  EXPECT_FALSE(context.abort());
  EXPECT_TRUE(context.isCommited);
  EXPECT_FALSE(context.isAborted);
  EXPECT_FALSE(manager->is_aborted(context.tranc_id_));
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("committed"), uint64_t{5})));
}

// 目的：正常提交应用最后一次暂存值、墓碑和事务标记，重复提交没有副作用。
// 场景：分别走四种隔离级别；RU 已提前写入，其余级别在 commit 前不可见。
TEST_F(TranCommitTest, AppliesWritesAndEndsOnlyOnce) {
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto key = "written-" + suffix;
    const auto removed = "deleted-" + suffix;
    const uint64_t id = 10 + static_cast<int>(level);
    SCOPED_TRACE(suffix);
    engine->put(removed, "old", 3);
    TranContext context(id, engine, manager, level);
    context.put(key, "first");
    context.put(key, "last");
    context.put(removed, "");
    if (level != IsolationLevel::READ_UNOP_COMMITTED) {
      EXPECT_FALSE(engine->get(key, 0));
      ASSERT_TRUE(engine->get(removed, 0));
    }
    ASSERT_TRUE(context.commit());
    EXPECT_TRUE(context.isCommited);
    EXPECT_FALSE(context.isAborted);
    EXPECT_EQ(engine->get(key, 0),
              std::make_optional(std::make_pair(std::string("last"), id)));
    EXPECT_FALSE(engine->get(removed, 0));
    EXPECT_EQ(engine->memtable.get_record(removed, 0),
              std::make_optional(std::make_pair(std::string(""), id)));
    // 事务标记不会作为普通点查询结果返回；用刷盘通知验证它确实存在。
    engine->flush();
    EXPECT_TRUE(manager->get_flushed_tranc_ids().contains(id));
    ASSERT_EQ(context.operations.back().getOperationType(), OperationType::OP_COMMIT);
    const auto operation_count = context.operations.size();
    EXPECT_FALSE(context.commit());
    EXPECT_EQ(context.operations.size(), operation_count);
    EXPECT_THROW(context.put("after-end", "value"), std::logic_error);
    EXPECT_THROW(context.get(key), std::logic_error);
  }
}

// 目的：内存/SST 中的新值和新墓碑都拒绝旧事务，且整个暂存集合零写入。
// 场景：三个暂存型级别各测四种冲突来源；另放一个无冲突 key，防止部分提交。
TEST_F(TranCommitTest, ConflictCancelsWithoutApplyingAnyPendingWrite) {
  uint64_t id = 10;
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    for (bool on_disk : {false, true}) {
      for (bool tombstone : {false, true}) {
        const auto key = "conflict-" + std::to_string(id);
        const auto extra = "must-not-appear-" + std::to_string(id);
        SCOPED_TRACE(key);
        const std::string existing = tombstone ? "" : "newer";
        engine->put(key, existing, id + 1);
        if (on_disk) {
          while (engine->memtable.get_total_size() > 0) {
            engine->flush();
          }
          ASSERT_FALSE(engine->memtable.get_record(key, 0));
        }
        TranContext context(id, engine, manager, level);
        context.put(key, "stale");
        context.put(extra, "pending");
        const auto operation_count = context.operations.size();
        const auto memory_size = engine->memtable.get_total_size();
        ASSERT_FALSE(context.commit());
        EXPECT_TRUE(context.isAborted);
        EXPECT_FALSE(context.isCommited);
        EXPECT_FALSE(engine->get(extra, 0));
        EXPECT_FALSE(engine->get(key, id));
        if (tombstone) {
          EXPECT_FALSE(engine->get(key, 0));
        } else {
          EXPECT_EQ(engine->get(key, 0),
                    std::make_optional(std::make_pair(existing, id + 1)));
        }
        EXPECT_EQ(engine->memtable.get_total_size(), memory_size);
        EXPECT_EQ(context.operations.size(), operation_count);
        EXPECT_FALSE(context.commit());
        EXPECT_THROW(context.put(extra, "retry"), std::logic_error);
        id += 2;
      }
    }
  }
}

// 目的：串行化分支检查已有值变化、缺失变存在、删除以及同值新版本。
// 场景：事务 10 缓存读取结果，版本 7 随后变化；不只检查“版本是否大于 10”。
TEST_F(TranCommitTest, SerialReadValidationIncludesMissingAndDeletedKeys) {
  for (int scenario = 0; scenario < 4; ++scenario) {
    const auto key = "read-" + std::to_string(scenario);
    const auto output = "output-" + std::to_string(scenario);
    SCOPED_TRACE(scenario);
    if (scenario != 1) {
      engine->put(key, "old", 3);
    }
    TranContext context(10, engine, manager, IsolationLevel::SERIALIZABLE);
    const auto first = context.get(key);
    EXPECT_EQ(first.has_value(), scenario != 1);
    context.put(output, "pending");
    engine->put(key, scenario == 2 ? "" : scenario == 3 ? "old" : "new", 7);
    ASSERT_FALSE(context.commit());
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(engine->get(output, 0));
  }
}

// 目的：RR 保持自身读缓存，但不使用 SERIALIZABLE 的读集合冲突检查。
// 场景：读 k@3 后出现 k@7，事务 10 只写另一个 key，仍允许提交。
TEST_F(TranCommitTest, RepeatableReadDoesNotValidateUnwrittenReadKeys) {
  engine->put("read", "old", 3);
  TranContext context(10, engine, manager, IsolationLevel::REPEATABLE_READ);
  EXPECT_EQ(context.get("read"), std::make_optional(std::string("old")));
  engine->put("read", "new", 7);
  context.put("output", "value");
  ASSERT_TRUE(context.commit());
  EXPECT_EQ(engine->get("output", 0),
            std::make_optional(std::make_pair(std::string("value"), uint64_t{10})));
}

// 目的：达到单表阈值会自动冻结，但没有达到总量阈值时不提前刷盘。
// 场景：小单表阈值、大总量阈值；随后显式刷盘，检查事务完成标记通知管理器。
TEST_F(TranCommitTest, FreezesTableAndRegistersBeforeFlush) {
  config->modify_lsm_per_mem_size_limit(128);
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  const std::string value(256, 'v');
  context.put("k", value);
  ASSERT_TRUE(context.commit());
  EXPECT_EQ(engine->memtable.get_cur_size(), 0u);
  EXPECT_GE(engine->memtable.get_frozen_size(), 128u);
  EXPECT_TRUE(engine->ssts.empty());
  EXPECT_FALSE(manager->get_flushed_tranc_ids().contains(5));
  engine->flush();
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  EXPECT_TRUE(manager->get_flushed_tranc_ids().contains(5));
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(value, uint64_t{5})));
}

// 目的：一次提交能连续刷出多张旧冻结表，使总量低于阈值。
// 场景：先积累三张冻结表，再调低总量阈值并提交；只调用一次 flush 无法达标。
TEST_F(TranCommitTest, FlushesMultipleTablesUntilBelowTotalLimit) {
  config->modify_lsm_per_mem_size_limit(128);
  const std::string value(180, 'v');
  for (int i = 0; i < 3; ++i) {
    engine->put("old-" + std::to_string(i), value, 1);
  }
  ASSERT_GT(engine->memtable.get_frozen_size(), 500u);
  config->modify_lsm_tol_mem_size_limit(200);
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  context.put("new", "value");
  ASSERT_TRUE(context.commit());
  EXPECT_LT(engine->memtable.get_total_size(), 200u);
  EXPECT_EQ(engine->memtable.get_frozen_size(), 0u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(engine->get("old-" + std::to_string(i), 0),
              std::make_optional(std::make_pair(value, uint64_t{1})));
  }
  EXPECT_EQ(engine->get("new", 0),
            std::make_optional(std::make_pair(std::string("value"), uint64_t{5})));
}

// 目的：刷盘编码失败会抛出异常，但不能把已经生效的提交误判为取消。
// 场景：关闭 WiscKey 的默认配置下写入超出 uint16_t 编码长度的 value，
//       检查已提交状态、内存数据保留、刷盘状态未登记以及不可重复提交。
TEST_F(TranCommitTest, FlushFailurePreservesCommittedStateAndMemory) {
  ASSERT_EQ(config->getWisckeyValueThreshold(), 0u);
  config->modify_lsm_per_mem_size_limit(128);
  config->modify_lsm_tol_mem_size_limit(256);
  const std::string value(70000, 'v');
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  context.put("k", value);
  EXPECT_THROW(context.commit(), std::length_error);
  EXPECT_TRUE(context.isCommited);
  EXPECT_FALSE(context.isAborted);
  EXPECT_FALSE(manager->get_flushed_tranc_ids().contains(5));
  EXPECT_TRUE(engine->ssts.empty());
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(value, uint64_t{5})));
  const auto operation_count = context.operations.size();
  EXPECT_FALSE(context.commit());
  EXPECT_EQ(context.operations.size(), operation_count);
}

// 目的：0 容量阈值触发刷盘后可以正常退出，避免对空 MemTable 无限循环。
TEST_F(TranCommitTest, ZeroLimitsStopAfterMemoryBecomesEmpty) {
  config->modify_lsm_per_mem_size_limit(0);
  config->modify_lsm_tol_mem_size_limit(0);
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  context.put("k", "v");
  ASSERT_TRUE(context.commit());
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  EXPECT_TRUE(manager->get_flushed_tranc_ids().contains(5));
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("v"), uint64_t{5})));
}

// 目的：尚未支持的故障注入以及缺失组件必须在应用数据前报错。
TEST_F(TranCommitTest, InvalidInputsDoNotApplyWritesOrEndTransaction) {
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  context.put("k", "v");
  const auto operation_count = context.operations.size();
  EXPECT_THROW(context.commit(true), std::logic_error);
  EXPECT_FALSE(context.isCommited);
  EXPECT_FALSE(context.isAborted);
  EXPECT_EQ(context.operations.size(), operation_count);
  EXPECT_FALSE(engine->get("k", 0));
  TranContext no_manager(6, engine, nullptr, IsolationLevel::REPEATABLE_READ);
  no_manager.put("missing-manager", "v");
  EXPECT_THROW(no_manager.commit(), std::logic_error);
  EXPECT_FALSE(engine->get("missing-manager", 0));
  TranContext no_engine(7, nullptr, manager, IsolationLevel::REPEATABLE_READ);
  EXPECT_THROW(no_engine.commit(), std::logic_error);
}

// 目的：并发提交和刷盘不形成锁顺序死锁，同 key 的新版本不会被旧事务覆盖。
// 场景：事务 5/6 同时提交，允许 5 先成功或因 6 抢先而被取消；6 必须成功。
TEST_F(TranCommitTest, ConcurrentCommitsAndFlushRespectVersionOrder) {
  TranContext older(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  TranContext newer(6, engine, manager, IsolationLevel::REPEATABLE_READ);
  older.put("k", "old");
  older.put("older-only", "value");
  newer.put("k", "new");
  std::latch start(1);
  bool older_result = false;
  bool newer_result = false;
  std::exception_ptr errors[3];
  std::thread first([&] {
    start.wait();
    try { older_result = older.commit(); }
    catch (...) { errors[0] = std::current_exception(); }
  });
  std::thread second([&] {
    start.wait();
    try { newer_result = newer.commit(); }
    catch (...) { errors[1] = std::current_exception(); }
  });
  std::thread flusher([&] {
    start.wait();
    try {
      for (int i = 0; i < 10; ++i) { engine->flush(); }
    } catch (...) { errors[2] = std::current_exception(); }
  });
  start.count_down();
  first.join();
  second.join();
  flusher.join();
  for (const auto &error : errors) {
    ASSERT_FALSE(static_cast<bool>(error));
  }
  EXPECT_TRUE(newer_result);
  EXPECT_EQ(older.isCommited, older_result);
  EXPECT_EQ(older.isAborted, !older_result);
  EXPECT_EQ(engine->get("older-only", 0).has_value(), older_result);
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("new"), uint64_t{6})));
}

TEST_F(LSMTest, TranContextTest) {
  LSM lsm(test_dir);
  {
    auto tran_ctx = lsm.begin_tran(IsolationLevel::REPEATABLE_READ);

    tran_ctx->put("key1", "value1");
    tran_ctx->put("key2", "value2");

    auto query = lsm.get("key1");
    // 事务还没有提交, 应该查不到数据
    EXPECT_FALSE(query.has_value());

    auto commit_res = tran_ctx->commit();
    EXPECT_TRUE(commit_res);

    // 事务已经提交, 应该可以查到数据
    query = lsm.get("key1");
    EXPECT_EQ(query.value(), "value1");
    query = lsm.get("key2");
    EXPECT_EQ(query.value(), "value2");

    auto tran_ctx2 = lsm.begin_tran(IsolationLevel::REPEATABLE_READ);
    tran_ctx2->put("key1", "value1");
    tran_ctx2->put("key2", "value2");

    lsm.put("key2", "value22");

    commit_res = tran_ctx2->commit();
    EXPECT_FALSE(commit_res);
  }
}

TEST_F(LSMTest, TrancIdTest) {
  // 注意是 LSMEngine 而不是 LSM
  // 因为 LSMEngine 才能手动控制事务id
  LSMEngine lsm(test_dir);

  // key00-key20 先插入, 此时事务id为1
  for (int i = 0; i < 20; i++) {
    std::ostringstream oss_key;
    oss_key << "key" << std::setw(2) << std::setfill('0') << i;
    std::string key = oss_key.str();
    lsm.put(key, "tranc1", 1);
  }
  lsm.flush();

  // key10-key10 再插入, 此时事务id为2
  for (int i = 0; i < 10; i++) {
    std::ostringstream oss_key;
    oss_key << "key" << std::setw(2) << std::setfill('0') << i;
    std::string key = oss_key.str();
    lsm.put(key, "tranc2", 2);
  }

  // 在事务id为1时进行遍历, 事务id为2的记录是不可见的
  for (int i = 0; i < 20; i++) {
    std::ostringstream oss_key;
    oss_key << "key" << std::setw(2) << std::setfill('0') << i;
    std::string key = oss_key.str();

    auto res = lsm.get(key, 1);

    EXPECT_EQ(res.value().first, "tranc1");
  }

  // 在事务id为2时进行遍历, 事务id为2的记录现在是可见的了
  for (int i = 0; i < 20; i++) {
    std::ostringstream oss_key;
    oss_key << "key" << std::setw(2) << std::setfill('0') << i;
    std::string key = oss_key.str();

    auto res = lsm.get(key, 2);
    if (i < 10) {
      EXPECT_EQ(res.value().first, "tranc2");
    } else {
      EXPECT_EQ(res.value().first, "tranc1");
    }
  }
}

TEST_F(LSMTest, Recover) {
  {
    LSM lsm(test_dir);

    lsm.put("xxx  ", "yyy");
    auto tran_ctx = lsm.begin_tran(IsolationLevel::REPEATABLE_READ);

    for (int i = 0; i < 100; i++) {
      std::ostringstream oss_key;
      std::ostringstream oss_value;
      oss_key << "key" << std::setw(2) << std::setfill('0') << i;
      oss_value << "value" << std::setw(2) << std::setfill('0') << i;
      std::string key = oss_key.str();
      std::string value = oss_value.str();

      tran_ctx->put(key, value);
    }

    // 提交事务时true表示不会真正写入
    tran_ctx->commit(true);
  }
  {
    LSM lsm(test_dir);

    for (int i = 0; i < 100; i++) {
      std::ostringstream oss_key;
      std::ostringstream oss_value;
      oss_key << "key" << std::setw(2) << std::setfill('0') << i;
      oss_value << "value" << std::setw(2) << std::setfill('0') << i;
      std::string key = oss_key.str();
      std::string value = oss_value.str();

      EXPECT_EQ(lsm.get(key).value(), value);
    }
  }
}

TEST_F(LSMTest, BigPersistence) {
  std::unordered_map<std::string, std::string> kvs;
  int num = 2000000;
  {
    LSM lsm(test_dir);
    for (int i = 0; i <= num; ++i) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      lsm.put(key, value);
      kvs[key] = value;
      if (i % 400000 == 0 && i != 0) {
        std::cout << "lsm put key at i = " << i << "......" << std::endl;
      }
      // 删除之前被10整除的key
      if (i % 10 == 0 && i != 0) {
        std::string del_key = "key" + std::to_string(i - 10);
        lsm.remove(del_key);
        kvs.erase(del_key);
      }
    }
  } // LSM destructor called here

  // Create new LSM instance
  LSM lsm(test_dir);
  for (int i = 0; i <= num; ++i) {
    std::string key = "key" + std::to_string(i);
    if (i % 400000 == 0 && i != 0) {
      std::cout << "lsm get key at i = " << i << "......" << std::endl;
    }
    if (kvs.find(key) != kvs.end()) {
      EXPECT_EQ(lsm.get(key).value(), kvs[key]);
    } else {
      EXPECT_EQ(lsm.get(key).has_value(), false);
    }
  }
}

// 伪随机，打乱vector
void random_change_vector(std::vector<int> &a) {
  std::vector<int> pri{3, 5, 7, 11, 13};
  int pri_pos = 0;
  int swap_cnt = (std::min)((int)a.size(), 50000);
  for (int i = 0; i < swap_cnt; i++) {
    int x1 = ((i * pri[pri_pos] + i * i + 19) % (a.size()));
    int x2 = ((i * i * pri[(pri_pos + 1) % 5] * pri[(pri_pos + 1) % 5] + i * i +
               31) %
              (a.size()));
    std::swap(a[x1], a[x2]);
  }
}

TEST_F(LSMTest, BigPersistence2) {
  // 固定num的后一半会被删除
  int num = 1000000;
  std::vector<int> idx1, idx2;
  idx1.reserve(num / 2 + 1), idx2.reserve(num / 2 + 1);

  for (int i = 0; i <= num; ++i) {
    if (i <= num / 2)
      idx1.push_back(i);
    else
      idx2.push_back(i);
  }

  random_change_vector(idx1);
  {
    LSM lsm(test_dir);
    for (int i : idx1) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      lsm.put(key, value);
    }
    for (int i : idx2) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      lsm.put(key, value);
    }
    random_change_vector(idx2);
    for (int i : idx2) {
      std::string key = "key" + std::to_string(i);
      lsm.remove(key);
    }
  } // LSM destructor called here

  // Create new LSM instance
  LSM lsm(test_dir);
  for (int i = 0; i <= num; ++i) {
    std::string key = "key" + std::to_string(i);
    if (i <= num / 2) {
      ASSERT_EQ(lsm.get(key).has_value(), true);
      ASSERT_EQ(lsm.get(key).value(),
                (std::string) "value" + std::to_string(i));
    } else {
      ASSERT_EQ(lsm.get(key).has_value(), false);
    }
  }
}

TEST_F(LSMTest, SmallConfigLargeDataPersistent) {
  setNoClear();

  auto &&config = const_cast<TomlConfig &>(TomlConfig::getInstance());
  // 手动设置，把size都调小一些
  config.modify_lsm_tol_mem_size_limit(98304);

  config.modify_lsm_per_mem_size_limit(4096);
  config.modify_lsm_block_size(1024);

  // 固定num的后一半会被删除
  int num = 30000;
  std::vector<int> idx1, idx2;
  idx1.reserve(num / 2 + 1), idx2.reserve(num / 2 + 1);

  for (int i = 0; i <= num; ++i) {
    if (i <= num / 2)
      idx1.push_back(i);
    else
      idx2.push_back(i);
  }

  random_change_vector(idx1);
  {
    LSM lsm(test_dir);

    for (int i : idx1) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      lsm.put(key, value);
    }
    for (int i : idx2) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      lsm.put(key, value);
    }
    random_change_vector(idx2);
    for (int i : idx2) {
      std::string key = "key" + std::to_string(i);
      lsm.remove(key);
    }
  } // LSM destructor called here

  // Create new LSM instance
  LSM lsm(test_dir);
  for (int i = 0; i <= num; ++i) {
    std::string key = "key" + std::to_string(i);
    if (i <= num / 2) {
      ASSERT_EQ(lsm.get(key).has_value(), true);
      ASSERT_EQ(lsm.get(key).value(),
                (std::string) "value" + std::to_string(i));
    } else {
      ASSERT_EQ(lsm.get(key).has_value(), false);
    }
  }
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
