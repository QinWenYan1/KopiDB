#include "config/config.h"
#include "consts.h"
#include "logger/logger.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include <filesystem>
#include <gtest/gtest.h>
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

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
