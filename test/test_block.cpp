#include "block/block.h"
#include "block/block_iterator.h"
#include "config/config.h"
#include "logger/logger.h"
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace ::tiny_lsm;

class BlockTest : public ::testing::Test {
protected:
  // 预定义的编码数据
  std::vector<uint8_t> getEncodedBlock() {
    /*
    Block layout (3 entries):
    Entry1: key="apple", value="red"
    Entry2: key="banana", value="yellow"
    Entry3: key="orange", value="orange"
    */
    std::vector<uint8_t> encoded = {
        // Data Section
        // Entry 1: "apple" -> "red"
        5, 0,                    // key_len = 5
        'a', 'p', 'p', 'l', 'e', // key
        3, 0,                    // value_len = 3
        'r', 'e', 'd',           // value
        1, 0, 0, 0, 0, 0, 0, 0,  // tranc_id = 1

        // Entry 2: "banana" -> "yellow"
        6, 0,                         // key_len = 6
        'b', 'a', 'n', 'a', 'n', 'a', // key
        6, 0,                         // value_len = 6
        'y', 'e', 'l', 'l', 'o', 'w', // value
        2, 0, 0, 0, 0, 0, 0, 0,       // tranc_id = 2

        // Entry 3: "orange" -> "orange3"
        6, 0,                              // key_len = 6
        'o', 'r', 'a', 'n', 'g', 'e',      // key
        7, 0,                              // value_len = 6
        'o', 'r', 'a', 'n', 'g', 'e', '3', // value
        3, 0, 0, 0, 0, 0, 0, 0,            // tranc_id = 3

        // Entry 4: "orange" -> "orange2"
        6, 0,                              // key_len = 6
        'o', 'r', 'a', 'n', 'g', 'e',      // key
        7, 0,                              // value_len = 6
        'o', 'r', 'a', 'n', 'g', 'e', '2', // value
        2, 0, 0, 0, 0, 0, 0, 0,            // tranc_id = 2

        // Entry 5: "orange" -> "orange1"
        6, 0,                              // key_len = 6
        'o', 'r', 'a', 'n', 'g', 'e',      // key
        7, 0,                              // value_len = 6
        'o', 'r', 'a', 'n', 'g', 'e', '1', // value
        1, 0, 0, 0, 0, 0, 0, 0,            // tranc_id = 1

        // Offset Section (每个entry的起始位置)
        0, 0,  // offset[0] = 0
        20, 0, // offset[1] = 12 (第二个entry的起始位置)
        44, 0, // offset[2] = 24 (第三个entry的起始位置)
        69, 0, // offset[3] = 36 (第四个entry的起始位置)
        94, 0, // offset[4] = 48 (第五个entry的起始位置)

        // Num of elements
        5, 0 // num_elements = 5
    };
    return encoded;
  }
};

// 测试解码
TEST_F(BlockTest, DecodeTest) {
  auto encoded = getEncodedBlock();
  auto block = Block::decode(encoded,false);

  // 验证第一个key
  EXPECT_EQ(block->get_first_key(), "apple");

  // 验证所有key-value对
  EXPECT_EQ(block->get_value_binary("apple", 0).value(), "red");
  EXPECT_EQ(block->get_value_binary("banana", 0).value(), "yellow");
  EXPECT_EQ(block->get_value_binary("orange", 0).value(), "orange3");

  // 指定事务id查询
  EXPECT_EQ(block->get_value_binary("orange", 1).value(), "orange1");
  EXPECT_EQ(block->get_value_binary("orange", 2).value(), "orange2");
  EXPECT_EQ(block->get_value_binary("orange", 3).value(), "orange3");
}

// 测试编码
TEST_F(BlockTest, EncodeTest) {
  Block block(1024);
  block.add_entry("apple", "red", 1, false);
  block.add_entry("banana", "yellow", 2, false);
  block.add_entry("orange", "orange3", 3, false);
  block.add_entry("orange", "orange2", 2, false);
  block.add_entry("orange", "orange1", 1, false);

  auto encoded = block.encode();

  // 解码并验证
  auto decoded = Block::decode(encoded);
  EXPECT_EQ(decoded->get_value_binary("apple", 1).value(), "red");
  EXPECT_EQ(decoded->get_value_binary("banana", 2).value(), "yellow");
  EXPECT_EQ(decoded->get_value_binary("orange", 0).value(), "orange3");

  // 指定事务id查询
  EXPECT_EQ(decoded->get_value_binary("orange", 1).value(), "orange1");
  EXPECT_EQ(decoded->get_value_binary("orange", 2).value(), "orange2");
  EXPECT_EQ(decoded->get_value_binary("orange", 3).value(), "orange3");
}

// 测试二分查找
TEST_F(BlockTest, BinarySearchTest) {
  Block block(1024);
  block.add_entry("apple", "red", 0, false);
  block.add_entry("banana", "yellow", 0, false);
  block.add_entry("orange", "orange", 0, false);

  // 测试存在的key
  EXPECT_EQ(block.get_value_binary("apple", 0).value(), "red");
  EXPECT_EQ(block.get_value_binary("banana", 0).value(), "yellow");
  EXPECT_EQ(block.get_value_binary("orange", 0).value(), "orange");

  // 测试不存在的key
  EXPECT_FALSE(block.get_value_binary("grape", 0).has_value());
  EXPECT_FALSE(block.get_value_binary("", 0).has_value());
}

// 测试边界情况
TEST_F(BlockTest, EdgeCasesTest) {
  Block block(1024);

  // 空block
  EXPECT_EQ(block.get_first_key(), "");
  EXPECT_FALSE(block.get_value_binary("any", 0).has_value());

  // 添加空key和value
  block.add_entry("", "", 0, false);
  EXPECT_EQ(block.get_first_key(), "");
  EXPECT_EQ(block.get_value_binary("", 0).value(), "");

  // 添加包含特殊字符的key和value
  block.add_entry("key\0with\tnull", "value\rwith\nnull", 0, false);
  std::string special_key("key\0with\tnull");
  std::string special_value("value\rwith\nnull");
  EXPECT_EQ(block.get_value_binary(special_key, 0).value(), special_value);
}

// 测试大数据量
TEST_F(BlockTest, LargeDataTest) {
  Block block(1024 * 32);
  const int n = 1000;

  // 添加大量数据
  for (int i = 0; i < n; i++) {
    // 使用 std::format 或 sprintf 进行补零
    char key_buf[16];
    snprintf(key_buf, sizeof(key_buf), "key%03d", i); // 补零到3位
    std::string key = key_buf;

    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "value%03d", i);
    std::string value = value_buf;

    block.add_entry(key, value, 0, false);
  }

  // 验证所有数据
  for (int i = 0; i < n; i++) {
    char key_buf[16];
    snprintf(key_buf, sizeof(key_buf), "key%03d", i);
    std::string key = key_buf;

    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "value%03d", i);
    std::string expected_value = value_buf;

    EXPECT_EQ(block.get_value_binary(key, 0).value(), expected_value);
  }
}

// 测试错误处理
TEST_F(BlockTest, ErrorHandlingTest) {
  // 测试解码无效数据
  std::vector<uint8_t> invalid_data = {1}; // 太短
  EXPECT_THROW(Block::decode(invalid_data), std::runtime_error);

  // 测试空vector
  std::vector<uint8_t> empty_data;
  EXPECT_THROW(Block::decode(empty_data), std::runtime_error);
}

// 目的：key/value 长度字段只有 16 位，超长输入应在修改 Block 前被拒绝。
// 场景：分别写入 65536 字节的 key、value，同时覆盖普通写入和强制写入。
// 预期：抛出 length_error，Block 仍为空；force_write 不能绕过格式边界。
TEST_F(BlockTest, RejectsUnencodableKeyAndValueLengths) {
  const std::string oversized(65536, 'x');
  for (bool force_write : {false, true}) {
    SCOPED_TRACE(force_write);
    Block key_block(32768);
    EXPECT_THROW(key_block.add_entry(oversized, "v", 1, force_write),
                 std::length_error);
    EXPECT_TRUE(key_block.is_empty());

    Block value_block(32768);
    EXPECT_THROW(value_block.add_entry("k", oversized, 1, force_write),
                 std::length_error);
    EXPECT_TRUE(value_block.is_empty());
  }
}

// 目的：65535 是合法的记录起点；只有下一条起点超出 16 位范围时才拒绝追加。
// 场景：第一条记录恰好占 65535 字节，第二条起点为 65535，第三条起点已超限。
// 预期：前两条可编解码、查询；强制追加第三条抛错，已有数据保持不变。
TEST_F(BlockTest, RejectsOffsetOverflowWithoutChangingExistingRecords) {
  Block block(32768);
  // 单字节 key 的 entry 开销为 2 + 1 + 2 + 8 = 13 字节。
  const std::string value(65535 - 13, 'x');
  ASSERT_TRUE(block.add_entry("k", value, 3, false));
  ASSERT_TRUE(block.add_entry("k", "", 2, true));
  ASSERT_EQ(block.get_offset_at(1), 65535u);

  const auto size_before = block.cur_size();
  ASSERT_THROW(block.add_entry("k", "old", 1, true), std::length_error);
  EXPECT_EQ(block.size(), 2u);
  EXPECT_EQ(block.cur_size(), size_before);

  auto decoded = Block::decode(block.encode());
  EXPECT_EQ(decoded->get_value_binary("k", 3), std::make_optional(value));
  EXPECT_EQ(decoded->get_value_binary("k", 2),
            std::make_optional(std::string("")));
  EXPECT_FALSE(decoded->get_value_binary("k", 1).has_value());
}

// 目的：格式边界检查不能误伤仍可编码的同 key 强制追加。
// 场景：两条记录总大小超过配置容量 32 字节，但其长度、偏移均在 16 位范围内。
// 预期：force_write 仍可突破配置容量，保留同 key 的历史版本。
TEST_F(BlockTest, ForceWriteWithinEncodingLimitsStillSucceeds) {
  Block block(32);
  ASSERT_TRUE(block.add_entry("k", std::string(20, 'n'), 2, false));
  ASSERT_TRUE(block.add_entry("k", std::string(20, 'o'), 1, true));
  EXPECT_GT(block.cur_size(), 32u);
  EXPECT_EQ(block.get_value_binary("k", 1),
            std::make_optional(std::string(20, 'o')));
}

// 测试迭代器
TEST_F(BlockTest, IteratorTest) {
  // 使用 make_shared 创建 Block
  auto block = std::make_shared<Block>(4096);

  // 1. 测试空block的迭代器
  EXPECT_EQ(block->begin(), block->end());

  // 2. 添加有序数据
  const int n = 100;
  std::vector<std::pair<std::string, std::string>> test_data;

  for (int i = 0; i < n; i++) {
    char key_buf[16], value_buf[16];
    snprintf(key_buf, sizeof(key_buf), "key%03d", i);
    snprintf(value_buf, sizeof(value_buf), "value%03d", i);

    block->add_entry(key_buf, value_buf, 0, false);
    test_data.emplace_back(key_buf, value_buf);
  }

  // 3. 测试正向遍历和数据正确性
  size_t count = 0;
  for (const auto &[key, value] : *block) { // 注意这里使用 *block
    EXPECT_EQ(key, test_data[count].first);
    EXPECT_EQ(value, test_data[count].second);
    count++;
  }
  EXPECT_EQ(count, test_data.size());

  // 4. 测试迭代器的比较和移动
  auto it = block->begin();
  EXPECT_EQ(it->first, "key000");
  ++it;
  EXPECT_EQ(it->first, "key001");
  ++it;
  EXPECT_EQ(it->first, "key002");

  // 5. 测试编码后的迭代
  auto encoded = block->encode();
  auto decoded_block = Block::decode(encoded);
  count = 0;
  for (auto it = decoded_block->begin(); it != decoded_block->end(); ++it) {
    EXPECT_EQ(it->first, test_data[count].first);
    EXPECT_EQ(it->second, test_data[count].second);
    count++;
  }
}

// 包含多个事务操作的key的迭代器
TEST_F(BlockTest, TrancIteratorTest) {
  auto block = std::make_shared<Block>(4096);

  // 添加多个事务操作的key
  block->add_entry("key1", "value1", 1, false);

  block->add_entry("key2", "value222", 3, false);
  block->add_entry("key2", "value22", 2, false);
  block->add_entry("key2", "value2", 1, false);

  block->add_entry("key3", "value3", 1, false);
  block->add_entry("key4", "value4", 2, false);
  block->add_entry("key5", "value5", 3, false);

  std::vector<std::pair<std::string, std::string>> expected_data = {
      {"key1", "value1"},
      {"key2", "value222"},
      {"key3", "value3"},
      {"key4", "value4"},
      {"key5", "value5"}};

  std::vector<std::pair<std::string, std::string>> results;

  for (auto it = block->begin(); it != block->end(); ++it) {
    results.emplace_back(it->first, it->second);
  }

  EXPECT_EQ(results, expected_data);
}

TEST_F(BlockTest, PredicateTest) {
  std::vector<uint8_t> encoded_p;
  {
    std::shared_ptr<Block> block1 =
        std::make_shared<Block>(TomlConfig::getInstance().getLsmBlockSize());
    int num = 50;

    for (int i = 0; i < num; ++i) {
      std::ostringstream oss_key;
      std::ostringstream oss_value;

      // 设置数字为4位长度，不足的部分用前导零填充
      oss_key << "key" << std::setw(4) << std::setfill('0') << i;
      oss_value << "value" << std::setw(4) << std::setfill('0') << i;

      std::string key = oss_key.str();
      std::string value = oss_value.str();

      block1->add_entry(key, value, 0, false);
    }

    auto result =
        block1->get_monotony_predicate_iters(0, [](const std::string &key) {
          if (key < "key0020") {
            return 1;
          }
          if (key >= "key0030") {
            return -1;
          }
          return 0;
        });
    EXPECT_TRUE(result.has_value());
    auto [it_begin, it_end] = result.value();
    EXPECT_EQ((*it_begin)->first, "key0020");
    EXPECT_EQ((*it_end)->first, "key0030");
    for (int i = 0; i < 5; i++) {
      ++(*it_begin);
    }
    EXPECT_EQ((*it_begin)->first, "key0025");

    encoded_p = block1->encode();
  }
  std::shared_ptr<Block> block2 = Block::decode(encoded_p);

  auto result =
      block2->get_monotony_predicate_iters(0, [](const std::string &key) {
        if (key < "key0020") {
          return 1;
        }
        if (key >= "key0030") {
          return -1;
        }
        return 0;
      });
  EXPECT_TRUE(result.has_value());
  auto [it_begin, it_end] = result.value();
  EXPECT_EQ((*it_begin)->first, "key0020");
  EXPECT_EQ((*it_end)->first, "key0030");
  for (int i = 0; i < 5; i++) {
    ++(*it_begin);
  }
  EXPECT_EQ((*it_begin)->first, "key0025");
}

// 包含了事务的谓词迭代器
TEST_F(BlockTest, TrancPredicateTest) {
  std::vector<uint8_t> encoded_p;

  {
    std::shared_ptr<Block> block1 =
        std::make_shared<Block>(TomlConfig::getInstance().getLsmBlockSize());
    int num = 50;

    block1->add_entry("key0", "value0", 0, false);
    block1->add_entry("key1", "value1", 1, false);
    block1->add_entry("key2", "value22", 10, false);
    block1->add_entry("key2", "value2", 2, false);
    block1->add_entry("key3", "value3", 3, false);
    block1->add_entry("key4", "value4444", 9, false);
    block1->add_entry("key4", "value444", 8, false);
    block1->add_entry("key4", "value44", 7, false);
    block1->add_entry("key4", "value4", 4, false);
    block1->add_entry("key5", "value5555", 8, false);
    block1->add_entry("key5", "value555", 7, false);
    block1->add_entry("key5", "value55", 6, false);
    block1->add_entry("key5", "value5", 5, false);
    block1->add_entry("key6", "value6", 6, false);

    encoded_p = block1->encode();
  }

  std::shared_ptr<Block> block2 = Block::decode(encoded_p);

  auto result =
      block2->get_monotony_predicate_iters(7, [](const std::string &key) {
        if (key < "key2") {
          return 1;
        }
        if (key >= "key6") {
          return -1;
        }
        return 0;
      });
  EXPECT_TRUE(result.has_value());
  auto [it_begin, it_end] = result.value();

  EXPECT_EQ((*it_end)->first, "key6");

  EXPECT_EQ((*it_begin)->first, "key2");
  EXPECT_EQ((*it_begin)->second, "value2");

  ++(*it_begin);
  EXPECT_EQ((*it_begin)->first, "key3");

  ++(*it_begin);
  EXPECT_EQ((*it_begin)->first, "key4");
  EXPECT_EQ((*it_begin)->second, "value44");

  // 遍历打印
  result = block2->get_monotony_predicate_iters(6, [](const std::string &key) {
    if (key < "key2") {
      return 1;
    }
    if (key >= "key7") {
      return -1;
    }
    return 0;
  });
  auto [it_begin2, it_end2] = result.value();

  std::vector<std::string> results;
  for (auto it = it_begin2; (*it) != (*it_end2); ++(*it)) {
    results.push_back((*it)->second);
  }

  std::vector<std::string> expected = {"value2", "value3", "value4", "value55",
                                       "value6"};
  EXPECT_EQ(results, expected);
}

// 目的：点查询选择不超过读上限的最新版本。
// 场景：k 的多个版本夹在相邻 key 之间；k 全部不可见时不能误返回邻居。
TEST_F(BlockTest, MvccPointReadSelectsNewestVisibleVersion) {
  auto block = std::make_shared<Block>(4096);
  ASSERT_TRUE(block->add_entry("a", "a1", 1, false));
  ASSERT_TRUE(block->add_entry("k", "k12", 12, false));
  ASSERT_TRUE(block->add_entry("k", "k9", 9, false));
  ASSERT_TRUE(block->add_entry("k", "k5", 5, false));
  ASSERT_TRUE(block->add_entry("z", "z1", 1, false));

  const std::vector<std::pair<uint64_t, uint64_t>> cases = {
      {0, 12}, {5, 5}, {8, 5}, {9, 9}, {11, 9}, {12, 12}, {20, 12}};
  for (const auto &[read_id, expected_id] : cases) {
    SCOPED_TRACE(read_id);
    auto value = block->get_value_binary("k", read_id);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "k" + std::to_string(expected_id));
    BlockIterator it(block, "k", read_id);
    ASSERT_NE(it, block->end());
    EXPECT_EQ(it->first, "k");
    EXPECT_EQ(it.get_cur_tranc_id(), expected_id);
  }

  EXPECT_FALSE(block->get_value_binary("k", 4).has_value());
  EXPECT_EQ(BlockIterator(block, "k", 4), block->end());
  EXPECT_FALSE(block->get_value_binary("missing", 0).has_value());
  // 最后一个 key 不可见时，查找必须安全结束。
  auto tail = std::make_shared<Block>(4096);
  ASSERT_TRUE(tail->add_entry("k", "new", 9, false));
  EXPECT_FALSE(tail->get_value_binary("k", 8).has_value());
}

// 目的：删除也是一个版本，不能跳过墓碑而读到被删除的旧值。
// 场景：k@5 写入、k@9 删除、k@12 再次写入；Block 要保留墓碑给上层合并。
TEST_F(BlockTest, MvccTombstoneDoesNotFallBackToOlderValue) {
  auto block = std::make_shared<Block>(4096);
  ASSERT_TRUE(block->add_entry("k", "new", 12, false));
  ASSERT_TRUE(block->add_entry("k", "", 9, false));
  ASSERT_TRUE(block->add_entry("k", "old", 5, false));

  const std::vector<std::tuple<uint64_t, uint64_t, std::string>> cases = {
      {0, 12, "new"}, {12, 12, "new"}, {11, 9, ""},
      {9, 9, ""}, {8, 5, "old"}, {5, 5, "old"}};
  for (const auto &[read_id, expected_id, expected_value] : cases) {
    SCOPED_TRACE(read_id);
    auto value = block->get_value_binary("k", read_id);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, expected_value);
    auto it = block->begin(read_id);
    ASSERT_NE(it, block->end());
    EXPECT_EQ(it->second, expected_value);
    EXPECT_EQ(it.get_cur_tranc_id(), expected_id);
    EXPECT_EQ(++it, block->end());
  }
  EXPECT_FALSE(block->get_value_binary("k", 4).has_value());
}

// 目的：编解码完整保留 64 位版本号，包括最高位和版本 0。
// 场景：同 key 有 UINT64_MAX、超过 32 位的版本、版本 0，落盘往返后仍正确查询。
TEST_F(BlockTest, MvccEncodingPreservesFullWidthTransactionIds) {
  const uint64_t max_id = std::numeric_limits<uint64_t>::max();
  const uint64_t middle_id = (uint64_t{1} << 40) + 9;
  Block block(4096);
  ASSERT_TRUE(block.add_entry("k", "max", max_id, false));
  ASSERT_TRUE(block.add_entry("k", "middle", middle_id, false));
  ASSERT_TRUE(block.add_entry("k", "zero", 0, false));

  auto decoded = Block::decode(block.encode());
  const std::vector<std::tuple<uint64_t, uint64_t, std::string>> cases = {
      {0, max_id, "max"}, {max_id, max_id, "max"},
      {max_id - 1, middle_id, "middle"}, {middle_id, middle_id, "middle"},
      {middle_id - 1, 0, "zero"}, {1, 0, "zero"}};
  for (const auto &[read_id, expected_id, expected_value] : cases) {
    SCOPED_TRACE(read_id);
    BlockIterator it(decoded, "k", read_id);
    ASSERT_NE(it, decoded->end());
    EXPECT_EQ(it->second, expected_value);
    EXPECT_EQ(it.get_cur_tranc_id(), expected_id);
  }
}

// 目的：先过滤不可见版本，再决定是否跳过同 key 的旧版本；墓碑仍然保留。
// 场景：a、c 整组不可见，b 有多个可见版本，d 的最新可见版本是墓碑。
TEST_F(BlockTest, MvccIteratorFiltersBeforeDeduplicating) {
  auto block = std::make_shared<Block>(4096);
  ASSERT_TRUE(block->add_entry("a", "a12", 12, false));
  ASSERT_TRUE(block->add_entry("b", "b12", 12, false));
  ASSERT_TRUE(block->add_entry("b", "b7", 7, false));
  ASSERT_TRUE(block->add_entry("b", "b3", 3, false));
  ASSERT_TRUE(block->add_entry("c", "c10", 10, false));
  ASSERT_TRUE(block->add_entry("c", "c9", 9, false));
  ASSERT_TRUE(block->add_entry("d", "", 8, false));
  ASSERT_TRUE(block->add_entry("d", "d2", 2, false));
  ASSERT_TRUE(block->add_entry("e", "e0", 0, false));

  using Record = std::tuple<std::string, std::string, uint64_t>;
  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    const std::vector<Record> expected = keep_all
        ? std::vector<Record>{{"b", "b7", 7}, {"b", "b3", 3},
                              {"d", "", 8}, {"d", "d2", 2}, {"e", "e0", 0}}
        : std::vector<Record>{{"b", "b7", 7}, {"d", "", 8}, {"e", "e0", 0}};
    BlockIterator it(block, size_t{0}, 8, keep_all);
    for (const auto &[key, value, id] : expected) {
      ASSERT_NE(it, block->end());
      EXPECT_EQ(it->first, key);
      EXPECT_EQ(it->second, value);
      EXPECT_EQ(it.get_cur_tranc_id(), id);
      ++it;
    }
    EXPECT_EQ(it, block->end());
  }
}

// 目的：范围起点和终点附近的不可见记录，不应让结果越过范围边界。
// 场景：前缀 p 的首 key 全部不可见，末尾及范围外的 q 也不可见。
// 全部匹配记录不可见时，允许返回空区间或 nullopt，两者都不能产生记录。
TEST_F(BlockTest, MvccRangesRespectInvisibleBoundaries) {
  auto block = std::make_shared<Block>(4096);
  ASSERT_TRUE(block->add_entry("a", "a1", 1, false));
  ASSERT_TRUE(block->add_entry("p0", "p0-new", 20, false));
  ASSERT_TRUE(block->add_entry("p1", "p1-new", 15, false));
  ASSERT_TRUE(block->add_entry("p1", "p1-old", 5, false));
  ASSERT_TRUE(block->add_entry("p2", "p2-new", 9, false));
  ASSERT_TRUE(block->add_entry("p2", "p2-old", 7, false));
  ASSERT_TRUE(block->add_entry("q", "q-new", 20, false));
  ASSERT_TRUE(block->add_entry("r", "r1", 1, false));

  auto range = block->iters_preffix(6, "p");
  ASSERT_TRUE(range.has_value());
  auto [begin, end] = *range;
  ASSERT_NE(*begin, *end);
  EXPECT_EQ((*begin)->first, "p1");
  EXPECT_EQ((*begin)->second, "p1-old");
  EXPECT_EQ(begin->get_cur_tranc_id(), 5);
  EXPECT_EQ(++(*begin), *end);

  auto invisible = block->get_monotony_predicate_iters(
      4, [](const std::string &key) {
        if (key < "p0") return 1;
        if (key >= "q") return -1;
        return 0;
      });
  if (invisible.has_value()) {
    EXPECT_EQ(*invisible->first, *invisible->second);
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
