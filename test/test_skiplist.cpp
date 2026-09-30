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

// 阅读约定：K@9 表示 key 为 K、tranc_id 为 9 的记录；读取上限 0 表示不限制版本。
// SkipList 的 remove 是物理摘除节点；MVCC 的逻辑删除通过写入空 value（墓碑）表示。

// 目的：验证单个 key 的插入、同版本更新和物理删除能连续工作。
// 场景：以版本 0 写入 key1，再覆盖它的值，最后调用 remove。
// 预期：两次查询分别得到原值、新值；删除后查询返回无效迭代器。
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

// 目的：验证 begin/end、解引用和 ++ 能遍历完整的有序结果。
// 场景：插入 key1、key2、key3，从 begin 连续前进到 end 并收集记录。
// 预期：恰好得到三个节点，key 的顺序为 key1、key2、key3。
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

// 目的：验证节点较多时，插入位置和查询寻路仍然正确。
// 场景：写入 10000 个不同 key，再按 key 逐条读取。
// 预期：每个 key 都返回对应 value；这是功能测试，不测吞吐或延迟。
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

// 目的：验证连续物理删除大量节点后，查询不会沿旧链接找到残留记录。
// 场景：插入 10000 个不同 key，然后逐个 remove，再逐个查询。
// 预期：所有已删除 key 的查询结果都无效。
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

// 目的：验证同一个 key、同一个版本号重复写入时，查询使用最后写入的值。
// 场景：连续三次以版本 0 写入 key1，值依次为 value1、value2、value3。
// 预期：最终查询返回 value3；节点数量另由同版本 MVCC 更新用例检查。
TEST(SkipListTest, DuplicateInsert) {
  SkipList skipList;

  // 重复插入相同的key
  skipList.put("key1", "value1", 0);
  skipList.put("key1", "value2", 0);
  skipList.put("key1", "value3", 0);

  // 验证最后一次插入的值
  EXPECT_EQ((skipList.get("key1", 0).get_value()), "value3");
}

// 目的：验证空表的查询和删除边界，避免访问不存在的节点。
// 场景：对刚创建的空跳表查询、删除一个不存在的 key。
// 预期：查询返回无效迭代器，删除操作正常返回。
TEST(SkipListTest, EmptySkipList) {
  SkipList skipList;

  // 验证空跳表的查找和删除
  EXPECT_FALSE(skipList.get("nonexistent_key", 0).is_valid());
  skipList.remove("nonexistent_key"); // 删除不存在的key
}

// 目的：验证插入和物理删除反复交替时，目标 key 的存在状态仍然正确。
// 场景：在 1000 个候选 key 中随机选择并操作 10000 次，用集合记录存在状态。
// 预期：每次插入后查到刚写入的值，每次删除后查不到该 key。
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

// 目的：验证 size_bytes 按 key 字节数 + value 字节数 + 版本号字节数记账。
// 场景：插入两个节点，物理删除其中一个，最后 clear。
// 预期：统计值分别等于两个节点之和、剩余节点大小、0；不含指针等额外开销。
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

// 目的：验证前缀范围的起点、右侧开区间边界以及无匹配时的行为。
// 场景：插入 apple、banana、cherry 等 key，查询 ap、a、cherry 和不存在的前缀。
// 预期：起点指向对应首条记录；a 的终点为 banana，cherry 的终点为 end。
//       不存在的前缀其起点与终点相等，表示空范围。
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

// 目的：验证单调谓词既能表达前缀匹配，也能表达左闭右开的 key 区间。
// 场景：分别查询 pre 前缀和 [l, n)，检查起点、终点及区间内的迭代顺序。
// 预期：前者按序返回 prefix1～prefix3；后者从 longerkey 到 midway，终点为 other。
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
            "other"); // other 是区间右侧第一个节点，不属于 [l, n)。
  EXPECT_EQ(range_begin_iter.get_key(), "longerkey");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter.get_key(), "medium");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter.get_key(), "midpoint");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter.get_key(), "midway");
}

// 目的：验证大量节点中的窄区间定位，以及物理删除后迭代是否跨过缺口。
// 场景：插入 key0000～key9999，删除 key1015，查询 [key1010, key1020)。
// 预期：起终点分别为 key1010、key1020；从起点前进五次到 key1016。
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

// 目的：验证最基本的 MVCC 行为：不同版本共存，查询受读取上限约束。
// 场景：同一个 key 写入版本 1、2，分别用读取上限 0、1、2 查询。
// 预期：上限 0 和 2 返回版本 2 的值，上限 1 仍能读取版本 1 的旧值。
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

// 目的：验证版本选择按版本号大小判断，不依赖插入先后顺序。
// 场景：按 K@9、K@5、K@7 的顺序写入，查询精确版本、版本间隙和范围外上限。
// 预期：上限 8 返回 K@7，上限 0 返回 K@9，上限 4 返回空；其他上限同理。
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

// 目的：验证不可见与不存在的 key 都返回空，查找不能越过目标 key 后误返回邻居。
// 场景：K 仅有版本 9、7，相邻 A、L 的版本为 1；用上限 6 查 K，并查多个缺失 key。
// 预期：K 和缺失 key 均查不到，而 L 仍能正常读到；同时覆盖空表查询。
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

// 目的：区分读取上限 0 与记录版本 0，并验证 uint64_t 最大版本号附近的边界。
// 场景：写入版本 0、MAX、MAX-1，再更新版本 0；用 0、MAX、MAX-1、MAX-2、1 查询。
// 预期：读取上限 0 选 MAX，小上限选版本 0；中间边界选择正确，最终仍只有三个版本。
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

// 目的：验证同版本原位更新只影响目标记录，且正确调整大小统计。
// 场景：保存 K@2、K@5、K@9，反复把 K@5 改为长值、短值、墓碑和恢复后的值。
// 预期：K@2、K@9 不变，始终只有三个节点，flush 内容及 size_bytes 与新值一致。
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

// 目的：验证墓碑也是一个版本，删除与重新写入都不会破坏历史读取。
// 场景：先写 K@9 墓碑，再补写 K@5 旧值，最后写 K@12 新值。
// 预期：上限 5～8 读旧值、9～11 读墓碑、12 读新值；上限 0 始终选最大版本。
//       墓碑在 SkipList 中是有效记录，不能跳过它返回更旧的值。
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

// 目的：验证底层遍历和 flush 提供完整有序记录，不提前去重或丢弃墓碑。
// 场景：交错写入 a、b、c 的五条记录，其中 a、b 有多个版本，b 的最新版本为墓碑。
// 预期：迭代和两次 flush 均得到相同的五条记录，按 key 升序、同 key 版本降序排列。
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

// 目的：验证前缀区间完整包含首尾 key 的全部版本，并排除区间外的 key。
// 场景：app、apple 各保存两个版本，其中包含墓碑；左右还有 ant、apq，查询 app 前缀。
// 预期：依次返回 app@8、app@2、apple@9 墓碑、apple@3，右侧开区间边界指向 apq。
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

// 目的：验证谓词查询在多版本场景下既不截断区间内版本，也不包含右边界记录。
// 场景：b 有三个版本、c 有两个版本，a、d 位于区间外；查询 [b, d)。
// 预期：返回 b、c 的全部五条记录（含墓碑），终点指向 d 的最大版本 d@7。
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

// 目的：验证 clear 清除全部历史记录，并允许同一个跳表对象重新使用。
// 场景：写入 K 的旧值和墓碑以及 L，clear 后重新写入 K@7。
// 预期：清空后遍历、flush 和查询为空且大小为 0；复用后只能查到 K@7，旧数据不残留。
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

// 目的：用较多 key 和版本组合验证可见性规则，补充少量手写样例的覆盖。
// 场景：8 个 key 各有 24 个版本（版本号 3、6、9……，部分为墓碑），固定种子打乱写入。
// 预期：对每个 key 检查上限 0～74，共 600 次查询，均返回最大可见版本或正确的空结果。
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

// 目的：验证物理 remove 每次只摘除最大版本，并维护剩余节点的链接和大小统计。
// 场景：K 有版本 9（墓碑）、7、5，左右有 A、L；连续删除 K，最后再重复删除一次。
// 预期：K 依次剩下 7/5、5、空；谓词查询不再返回已删节点，A、L 和最终大小保持正确。
//       这是跳表物理删除的约定，数据库逻辑删除仍然通过 put(key, "", id) 完成。
TEST(SkipListTest, MvccPhysicalRemovePreservesRemainingVersions) {
  SkipList list;
  list.put("A", "left", 1);
  list.put("K", "old", 5);
  list.put("K", "", 9);
  list.put("K", "middle", 7);
  list.put("L", "right", 1);

  list.remove("K");
  expect_skiplist_record(list, "K", 0, "middle", 7);
  expect_skiplist_record(list, "K", 6, "old", 5);
  const std::vector<SkipListRecord> after_first = {
      {"A", "left", 1}, {"K", "middle", 7},
      {"K", "old", 5}, {"L", "right", 1}};
  EXPECT_EQ(list.flush(), after_first);

  // 谓词查询会使用 backward_，也应看不到已摘除的墓碑节点。
  auto range = list.iters_monotony_predicate([](const std::string &key) {
    return key < "K" ? 1 : (key > "K" ? -1 : 0);
  });
  ASSERT_TRUE(range.has_value());
  const std::vector<uint64_t> expected_versions = {7, 5};
  std::vector<uint64_t> versions;
  for (auto it = range->first; it != range->second; ++it) {
    ASSERT_LT(versions.size(), expected_versions.size());
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it.get_key(), "K");
    versions.push_back(it.get_cur_tranc_id());
  }
  EXPECT_EQ(versions, expected_versions);

  list.remove("K");
  expect_skiplist_record(list, "K", 0, "old", 5);
  list.remove("K");
  EXPECT_TRUE(list.get("K", 0).is_end());
  const std::vector<SkipListRecord> remaining = {
      {"A", "left", 1}, {"L", "right", 1}};
  EXPECT_EQ(list.flush(), remaining);
  EXPECT_EQ(list.get_size(), 2 * (1 + sizeof(uint64_t)) +
                                 std::string("left").size() +
                                 std::string("right").size());
  const auto size_before = list.get_size();
  list.remove("K"); // 已无任何版本，重复删除不应改变大小。
  EXPECT_EQ(list.get_size(), size_before);
  EXPECT_EQ(list.flush(), remaining);
}

// 目的：旧版用例尝试检查并发读、写和遍历能否完成，以及最终节点数是否合理。
// 场景：4 个读线程和 2 个写线程各执行 1000 次操作，结束后遍历统计节点数。
// 预期：原用例要求最终节点数大于 0 且不超过写入操作数，不逐条核对并发读取结果。
// 状态：已停用。当前 SkipList 不自行加锁，并发保护由上层 MemTable 负责；不计入运行用例。
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
