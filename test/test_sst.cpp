#include "config/config.h"
#include "consts.h"
#include "logger/logger.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <stdexcept>
#include <tuple>

using namespace ::tiny_lsm;

class SSTTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 创建测试目录
    if (!std::filesystem::exists("test_data")) {
      std::filesystem::create_directory("test_data");
    }
  }

  void TearDown() override {
    // 清理测试文件
    std::filesystem::remove_all("test_data");
  }

  // 辅助函数：创建一个包含有序数据的SST
  std::shared_ptr<SST> create_test_sst(size_t block_size, size_t num_entries) {
    SSTBuilder builder(block_size, true);

    for (size_t i = 0; i < num_entries; i++) {
      std::string key = "key" + std::to_string(i);
      std::string value = "value" + std::to_string(i);
      builder.add(key, value, 0);
    }

    auto block_cache = std::make_shared<BlockCache>(
        TomlConfig::getInstance().getLsmBlockCacheCapacity(),
        TomlConfig::getInstance().getLsmBlockCacheK());

    return builder.build(1, "test_data/test.sst", block_cache);
  }
};

// 测试基本的写入和读取
TEST_F(SSTTest, BasicWriteAndRead) {
  SSTBuilder builder(1024, true); // 1KB block size
  auto block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 添加一些数据
  builder.add("key1", "value1", 0);
  builder.add("key2", "value2", 0);
  builder.add("key3", "value3", 0);

  // 构建SST
  auto sst = builder.build(1, "test_data/basic.sst", block_cache);

  // 验证基本属性
  EXPECT_EQ(sst->get_first_key(), "key1");
  EXPECT_EQ(sst->get_last_key(), "key3");
  EXPECT_EQ(sst->get_sst_id(), 1);
  EXPECT_GT(sst->sst_size(), 0);

  // 读取并验证数据
  auto block = sst->read_block(0);
  EXPECT_TRUE(block != nullptr);
  auto value = block->get_value_binary("key2", 0);
  EXPECT_TRUE(value.has_value());
  EXPECT_EQ(*value, "value2");
}

// 测试block分裂
TEST_F(SSTTest, BlockSplitting) {
  // 使用小的block size强制分裂
  SSTBuilder builder(64, true); // 很小的block size
  auto block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 添加足够多的数据以触发分裂
  for (int i = 0; i < 10; i++) {
    std::string key = "key" + std::to_string(i);
    std::string value = std::string(20, 'v') + std::to_string(i); // 较大的value
    builder.add(key, value, 0);
  }

  auto sst = builder.build(1, "test_data/split.sst", block_cache);

  // 验证有多个block
  EXPECT_GT(sst->num_blocks(), 1);

  // 验证每个block都可以正确读取
  for (size_t i = 0; i < sst->num_blocks(); i++) {
    auto block = sst->read_block(i);
    EXPECT_TRUE(block != nullptr);
  }
}

// 测试key查找
TEST_F(SSTTest, KeySearch) {
  auto sst = create_test_sst(256, 100); // 创建包含100个entry的SST

  // 测试find_block_idx
  int64_t idx = sst->find_block_idx("key50");
  auto block = sst->read_block(idx);
  auto value = block->get_value_binary("key50", 0);
  EXPECT_TRUE(value.has_value());
  EXPECT_EQ(*value, "value50");

  // 测试边界情况
  EXPECT_EQ(sst->find_block_idx("key999"), -1);
}

// 测试元数据
TEST_F(SSTTest, Metadata) {
  auto sst = create_test_sst(512, 10);

  // 验证block数量
  EXPECT_GT(sst->num_blocks(), 0);

  // 验证首尾key
  EXPECT_EQ(sst->get_first_key(), "key0");
  EXPECT_EQ(sst->get_last_key(), "key9");
}

// 测试空SST构建
TEST_F(SSTTest, EmptySST) {
  SSTBuilder builder(1024, true);
  auto block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());
  EXPECT_THROW(builder.build(1, "test_data/empty.sst", block_cache),
               std::runtime_error);
}

// 测试SST重新打开
TEST_F(SSTTest, ReopenSST) {
  // 首先创建一个SST
  auto sst = create_test_sst(256, 10);
  auto block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 重新打开SST
  FileObj file = FileObj::open("test_data/test.sst", false);
  auto reopened_sst = SST::open(1, std::move(file), block_cache);

  // 验证数据一致性
  EXPECT_EQ(sst->get_first_key(), reopened_sst->get_first_key());
  EXPECT_EQ(sst->get_last_key(), reopened_sst->get_last_key());
  EXPECT_EQ(sst->num_blocks(), reopened_sst->num_blocks());

  // 重新打开后通过点查询读数据，验证恢复出的 Bloom 不会漏掉已有 key。
  for (int i = 0; i < 10; ++i) {
    const auto key = "key" + std::to_string(i);
    auto it = reopened_sst->get(key, 0);
    ASSERT_TRUE(it.is_valid()) << key;
    EXPECT_EQ(it->first, key);
    EXPECT_EQ(it->second, "value" + std::to_string(i));
  }
}

TEST_F(SSTTest, EndDoesNotReadBlocks) {
  SSTBuilder builder(1024, true);
  builder.add("a", "value-a", 0);
  builder.add("z", "value-z", 0);
  auto cache = std::make_shared<BlockCache>(2, 2);
  auto sst = builder.build(1, "test_data/end.sst", cache);

  // build 不读取数据块，缓存初始为空。
  // 创建结束迭代器也不应读取首块并把它放进缓存。
  auto end = sst->end();
  EXPECT_TRUE(end.is_end());
  EXPECT_FALSE(end.is_valid());
  EXPECT_EQ(cache->get(1, 0), nullptr);

  // SST::get 对范围外的 key 应直接返回关联当前 SST 的 end，
  // 不读取块，也无需调用逐版本的回滚判断。
  bool callback_called = false;
  const auto is_aborted = [&callback_called](uint64_t) {
    callback_called = true;
    return false;
  };
  for (const std::string key : {"", "zz"}) {
    auto missing = sst->get(key, 0, is_aborted);
    EXPECT_TRUE(missing.is_end());
    EXPECT_TRUE(missing == end);
  }
  EXPECT_FALSE(callback_called);
  EXPECT_EQ(cache->get(1, 0), nullptr);

  // 结束迭代器仍须关联当前 SST，并与正常遍历耗尽后的状态相等。
  auto it = sst->begin(0);
  ASSERT_TRUE(it.is_valid());
  ++it;
  ASSERT_TRUE(it.is_valid());
  ++it;
  EXPECT_TRUE(it == end);
}

// 测试大文件
TEST_F(SSTTest, LargeSST) {
  SSTBuilder builder(4096, true); // 4KB blocks
  auto block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 添加大量数据
  for (int i = 0; i < 1000; i++) {
    // key格式：key000, key001, ..., key999
    std::string key = "key" + std::string(3 - std::to_string(i).length(), '0') +
                      std::to_string(i);

    // value格式：val000, val001, ..., val999
    std::string value = "val" +
                        std::string(3 - std::to_string(i).length(), '0') +
                        std::to_string(i);

    builder.add(key, value, 0);
  }

  auto sst = builder.build(1, "test_data/large.sst", block_cache);

  // 验证数据完整性
  EXPECT_GT(sst->num_blocks(), 1);
  EXPECT_EQ(sst->get_first_key(), "key000");
  EXPECT_EQ(sst->get_last_key(), "key999");

  // 随机访问一些key
  std::vector<int> test_indices = {0, 100, 500, 999};
  for (int i : test_indices) {
    std::string key = "key" + std::string(3 - std::to_string(i).length(), '0') +
                      std::to_string(i);
    int64_t idx = sst->find_block_idx(key);
    auto block = sst->read_block(idx);
    auto value = block->get_value_binary(key, 0);
    EXPECT_TRUE(value.has_value());

    // 构造期望的value
    std::string expected_value =
        "val" + std::string(3 - std::to_string(i).length(), '0') +
        std::to_string(i);
    EXPECT_EQ(*value, expected_value);
  }
}

TEST_F(SSTTest, LargeSSTPredicate) {
  SSTBuilder builder(4096, true); // 4KB blocks
  auto block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 添加大量数据
  for (int i = 0; i < 1000; i++) {
    // key格式：key000, key001, ..., key999
    std::string key = "key" + std::string(3 - std::to_string(i).length(), '0') +
                      std::to_string(i);

    // value格式：val000, val001, ..., val999
    std::string value = "val" +
                        std::string(3 - std::to_string(i).length(), '0') +
                        std::to_string(i);

    builder.add(key, value, 0);
  }

  auto sst = builder.build(1, "test_data/large.sst", block_cache);

  auto result =
      sst_iters_monotony_predicate(sst, 0, [](const std::string &key) {
        if (key < "key300") {
          return 1;
          ;
        }
        if (key > "key500") {
          return -1;
          ;
        }
        return 0;
        // return key >= "key300" && key <= "key500";
      });
  EXPECT_TRUE(result.has_value());
  auto [iter_begin, iter_end] = result.value();
  EXPECT_EQ(iter_begin.key(), "key300");
  for (int i = 0; i < 100; i++) {
    ++iter_begin;
  }
  EXPECT_EQ(iter_begin.key(), "key400");
  EXPECT_EQ(iter_end.key(), "key501");
}

// 目的：同 key 的版本不能被拆到不同块，重新打开 SST 后仍能按快照查询。
// 场景：k@15 写入、k@9 删除、k@5 旧值；另有超过 32 位的事务 ID。
TEST_F(SSTTest, MvccReopenPreservesVersionsTombstonesAndIdRange) {
  const uint64_t large_id = (uint64_t{1} << 40) + 7;
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), large_id);
  builder.add("k", "new", 15);
  builder.add("k", "", 9);
  builder.add("k", "old", 5);
  builder.add("z", std::string(24, 'z'), 2);
  auto built = builder.build(1, "test_data/mvcc.sst",
                            std::make_shared<BlockCache>(8, 2));

  // 用新缓存重新打开，确保验证的是持久化数据，而非旧的缓存块。
  auto reopened = SST::open(1, FileObj::open("test_data/mvcc.sst", false),
                           std::make_shared<BlockCache>(8, 2));
  for (const auto &sst : {built, reopened}) {
    ASSERT_EQ(sst->num_blocks(), 3);
    EXPECT_EQ(sst->get_tranc_id_range().first, 2);
    EXPECT_EQ(sst->get_tranc_id_range().second, large_id);
    auto block_idx = sst->find_block_idx("k");
    ASSERT_GE(block_idx, 0);
    EXPECT_EQ(sst->read_block(block_idx)->size(), 3);

    const std::vector<std::tuple<uint64_t, uint64_t, std::string>> cases = {
        {0, 15, "new"}, {15, 15, "new"}, {14, 9, ""},
        {9, 9, ""}, {8, 5, "old"}, {5, 5, "old"}};
    for (const auto &[read_id, expected_id, expected_value] : cases) {
      SCOPED_TRACE(read_id);
      auto it = sst->get("k", read_id);
      ASSERT_TRUE(it.is_valid());
      EXPECT_EQ(it->first, "k");
      EXPECT_EQ(it->second, expected_value);
      EXPECT_EQ(it.get_cur_tranc_id(), expected_id);
    }
    EXPECT_TRUE(sst->get("k", 4) == sst->end());
    EXPECT_TRUE(sst->get("missing", 0) == sst->end());
    auto large = sst->get("a", 0);
    ASSERT_TRUE(large.is_valid());
    EXPECT_EQ(large.get_cur_tranc_id(), large_id);
  }
}

// 目的：begin 和 ++ 都能连续跳过整块不可见记录，且保留可见墓碑。
// 场景：首块 a、中间 c/d、末块 f 不可见；b 有两个可见版本，e 是墓碑。
TEST_F(SSTTest, MvccIterationSkipsInvisibleBlocksAndPreservesVersions) {
  SSTBuilder builder(32, true);
  const std::string value(24, 'v');
  builder.add("a", value, 20);
  builder.add("b", value, 12);
  builder.add("b", value, 7);
  builder.add("b", value, 3);
  builder.add("c", value, 19);
  builder.add("d", value, 18);
  builder.add("e", "", 8);
  builder.add("e", value, 2);
  builder.add("f", value, 17);
  auto sst = builder.build(1, "test_data/iterate-mvcc.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 6);

  using Record = std::tuple<std::string, std::string, uint64_t>;
  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    const std::vector<Record> expected = keep_all
        ? std::vector<Record>{{"b", value, 7}, {"b", value, 3},
                              {"e", "", 8}, {"e", value, 2}}
        : std::vector<Record>{{"b", value, 7}, {"e", "", 8}};
    auto it = sst->begin(8, keep_all);
    for (const auto &[key, expected_value, id] : expected) {
      ASSERT_TRUE(it.is_valid());
      EXPECT_EQ(it->first, key);
      EXPECT_EQ(it->second, expected_value);
      EXPECT_EQ(it.get_cur_tranc_id(), id);
      ++it;
    }
    EXPECT_TRUE(it.is_end());
    EXPECT_TRUE(it == sst->end());
  }
  EXPECT_TRUE(sst->begin(1) == sst->end());
}

// 目的：SST 合并按真实版本决胜，且不能提前删除墓碑、复活其他表的旧值。
// 场景：SST 1 有 k@9 墓碑，SST 2 有 k@5 旧值；文件 ID 顺序与版本顺序相反。
TEST_F(SSTTest, MvccMergeChoosesVisibleVersionAndRetainsTombstone) {
  auto cache = std::make_shared<BlockCache>(8, 2);
  SSTBuilder first(128, true);
  first.add("k", "", 9);
  auto tombstone_sst = first.build(1, "test_data/tombstone.sst", cache);
  SSTBuilder second(128, true);
  second.add("k", "old", 5);
  auto old_sst = second.build(2, "test_data/old.sst", cache);

  for (uint64_t read_id : {uint64_t{0}, uint64_t{8}}) {
    SCOPED_TRACE(read_id);
    auto range = SstIterator::merge_sst_iterator(
        {tombstone_sst->begin(read_id), old_sst->begin(read_id)}, read_id);
    auto &it = range.first;
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it->first, "k");
    EXPECT_EQ(it->second, read_id == 0 ? "" : "old");
    EXPECT_EQ(it.get_cur_tranc_id(), read_id == 0 ? 9 : 5);
    ++it;
    EXPECT_TRUE(it.is_end());
  }

  auto all = SstIterator::merge_sst_iterator(
      {tombstone_sst->begin(0, true), old_sst->begin(0, true)}, 0, true);
  for (uint64_t id : {uint64_t{9}, uint64_t{5}}) {
    ASSERT_TRUE(all.first.is_valid());
    EXPECT_EQ(all.first.get_cur_tranc_id(), id);
    ++all.first;
  }
  EXPECT_TRUE(all.first.is_end());
}

// 目的：范围内的首块全部不可见时，begin 应定位到后续块的首条可见记录。
// 场景：a@20 不可见、b@5 可见，查询 [a, b]，读上限为 8。
TEST_F(SSTTest, MvccPredicateSkipsInvisibleLeadingBlock) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 20);
  builder.add("b", std::string(24, 'b'), 5);
  auto sst = builder.build(1, "test_data/range-start.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 2);

  auto range = sst_iters_monotony_predicate(
      sst, 8, [](const std::string &key) {
        if (key < "a") return 1;
        if (key > "b") return -1;
        return 0;
      });
  ASSERT_TRUE(range.has_value());
  auto &[begin, end] = *range;
  ASSERT_TRUE(begin.is_valid());
  EXPECT_EQ(begin->first, "b");
  EXPECT_EQ(begin.get_cur_tranc_id(), 5);
  ++begin;
  EXPECT_TRUE(begin == end);
}

// 目的：范围终点必须能被正常 ++ 到达，不能停留在中间块的尾后位置。
// 场景：a@5 可见，范围内的 b@20 不可见，下一块 c@5 已在范围外。
TEST_F(SSTTest, MvccPredicateStopsAtBlockBoundary) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 5);
  builder.add("b", std::string(24, 'b'), 20);
  builder.add("c", std::string(24, 'c'), 5);
  auto sst = builder.build(1, "test_data/range-end.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 3);

  auto range = sst_iters_monotony_predicate(
      sst, 8, [](const std::string &key) {
        if (key < "a") return 1;
        if (key > "b") return -1;
        return 0;
      });
  ASSERT_TRUE(range.has_value());
  auto &[begin, end] = *range;
  ASSERT_TRUE(begin.is_valid());
  EXPECT_EQ(begin->first, "a");
  ++begin;
  // 正确结果只有 a；一次 ++ 后必须到达该范围的 end，而不是继续产出 c。
  EXPECT_TRUE(begin == end);
}

// 目的：全部命中记录都不可见时，必须返回 nullopt 或 begin == end 的空区间。
// 场景：最后一个块仅有 a@20，读上限为 8；不能返回无法解引用的非空区间。
TEST_F(SSTTest, MvccPredicateAllInvisibleReturnsEmptyRange) {
  SSTBuilder builder(32, true);
  builder.add("a", "new", 20);
  auto sst = builder.build(1, "test_data/range-empty.sst",
                          std::make_shared<BlockCache>(8, 2));
  auto range = sst_iters_monotony_predicate(
      sst, 8, [](const std::string &) { return 0; });
  if (range.has_value()) {
    EXPECT_TRUE(range->first == range->second);
  }
}

// 目的：同一迭代器重新 seek 后，* 和 -> 必须返回新位置的内容。
// 场景：先读取 a 填充缓存，再 seek 到 b；b@12 不可见，应读到 b@5。
TEST_F(SSTTest, MvccSeekRefreshesCachedValue) {
  SSTBuilder builder(128, true);
  builder.add("a", "value-a", 3);
  builder.add("b", "new-b", 12);
  builder.add("b", "visible-b", 5);
  auto sst = builder.build(1, "test_data/seek-cache.sst",
                          std::make_shared<BlockCache>(8, 2));
  auto it = sst->begin(8);
  ASSERT_TRUE(it.is_valid());
  ASSERT_EQ(it->first, "a"); // 触发 cached_value 填充。

  it.seek("b");
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it.key(), "b");
  EXPECT_EQ(it.get_cur_tranc_id(), 5);
  EXPECT_EQ(it->first, "b");
  EXPECT_EQ(it->second, "visible-b");
  EXPECT_EQ((*it).first, "b");
}

// 目的：SST::get、seek 和按 key 构造均将回滚判断、读取上限传入块内查询，保留有效墓碑。
// 场景：重新打开 SST；k@11、k@7 已取消，k@9 是有效墓碑，k@5 是旧值。
// 预期：上限 >= 9 或 0 返回墓碑@9，上限 5/8 返回 old@5；两种遍历模式一致。
TEST_F(SSTTest, RollbackSeekSelectsVisibleVersionAndRetainsTombstone) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 1);
  builder.add("k", "cancelled-11", 11);
  builder.add("k", "", 9);
  builder.add("k", "cancelled-7", 7);
  builder.add("k", "old", 5);
  builder.add("z", std::string(24, 'z'), 1);
  auto built = builder.build(1, "test_data/rollback-seek.sst",
                            std::make_shared<BlockCache>(8, 2));
  auto sst = SST::open(1, FileObj::open("test_data/rollback-seek.sst", false),
                       std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 3);
  const auto is_aborted = [](uint64_t id) { return id == 11 || id == 7; };
  const std::vector<std::tuple<uint64_t, uint64_t, std::string>> cases = {
      {0, 9, ""}, {5, 5, "old"}, {8, 5, "old"},
      {9, 9, ""}, {10, 9, ""}, {11, 9, ""}};

  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    for (const auto &[read_id, expected_id, expected_value] : cases) {
      SCOPED_TRACE(read_id);
      SstIterator it(sst, "a", read_id, keep_all, is_aborted);
      it.seek("k");
      ASSERT_TRUE(it.is_valid());
      EXPECT_EQ(it->first, "k");
      EXPECT_EQ(it->second, expected_value);
      EXPECT_EQ(it.get_cur_tranc_id(), expected_id);

      SstIterator keyed(sst, "k", read_id, keep_all, is_aborted);
      ASSERT_TRUE(keyed.is_valid());
      EXPECT_EQ(keyed->first, "k");
      EXPECT_EQ(keyed->second, expected_value);
      EXPECT_EQ(keyed.get_cur_tranc_id(), expected_id);

      auto queried = sst->get("k", read_id, is_aborted);
      ASSERT_TRUE(queried.is_valid());
      EXPECT_EQ(queried->first, "k");
      EXPECT_EQ(queried->second, expected_value);
      EXPECT_EQ(queried.get_cur_tranc_id(), expected_id);
    }
    SstIterator historical(sst, "a", 4, keep_all, is_aborted);
    historical.seek("k");
    EXPECT_TRUE(historical.is_end());
    EXPECT_TRUE(historical == sst->end());
    SstIterator keyed_historical(sst, "k", 4, keep_all, is_aborted);
    EXPECT_TRUE(keyed_historical.is_end());
    EXPECT_TRUE(keyed_historical == sst->end());
    EXPECT_TRUE(sst->get("k", 4, is_aborted) == sst->end());
  }

  // 墓碑自身也被取消时，公开点查必须恢复更旧的有效值。
  auto old = sst->get("k", 0, [](uint64_t id) {
    return id == 11 || id == 9 || id == 7;
  });
  ASSERT_TRUE(old.is_valid());
  EXPECT_EQ(old->first, "k");
  EXPECT_EQ(old->second, "old");
  EXPECT_EQ(old.get_cur_tranc_id(), 5);
}

// 目的：重复 seek 必须清除缓存；seek 和按 key 构造在全部取消或缺失时不能返回相邻 key。
// 场景：构造时保存状态查询回调，读取 k@9 后登记取消 9，再 seek 应读到 k@5。
// 使用另一迭代器验证整个 k 被取消的情况；seek 始终沿用各自构造时的回调。
// 预期：缓存刷新、end 后可重新定位；未配置回调的独立迭代器仍能读到 k@9。
TEST_F(SSTTest, RollbackSeekRefreshesCacheAndHandlesNoMatch) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 1);
  builder.add("k", "new", 9);
  builder.add("k", "old", 5);
  builder.add("z", std::string(24, 'z'), 1);
  auto sst = builder.build(1, "test_data/rollback-seek-cache.sst",
                          std::make_shared<BlockCache>(8, 2));
  bool cancel_latest = false;
  SstIterator it(sst, "k", 0, false, [&cancel_latest](uint64_t id) {
    return cancel_latest && id == 9;
  });
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->second, "new"); // 填充缓存，后面的 seek 必须使其失效。

  // 回调没有更换；它查询的取消状态变了，重新定位必须读取最新状态。
  cancel_latest = true;
  it.seek("k");
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->second, "old");
  EXPECT_EQ(it.get_cur_tranc_id(), 5);
  SstIterator keyed_old(sst, "k", 0, false,
                        [](uint64_t id) { return id == 9; });
  ASSERT_TRUE(keyed_old.is_valid());
  EXPECT_EQ(keyed_old->first, "k");
  EXPECT_EQ(keyed_old->second, "old");
  EXPECT_EQ(keyed_old.get_cur_tranc_id(), 5);
  auto queried_old = sst->get("k", 0, [](uint64_t id) { return id == 9; });
  ASSERT_TRUE(queried_old.is_valid());
  EXPECT_EQ(queried_old->first, "k");
  EXPECT_EQ(queried_old->second, "old");
  EXPECT_EQ(queried_old.get_cur_tranc_id(), 5);

  const auto cancel_k = [](uint64_t id) { return id == 9 || id == 5; };
  SstIterator all_cancelled(sst, "a", 0, false, cancel_k);
  for (const std::string key : {"k", "missing", "", "zz"}) {
    SCOPED_TRACE(key);
    all_cancelled.seek(key);
    EXPECT_TRUE(all_cancelled.is_end());
    EXPECT_FALSE(all_cancelled.is_valid());
    EXPECT_TRUE(all_cancelled == sst->end());
    EXPECT_THROW((void)*all_cancelled, std::runtime_error);

    SstIterator keyed_missing(sst, key, 0, false, cancel_k);
    EXPECT_TRUE(keyed_missing.is_end());
    EXPECT_FALSE(keyed_missing.is_valid());
    EXPECT_TRUE(keyed_missing == sst->end());

    auto queried_missing = sst->get(key, 0, cancel_k);
    EXPECT_TRUE(queried_missing.is_end());
    EXPECT_FALSE(queried_missing.is_valid());
    EXPECT_TRUE(queried_missing == sst->end());

    // end 状态也可以重新 seek；跨块后不能残留之前缓存的 k。
    all_cancelled.seek("z");
    ASSERT_TRUE(all_cancelled.is_valid());
    EXPECT_EQ(all_cancelled->first, "z");
    EXPECT_EQ(all_cancelled->second, std::string(24, 'z'));
    EXPECT_EQ(all_cancelled.get_cur_tranc_id(), 1);
  }
  all_cancelled.seek("k");
  EXPECT_TRUE(all_cancelled.is_end());
  // 另一个迭代器的规则不会覆盖本对象构造时保存的规则。
  it.seek("k");
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->second, "old");
  EXPECT_EQ(it.get_cur_tranc_id(), 5);
  SstIterator unfiltered(sst, "k", 0);
  ASSERT_TRUE(unfiltered.is_valid());
  EXPECT_EQ(unfiltered->second, "new");
  EXPECT_EQ(unfiltered.get_cur_tranc_id(), 9);

  bool callback_called = false;
  SstIterator empty(nullptr, "k", 0, false, [&callback_called](uint64_t) {
    callback_called = true;
    return false;
  });
  empty.seek("k");
  EXPECT_TRUE(empty.is_end());
  EXPECT_FALSE(empty.is_valid());
  EXPECT_FALSE(callback_called);
  SstIterator keyed_empty(nullptr, "k", 0, false,
                          [&callback_called](uint64_t) {
                            callback_called = true;
                            return false;
                          });
  EXPECT_TRUE(keyed_empty.is_end());
  EXPECT_FALSE(keyed_empty.is_valid());
  EXPECT_FALSE(callback_called);
}

// 目的：seek 的回滚判断异常必须上报且留下统一 end 状态；构造函数和 SST::get 也不能吞掉异常。
// 场景：先填充缓存；定位中间块的 k 时，分别抛出标准异常和非标准异常；随后再 seek 到 z。
// 预期：异常未被吞掉，is_end 和与 SST::end 的比较一致，后续定位可以恢复使用。
TEST_F(SSTTest, RollbackSeekPropagatesErrorAndLeavesEndState) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 1);
  builder.add("k", "value", 9);
  builder.add("z", std::string(24, 'z'), 1);
  auto sst = builder.build(1, "test_data/rollback-seek-error.sst",
                          std::make_shared<BlockCache>(8, 2));
  for (bool standard_exception : {true, false}) {
    SCOPED_TRACE(standard_exception);
    const auto failing_check = [standard_exception](uint64_t id) -> bool {
      // a/z 的版本为 1，可以正常定位；只有查询 k@9 的状态时抛异常。
      if (id != 9) {
        return false;
      }
      if (standard_exception) {
        throw std::runtime_error("rollback lookup failed");
      }
      // 模拟回调抛出不继承 std::exception 的异常，验证 catch (...) 也会清理状态。
      throw 42;
    };
    SstIterator it(sst, "a", 0, false, failing_check);
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it->first, "a"); // 填充缓存，后续失败定位应清理它。

    if (standard_exception) {
      EXPECT_THROW(it.seek("k"), std::runtime_error);
      EXPECT_THROW((SstIterator(sst, "k", 0, false, failing_check)),
                   std::runtime_error);
      EXPECT_THROW(sst->get("k", 0, failing_check), std::runtime_error);
    } else {
      EXPECT_THROW(it.seek("k"), int);
      EXPECT_THROW((SstIterator(sst, "k", 0, false, failing_check)), int);
      EXPECT_THROW(sst->get("k", 0, failing_check), int);
    }
    EXPECT_TRUE(it.is_end());
    EXPECT_FALSE(it.is_valid());
    EXPECT_TRUE(it == sst->end());
    EXPECT_THROW((void)*it, std::runtime_error);

    it.seek("z");
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it->first, "z");
    EXPECT_EQ(it.get_cur_tranc_id(), 1);
  }
}

// 目的：回调由迭代器独立保存，调用方清空函数对象后，重复 seek 仍按原规则过滤。
// 场景：a/b 各有取消的 @9 和有效的 @5；跨 key、查询缺失后再次定位都应读到 @5。
TEST_F(SSTTest, RollbackSeekRetainsCallbackAfterCallerReset) {
  SSTBuilder builder(32, true);
  builder.add("a", "cancelled-a", 9);
  builder.add("a", "old-a", 5);
  builder.add("b", "cancelled-b", 9);
  builder.add("b", "old-b", 5);
  auto sst = builder.build(1, "test_data/seek-owned-callback.sst",
                          std::make_shared<BlockCache>(8, 2));
  std::function<bool(uint64_t)> is_aborted =
      [](uint64_t id) { return id == 9; };
  SstIterator it(sst, "a", 0, false, is_aborted);
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->second, "old-a");
  is_aborted = {};

  for (const std::string key : {"b", "a"}) {
    SCOPED_TRACE(key);
    it.seek(key);
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it->first, key);
    EXPECT_EQ(it->second, "old-" + key);
    EXPECT_EQ(it.get_cur_tranc_id(), 5);
    it.seek("missing");
    EXPECT_TRUE(it == sst->end());
  }
  it.seek("a");
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it.get_cur_tranc_id(), 5);
}

// 目的：跨块 ++ 必须沿用构造时的回滚判断，连续不可见块也应跳过。
// 场景：a 起步；b 的新墓碑取消，c/d 整块取消；e 有有效墓碑，f 有多个有效版本，g 取消。
// 同时验证普通/全版本模式、读取上限、解引用缓存和最终 end 状态。
TEST_F(SSTTest, RollbackIncrementPreservesFilterAcrossBlocks) {
  SSTBuilder builder(32, true);
  const std::string a(24, 'a'), b(24, 'b'), older_b(24, 'B');
  const std::string older_e(24, 'e'), f(24, 'f'), older_f(24, 'F');
  builder.add("a", a, 1);
  builder.add("b", "", 9);
  builder.add("b", b, 5);
  builder.add("b", older_b, 3);
  builder.add("c", std::string(24, 'c'), 9);
  builder.add("d", "", 9);
  builder.add("d", std::string(24, 'd'), 8);
  builder.add("e", "", 7);
  builder.add("e", older_e, 2);
  builder.add("f", f, 10);
  builder.add("f", older_f, 4);
  builder.add("g", std::string(24, 'g'), 9);
  auto sst = builder.build(1, "test_data/increment-rollback.sst",
                          std::make_shared<BlockCache>(8, 2));
  // 每个 key 单独占块，确保 b 及后续记录必须经由跨块分支访问。
  ASSERT_EQ(sst->num_blocks(), 7);

  using Row = std::tuple<std::string, std::string, uint64_t>;
  const std::vector<Row> latest = {
      {"a", a, 1}, {"b", b, 5}, {"e", "", 7}, {"f", f, 10}};
  const std::vector<Row> historical = {
      {"a", a, 1}, {"b", b, 5}, {"e", older_e, 2}, {"f", older_f, 4}};
  const std::vector<Row> latest_all = {
      {"a", a, 1}, {"b", b, 5}, {"b", older_b, 3}, {"e", "", 7},
      {"e", older_e, 2}, {"f", f, 10}, {"f", older_f, 4}};
  const std::vector<Row> historical_all = {
      {"a", a, 1}, {"b", b, 5}, {"b", older_b, 3},
      {"e", older_e, 2}, {"f", older_f, 4}};

  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    for (uint64_t read_id : {0, 5, 10}) {
      SCOPED_TRACE(read_id);
      std::function<bool(uint64_t)> is_aborted =
          [](uint64_t id) { return id == 9 || id == 8; };
      SstIterator it(sst, "a", read_id, keep_all, is_aborted);
      // 调用方不再保存回调，后续跨块仍须使用迭代器自己的副本。
      is_aborted = {};
      const auto &expected = keep_all
          ? (read_id == 5 ? historical_all : latest_all)
          : (read_id == 5 ? historical : latest);
      for (const auto &[key, value, version] : expected) {
        ASSERT_TRUE(it.is_valid());
        EXPECT_EQ(it->first, key);
        EXPECT_EQ(it->second, value); // 填充缓存，下一次 ++ 必须更新。
        EXPECT_EQ(it.get_cur_tranc_id(), version);
        EXPECT_EQ((*it).first, key);
        ++it;
      }
      EXPECT_TRUE(it.is_end());
      EXPECT_TRUE(it == sst->end());
      EXPECT_THROW((void)*it, std::runtime_error);
      ++it;
      EXPECT_TRUE(it == sst->end());
    }
  }
}

// 目的：后续块全部取消时，一次 ++ 应直接到 end；无回调的迭代器保持原行为。
// 场景：a 有效，b 为取消值、c 为取消墓碑；两个迭代器共享同一 SST 和块缓存。
TEST_F(SSTTest, RollbackIncrementSkipsCancelledTailWithoutAffectingOtherReader) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 1);
  builder.add("b", std::string(24, 'b'), 9);
  builder.add("c", "", 9);
  auto sst = builder.build(1, "test_data/increment-cancelled-tail.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 3);
  SstIterator filtered(sst, "a", 0, false,
                       [](uint64_t id) { return id == 9; });
  SstIterator unfiltered(sst, "a", 0);
  ASSERT_TRUE(filtered.is_valid());
  EXPECT_EQ(filtered->first, "a");
  ++filtered;
  EXPECT_TRUE(filtered == sst->end());

  ++unfiltered;
  ASSERT_TRUE(unfiltered.is_valid());
  EXPECT_EQ(unfiltered->first, "b");
  EXPECT_EQ(unfiltered.get_cur_tranc_id(), 9);
  ++unfiltered;
  ASSERT_TRUE(unfiltered.is_valid());
  EXPECT_EQ(unfiltered->first, "c");
  EXPECT_TRUE(unfiltered->second.empty());
  ++unfiltered;
  EXPECT_TRUE(unfiltered == sst->end());
}

// 目的：seek_first 重新定位时沿用已保存的回调，跳过连续不可见的起始块。
// 场景：a/b 整块取消，c 的 @9 取消、@7 为有效墓碑；先定位 z 填充缓存再回到开头。
// 验证读取上限、普通/全版本模式、回调副本与重复定位后的缓存刷新。
TEST_F(SSTTest, RollbackSeekFirstSkipsCancelledPrefixAndRefreshesCache) {
  SSTBuilder builder(32, true);
  const std::string old_c(24, 'c'), older_c(24, 'C');
  builder.add("a", std::string(24, 'a'), 9);
  builder.add("b", "", 9);
  builder.add("b", std::string(24, 'b'), 8);
  builder.add("c", "", 9);
  builder.add("c", "", 7);
  builder.add("c", old_c, 5);
  builder.add("c", older_c, 3);
  builder.add("d", std::string(24, 'd'), 10);
  builder.add("d", std::string(24, 'D'), 4);
  builder.add("z", std::string(24, 'z'), 1);
  auto sst = builder.build(1, "test_data/seek-first-rollback.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 5);

  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    for (uint64_t read_id : {0, 6, 10}) {
      SCOPED_TRACE(read_id);
      std::function<bool(uint64_t)> is_aborted =
          [](uint64_t id) { return id == 9 || id == 8; };
      SstIterator it(sst, "z", read_id, keep_all, is_aborted);
      is_aborted = {};
      for (int repeat = 0; repeat < 2; ++repeat) {
        ASSERT_TRUE(it.is_valid());
        EXPECT_EQ(it->first, "z"); // 填充旧缓存。
        it.seek_first();
        ASSERT_TRUE(it.is_valid());
        EXPECT_EQ(it->first, "c");
        EXPECT_EQ(it->second, read_id == 6 ? old_c : "");
        EXPECT_EQ(it.get_cur_tranc_id(), read_id == 6 ? 5 : 7);
        EXPECT_EQ((*it).first, "c");

        // 新块迭代器也须保留原来的遍历模式和回滚判断。
        ++it;
        ASSERT_TRUE(it.is_valid());
        EXPECT_EQ(it->first, keep_all ? "c" : "d");
        EXPECT_EQ(it.get_cur_tranc_id(), keep_all
            ? (read_id == 6 ? 3 : 5)
            : (read_id == 6 ? 4 : 10));
        it.seek("z");
      }
    }
  }
}

// 目的：已读取的版本全部取消后，seek_first 应清理旧缓存并进入统一 end 状态。
// 场景：a@9 为普通值、b@9 为墓碑；改变回调查询的状态后重新定位。
// 同时验证没有回调的读取仍可见原始记录，以及空 SST 句柄不会调用回调。
TEST_F(SSTTest, RollbackSeekFirstHandlesAllCancelledAndNullSst) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 9);
  builder.add("b", "", 9);
  auto sst = builder.build(1, "test_data/seek-first-all-cancelled.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 2);
  bool cancelled = false;
  SstIterator it(sst, "a", 0, false, [&cancelled](uint64_t id) {
    return cancelled && id == 9;
  });
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->first, "a");
  cancelled = true;
  for (int repeat = 0; repeat < 2; ++repeat) {
    it.seek_first();
    EXPECT_TRUE(it.is_end());
    EXPECT_FALSE(it.is_valid());
    EXPECT_TRUE(it == sst->end());
    EXPECT_THROW((void)*it, std::runtime_error);
  }

  auto unfiltered = sst->begin(0);
  unfiltered.seek_first();
  ASSERT_TRUE(unfiltered.is_valid());
  EXPECT_EQ(unfiltered->first, "a");
  EXPECT_EQ(unfiltered.get_cur_tranc_id(), 9);

  bool callback_called = false;
  SstIterator empty(nullptr, "a", 0, false, [&callback_called](uint64_t) {
    callback_called = true;
    return true;
  });
  empty.seek_first();
  EXPECT_TRUE(empty.is_end());
  EXPECT_FALSE(empty.is_valid());
  EXPECT_FALSE(callback_called);
}

// 目的：普通构造必须先保存回调，再定位首条记录；之后的 ++ 继续使用同一规则。
// 场景：a 整块取消，b 的新墓碑取消但旧墓碑有效，c 保留有效旧值，d 整块取消。
// 覆盖读取上限、两种遍历模式、回调独立保存、默认参数、全部取消及空 SST 句柄。
TEST_F(SSTTest, RollbackDefaultConstructorInitializesFilterBeforeFirstSeek) {
  SSTBuilder builder(32, true);
  const std::string b(24, 'b'), older_b(24, 'B'), c(24, 'c');
  builder.add("a", std::string(24, 'a'), 9);
  builder.add("b", "", 9);
  builder.add("b", "", 7);
  builder.add("b", b, 5);
  builder.add("b", older_b, 3);
  builder.add("c", std::string(24, 'C'), 9);
  builder.add("c", c, 4);
  builder.add("d", std::string(24, 'd'), 9);
  auto sst = builder.build(1, "test_data/default-ctor-rollback.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 4);
  using Row = std::tuple<std::string, std::string, uint64_t>;
  const std::vector<Row> latest = {{"b", "", 7}, {"c", c, 4}};
  const std::vector<Row> historical = {{"b", b, 5}, {"c", c, 4}};
  const std::vector<Row> latest_all = {
      {"b", "", 7}, {"b", b, 5}, {"b", older_b, 3}, {"c", c, 4}};
  const std::vector<Row> historical_all = {
      {"b", b, 5}, {"b", older_b, 3}, {"c", c, 4}};

  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    for (uint64_t read_id : {0, 6, 10}) {
      SCOPED_TRACE(read_id);
      std::function<bool(uint64_t)> is_aborted =
          [](uint64_t id) { return id == 9; };
      SstIterator it(sst, read_id, keep_all, is_aborted);
      is_aborted = {};
      const auto &expected = keep_all
          ? (read_id == 6 ? historical_all : latest_all)
          : (read_id == 6 ? historical : latest);
      for (const auto &[key, value, version] : expected) {
        ASSERT_TRUE(it.is_valid());
        EXPECT_EQ(it->first, key);
        EXPECT_EQ(it->second, value);
        EXPECT_EQ(it.get_cur_tranc_id(), version);
        ++it;
      }
      EXPECT_TRUE(it.is_end());
      EXPECT_TRUE(it == sst->end());
    }
  }

  SstIterator unfiltered(sst, 0);
  ASSERT_TRUE(unfiltered.is_valid());
  EXPECT_EQ(unfiltered->first, "a");
  EXPECT_EQ(unfiltered.get_cur_tranc_id(), 9);
  SstIterator all_cancelled(sst, 0, false, [](uint64_t) { return true; });
  EXPECT_TRUE(all_cancelled == sst->end());

  bool callback_called = false;
  SstIterator empty(nullptr, 0, false, [&callback_called](uint64_t) {
    callback_called = true;
    return true;
  });
  EXPECT_TRUE(empty.is_end());
  EXPECT_FALSE(callback_called);
}

// 目的：公开 begin 必须把回滚规则交给新迭代器，首次定位和后续跨块推进都要生效。
// 场景：a 整块取消；b 的新墓碑取消，应读到旧值；c 的有效墓碑必须保留；
// d 整块取消后继续到 e。覆盖普通/全版本模式，以及整张表全部取消的结果。
TEST_F(SSTTest, RollbackBeginPassesFilterToIterator) {
  SSTBuilder builder(32, true);
  const std::string old_b(24, 'b'), old_c(24, 'c'), value_e(24, 'e');
  builder.add("a", std::string(24, 'a'), 9);
  builder.add("b", "", 9);
  builder.add("b", old_b, 3);
  builder.add("c", "", 7);
  builder.add("c", old_c, 2);
  builder.add("d", std::string(24, 'd'), 9);
  builder.add("e", value_e, 4);
  auto sst = builder.build(1, "test_data/begin-rollback.sst",
                          std::make_shared<BlockCache>(8, 2));
  ASSERT_EQ(sst->num_blocks(), 5);
  using Row = std::tuple<std::string, std::string, uint64_t>;
  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    std::function<bool(uint64_t)> is_aborted =
        [](uint64_t id) { return id == 9; };
    auto it = sst->begin(0, keep_all, is_aborted);
    is_aborted = {}; // 返回的迭代器须独立保存规则。
    std::vector<Row> expected = {{"b", old_b, 3}, {"c", "", 7}};
    if (keep_all) {
      expected.emplace_back("c", old_c, 2);
    }
    expected.emplace_back("e", value_e, 4);
    for (const auto &[key, value, version] : expected) {
      ASSERT_TRUE(it.is_valid());
      ASSERT_EQ(it->first, key);
      EXPECT_EQ(it->second, value);
      EXPECT_EQ(it.get_cur_tranc_id(), version);
      ++it;
    }
    EXPECT_TRUE(it == sst->end());
  }
  auto all_cancelled = sst->begin(0, false, [](uint64_t) { return true; });
  EXPECT_TRUE(all_cancelled == sst->end());
}

// 目的：谓词区间先排除回滚版本，再选同 key 的有效版本；有效墓碑仍交给上层。
// 场景：a 整组取消，b 的新值和新墓碑取消，c 的墓碑有效，d 有可见旧值；
// 查询 [a, e] 时，取消的 e 被跳过，终点必须与正常 ++ 一样落到范围外的 f。
// 同时覆盖单块/跨块、历史读取上限，以及调用方清空回调后的持续遍历。
TEST_F(SSTTest, RollbackPredicatePreservesVersionsAndReachableBoundary) {
  using Row = std::tuple<std::string, std::string, uint64_t>;
  const std::string old_b(24, 'b'), old_c(24, 'c'), old_d(24, 'd');
  for (size_t block_size : {32, 1024}) {
    SCOPED_TRACE(block_size);
    SSTBuilder builder(block_size, true);
    builder.add("a", std::string(24, 'a'), 9);
    builder.add("b", "", 9);
    builder.add("b", std::string(24, 'B'), 8);
    builder.add("b", old_b, 5);
    builder.add("c", "", 7);
    builder.add("c", old_c, 3);
    builder.add("d", std::string(24, 'D'), 9);
    builder.add("d", old_d, 4);
    builder.add("e", std::string(24, 'e'), 9);
    builder.add("f", std::string(24, 'f'), 4);
    auto sst = builder.build(
        block_size, "test_data/predicate-rollback-" +
                        std::to_string(block_size) + ".sst",
        std::make_shared<BlockCache>(16, 2));
    ASSERT_EQ(sst->num_blocks(), block_size == 32 ? 6 : 1);

    for (uint64_t read_id : {0, 6, 10}) {
      SCOPED_TRACE(read_id);
      std::function<bool(uint64_t)> is_aborted =
          [](uint64_t id) { return id == 9 || id == 8; };
      auto range = sst_iters_monotony_predicate(
          sst, read_id, [](const std::string &key) {
            if (key < "a") return 1;
            if (key > "e") return -1;
            return 0;
          }, is_aborted);
      ASSERT_TRUE(range.has_value());
      is_aborted = {};
      auto &[it, end] = *range;
      ASSERT_TRUE(end.is_valid());
      EXPECT_EQ(end->first, "f");
      const std::vector<Row> expected = {
          {"b", old_b, 5},
          {"c", read_id == 6 ? old_c : "", read_id == 6 ? 3 : 7},
          {"d", old_d, 4}};
      // 按明确的预期条数推进，终点错误时直接失败，避免测试本身死循环。
      for (const auto &[key, value, version] : expected) {
        ASSERT_TRUE(it != end);
        ASSERT_TRUE(it.is_valid());
        EXPECT_EQ(it->first, key);
        EXPECT_EQ(it->second, value);
        EXPECT_EQ(it.get_cur_tranc_id(), version);
        ++it;
      }
      EXPECT_TRUE(it == end);
    }
  }
}

// 目的：没有有效命中时返回空结果；查询覆盖到 SST 尾部时能到达统一 end。
// 场景：a/c 只有已取消版本，b@9 取消后剩 b@3；只查 a、全部取消、
// 历史上限过低及缺失区间均无结果，默认不传回调仍兼容原来的读取方式。
TEST_F(SSTTest, RollbackPredicateHandlesEmptyRangesAndCancelledTail) {
  SSTBuilder builder(32, true);
  builder.add("a", std::string(24, 'a'), 9);
  builder.add("b", "", 9);
  builder.add("b", std::string(24, 'b'), 3);
  builder.add("c", std::string(24, 'c'), 9);
  auto sst = builder.build(1, "test_data/predicate-rollback-empty.sst",
                          std::make_shared<BlockCache>(8, 2));
  auto all = [](const std::string &) { return 0; };
  auto is_aborted = [](uint64_t id) { return id == 9; };
  auto range = sst_iters_monotony_predicate(sst, 0, all, is_aborted);
  ASSERT_TRUE(range.has_value());
  auto &[it, end] = *range;
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it->first, "b");
  EXPECT_EQ(it.get_cur_tranc_id(), 3);
  EXPECT_TRUE(end == sst->end());
  ++it;
  EXPECT_TRUE(it == end);

  EXPECT_FALSE(sst_iters_monotony_predicate(
      sst, 0, [](const std::string &key) { return -key.compare("a"); },
      is_aborted).has_value());
  EXPECT_FALSE(sst_iters_monotony_predicate(
      sst, 0, all, [](uint64_t) { return true; }).has_value());
  EXPECT_FALSE(sst_iters_monotony_predicate(sst, 2, all, is_aborted).has_value());
  EXPECT_FALSE(sst_iters_monotony_predicate(
      sst, 0, [](const std::string &key) { return -key.compare("z"); },
      is_aborted).has_value());
  EXPECT_FALSE(sst_iters_monotony_predicate(nullptr, 0, all, is_aborted).has_value());
  auto unfiltered = sst_iters_monotony_predicate(sst, 0, all);
  ASSERT_TRUE(unfiltered.has_value());
  ASSERT_TRUE(unfiltered->first.is_valid());
  EXPECT_EQ(unfiltered->first->first, "a");
  EXPECT_EQ(unfiltered->first.get_cur_tranc_id(), 9);
}

// 目的：前一块已经很大时，不同 key 仍应正常换块，不能误报新记录偏移超限。
// 场景：单条 value 的 65530 字节可由 uint16_t 编码，但整条记录超过 65535 字节；
// 下一条是不同 key，可以放到新块。此测试不要求把同 key 的版本拆到不同块。
TEST_F(SSTTest, AuditDifferentKeyCanStartBlockAfterLargeEntry) {
  SSTBuilder builder(4096, true);
  const std::string large(65530, 'v');
  ASSERT_NO_THROW(builder.add("a", large, 1));
  ASSERT_NO_THROW(builder.add("b", "small", 2));
  auto cache = std::make_shared<BlockCache>(16, 2);
  auto sst = builder.build(1, "test_data/audit-large-entry.sst", cache);
  auto a = sst->get("a", 0);
  ASSERT_TRUE(a.is_valid());
  EXPECT_EQ(a->second, large);
  auto b = sst->get("b", 0);
  ASSERT_TRUE(b.is_valid());
  EXPECT_EQ(b->second, "small");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
