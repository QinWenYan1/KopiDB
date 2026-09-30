#include "consts.h"
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

// 测试基本的插入和查询操作
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

// 测试删除操作
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

// 测试冻结表操作
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

// 测试大量数据操作
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

// 测试内存大小跟踪
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

// 测试多次冻结表操作
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

// 测试迭代器在复杂操作序列下的行为
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

// 活跃表和冻结表中的版本都必须服从读取上限；点查询保留墓碑供上层处理。
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

// 范围查询须先跳过不可见版本，再选最新可见版本，最后应用墓碑。
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

// 冻结时间较新的表不一定保存更大的版本号；点查询和遍历应选择同一版本。
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

// 版本更大的墓碑即使位于较早冻结的表，也必须挡住较小版本的旧值。
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

// 版本号相同时保留较新表中的值，尤其不能破坏 id = 0 的普通更新行为。
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
