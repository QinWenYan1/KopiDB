#include "iterator/iterator.h"
#include "logger/logger.h"
#include "memtable/memtable.h"
#include <gtest/gtest.h>
#include <iomanip>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <chrono>

using namespace ::tiny_lsm;

// 阅读约定：K@9 表示 key 为 K、tranc_id 为 9 的记录；读取上限 0 表示不限制版本。
// MemTable 的点查询保留墓碑供上层识别；默认范围遍历会隐藏被最新可见墓碑删除的 key。

// 目的：验证 MemTable 能正确封装底层跳表的写入、同版本更新和查询。
// 场景：以版本 0 写入 key1，更新它的值，并查询另一个不存在的 key。
// 预期：两次读取 key1 分别得到原值、新值，不存在的 key 返回无效迭代器。
TEST(MemTableTest, BasicOperations) {
  MemTable memtable;

  // 测试插入和查找
  memtable.put("key1", "value1", 0);
  EXPECT_EQ(memtable.get("key1", 0).get_value(), "value1");

  // 测试更新
  memtable.put("key1", "new_value", 0);
  EXPECT_EQ(memtable.get("key1", 0).get_value(), "new_value");

  // 测试不存在的key
  EXPECT_FALSE(memtable.get("nonexistent", 0).is_valid());
}

// 目的：验证 MemTable 的 remove 通过墓碑表达删除，而非物理摘除跳表节点。
// 场景：删除一个已写入的 key，再删除一个从未写入的 key，均使用版本 0。
// 预期：两次点查询都能取得空 value，表示对应位置保存了墓碑。
TEST(MemTableTest, RemoveOperations) {
  MemTable memtable;

  // 插入并删除
  memtable.put("key1", "value1", 0);
  memtable.remove("key1", 0);
  EXPECT_TRUE(memtable.get("key1", 0).get_value().empty());

  // 删除不存在的key
  memtable.remove("nonexistent", 0);
  EXPECT_TRUE(memtable.get("nonexistent", 0).get_value().empty());
}

// 目的：验证一次冻结后，查询仍能同时访问冻结表和新的活跃表。
// 场景：写入 key1、key2 后冻结，再向新活跃表写入 key3。
// 预期：三个 key 均能查到各自的值，切换活跃表不会丢失旧表数据。
TEST(MemTableTest, FrozenTableOperations) {
  MemTable memtable;

  // 在当前表中插入数据
  memtable.put("key1", "value1", 0);
  memtable.put("key2", "value2", 0);

  // 冻结当前表
  memtable.frozen_cur_table();

  // 在新的当前表中插入数据
  memtable.put("key3", "value3", 0);

  // 验证所有数据都能被访问到
  EXPECT_EQ(memtable.get("key1", 0).get_value(), "value1");
  EXPECT_EQ(memtable.get("key2", 0).get_value(), "value2");
  EXPECT_EQ(memtable.get("key3", 0).get_value(), "value3");
}

// 目的：验证 MemTable 在较多记录下仍能正确传递写入和点查询操作。
// 场景：写入 1000 个不同 key，随后逐个查询并与对应 value 比较。
// 预期：每个 key 都返回正确值；本用例不要求一定触发自动冻结，也不测性能。
TEST(MemTableTest, LargeScaleOperations) {
  MemTable memtable;
  const int num_entries = 1000;

  // 插入大量数据
  for (int i = 0; i < num_entries; i++) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::to_string(i);
    memtable.put(key, value, 0);
  }

  // 验证数据
  for (int i = 0; i < num_entries; i++) {
    std::string key = "key" + std::to_string(i);
    std::string expected = "value" + std::to_string(i);
    EXPECT_EQ(memtable.get(key, 0).get_value(), expected);
  }
}

// 目的：验证活跃表冻结时，其统计大小会转移到冻结表统计中。
// 场景：检查空表总大小，写入一条记录，记住冻结前总大小，再执行冻结。
// 预期：空表大小为 0，写入后活跃表大小大于 0，冻结表大小等于冻结前总大小。
TEST(MemTableTest, MemorySizeTracking) {
  MemTable memtable;

  // 初始大小应该为0
  EXPECT_EQ(memtable.get_total_size(), 0);

  // 添加数据后大小应该增加
  memtable.put("key1", "value1", 0);
  EXPECT_GT(memtable.get_cur_size(), 0);

  // 冻结表后，frozen_size应该增加
  size_t size_before_freeze = memtable.get_total_size();
  memtable.frozen_cur_table();
  EXPECT_EQ(memtable.get_frozen_size(), size_before_freeze);
}

// 目的：验证查询会访问多个冻结表，不会只检查队头或活跃表。
// 场景：key1、key2 分别写入并冻结，key3 留在当前活跃表。
// 预期：三个 key 分布在三张表中，仍然都能正确读取。
TEST(MemTableTest, MultipleFrozenTables) {
  MemTable memtable;

  // 第一次冻结
  memtable.put("key1", "value1", 0);
  memtable.frozen_cur_table();

  // 第二次冻结
  memtable.put("key2", "value2", 0);
  memtable.frozen_cur_table();

  // 在当前表中添加数据
  memtable.put("key3", "value3", 0);

  // 验证所有数据都能访问
  EXPECT_EQ(memtable.get("key1", 0).get_value(), "value1");
  EXPECT_EQ(memtable.get("key2", 0).get_value(), "value2");
  EXPECT_EQ(memtable.get("key3", 0).get_value(), "value3");
}

// 目的：验证跨表归并遍历能处理覆盖、删除和删除后重写，并按 key 排序。
// 场景：三批版本 0 的写入穿插两次冻结，包含多次更新 key2、删除后重写 key1、删除 key3。
// 预期：各阶段结果正确；最终只有 key1、key2、key4、key5，使用最新表中的值。
//       遍历隐藏 key3，而点查询仍返回 key3 的墓碑，供上层判断删除状态。
TEST(MemTableTest, IteratorComplexOperations) {
  MemTable memtable;

  // 第一批操作：基本插入
  memtable.put("key1", "value1", 0);
  memtable.put("key2", "value2", 0);
  memtable.put("key3", "value3", 0);

  // 验证第一批操作
  std::vector<std::pair<std::string, std::string>> result1;
  for (auto it = memtable.begin(0); it != memtable.end(); ++it) {
    result1.push_back(*it);
  }
  ASSERT_EQ(result1.size(), 3);
  EXPECT_EQ(result1[0].first, "key1");
  EXPECT_EQ(result1[0].second, "value1");
  EXPECT_EQ(result1[2].second, "value3");

  // 冻结当前表
  memtable.frozen_cur_table();

  // 第二批操作：更新和删除
  memtable.put("key2", "value2_updated", 0); // 更新已存在的key
  memtable.remove("key1", 0);                // 删除一个key
  memtable.put("key4", "value4", 0);         // 插入新key

  // 验证第二批操作
  std::vector<std::pair<std::string, std::string>> result2;
  for (auto it = memtable.begin(0); it != memtable.end(); ++it) {
    result2.push_back(*it);
  }
  ASSERT_EQ(result2.size(), 3); // key1被删除，key4被添加
  EXPECT_EQ(result2[0].first, "key2");
  EXPECT_EQ(result2[0].second, "value2_updated");
  EXPECT_EQ(result2[2].first, "key4");

  // 再次冻结当前表
  memtable.frozen_cur_table();

  // 第三批操作：混合操作
  memtable.put("key1", "value1_new", 0); // 重新插入被删除的key
  memtable.remove("key3", 0);            // 删除一个在第一个frozen table中的key
  memtable.put("key2", "value2_final", 0); // 再次更新key2
  memtable.put("key5", "value5", 0);       // 插入新key

  // 验证最终结果
  std::vector<std::pair<std::string, std::string>> final_result;
  for (auto it = memtable.begin(0); it != memtable.end(); ++it) {
    final_result.push_back(*it);
  }

  // 验证最终状态
  ASSERT_EQ(final_result.size(), 4); // key1, key2, key4, key5

  // 验证具体内容
  EXPECT_EQ(final_result[0].first, "key1");
  EXPECT_EQ(final_result[0].second, "value1_new");

  EXPECT_EQ(final_result[1].first, "key2");
  EXPECT_EQ(final_result[1].second, "value2_final");

  EXPECT_EQ(final_result[2].first, "key4");
  EXPECT_EQ(final_result[2].second, "value4");

  EXPECT_EQ(final_result[3].first, "key5");
  EXPECT_EQ(final_result[3].second, "value5");

  // 验证被删除的key确实不存在
  bool has_key3 = false;
  auto res = memtable.get("key3", 0);
  EXPECT_TRUE(res.get_value().empty());
}

// 目的：对 MemTable 的并发读写、遍历及冻结进行基本运行和统计检查。
// 场景：2 个写线程、4 个读线程各执行 1000 次操作，另一个线程执行 5 次冻结。
// 预期：线程完成，最终总大小大于 0，记录数不超过写入操作数，冻结时大小关系合理。
// 覆盖边界：未逐条断言并发读取的值，也未验证完整的事务隔离或所有数据竞争场景。
TEST(MemTableTest, ConcurrentOperations) {
  MemTable memtable;
  const int num_readers = 4;       // 读线程数
  const int num_writers = 2;       // 写线程数
  const int num_operations = 1000; // 每个线程的操作数

  // 用于同步所有线程的开始
  std::atomic<bool> start{false};
  // 用于等待所有线程完成
  std::atomic<int> completion_counter{num_readers + num_writers +
                                      1}; // +1 for freeze thread

  // 记录写入的键，用于验证
  std::vector<std::string> inserted_keys;
  std::mutex keys_mutex;

  // 写线程函数
  auto writer_func = [&](int thread_id) {
    while (!start) {
      std::this_thread::yield();
    }

    for (int i = 0; i < num_operations; ++i) {
      std::string key =
          "key_" + std::to_string(thread_id) + "_" + std::to_string(i);
      std::string value =
          "value_" + std::to_string(thread_id) + "_" + std::to_string(i);

      if (i % 3 == 0) {
        // 插入操作
        memtable.put(key, value, 0);
        {
          std::lock_guard<std::mutex> lock(keys_mutex);
          inserted_keys.push_back(key);
        }
      } else if (i % 3 == 1) {
        // 删除操作
        memtable.remove(key, 0);
      } else {
        // 更新操作
        memtable.put(key, value + "_updated", 0);
      }

      std::this_thread::sleep_for(std::chrono::microseconds(rand() % 100));
    }

    completion_counter--;
  };

  // 读线程函数
  auto reader_func = [&](int thread_id) {
    while (!start) {
      std::this_thread::yield();
    }

    int found_count = 0;
    for (int i = 0; i < num_operations; ++i) {
      // 随机选择一个已插入的key进行查询
      std::string key_to_find;
      {
        std::lock_guard<std::mutex> lock(keys_mutex);
        if (!inserted_keys.empty()) {
          key_to_find = inserted_keys[rand() % inserted_keys.size()];
        }
      }

      if (!key_to_find.empty()) {
        auto result = memtable.get(key_to_find, 0);
        if (result.is_valid()) {
          found_count++;
        }
      }

      // 每隔一段时间进行一次遍历操作
      if (i % 100 == 0) {
        std::vector<std::pair<std::string, std::string>> items;
        for (auto it = memtable.begin(0); it != memtable.end(); ++it) {
          items.push_back(*it);
        }
      }

      std::this_thread::sleep_for(std::chrono::microseconds(rand() % 50));
    }

    completion_counter--;
  };

  // 冻结线程函数
  auto freeze_func = [&]() {
    while (!start) {
      std::this_thread::yield();
    }

    // 定期执行冻结操作
    for (int i = 0; i < 5; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      memtable.frozen_cur_table();

      // 验证冻结后的表
      size_t frozen_size = memtable.get_frozen_size();
      EXPECT_GE(frozen_size, 0);

      // 验证总大小
      size_t total_size = memtable.get_total_size();
      EXPECT_GE(total_size, frozen_size);
    }

    completion_counter--;
  };

  // 创建并启动写线程
  std::vector<std::thread> writers;
  for (int i = 0; i < num_writers; ++i) {
    writers.emplace_back(writer_func, i);
  }

  // 创建并启动读线程
  std::vector<std::thread> readers;
  for (int i = 0; i < num_readers; ++i) {
    readers.emplace_back(reader_func, i);
  }

  // 创建并启动冻结线程
  std::thread freeze_thread(freeze_func);

  // 给线程一点时间进入等待状态
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 记录开始时间
  auto start_time = std::chrono::high_resolution_clock::now();

  // 发送开始信号
  start = true;

  // 等待所有线程完成
  while (completion_counter > 0) {
    std::this_thread::yield();
  }

  // 记录结束时间
  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);

  // 等待所有线程结束
  for (auto &w : writers) {
    w.join();
  }
  for (auto &r : readers) {
    r.join();
  }
  freeze_thread.join();

  // 验证最终状态
  size_t final_size = 0;
  for (auto it = memtable.begin(0); it != memtable.end(); ++it) {
    final_size++;
  }

  // 输出统计信息
  // std::cout << "Concurrent test completed in " << duration.count()
  //           << "ms\nFinal memtable size: " << final_size
  //           << "\nTotal size: " << memtable.get_total_size()
  //           << "\nFrozen size: " << memtable.get_frozen_size() << std::endl;

  // 基本正确性检查
  EXPECT_GT(memtable.get_total_size(), 0);             // 总大小应该大于0
  EXPECT_LE(final_size, num_writers * num_operations); // 大小不应超过最大可能值
}

// 目的：验证前缀查询能合并多张表，应用更新和墓碑，并排除其他前缀。
// 场景：三批数据经过两次冻结，更新 abc，删除 ab、abcd，查询前缀 ab。
// 预期：按序得到 abc、abcde、abcdef、abcdefg、abcdefgh，abc 使用更新后的值。
// 覆盖边界：当前断言逐项检查返回内容，没有单独断言结果总数。
TEST(MemTableTest, PreffixIter) {
  MemTable memtable;

  // 在当前表中插入数据
  memtable.put("abc", "3", 0);
  memtable.put("abcde", "5", 0);
  memtable.put("abcd", "4", 0);
  memtable.put("xxx", "-1", 0);
  memtable.put("abcdef", "6", 0);
  memtable.put("yyyy", "-1", 0);

  // 冻结当前表
  memtable.frozen_cur_table();

  // 在新的当前表中插入数据
  memtable.put("zz", "-1", 0);
  memtable.put("abcdefg", "7", 0);
  memtable.remove("abcd", 0);
  memtable.put("abcdefgh", "8", 0);
  memtable.put("ab", "2", 0);
  memtable.put("wwwwww", "-1", 0);

  // 冻结当前表
  memtable.frozen_cur_table();

  // 在新的当前表中插入数据
  memtable.put("mmmmm", "-1", 0);
  memtable.remove("ab", 0);
  memtable.put("abc", "33", 0);

  int id = 0;
  std::vector<std::pair<std::string, std::string>> answer{{"abc", "33"},
                                                          {"abcde", "5"},
                                                          {"abcdef", "6"},
                                                          {"abcdefg", "7"},
                                                          {"abcdefgh", "8"}};

  for (auto it = memtable.iters_preffix("ab", 0); !it.is_end(); ++it) {
    EXPECT_EQ(it->first, answer[id].first);
    EXPECT_EQ(it->second, answer[id].second);
    id++;
  }
}

// 目的：验证 MemTable 能把单调谓词查询结果转换为可遍历的 HeapIterator 区间。
// 场景：插入多组 key，分别用谓词查询 pre 前缀及 [l, n) 区间。
// 预期：前者返回 prefix1～prefix3；后者依次返回 longerkey、medium、midpoint、midway 后结束。
TEST(MemTableTest, ItersPredicate_Base) {
  MemTable memtable;
  memtable.put("prefix1", "value1", 0);
  memtable.put("prefix2", "value2", 0);
  memtable.put("prefix3", "value3", 0);
  memtable.put("other", "value4", 0);
  memtable.put("longerkey", "value5", 0);
  memtable.put("averylongkey", "value6", 0);
  memtable.put("medium", "value7", 0);
  memtable.put("midway", "value8", 0);
  memtable.put("midpoint", "value9", 0);

  // 测试前缀匹配
  auto prefix_result =
      memtable.iters_monotony_predicate(0, [](const std::string &key) {
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
  EXPECT_EQ(prefix_begin_iter->first, "prefix1");
  EXPECT_TRUE(prefix_end_iter.is_end());

  EXPECT_EQ(prefix_begin_iter->second, "value1");
  ++prefix_begin_iter;
  EXPECT_EQ(prefix_begin_iter->second, "value2");
  ++prefix_begin_iter;
  EXPECT_EQ(prefix_begin_iter->second, "value3");

  // 测试范围匹配
  auto range = std::make_pair("l", "n"); // [l, n)
  auto range_result =
      memtable.iters_monotony_predicate(0, [&range](const std::string &key) {
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
  EXPECT_EQ(range_begin_iter->first, "longerkey");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter->first, "medium");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter->first, "midpoint");
  ++range_begin_iter;
  EXPECT_EQ(range_begin_iter->first, "midway");
  ++range_begin_iter;
  EXPECT_TRUE(range_begin_iter.is_end());
}

// 目的：验证大量记录中的窄区间查询能够处理墓碑，并正确结束迭代。
// 场景：写入 key0000～key9999，逻辑删除 key1015，查询 [key1010, key1020)。
// 预期：起点为 key1010，前进五次到 key1016，继续推进后到 end，不返回删除的 key。
TEST(MemTableTest, ItersPredicate_Large) {
  MemTable memtable;
  int num = 10000;

  for (int i = 0; i < num; ++i) {
    std::ostringstream oss_key;
    std::ostringstream oss_value;

    // 设置数字为4位长度，不足的部分用前导零填充
    oss_key << "key" << std::setw(4) << std::setfill('0') << i;
    oss_value << "value" << std::setw(4) << std::setfill('0') << i;

    std::string key = oss_key.str();
    std::string value = oss_value.str();

    memtable.put(key, value, 0);
  }

  memtable.remove("key1015", 0);

  auto result =
      memtable.iters_monotony_predicate(0, [](const std::string &key) {
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
  EXPECT_EQ(range_begin_iter->first, "key1010");
  for (int i = 0; i < 5; i++) {
    ++range_begin_iter;
  }
  EXPECT_EQ(range_begin_iter->first, "key1016");
  for (int i = 0; i < 5; i++) {
    ++range_begin_iter;
  }
  EXPECT_TRUE(range_begin_iter.is_end());
}

// 目的：验证单条和批量点查询跨表传递读取上限，同时保留墓碑及真实版本号。
// 场景：两张冻结表分别存 K@5、K@9，活跃表存 K@12 墓碑；用多个读取上限查询。
// 预期：上限 8 读版本 5，11 读版本 9，12 或 0 读墓碑；上限 4 及缺失 key 查不到。
TEST(MemTableTest, MvccReadBoundsAcrossFrozenTables) {
  MemTable memtable;
  memtable.put("K", "old", 5);
  memtable.frozen_cur_table();
  memtable.put("K", "new", 9);
  memtable.frozen_cur_table();
  memtable.remove("K", 12);

  struct ReadCase {
    uint64_t read_id;
    std::string value;
    uint64_t version;
  };
  const std::vector<ReadCase> cases = {
      {5, "old", 5}, {8, "old", 5}, {9, "new", 9},
      {11, "new", 9}, {12, "", 12}, {0, "", 12}};
  EXPECT_TRUE(memtable.get("K", 4).is_end());
  for (const auto &entry : cases) {
    SCOPED_TRACE(entry.read_id);
    auto it = memtable.get("K", entry.read_id);
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it.get_value(), entry.value);
    EXPECT_EQ(it.get_cur_tranc_id(), entry.version);

    const auto batch = memtable.get_batch({"K", "missing"}, entry.read_id);
    ASSERT_EQ(batch.size(), 2u);
    ASSERT_TRUE(batch[0].second.has_value());
    EXPECT_EQ(batch[0].second->first, entry.value);
    EXPECT_EQ(batch[0].second->second, entry.version);
    EXPECT_FALSE(batch[1].second.has_value());
  }
}

// 目的：验证范围查询先筛选可见版本，再去重并处理墓碑，避免旧值错误地重新出现。
// 场景：冻结表中有 K@5、K@9，活跃表中有 K@12 墓碑，分别进行全量、前缀和谓词遍历。
// 预期：上限 5/8 只返回 K@5，9/11 只返回 K@9；上限 4、12、0 的范围结果为空。
TEST(MemTableTest, MvccRangesFilterVersionsBeforeDeduplication) {
  MemTable memtable;
  memtable.put("K", "old", 5);
  memtable.frozen_cur_table();
  memtable.put("K", "new", 9);
  memtable.frozen_cur_table();
  memtable.remove("K", 12);

  for (uint64_t read_id : {0, 4, 5, 8, 9, 11, 12}) {
    SCOPED_TRACE(read_id);
    const bool has_value = read_id >= 5 && read_id < 12;
    auto check = [&](HeapIterator it) {
      if (!has_value) {
        EXPECT_TRUE(it.is_end());
        return;
      }
      ASSERT_TRUE(it.is_valid());
      EXPECT_EQ(it->first, "K");
      EXPECT_EQ(it->second, read_id < 9 ? "old" : "new");
      EXPECT_EQ(it.get_cur_tranc_id(), read_id < 9 ? 5u : 9u);
      ++it;
      EXPECT_TRUE(it.is_end()); // 同 key 的其他版本不能再次返回。
    };
    check(memtable.begin(read_id));
    check(memtable.iters_preffix("K", read_id));
    auto range = memtable.iters_monotony_predicate(
        read_id, [](const std::string &key) {
          return key < "K" ? 1 : (key > "K" ? -1 : 0);
        });
    if (has_value) {
      ASSERT_TRUE(range.has_value());
    }
    if (range.has_value()) {
      check(range->first);
      EXPECT_TRUE(range->second.is_end());
    }
  }
}

// 目的：验证跨冻结表选值依据真实版本号，单条查询、批量查询与遍历保持一致。
// 场景：先写 K@9 并冻结，再写 K@5 并冻结，使队头表反而保存较小版本，活跃表为空。
// 预期：上限 0/9/10 均选 K@9，上限 8 选 K@5，上限 4 查不到；不能命中队头就返回。
TEST(MemTableTest, MvccFrozenTablesChooseHighestVisibleVersion) {
  MemTable memtable;
  memtable.put("K", "v9", 9);
  memtable.frozen_cur_table();
  memtable.put("K", "v5", 5);
  memtable.frozen_cur_table(); // 活跃表为空，查询必须经过 frozen_get_。

  for (uint64_t read_id : {0, 8, 9, 10}) {
    SCOPED_TRACE(read_id);
    const uint64_t expected_id = read_id == 8 ? 5 : 9;
    const std::string expected_value = read_id == 8 ? "v5" : "v9";
    auto point = memtable.get("K", read_id);
    ASSERT_TRUE(point.is_valid());
    EXPECT_EQ(point.get_cur_tranc_id(), expected_id);
    EXPECT_EQ(point.get_value(), expected_value);

    auto scan = memtable.begin(read_id);
    ASSERT_TRUE(scan.is_valid());
    EXPECT_EQ(scan.get_cur_tranc_id(), expected_id);
    EXPECT_EQ(scan->second, expected_value);

    const auto batch = memtable.get_batch({"K"}, read_id);
    ASSERT_EQ(batch.size(), 1u);
    ASSERT_TRUE(batch[0].second.has_value());
    EXPECT_EQ(batch[0].second->first, expected_value);
    EXPECT_EQ(batch[0].second->second, expected_id);
  }
  EXPECT_TRUE(memtable.get("K", 4).is_end());
}

// 目的：验证较早冻结的高版本墓碑仍能阻止较小版本旧值被当作最新值返回。
// 场景：先写 K@9 墓碑并冻结，再写 K@5 旧值并冻结，随后查询最新值及上限 8。
// 预期：最新点查询返回版本 9 的墓碑，默认遍历为空；上限 8 仍能读取版本 5 的旧值。
TEST(MemTableTest, MvccFrozenTombstoneWinsOverOlderValue) {
  MemTable memtable;
  memtable.remove("K", 9);
  memtable.frozen_cur_table();
  memtable.put("K", "old", 5);
  memtable.frozen_cur_table();

  auto latest = memtable.get("K", 0);
  ASSERT_TRUE(latest.is_valid());
  EXPECT_EQ(latest.get_cur_tranc_id(), 9u);
  EXPECT_TRUE(latest.get_value().empty());
  EXPECT_TRUE(memtable.begin(0).is_end());

  auto snapshot = memtable.get("K", 8);
  ASSERT_TRUE(snapshot.is_valid());
  EXPECT_EQ(snapshot.get_cur_tranc_id(), 5u);
  EXPECT_EQ(snapshot.get_value(), "old");
}

// 目的：验证版本号相同时的跨表取舍，保护版本 0 的普通覆盖写语义。
// 场景：先写 K=old 并冻结，再用相同版本写 K=new 并冻结；分别测试版本 0、7。
// 预期：不限制版本的点查询返回较新冻结表中的 new，且记录版本号保持不变。
TEST(MemTableTest, MvccEqualVersionsPreferNewerFrozenTable) {
  for (uint64_t version : {0, 7}) {
    SCOPED_TRACE(version);
    MemTable memtable;
    memtable.put("K", "old", version);
    memtable.frozen_cur_table();
    memtable.put("K", "new", version);
    memtable.frozen_cur_table();
    auto it = memtable.get("K", 0);
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it.get_value(), "new");
    EXPECT_EQ(it.get_cur_tranc_id(), version);
  }
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  // reset_log_level("trace"); // ! 慎用, 日志输出量非常大
  return RUN_ALL_TESTS();
}
