#include "logger/logger.h"
#include "skiplist/skiplist.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iomanip>
#include <latch>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace ::tiny_lsm;

// 测试基本插入、查找和删除
TEST(SkipListTest, BasicOperations) {
  SkipList skipList;

  // 测试插入和查找
  skipList.put("key1", "value1", 0);
  EXPECT_EQ(skipList.get("key1", 0).get_value(), "value1");

  // 测试更新
  skipList.put("key1", "new_value", 0);
  EXPECT_EQ(skipList.get("key1", 0).get_value(), "new_value");

  // 测试删除
  skipList.remove("key1");
  EXPECT_FALSE(skipList.get("key1", 0).is_valid());
}

// 测试迭代器
TEST(SkipListTest, Iterator) {
  SkipList skipList;
  skipList.put("key1", "value1", 0);
  skipList.put("key2", "value2", 0);
  skipList.put("key3", "value3", 0);

  // 测试迭代器
  std::vector<std::pair<std::string, std::string>> result;
  for (auto it = skipList.begin(); it != skipList.end(); ++it) {
    result.push_back(*it);
  }

  EXPECT_EQ(result.size(), 3);
  EXPECT_EQ(std::get<0>(result[0]), "key1");
  EXPECT_EQ(std::get<0>(result[1]), "key2");
  EXPECT_EQ(std::get<0>(result[2]), "key3");
}

// 测试大量数据插入和查找
TEST(SkipListTest, LargeScaleInsertAndGet) {
  SkipList skipList;
  const int num_elements = 10000;

  // 插入大量数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    skipList.put(key, value, 0);
  }

  // 验证插入的数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string expected_value = "value" + std::to_string(i);
    EXPECT_EQ((skipList.get(key, 0).get_value()), expected_value);
  }
}

// 测试大量数据删除
TEST(SkipListTest, LargeScaleRemove) {
  SkipList skipList;
  const int num_elements = 10000;

  // 插入大量数据
  // std::cout << "********************** insert **********************"
  //           << std::endl;
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    skipList.put(key, value, 0);

    // skipList.print_skiplist();
  }

  // std::cout << "********************** remove **********************"
  // << std::endl;
  // 删除所有数据
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    skipList.remove(key);

    // skipList.print_skiplist();
  }

  // 验证所有数据已被删除
  for (int i = 0; i < num_elements; ++i) {
    std::string key = "key" + std::to_string(i);
    EXPECT_FALSE(skipList.get(key, 0).is_valid());
  }
}

// 测试重复插入
TEST(SkipListTest, DuplicateInsert) {
  SkipList skipList;

  // 重复插入相同的key
  skipList.put("key1", "value1", 0);
  skipList.put("key1", "value2", 0);
  skipList.put("key1", "value3", 0);

  // 验证最后一次插入的值
  EXPECT_EQ((skipList.get("key1", 0).get_value()), "value3");
}

// 测试空跳表
TEST(SkipListTest, EmptySkipList) {
  SkipList skipList;

  // 验证空跳表的查找和删除
  EXPECT_FALSE(skipList.get("nonexistent_key", 0).is_valid());
  skipList.remove("nonexistent_key"); // 删除不存在的key
}

// 测试随机插入和删除
TEST(SkipListTest, RandomInsertAndRemove) {
  SkipList skipList;
  std::unordered_set<std::string> keys;
  const int num_operations = 10000;

  for (int i = 0; i < num_operations; ++i) {
    std::string key = "key" + std::to_string(rand() % 1000);
    std::string value = "value" + std::to_string(rand() % 1000);

    if (keys.find(key) == keys.end()) {
      // 插入新key
      skipList.put(key, value, 0);
      keys.insert(key);
    } else {
      // 删除已存在的key
      skipList.remove(key);
      keys.erase(key);
    }

    // 验证当前状态
    if (keys.find(key) != keys.end()) {
      EXPECT_EQ((skipList.get(key, 0).get_value()), value);
    } else {
      EXPECT_FALSE(skipList.get(key, 0).is_valid());
    }
  }
}

// 测试内存大小跟踪
TEST(SkipListTest, MemorySizeTracking) {
  SkipList skipList;

  // 插入数据
  skipList.put("key1", "value1", 0);
  skipList.put("key2", "value2", 0);

  // 验证内存大小
  size_t expected_size = sizeof("key1") - 1 + sizeof("value1") - 1 +
                         sizeof(uint64_t) + sizeof("key2") - 1 +
                         sizeof("value2") - 1 + sizeof(uint64_t);
  EXPECT_EQ(skipList.get_size(), expected_size);

  // 删除数据
  skipList.remove("key1");
  expected_size -= sizeof("key1") - 1 + sizeof("value1") - 1 + sizeof(uint64_t);
  EXPECT_EQ(skipList.get_size(), expected_size);

  skipList.clear();
  EXPECT_EQ(skipList.get_size(), 0);
}

TEST(SkipListTest, IteratorPreffix) {
  SkipList skipList;

  // 插入一些测试数据
  skipList.put("apple", "0", 0);
  skipList.put("apple2", "1", 0);
  skipList.put("apricot", "2", 0);
  skipList.put("banana", "3", 0);
  skipList.put("berry", "4", 0);
  skipList.put("cherry", "5", 0);
  skipList.put("cherry2", "6", 0);

  // 测试前缀 "ap"
  auto it = skipList.begin_preffix("ap");
  EXPECT_EQ(it.get_key(), "apple");

  // 测试前缀 "ba"
  it = skipList.begin_preffix("ba");
  EXPECT_EQ(it.get_key(), "banana");

  // 测试前缀 "ch"
  it = skipList.begin_preffix("ch");
  EXPECT_EQ(it.get_key(), "cherry");

  // 测试前缀 "z"
  it = skipList.begin_preffix("z");
  EXPECT_TRUE(it == skipList.end());

  // 测试前缀 "berr"
  it = skipList.begin_preffix("berr");
  EXPECT_EQ(it.get_key(), "berry");

  // 测试前缀 "a"
  it = skipList.begin_preffix("a");
  EXPECT_EQ(it.get_key(), "apple");

  // 测试前缀结束位置
  it = skipList.end_preffix("a");
  EXPECT_EQ(it.get_key(), "banana");

  it = skipList.end_preffix("cherry");
  EXPECT_TRUE(it == skipList.end());

  EXPECT_EQ(skipList.begin_preffix("not exist"),
            skipList.end_preffix("not exist"));
}

TEST(SkipListTest, ItersPredicate_Base) {

  SkipList skipList;
  skipList.put("prefix1", "value1", 0);
  skipList.put("prefix2", "value2", 0);
  skipList.put("prefix3", "value3", 0);
  skipList.put("other", "value4", 0);
  skipList.put("longerkey", "value5", 0);
  skipList.put("averylongkey", "value6", 0);
  skipList.put("medium", "value7", 0);
  skipList.put("midway", "value8", 0);
  skipList.put("midpoint", "value9", 0);

  // 测试前缀匹配
  auto prefix_result =
      skipList.iters_monotony_predicate([](const std::string &key) {
        auto match_str = key.substr(0, 3);
        if (match_str == "pre") {
          return 0;
        } else if (match_str < "pre") {
          return 1;
        }
        return -1;
      });
  ASSERT_TRUE(prefix_result.has_value());
  auto [prefix_begin_iter, prefix_end_iter] = prefix_result.value();
  EXPECT_EQ(prefix_begin_iter.get_key(), "prefix1");
  EXPECT_TRUE(prefix_end_iter.is_end());

  EXPECT_EQ(prefix_begin_iter.get_value(), "value1");
  ++prefix_begin_iter;
  EXPECT_EQ(prefix_begin_iter.get_value(), "value2");
  ++prefix_begin_iter;
  EXPECT_EQ(prefix_begin_iter.get_value(), "value3");

  // 测试范围匹配
  auto range = std::make_pair("l", "n"); // [l, n)
  auto range_result =
      skipList.iters_monotony_predicate([&range](const std::string &key) {
        if (key < range.first) {
          return 1;
        } else if (key >= range.second) {
          return -1;
        } else {
          return 0;
        }
      });
  ASSERT_TRUE(range_result.has_value());
  auto [range_begin_iter, range_end_iter] = range_result.value();
  EXPECT_EQ(range_end_iter.get_key(),
            "other"); // end_iter 是开区间，所以指向 "prefix1"
  EXPECT_EQ(range_begin_iter.get_key(), "longerkey");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter.get_key(), "medium");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter.get_key(), "midpoint");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter.get_key(), "midway");
}

TEST(SkipListTest, ItersPredicate_Large) {
  SkipList skipList;
  int num = 10000;

  for (int i = 0; i < num; ++i) {
    std::ostringstream oss_key;
    std::ostringstream oss_value;

    // 设置数字为4位长度，不足的部分用前导零填充
    oss_key << "key" << std::setw(4) << std::setfill('0') << i;
    oss_value << "value" << std::setw(4) << std::setfill('0') << i;

    std::string key = oss_key.str();
    std::string value = oss_value.str();

    skipList.put(key, value, 0);
  }

  skipList.remove("key1015");

  auto result = skipList.iters_monotony_predicate([](const std::string &key) {
    if (key < "key1010") {
      return 1;
    } else if (key >= "key1020") {
      return -1;
    } else {
      return 0;
    }
  });

  ASSERT_TRUE(result.has_value());
  auto [range_begin_iter, range_end_iter] = result.value();
  EXPECT_EQ(range_begin_iter.get_key(), "key1010");
  EXPECT_EQ(range_end_iter.get_key(), "key1020");
  for (int i = 0; i < 5; i++) {
    ++range_begin_iter;
  }
  EXPECT_EQ(range_begin_iter.get_key(), "key1016");
}

// 测试包含事务 id 的插入和查找
TEST(SkipListTest, TransactionId) {
  SkipList skipList;
  skipList.put("key1", "value1", 1);
  skipList.put("key1", "value2", 2);

  // 验证事务 id
  // 不指定事务 id，应该返回最新的值
  EXPECT_EQ((skipList.get("key1", 0).get_value()), "value2");
  // 指定 1 表示只能查找事务 id 小于等于 1 的值
  EXPECT_EQ((skipList.get("key1", 1).get_value()), "value1");
  // 指定 2 表示只能查找事务 id 小于等于 2 的值
  EXPECT_EQ((skipList.get("key1", 2).get_value()), "value2");
}

namespace {
using SkipListRecord = std::tuple<std::string, std::string, uint64_t>;

// 先判空再读取，避免查找失败时测试直接崩溃；同时核对真实版本号。
void expect_skiplist_record(SkipList &list, const std::string &key,
                            uint64_t read_id, const std::string &value,
                            uint64_t version) {
  SCOPED_TRACE("key=" + key + ", read_id=" + std::to_string(read_id));
  auto it = list.get(key, read_id);
  ASSERT_TRUE(it.is_valid());
  EXPECT_EQ(it.get_key(), key);
  EXPECT_EQ(it.get_value(), value);
  EXPECT_EQ(it.get_cur_tranc_id(), version);
}
} // namespace

// 写入顺序不能决定新旧；精确命中或落在版本间隙时都要选最大可见版本。
TEST(SkipListTest, MvccOutOfOrderVersionsAndReadBounds) {
  SkipList list;
  list.put("K", "v9", 9);
  list.put("K", "v5", 5);
  list.put("K", "v7", 7);

  const std::vector<std::pair<uint64_t, uint64_t>> cases = {
      {0, 9}, {5, 5}, {6, 5}, {7, 7}, {8, 7}, {9, 9}, {10, 9}};
  for (const auto &[read_id, version] : cases) {
    expect_skiplist_record(list, "K", read_id,
                           "v" + std::to_string(version), version);
  }
  EXPECT_TRUE(list.get("K", 4).is_end());
}

// K 的版本全部不可见时，即使后面的 L 可见，也不能把 L 当成查询结果。
TEST(SkipListTest, MvccInvisibleAndMissingKeysDoNotCrossKey) {
  SkipList list;
  EXPECT_TRUE(list.get("K", 5).is_end());
  list.put("A", "left", 1);
  list.put("K", "v9", 9);
  list.put("K", "v7", 7);
  list.put("L", "right", 1);

  auto invisible = list.get("K", 6);
  EXPECT_FALSE(invisible.is_valid());
  EXPECT_TRUE(invisible.is_end());
  for (const std::string key : {"0", "B", "Z"}) {
    SCOPED_TRACE(key);
    EXPECT_TRUE(list.get(key, 0).is_end());
    EXPECT_TRUE(list.get(key, 6).is_end());
  }
  expect_skiplist_record(list, "L", 6, "right", 1);
}

// 读取上限 0 表示不限制版本；记录自身的版本 0 仍然是一个普通旧版本。
// 同时覆盖 uint64_t 最大值，防止比较或定位时通过 id + 1 引入溢出。
TEST(SkipListTest, MvccZeroAndMaxTransactionIds) {
  SkipList list;
  const uint64_t max_id = std::numeric_limits<uint64_t>::max();
  list.put("K", "base", 0);
  list.put("K", "latest", max_id);
  list.put("K", "previous", max_id - 1);
  list.put("K", "base-updated", 0);

  expect_skiplist_record(list, "K", 0, "latest", max_id);
  expect_skiplist_record(list, "K", max_id, "latest", max_id);
  expect_skiplist_record(list, "K", max_id - 1, "previous", max_id - 1);
  expect_skiplist_record(list, "K", max_id - 2, "base-updated", 0);
  expect_skiplist_record(list, "K", 1, "base-updated", 0);
  EXPECT_EQ(list.flush().size(), 3u);
}

// 更新中间版本只能改变该节点；扩容、缩短、改墓碑时都要正确维护大小。
TEST(SkipListTest, MvccSameVersionUpdatePreservesHistoryAndSize) {
  SkipList list;
  list.put("K", "old", 2);
  list.put("K", "middle", 5);
  list.put("K", "new", 9);

  for (const std::string value : {"a-much-longer-value", "x", "", "restored"}) {
    SCOPED_TRACE(value);
    list.put("K", value, 5);
    expect_skiplist_record(list, "K", 5, value, 5);
    expect_skiplist_record(list, "K", 2, "old", 2);
    expect_skiplist_record(list, "K", 0, "new", 9);

    const std::vector<SkipListRecord> expected = {
        {"K", "new", 9}, {"K", value, 5}, {"K", "old", 2}};
    EXPECT_EQ(list.flush(), expected); // 仍然只有三个版本，没有重复节点。
    EXPECT_EQ(list.get_size(), 3 * (1 + sizeof(uint64_t)) +
                                   std::string("old").size() + value.size() +
                                   std::string("new").size());
  }
}

// SkipList 必须保留并返回墓碑，让上层识别删除；旧快照仍能读取旧值。
TEST(SkipListTest, MvccTombstoneAndReinsertPreserveSnapshots) {
  SkipList list;
  list.put("K", "", 9);
  list.put("K", "old", 5); // 旧版本后写入，也不能盖过新墓碑。
  expect_skiplist_record(list, "K", 0, "", 9);
  expect_skiplist_record(list, "K", 8, "old", 5);

  list.put("K", "reborn", 12);
  EXPECT_TRUE(list.get("K", 4).is_end());
  expect_skiplist_record(list, "K", 5, "old", 5);
  expect_skiplist_record(list, "K", 8, "old", 5);
  expect_skiplist_record(list, "K", 9, "", 9);
  expect_skiplist_record(list, "K", 11, "", 9);
  expect_skiplist_record(list, "K", 12, "reborn", 12);
  expect_skiplist_record(list, "K", 0, "reborn", 12);
}

// 底层遍历与 flush 均须保留全部版本和墓碑，且按 key 升序、版本降序排列。
TEST(SkipListTest, MvccIteratorAndFlushPreserveAllVersions) {
  SkipList list;
  list.put("b", "b3", 3);
  list.put("a", "a4", 4);
  list.put("b", "", 8);
  list.put("a", "a1", 1);
  list.put("c", "c2", 2);
  const std::vector<SkipListRecord> expected = {
      {"a", "a4", 4}, {"a", "a1", 1}, {"b", "", 8},
      {"b", "b3", 3}, {"c", "c2", 2}};

  std::vector<SkipListRecord> actual;
  for (auto it = list.begin(); it != list.end(); ++it) {
    ASSERT_LT(actual.size(), expected.size()); // 链表若成环，及时失败。
    ASSERT_TRUE(it.is_valid());
    actual.emplace_back(it.get_key(), it.get_value(), it.get_cur_tranc_id());
  }
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(list.flush(), expected);
  EXPECT_EQ(list.flush(), expected); // 导出本身不应清空或修改跳表。
}

// 前缀范围首尾的同 key 多版本不能被截断，墓碑也不能提前过滤。
TEST(SkipListTest, MvccPrefixRangePreservesAllVersions) {
  SkipList list;
  list.put("ant", "outside-left", 1);
  list.put("app", "old", 2);
  list.put("apple", "old", 3);
  list.put("app", "new", 8);
  list.put("apple", "", 9);
  list.put("apq", "outside-right", 4);
  const std::vector<SkipListRecord> expected = {
      {"app", "new", 8}, {"app", "old", 2},
      {"apple", "", 9}, {"apple", "old", 3}};

  const auto end = list.end_preffix("app");
  ASSERT_TRUE(end.is_valid());
  EXPECT_EQ(end.get_key(), "apq");
  std::vector<SkipListRecord> actual;
  for (auto it = list.begin_preffix("app"); it != end; ++it) {
    ASSERT_LT(actual.size(), expected.size());
    ASSERT_TRUE(it.is_valid());
    actual.emplace_back(it.get_key(), it.get_value(), it.get_cur_tranc_id());
  }
  EXPECT_EQ(actual, expected);
}

// 谓词区间 [b, d) 应包含 b、c 的全部版本，并停在 d 的第一个版本之前。
TEST(SkipListTest, MvccPredicateRangePreservesAllVersions) {
  SkipList list;
  list.put("a", "outside-left", 1);
  list.put("b", "b2", 2);
  list.put("c", "c3", 3);
  list.put("b", "", 9);
  list.put("c", "c8", 8);
  list.put("b", "b5", 5);
  list.put("d", "d4", 4);
  list.put("d", "d7", 7);

  auto range = list.iters_monotony_predicate([](const std::string &key) {
    if (key < "b")
      return 1;
    if (key >= "d")
      return -1;
    return 0;
  });
  ASSERT_TRUE(range.has_value());
  ASSERT_TRUE(range->second.is_valid());
  EXPECT_EQ(range->second.get_key(), "d");
  EXPECT_EQ(range->second.get_cur_tranc_id(), 7u);

  const std::vector<SkipListRecord> expected = {
      {"b", "", 9}, {"b", "b5", 5}, {"b", "b2", 2},
      {"c", "c8", 8}, {"c", "c3", 3}};
  std::vector<SkipListRecord> actual;
  for (auto it = range->first; it != range->second; ++it) {
    ASSERT_LT(actual.size(), expected.size());
    ASSERT_TRUE(it.is_valid());
    actual.emplace_back(it.get_key(), it.get_value(), it.get_cur_tranc_id());
  }
  EXPECT_EQ(actual, expected);
}

// 清空后旧版本、墓碑与大小统计全部消失，复用跳表也不能找回旧数据。
TEST(SkipListTest, MvccClearThenReuseDropsOldVersions) {
  SkipList list;
  list.put("K", "old", 5);
  list.put("K", "", 9);
  list.put("L", "other", 3);
  list.clear();

  EXPECT_EQ(list.get_size(), 0u);
  EXPECT_TRUE(list.begin() == list.end());
  EXPECT_TRUE(list.flush().empty());
  EXPECT_TRUE(list.get("K", 0).is_end());
  EXPECT_TRUE(list.get("K", 5).is_end());
  list.put("K", "fresh", 7);
  expect_skiplist_record(list, "K", 0, "fresh", 7);
  EXPECT_TRUE(list.get("K", 5).is_end());
  EXPECT_TRUE(list.get("L", 0).is_end());
  EXPECT_EQ(list.get_size(), 1 + std::string("fresh").size() + sizeof(uint64_t));
}

// 用固定种子打乱多个 key 的版本写入顺序，逐一验证所有读取上限。
TEST(SkipListTest, MvccShuffledVersionsMatchExpectedSnapshots) {
  SkipList list;
  constexpr int key_count = 8;
  constexpr uint64_t version_count = 24;
  std::vector<SkipListRecord> records;
  for (int key_index = 0; key_index < key_count; ++key_index) {
    const std::string key = "key" + std::to_string(key_index);
    for (uint64_t version = 1; version <= version_count; ++version) {
      // 版本号为 3、6、9……，故读取上限也会覆盖两个版本之间的间隙。
      const auto value = version % 7 == 0 ? "" : "v" + std::to_string(version);
      records.emplace_back(key, value, version * 3);
    }
  }
  std::mt19937 gen(20260930);
  std::shuffle(records.begin(), records.end(), gen);
  for (const auto &[key, value, id] : records) {
    list.put(key, value, id);
  }

  for (int key_index = 0; key_index < key_count; ++key_index) {
    const std::string key = "key" + std::to_string(key_index);
    for (uint64_t read_id = 0; read_id <= version_count * 3 + 2; ++read_id) {
      SCOPED_TRACE("key=" + key + ", read_id=" + std::to_string(read_id));
      const uint64_t version = read_id == 0
                                   ? version_count
                                   : std::min(read_id / 3, version_count);
      if (version == 0) {
        EXPECT_TRUE(list.get(key, read_id).is_end());
      } else {
        const auto value = version % 7 == 0 ? "" : "v" + std::to_string(version);
        expect_skiplist_record(list, key, read_id, value, version * 3);
      }
    }
  }
}

// ! 现在的实现, 并发的锁由 SkipList 的上层 MemTable 实现, 因此不需要测试
// SkipList 的并发性
// // 测试跳表的并发性能
// TEST(SkipListTest, ConcurrentOperations) {
//   SkipList skipList;
//   const int num_readers = 4;       // 读线程数
//   const int num_writers = 2;       // 写线程数
//   const int num_operations = 1000; // 每个线程的操作数

//   // 用于同步所有线程的开始
//   std::atomic<bool> start{false};
//   // 用于等待所有线程完成
//   std::latch completion_latch((num_readers + num_writers));

//   // 记录写入的键，用于验证
//   std::vector<std::string> inserted_keys;
//   std::mutex keys_mutex;

//   // 写线程函数
//   auto writer_func = [&](int thread_id) {
//     // 等待开始信号
//     while (!start) {
//       std::this_thread::yield();
//     }

//     // 执行写操作
//     for (int i = 0; i < num_operations; ++i) {
//       std::string key =
//           "key_" + std::to_string(thread_id) + "_" + std::to_string(i);
//       std::string value =
//           "value_" + std::to_string(thread_id) + "_" + std::to_string(i);

//       if (i % 2 == 0) {
//         // 插入操作
//         skipList.put(key, value);
//         {
//           std::lock_guard<std::mutex> lock(keys_mutex);
//           inserted_keys.push_back(key);
//         }
//       } else {
//         // 删除操作
//         skipList.remove(key);
//       }

//       // 随机休眠一小段时间，模拟实际工作负载
//       std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
//     }

//     completion_latch.count_down();
//   };

//   // 读线程函数
//   auto reader_func = [&](int thread_id) {
//     // 等待开始信号
//     while (!start) {
//       std::this_thread::yield();
//     }

//     int found_count = 0;
//     // 执行读操作
//     for (int i = 0; i < num_operations; ++i) {
//       // 随机选择一个已插入的key进行查询
//       std::string key_to_find;
//       {
//         std::lock_guard<std::mutex> lock(keys_mutex);
//         if (!inserted_keys.empty()) {
//           key_to_find = inserted_keys[rand() % inserted_keys.size()];
//         }
//       }

//       if (!key_to_find.empty()) {
//         auto result = skipList.get(key_to_find);
//         if (result.is_valid()) {
//           found_count++;
//         }
//       }

//       // 每隔一段时间进行一次遍历操作
//       if (i % 100 == 0) {
//         std::vector<std::pair<std::string, std::string>> items;
//         for (auto it = skipList.begin(); it != skipList.end(); ++it) {
//           items.push_back(*it);
//         }
//       }

//       std::this_thread::sleep_for(std::chrono::microseconds(rand() % 50));
//     }

//     completion_latch.count_down();
//   };

//   // 创建并启动写线程
//   std::vector<std::thread> writers;
//   for (int i = 0; i < num_writers; ++i) {
//     writers.emplace_back(writer_func, i);
//   }

//   // 创建并启动读线程
//   std::vector<std::thread> readers;
//   for (int i = 0; i < num_readers; ++i) {
//     readers.emplace_back(reader_func, i);
//   }

//   // 给线程一点时间进入等待状态
//   std::this_thread::sleep_for(std::chrono::milliseconds(100));

//   // 记录开始时间
//   auto start_time = std::chrono::high_resolution_clock::now();

//   // 发送开始信号
//   start = true;

//   // 等待所有线程完成
//   completion_latch.wait();

//   // 记录结束时间
//   auto end_time = std::chrono::high_resolution_clock::now();
//   auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
//       end_time - start_time);

//   // 等待所有线程结束
//   for (auto &w : writers) {
//     w.join();
//   }
//   for (auto &r : readers) {
//     r.join();
//   }

//   // 验证跳表的最终状态
//   size_t final_size = 0;
//   for (auto it = skipList.begin(); it != skipList.end(); ++it) {
//     final_size++;
//   }

//   //   std::cout << "Concurrent test completed in " << duration.count()
//   //             << "ms\nFinal skiplist size: " << final_size << std::endl;

//   // 基本正确性检查
//   EXPECT_GT(final_size, 0); // 跳表不应该为空
//   EXPECT_LE(final_size,
//             num_writers * num_operations); // 跳表大小不应超过最大可能值
// }

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
