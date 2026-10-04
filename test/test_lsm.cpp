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

// 目的：排序使用真实版本号，而不依赖子迭代器 keep_all_versions 的设置。
// 场景：每路只有一条记录，真实版本分别为 9、5，读上限同为 10。
TEST(IteratorMvccTest, TwoMergeOrdersByActualVersionNotReadBound) {
  auto a = make_mvcc_heap({{"k", "new", 9}}, 10, false);
  auto b = make_mvcc_heap({{"k", "old", 5}}, 10, false);
  TwoMergeIterator it(a, b, 0, true);
  expect_mvcc_records(it, {{"k", "new", 9}, {"k", "old", 5}});
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
