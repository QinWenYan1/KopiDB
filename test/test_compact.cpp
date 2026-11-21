#include "config/config.h"
#include "consts.h"
#include "logger/logger.h"
#include "lsm/engine.h"
#include "lsm/transaction.h"
#include "sst/sst_iterator.h"
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <tuple>

using namespace ::kopidb;

class CompactTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a temporary test directory
    test_dir = "dir_test_compact_data";
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
    std::filesystem::create_directory(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
  }

  // Build a fully-wired LSMEngine + TranManager pair (mirrors LSM::LSM).
  std::pair<std::shared_ptr<LSMEngine>, std::shared_ptr<TranManager>>
  make_engine() {
    auto eng = std::make_shared<LSMEngine>(test_dir);
    auto mgr = std::make_shared<TranManager>(test_dir);
    mgr->set_engine(eng);
    eng->set_tran_manager(mgr);
    mgr->init_new_wal();
    return {eng, mgr};
  }

  std::string test_dir;
};

// Helper: write N filler keys through LSMEngine with auto-incrementing tranc
// ids so each put uses a fresh id (avoids MVCC collisions with the test keys).
static void write_filler_keys(LSMEngine &eng, TranManager &mgr, int count,
                               int start_idx = 0) {
  for (int i = start_idx; i < start_idx + count; ++i) {
    std::ostringstream k, v;
    k << "filler_" << std::setw(6) << std::setfill('0') << i;
    v << "fval_" << i;
    eng.put(k.str(), v.str(), mgr.getNextTransactionId());
  }
}

// 目的：验证普通写入和删除在正常关闭、重开后保持存在性。
// 场景：写入 100000 个补零至六位的 key（如 key000001），并执行 9999 次删除。
// 预期：重开后存活 key 仍存在，已删除及从未写入的 key 不存在。
//       本用例没有逐项比较存活 value，也没有断言 compaction 必须发生；
//       专门的合并／多版本场景由本套件其他用例验证。
TEST_F(CompactTest, Persistence) {
  std::unordered_map<std::string, std::string> kvs;
  int num = 100000;
  {
    LSM lsm(test_dir);
    for (int i = 0; i < num; ++i) {
      std::ostringstream oss_key, oss_value;
      oss_key << "key" << std::setw(6) << std::setfill('0') << i;
      oss_value << "value" << std::setw(6) << std::setfill('0') << i;
      std::string key = oss_key.str();
      std::string value = oss_value.str();
      lsm.put(key, value);
      kvs[key] = value;

      // 删除之前被10整除的key
      if (i % 10 == 0 && i != 0) {
        std::ostringstream oss_del_key;
        oss_del_key << "key" << std::setw(6) << std::setfill('0') << (i - 10);
        std::string del_key = oss_del_key.str();
        lsm.remove(del_key);
        kvs.erase(del_key);
      }
    }
  } // LSM destructor called here

  std::cout << "LSM destructor called here" << std::endl;

  // Create new LSM instance
  LSM lsm(test_dir);
  for (int i = 0; i < num; ++i) {
    std::ostringstream oss_key;
    oss_key << "key" << std::setw(6) << std::setfill('0') << i;
    std::string key = oss_key.str();
    if (kvs.find(key) != kvs.end()) {
      auto res = lsm.get(key);
      EXPECT_EQ(res.has_value(), true);
      if (!res.has_value()) {
        std::cout << "key: " << key << " not found" << std::endl;
        exit(-1);
      }
    } else {
      EXPECT_FALSE(lsm.get(key).has_value());
    }
  }

  // Query a not exist key
  EXPECT_FALSE(lsm.get("nonexistent").has_value());
}

// ──────────────────────────────────────────────────────────────────────────────
// Test: after L0→L1 compaction, all per-key MVCC versions must still be
// readable with the corresponding tranc_id.
//
// Strategy:
//   1. Shrink mem/block limits so every put flushes a new L0 SST.
//   2. Write the same key ("mvcc_key") three times with large tranc_ids
//      (V1=1000, V2=2000, V3=3000), interleaved with filler puts that use
//      auto-assigned ids (1, 2, 3...), avoiding any tranc_id collision.
//   3. Accumulate > lsm_sst_level_ratio L0 SSTs to trigger L0→L1 compaction.
//   4. Verify engine->get("mvcc_key", id) returns the correct version for
//      each snapshot, and that an id just below V1 returns nothing.
// ──────────────────────────────────────────────────────────────────────────────
TEST_F(CompactTest, MultiVersionSurvivedAfterL0Compact) {
  auto &cfg = const_cast<TomlConfig &>(TomlConfig::getInstance());
  cfg.modify_lsm_tol_mem_size_limit(1);
  cfg.modify_lsm_block_size(64);

  auto [eng, mgr] = make_engine();

  // Use large tranc_ids for MVCC versions so they don't collide with the
  // auto-incrementing ids used by filler writes (which start from 1).
  const uint64_t V1 = 1000, V2 = 2000, V3 = 3000;

  // Write mvcc_key with three explicit tranc_ids, each separated by a filler
  // put that triggers a flush so the versions land in different SSTs.
  eng->put("mvcc_key", "version_1", V1);
  write_filler_keys(*eng, *mgr, 1, 0); // flush → L0 SST

  eng->put("mvcc_key", "version_2", V2);
  write_filler_keys(*eng, *mgr, 1, 1); // flush → L0 SST

  eng->put("mvcc_key", "version_3", V3);
  write_filler_keys(*eng, *mgr, 1, 2); // flush → L0 SST

  // Push enough additional SSTs so L0 exceeds lsm_sst_level_ratio (default 4)
  // and the next flush triggers L0→L1 compaction.
  write_filler_keys(*eng, *mgr, 3, 10);

  // Flush any remaining memtable data.
  while (eng->memtable.get_total_size() > 0) {
    eng->flush();
  }

  // L1 must be non-empty (compaction happened).
  // L0 may have at most 1 SST remaining (the post-compaction flush).
  ASSERT_LT(eng->level_sst_ids[0].size(), 2u)
      << "Expected at most 1 SST remaining in L0";
  ASSERT_GT(eng->level_sst_ids[1].size(), 0u)
      << "Expected L1 to have SSTs after compaction";

  // tranc_id just below V1 → no version visible for mvcc_key.
  EXPECT_FALSE(eng->get("mvcc_key", V1 - 1).has_value())
      << "tranc_id just below V1 should see no version of mvcc_key";

  // tranc_id=V1 → version_1.
  {
    auto res = eng->get("mvcc_key", V1);
    ASSERT_TRUE(res.has_value()) << "tranc_id=V1 should find mvcc_key";
    EXPECT_EQ(res->first, "version_1")
        << "tranc_id=V1 should return version_1, got: " << res->first;
  }

  // tranc_id=V2 → version_2 (latest visible at id≤V2).
  {
    auto res = eng->get("mvcc_key", V2);
    ASSERT_TRUE(res.has_value()) << "tranc_id=V2 should find mvcc_key";
    EXPECT_EQ(res->first, "version_2")
        << "tranc_id=V2 should return version_2, got: " << res->first;
  }

  // tranc_id=V3 → version_3.
  {
    auto res = eng->get("mvcc_key", V3);
    ASSERT_TRUE(res.has_value()) << "tranc_id=V3 should find mvcc_key";
    EXPECT_EQ(res->first, "version_3")
        << "tranc_id=V3 should return version_3, got: " << res->first;
  }

  // Large tranc_id → version_3 (the latest).
  {
    auto res = eng->get("mvcc_key", V3 + 10000);
    ASSERT_TRUE(res.has_value()) << "large tranc_id should find mvcc_key";
    EXPECT_EQ(res->first, "version_3")
        << "large tranc_id should return version_3, got: " << res->first;
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// Test: versions survive a second compaction (L1→L2).
// Uses same setup but writes enough data to push L1 past lsm_sst_level_ratio.
// ──────────────────────────────────────────────────────────────────────────────
TEST_F(CompactTest, MultiVersionSurvivedAfterL1Compact) {
  auto &cfg = const_cast<TomlConfig &>(TomlConfig::getInstance());
  cfg.modify_lsm_tol_mem_size_limit(1);
  cfg.modify_lsm_block_size(64);

  auto [eng, mgr] = make_engine();

  const uint64_t V1 = 10000, V2 = 20000;

  // Write two versions of mvcc_key2.
  eng->put("mvcc_key2", "v1", V1);
  write_filler_keys(*eng, *mgr, 1, 100);

  eng->put("mvcc_key2", "v2", V2);
  write_filler_keys(*eng, *mgr, 1, 101);

  // Trigger multiple L0→L1 compactions by writing enough filler.
  // Each batch of (ratio + 1) flushes causes one L0→L1 compaction;
  // repeating it (ratio + 1) times fills L1 and triggers L1→L2.
  int ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  for (int round = 0; round < ratio + 1; ++round) {
    write_filler_keys(*eng, *mgr, ratio + 1, 200 + round * (ratio + 1));
  }

  while (eng->memtable.get_total_size() > 0) {
    eng->flush();
  }

  // After all compactions there must be data in at least L1 (or L2).
  bool has_deep_level = eng->level_sst_ids[1].size() > 0 ||
                        eng->level_sst_ids[2].size() > 0;
  ASSERT_TRUE(has_deep_level) << "Expected SSTs in L1 or L2 after compaction";

  // tranc_id just below V1 → no version visible.
  EXPECT_FALSE(eng->get("mvcc_key2", V1 - 1).has_value())
      << "tranc_id just below V1 should see no version of mvcc_key2";

  // Version V1 must still be readable.
  {
    auto res = eng->get("mvcc_key2", V1);
    ASSERT_TRUE(res.has_value()) << "tranc_id=V1 should find mvcc_key2";
    EXPECT_EQ(res->first, "v1")
        << "tranc_id=V1 should return v1, got: " << res->first;
  }

  // Version V2 must still be readable.
  {
    auto res = eng->get("mvcc_key2", V2);
    ASSERT_TRUE(res.has_value()) << "tranc_id=V2 should find mvcc_key2";
    EXPECT_EQ(res->first, "v2")
        << "tranc_id=V2 should return v2, got: " << res->first;
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// Test: tombstone survives compaction – a deleted key must not be visible after
// flushing enough data to trigger L0→L1 compaction.
// ──────────────────────────────────────────────────────────────────────────────
TEST_F(CompactTest, TombstoneSurvivedAfterL0Compact) {
  auto &cfg = const_cast<TomlConfig &>(TomlConfig::getInstance());
  cfg.modify_lsm_tol_mem_size_limit(98304);
  cfg.modify_lsm_per_mem_size_limit(4096);
  cfg.modify_lsm_block_size(1024);

  const int N = 30001;
  // Pseudo-shuffle matching random_change_vector from test_lsm.cpp
  std::vector<int> idx1, idx2;
  for (int i = 0; i <= N; ++i) {
    if (i <= N / 2)
      idx1.push_back(i);
    else
      idx2.push_back(i);
  }
  // Apply same shuffle as test_lsm.cpp random_change_vector
  {
    std::vector<int> pri{3, 5, 7, 11, 13};
    int pri_pos = 0;
    int swap_cnt = std::min((int)idx1.size(), 50000);
    for (int i = 0; i < swap_cnt; i++) {
      int x1 = ((i * pri[pri_pos] + i * i + 19) % (idx1.size()));
      int x2 = ((i * pri[(pri_pos + 1) % 5] + i * 3 + 37) % (idx1.size()));
      std::swap(idx1[x1], idx1[x2]);
      pri_pos = (pri_pos + 1) % 5;
    }
  }

  {
    LSM lsm(test_dir);
    for (int i : idx1) {
      lsm.put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    for (int i : idx2) {
      lsm.put("key" + std::to_string(i), "value" + std::to_string(i));
    }
    // Shuffle idx2 the same way before deleting
    {
      std::vector<int> pri{3, 5, 7, 11, 13};
      int pri_pos = 0;
      int swap_cnt = std::min((int)idx2.size(), 50000);
      for (int i = 0; i < swap_cnt; i++) {
        int x1 = ((i * pri[pri_pos] + i * i + 19) % (idx2.size()));
        int x2 = ((i * pri[(pri_pos + 1) % 5] + i * 3 + 37) % (idx2.size()));
        std::swap(idx2[x1], idx2[x2]);
        pri_pos = (pri_pos + 1) % 5;
      }
    }
    for (int i : idx2) {
      lsm.remove("key" + std::to_string(i));
    }
  }

  LSM lsm(test_dir);
  int fail_count = 0;
  for (int i = 0; i <= N; ++i) {
    std::string key = "key" + std::to_string(i);
    if (i <= N / 2) {
      EXPECT_TRUE(lsm.get(key).has_value())
          << "key " << key << " should exist but was not found";
    } else {
      if (lsm.get(key).has_value()) {
        if (fail_count < 5) {
          std::cout << "FAIL: deleted key " << key
                    << " returns value: " << lsm.get(key).value_or("(none)")
                    << "\n";
        }
        fail_count++;
        EXPECT_FALSE(lsm.get(key).has_value())
            << "key " << key << " was deleted but still visible";
      }
    }
  }
  if (fail_count > 0) {
    std::cout << "Total failures: " << fail_count << "\n";
  }
}

// Replicates BigPersistence2 logic with a smaller dataset to debug tombstone
// survival with default config.
TEST_F(CompactTest, TombstoneDefaultConfig) {
  // Reset config to defaults
  auto &cfg = const_cast<TomlConfig &>(TomlConfig::getInstance());
  cfg.modify_lsm_tol_mem_size_limit(67108864);  // 64MB default
  cfg.modify_lsm_per_mem_size_limit(4194304);   // 4MB default
  cfg.modify_lsm_block_size(32768);             // 32KB default

  const int num = 1000000;
  std::vector<int> idx1, idx2;
  for (int i = 0; i <= num; ++i) {
    if (i <= num / 2) idx1.push_back(i);
    else idx2.push_back(i);
  }

  // Shuffle idx1 (exact copy of random_change_vector from test_lsm.cpp)
  {
    std::vector<int> pri{3, 5, 7, 11, 13};
    int pri_pos = 0;
    int swap_cnt = std::min((int)idx1.size(), 50000);
    for (int i = 0; i < swap_cnt; i++) {
      int x1 = ((i * pri[pri_pos] + i * i + 19) % (idx1.size()));
      int x2 = ((i * i * pri[(pri_pos + 1) % 5] * pri[(pri_pos + 1) % 5] + i * i +
                 31) % (idx1.size()));
      std::swap(idx1[x1], idx1[x2]);
      // NOTE: pri_pos is NOT incremented (matches test_lsm.cpp random_change_vector)
    }
  }

  {
    LSM lsm(test_dir);
    for (int i : idx1)
      lsm.put("key" + std::to_string(i), "value" + std::to_string(i));
    for (int i : idx2)
      lsm.put("key" + std::to_string(i), "value" + std::to_string(i));
    // Shuffle idx2 then delete (exact copy of random_change_vector)
    {
      std::vector<int> pri{3, 5, 7, 11, 13};
      int pri_pos = 0;
      int swap_cnt = std::min((int)idx2.size(), 50000);
      for (int i = 0; i < swap_cnt; i++) {
        int x1 = ((i * pri[pri_pos] + i * i + 19) % (idx2.size()));
        int x2 = ((i * i * pri[(pri_pos + 1) % 5] * pri[(pri_pos + 1) % 5] + i * i +
                   31) % (idx2.size()));
        std::swap(idx2[x1], idx2[x2]);
        // NOTE: pri_pos is NOT incremented (matches test_lsm.cpp)
      }
    }
    for (int i : idx2)
      lsm.remove("key" + std::to_string(i));
  }

  // Reload and verify
  {
    LSM lsm(test_dir);
    int fail_count = 0;
    for (int i = 0; i <= num; ++i) {
      std::string key = "key" + std::to_string(i);
      if (i > num / 2) {
        if (lsm.get(key).has_value()) {
          if (fail_count < 5)
            std::cout << "FAIL: deleted key " << key
                      << " = " << lsm.get(key).value_or("?") << "\n";
          fail_count++;
        }
      }
    }
    if (fail_count > 0) std::cout << "Total failures: " << fail_count << "\n";
    EXPECT_EQ(fail_count, 0) << "Some deleted keys are still visible";
  }
}

// 用已知层布局触发公共 flush，直接读输出文件，避免查询过滤掩盖物理残留。
class A16FilterTest : public CompactTest {
protected:
  using Row = std::tuple<std::string, uint64_t, std::string>;

  static void add_input(LSMEngine &engine, size_t level, std::vector<Row> rows) {
    const auto &cfg = TomlConfig::getInstance();
    SSTBuilder builder(cfg.getLsmBlockSize(), true, engine.vlog_, cfg.getWisckeyValueThreshold());
    for (const auto &[key, version, value] : rows)
      builder.add(key, value, version);
    const auto id = engine.next_sst_id++;
    engine.ssts[id] = builder.build(id, engine.get_sst_path(id, level), engine.block_cache);
    auto &ids = engine.level_sst_ids[level];
    if (level == 0) ids.push_front(id);
    else ids.push_back(id);
    engine.cur_max_level = std::max(engine.cur_max_level, level);
  }

  void check_merge(size_t src) {
    const size_t dst = src + 1;
    const size_t ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
    ASSERT_GE(ratio, 2u);
    const std::string old(160, 'O');
    const std::vector<Row> expected{{"a", 1, old}, {"b", 1, old},
                                    {"c", 3, ""}, {"c", 1, old}, {"d", 1, old}};
    {
      auto [engine, manager] = make_engine();
      // 构造一份真实 SST 布局：两侧分别有 @2 的脏值和脏墓碑；
      // c@3 是合法删除，c@1 必须同时保留给旧快照。
      add_input(*engine, dst, {{"a", 2, ""}, {"a", 1, old}, {"b", 1, old},
                               {"c", 2, "dirty"}, {"c", 1, old}, {"d", 1, old}});
      add_input(*engine, src, {{"b", 2, "dirty"}, {"c", 3, ""}, {"d", 2, ""}});
      for (size_t i = 1; i < ratio; ++i)
        add_input(*engine, src, {{"z" + std::to_string(100000 + i), 2, "dirty"}});
      // 当 L0、L1 同时到阈值，flush 会先合并 L1→L2，再合并 L0→L1。
      if (src == 1)
        for (size_t i = 0; i < ratio; ++i)
          add_input(*engine, 0, {{"z_trigger", 2, "dirty"}});
      for (int id = 1; id <= 3; ++id) {
        ASSERT_EQ(manager->getNextTransactionId(), id);
        manager->add_ready_to_flush_tranc_id(id, id == 2 ? TransactionState::ABORTED
                                                         : TransactionState::OP_COMMITTED);
        manager->add_flushed_tranc_id(id);
      }
      engine->put("zz_flush", "t", manager->getNextTransactionId());
      engine->flush();
      ASSERT_FALSE(engine->level_sst_ids[dst].empty());
      if (src == 1) {
        EXPECT_TRUE(engine->level_sst_ids[1].empty());
        for (const auto &entry : std::filesystem::directory_iterator(test_dir))
          EXPECT_NE(entry.path().extension(), ".1");
      }
    }
    // 释放前一实例，再用新的 VLog、缓存和管理器打开现有文件。
    auto [reopened, manager] = make_engine();
    ASSERT_TRUE(manager->is_aborted(2));
    std::vector<Row> actual;
    for (auto id : reopened->level_sst_ids[dst]) {
      auto table = reopened->ssts.at(id);
      EXPECT_EQ(table->is_wisckey(), TomlConfig::getInstance().getWisckeyValueThreshold() > 0);
      for (auto it = table->begin(0, true); it.is_valid(); ++it)
        actual.emplace_back(it.key(), it.get_cur_tranc_id(), it.value());
    }
    ASSERT_EQ(actual, expected);
    EXPECT_FALSE(reopened->get("c", 0));
    auto snapshot = reopened->get("c", 1);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->first, old);
  }
};

TEST_F(A16FilterTest, FiltersBothInputsAtL0) { check_merge(0); }
TEST_F(A16FilterTest, FiltersBothDeepInputsAndProducesNoEmptyL1) { check_merge(1); }

TEST_F(A16FilterTest, ActiveVersionCanCommitAfterCompaction) {
  auto [engine, manager] = make_engine();
  auto tx = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  for (int i = 0; i < ratio; ++i) {
    tx->put("live" + std::to_string(i), "pending");
    engine->flush();
  }
  engine->put("zz_flush", "t", manager->getNextTransactionId());
  engine->flush();
  ASSERT_FALSE(engine->level_sst_ids[1].empty());
  ASSERT_FALSE(engine->get("live0", 0, true));
  ASSERT_TRUE(tx->commit());
  auto record = engine->get("live0", 0, true);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->first, "pending");
  EXPECT_EQ(record->second, tx->tranc_id_);
}

TEST_F(A16FilterTest, MissingManagerPreservesStoredVersions) {
  LSMEngine engine(test_dir);
  add_input(engine, 1, {{"k", 1, "old"}});
  const auto ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  for (int i = 0; i < ratio; ++i)
    add_input(engine, 0, {{"k", static_cast<uint64_t>(i + 2), "new"}});
  engine.put("zz_flush", "t", ratio + 2);
  engine.flush();
  std::vector<uint64_t> versions;
  for (auto id : engine.level_sst_ids[1])
    for (auto it = engine.ssts.at(id)->begin(0, true); it.is_valid(); ++it)
      versions.push_back(it.get_cur_tranc_id());
  ASSERT_EQ(versions.size(), static_cast<size_t>(ratio + 1));
  for (size_t i = 0; i < versions.size(); ++i)
    EXPECT_EQ(versions[i], versions.size() - i);
}

// RU 写完后在首次刷盘前取消。用另一笔只读提交提供真实完成标记，
// 即使没有 SST，通知也必须让检查点越过两笔已结束的事务。
TEST_F(A16FilterTest, AbortedFirstFlushNotifiesWithoutWritingSstOrVlog) {
  uint64_t cancelled_id, finished_id;
  {
    auto [engine, manager] = make_engine();
    auto cancelled = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
    cancelled_id = cancelled->tranc_id_;
    cancelled->put("large", std::string(160, 'x'));
    cancelled->remove("gone");
    ASSERT_TRUE(cancelled->abort());
    auto reader = manager->new_tranc(IsolationLevel::READ_OP_COMMITTED);
    finished_id = reader->tranc_id_;
    ASSERT_TRUE(reader->commit());
    const auto tail = engine->vlog_->tail_offset();
    ASSERT_LT(manager->get_checkpoint_tranc_id(), finished_id);
    ASSERT_EQ(engine->flush(), finished_id);
    EXPECT_EQ(manager->get_checkpoint_tranc_id(), finished_id);
    EXPECT_EQ(engine->memtable.get_total_size(), 0u);
    EXPECT_EQ(engine->vlog_->tail_offset(), tail);
    EXPECT_TRUE(engine->ssts.empty());
    for (const auto &file : std::filesystem::directory_iterator(test_dir))
      EXPECT_FALSE(file.path().filename().string().starts_with("sst_"));
  }
  auto [reopened, manager] = make_engine();
  EXPECT_TRUE(manager->is_aborted(cancelled_id));
  EXPECT_EQ(manager->get_checkpoint_tranc_id(), finished_id);
  EXPECT_FALSE(reopened->get("large", 0));
  EXPECT_FALSE(reopened->get("gone", 0));
}

TEST_F(A16FilterTest, FirstFlushWithoutManagerPreservesHistoryAndTombstone) {
  LSMEngine engine(test_dir);
  const std::string old(160, 'v');
  engine.put("k", old, 3);
  engine.remove("k", 5);
  ASSERT_EQ(engine.flush(), 5u);
  ASSERT_EQ(engine.level_sst_ids[0].size(), 1u);
  const auto id = engine.level_sst_ids[0].front();
  auto disk = SST::open(id, FileObj::open(engine.get_sst_path(id, 0), false),
                        std::make_shared<BlockCache>(16, 2), engine.vlog_);
  auto it = disk->begin(0, true);
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it.get_cur_tranc_id(), 5u);
  EXPECT_TRUE(it.value().empty());
  ++it;
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it.get_cur_tranc_id(), 3u);
  EXPECT_EQ(it.value(), old);
  EXPECT_TRUE((++it).is_end());
}

// A16 专项基准，在独立目录用 --gtest_also_run_disabled_tests 显式运行。
// 先运行 PrepareSeeds 一次，再对同一份种子重复运行 Measure0 或 Measure50。
// 旧程序期望保留全部版本；新版测量时设置 KOPIDB_A16_PRUNED=1。
namespace {
namespace fs = std::filesystem;
constexpr size_t a16_keys = 10000;

std::string a16_key(size_t i) { return "k" + std::to_string(100000 + i); }
std::string a16_value(bool newer) { return std::string(128, newer ? 'N' : 'O'); }

TEST(A16Compaction, DISABLED_PrepareSeeds) {
  fs::path committed = "a16-seeds/0", aborted = "a16-seeds/50";
  ASSERT_TRUE(fs::create_directory("a16-seeds")) << "请在新的工作目录准备种子";
  fs::create_directory(committed);
  const auto &cfg = TomlConfig::getInstance();
  auto cache = std::make_shared<BlockCache>(cfg.getLsmBlockCacheCapacity(),
                                            cfg.getLsmBlockCacheK());
  // 一张 L1 存全部旧版本；四张 L0 交错存放全部新版本，key 范围重叠。
  // 两种负载复用完全相同的 SST，唯一差异是事务状态文件。
  for (size_t shard = 0; shard <= 4; ++shard) {
    const bool newer = shard != 0;
    SSTBuilder builder(cfg.getLsmBlockSize(), true);
    for (size_t i = newer ? shard - 1 : 0; i < a16_keys; i += newer ? 4 : 1)
      builder.add(a16_key(i), a16_value(newer), 2 * i + (newer ? 2 : 1));
    const auto id = shard + 1;
    const auto file = "sst_" + std::to_string(id) + (newer ? ".0" : ".1");
    builder.build(id, (committed / file).string(), cache);
  }
  fs::copy(committed, aborted, fs::copy_options::recursive);
  for (const auto &dir : {committed, aborted}) {
    TranManager manager(dir.string());
    for (size_t n = 0; n < 2 * a16_keys; ++n) {
      const auto id = manager.getNextTransactionId();
      ASSERT_EQ(id, n + 1);
      const bool skip = dir == aborted && id % 2 == 0;
      manager.add_ready_to_flush_tranc_id(id, skip ? TransactionState::ABORTED
                                                   : TransactionState::OP_COMMITTED);
      manager.add_flushed_tranc_id(id);
    }
    manager.write_tranc_id_file();
  }
}

void measure_a16(const fs::path &dir, int percent) {
  const auto seed = fs::path("a16-seeds") / std::to_string(percent);
  ASSERT_TRUE(fs::exists(seed / "tranc_id")) << "请先运行 DISABLED_PrepareSeeds";
  fs::copy(seed, dir, fs::copy_options::recursive);
  auto engine = std::make_shared<LSMEngine>(dir.string());
  auto manager = std::make_shared<TranManager>(dir.string());
  engine->set_tran_manager(manager);
  ASSERT_EQ(engine->level_sst_ids[0].size(), 4u);
  ASSERT_EQ(engine->level_sst_ids[1].size(), 1u);
  ASSERT_EQ(TomlConfig::getInstance().getLsmSstLevelRatio(), 4);
  ASSERT_EQ(TomlConfig::getInstance().getWisckeyValueThreshold(), 0u);
  ASSERT_EQ(engine->put("zz-trigger", "trigger", manager->getNextTransactionId()), 0u);

  // 在同步日志的两个合并标记处取单调时钟，不把随后单独生成的 L0 计入。
  using Clock = std::chrono::steady_clock;
  Clock::time_point start, finish;
  int starts = 0, finishes = 0;
  auto logger = spdlog::default_logger();
  logger->sinks().push_back(std::make_shared<spdlog::sinks::callback_sink_st>(
      [&](const spdlog::details::log_msg &msg) {
        std::string_view text(msg.payload.data(), msg.payload.size());
        if (text == "LSMEngine--Compaction: Starting full compaction from level0 to level1") {
          start = Clock::now();
          ++starts;
        } else if (text == "LSMEngine--Compaction: Finished compaction. New SSTs added at level1") {
          finish = Clock::now();
          ++finishes;
        }
      }));
  EXPECT_NO_THROW(engine->flush());
  logger->sinks().pop_back();
  ASSERT_EQ(starts, 1);
  ASSERT_EQ(finishes, 1);
  ASSERT_GT(finish, start);

  const char *mode = std::getenv("KOPIDB_A16_PRUNED");
  const bool prune = mode && std::string_view(mode) == "1" && percent == 50;
  const auto &outputs = engine->level_sst_ids[1];
  ASSERT_FALSE(outputs.empty());
  std::vector<bool> seen(2 * a16_keys + 1, false);
  std::vector<uint64_t> sample;
  size_t records = 0;
  uintmax_t bytes = 0;
  auto cache = std::make_shared<BlockCache>(16, 2);
  for (auto id : outputs) {
    const auto path = engine->get_sst_path(id, 1);
    bytes += fs::file_size(path);
    auto disk = SST::open(id, FileObj::open(path, false), cache);
    for (auto it = disk->begin(0, true); it.is_valid(); ++it) {
      const auto version = it.get_cur_tranc_id();
      ASSERT_GE(version, 1u);
      ASSERT_LT(version, seen.size());
      ASSERT_FALSE(seen[version]);
      seen[version] = true;
      ++records;
      const auto key_index = (version - 1) / 2;
      ASSERT_EQ(it.key(), a16_key(key_index));
      ASSERT_EQ(it.value(), a16_value(version % 2 == 0));
      if (key_index == 2) sample.push_back(version);
    }
  }
  for (size_t i = 0; i < a16_keys; ++i) {
    ASSERT_TRUE(seen[2*i+1]);
    ASSERT_EQ(seen[2*i+2], !prune);
  }
  const auto key = a16_key(2);
  ASSERT_EQ(manager->is_aborted(6), percent == 50);
  auto visible = engine->get(key, 0);
  ASSERT_TRUE(visible);
  ASSERT_EQ(visible->second, percent == 50 ? 5u : 6u);
  ASSERT_EQ(visible->first, a16_value(percent == 0));
  auto old = engine->get(key, 5);
  ASSERT_TRUE(old);
  ASSERT_EQ(old->second, 5u);
  ASSERT_EQ(sample, prune ? std::vector<uint64_t>{5} : (std::vector<uint64_t>{6, 5}));
  // 先用一遍预热，再对同一序列连续计时 25 遍。只拉长采样窗口，
  // 保持数据集、读缓存和每遍的访问顺序与原先的 10,000 次基线一致。
  constexpr size_t timed_passes = 25;
  for (bool hot : {true, false}) {
    std::vector<std::string> queries;
    queries.reserve(a16_keys);
    uint64_t expected_sum = 0;
    for (size_t i = 0; i < a16_keys; ++i) {
      // 7919 与 10000 互质，因而离散查询恰好访问每个 key 一次。
      const size_t k = hot ? 2 : (7919 * i) % a16_keys;
      queries.push_back(a16_key(k));
      expected_sum += 2 * k + (percent == 50 ? 1 : 2);
    }
    auto read_all = [&]() {
      uint64_t sum = 0;
      for (const auto &q : queries) {
        const auto result = engine->get(q, 0);
        if (result) sum += result->second;
      }
      return sum;
    };
    ASSERT_EQ(read_all(), expected_sum);
    const auto query_start = Clock::now();
    uint64_t sum = 0;
    for (size_t pass = 0; pass < timed_passes; ++pass)
      sum += read_all();
    const auto query_end = Clock::now();
    ASSERT_EQ(sum, expected_sum * timed_passes);
    const size_t calls = queries.size() * timed_passes;
    const auto us = std::chrono::duration<double, std::micro>(query_end - query_start);
    std::cout << "A16_GET percent=" << percent << " workload=" << (hot ? "hot" : "all")
              << " calls=" << calls << " us_per_get=" << std::fixed
              << std::setprecision(6) << us.count() / calls
              << " elapsed_ms=" << us.count() / 1000.0 << '\n';
  }
  std::cout << "A16 percent=" << percent << " records=" << records << " bytes=" << bytes
            << " files=" << outputs.size() << " sample=" << (prune ? "5" : "6,5")
            << " visible=" << visible->second << " ms=" << std::fixed << std::setprecision(6)
            << std::chrono::duration<double, std::milli>(finish - start).count() << '\n';
}
} // namespace

// 首次刷盘对照：输入全部放在同一张 MemTable，L0 起初为空，确保没有合并。
// 准备记录和登记事务状态都在计时外；只测公共 flush()，随后读取物理输出。
static void measure_a16_first_flush(const std::string &dir, bool abort_new) {
  auto engine = std::make_shared<LSMEngine>(dir);
  auto manager = std::make_shared<TranManager>(dir);
  engine->set_tran_manager(manager);
  const auto threshold = TomlConfig::getInstance().getWisckeyValueThreshold();
  ASSERT_TRUE(threshold == 0 || threshold == 64);
  for (size_t i = 0; i < a16_keys; ++i) {
    for (bool newer : {false, true}) {
      auto id = manager->getNextTransactionId();
      engine->memtable.put(a16_key(i), a16_value(newer), id);
      manager->add_ready_to_flush_tranc_id(id, newer && abort_new
          ? TransactionState::ABORTED : TransactionState::OP_COMMITTED);
    }
  }
  ASSERT_EQ(engine->memtable.get_frozen_size(), 0u);
  const auto before = engine->vlog_->tail_offset();
  const auto start = std::chrono::steady_clock::now();
  engine->flush();
  const auto end = std::chrono::steady_clock::now();
  ASSERT_EQ(engine->memtable.get_total_size(), 0u);
  ASSERT_EQ(engine->level_sst_ids[0].size(), 1u);
  const auto id = engine->level_sst_ids[0].front();
  const auto path = engine->get_sst_path(id, 0);
  auto sst = SST::open(id, FileObj::open(path, false), std::make_shared<BlockCache>(16, 2),
                       engine->vlog_);
  const char *mode = std::getenv("KOPIDB_A16_FLUSH_PRUNED");
  const bool pruned = abort_new && mode && std::string_view(mode) == "1";
  auto it = sst->begin(0, true);
  size_t rows = 0;
  for (size_t i = 0; i < a16_keys; ++i) {
    for (bool newer : {true, false}) {
      if (pruned && newer) continue;
      ASSERT_TRUE(it.is_valid());
      ASSERT_EQ(it.key(), a16_key(i));
      ASSERT_EQ(it.get_cur_tranc_id(), 2*i + (newer ? 2 : 1));
      ASSERT_EQ(it.value(), a16_value(newer));
      ++it;
      ++rows;
    }
  }
  ASSERT_TRUE(it.is_end());
  std::cout << "A16_FLUSH threshold=" << threshold << " percent=" << (abort_new ? 50 : 0)
            << " rows=" << rows << " bytes=" << std::filesystem::file_size(path)
            << " vlog=" << engine->vlog_->tail_offset() - before << " ms="
            << std::fixed << std::setprecision(6)
            << std::chrono::duration<double, std::milli>(end - start).count() << '\n';
}

TEST_F(CompactTest, DISABLED_A16FirstFlush0) { measure_a16_first_flush(test_dir, false); }
TEST_F(CompactTest, DISABLED_A16FirstFlush50) { measure_a16_first_flush(test_dir, true); }

TEST_F(CompactTest, DISABLED_A16Measure0) { measure_a16(test_dir, 0); }
TEST_F(CompactTest, DISABLED_A16Measure50) { measure_a16(test_dir, 50); }

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
