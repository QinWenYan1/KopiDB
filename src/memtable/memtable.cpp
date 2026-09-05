#include "memtable/memtable.h"
#include "config/config.h"
#include "consts.h"
#include "iterator/iterator.h"
#include "skiplist/skiplist.h"
#include "spdlog/spdlog.h"
#include "sst/sst.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace tiny_lsm {

class BlockCache;

// MemTable implementation using PIMPL idiom
MemTable::MemTable() : frozen_bytes(0) {
  current_table = std::make_shared<SkipList>();
}
MemTable::~MemTable() = default;

// 用于检查本元素是否是合法事务版本
// 事务可见性: tranc_id == 0 的条目(非事务写入)对所有读者可见;
// 否则仅当 条目不比读者的快照新 时可见。Lab 5 改可见性规则只动这里
bool MemTable::tranc_visible(uint64_t entry_tranc_id, uint64_t read_tranc_id) {
  // read == 0: 非事务读, 看全部
  // 否则: 条目是在读者拍照之后写入的(条目 > read) -> 不可见
  return read_tranc_id == 0 || entry_tranc_id <= read_tranc_id;
}

void MemTable::put_(const std::string &key, const std::string &value,
                    uint64_t tranc_id) {
  // Lab2.1 无锁版本的 put
  // ? 直接调用 current_table 的 put 方法
  spdlog::trace("MemTable--put_({}, {}, {})", key, value, tranc_id);
  current_table->put(key, value, tranc_id);
}

void MemTable::put(const std::string &key, const std::string &value,
                   uint64_t tranc_id) {
  // Lab2.1 有锁版本的 put
  // ? 加 cur_mtx 写锁后调用 put_()
  // ? 若 current_table 超过 LsmPerMemSizeLimit, 还需加 frozen_mtx 写锁并调用
  spdlog::trace("MemTable--put({}, {}, {})", key, value, tranc_id);
  // frozen_cur_table_() 加写入锁，保护 current_table
  std::lock_guard<std::shared_mutex> put_lock(cur_mtx);
  put_(key, value, tranc_id);

  // 检查是否需要 froze memtable
  if (current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    // 加入冻结锁，保护 frozen table / frozen 队列
    std::lock_guard<std::shared_mutex> frozen_lock(frozen_mtx);
    frozen_cur_table_();
  }
}

void MemTable::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs,
    uint64_t tranc_id) {
  // Lab2.1 有锁版本的 put_batch
  // ? 加 cur_mtx 写锁后遍历 kvs 依次调用 put_()
  // ? 结束后若超限则冻结当前表
  spdlog::trace("MemTable--put_batch with {} kvs", kvs.size());
  // 加写入锁，保护 current_table
  std::lock_guard<std::shared_mutex> put_lock(cur_mtx);
  for (const auto &e : kvs) {
    put_(e.first, e.second, tranc_id);
  }

  // 检查是否需要 froze memtable
  if (current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    // 加入冻结锁，保护 frozen table / frozen 队列
    std::lock_guard<std::shared_mutex> frozen_lock(frozen_mtx);
    frozen_cur_table_();
  }
}

SkipListIterator MemTable::cur_get_(const std::string &key, uint64_t tranc_id) {
  // 检查当前活跃的memtable
  // Lab2.1 从活跃跳表中查询
  // ? 调用 current_table->get(), 找到则返回; 未找到则返回空迭代器
  spdlog::trace("MemTable--cur_get_({}, {})", key, tranc_id);
  return current_table->get(key, tranc_id);
}

SkipListIterator MemTable::frozen_get_(const std::string &key,
                                       uint64_t tranc_id) {
  // Lab2.1 从冻结跳表中查询
  // ? 遍历 frozen_tables (注意顺序：越靠前越新), 找到即返回
  // ? tranc_id 直接传递到 get() 即可
  // 冻结队列头新尾旧, 从头扫, 首个命中即最新版本
  spdlog::trace("MemTable--frozen_get_({}, {})", key, tranc_id);
  for (const auto &e : frozen_tables) {
    auto iter = e->get(key, tranc_id);
    if (iter.is_valid())
      return iter;
  }
  return SkipListIterator{};
}

SkipListIterator MemTable::get(const std::string &key, uint64_t tranc_id) {
  // Lab2.1 查询, 建议复用 cur_get_ 和 frozen_get_
  // ? 先加 cur_mtx 读锁查活跃表, 未命中则释放锁后加 frozen_mtx 读锁查冻结表
  spdlog::trace("MemTable--get({}, {})", key, tranc_id);
  { // 先来 current table 寻找
    std::shared_lock<std::shared_mutex> get_lock(cur_mtx);
    auto it = cur_get_(key, tranc_id);
    if (it.is_valid())
      return it; // 命中就返回
  }              // 每命中，释放cur_mtx

  { // 没命中，来frozen tables 寻找
    std::shared_lock<std::shared_mutex> get_lock(frozen_mtx);
    return frozen_get_(key, tranc_id);
  }
}

SkipListIterator MemTable::get_(const std::string &key, uint64_t tranc_id) {
  // Lab2.1 查询, 无锁版本
  spdlog::trace("MemTable--get_({}, {})", key, tranc_id);
  // ? 直接调用 cur_get_ 和 frozen_get_
  auto it = cur_get_(key, tranc_id);
  if (it.is_valid())
    return it; // 命中就返回

  // 没命中，来frozen tables 寻找
  return frozen_get_(key, tranc_id);
}

std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
MemTable::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  spdlog::trace("MemTable--get_batch with {} keys", keys.size());

  std::vector<
      std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
      results;
  results.reserve(keys.size());

  // 1. 先获取活跃表的锁
  std::shared_lock<std::shared_mutex> slock1(cur_mtx);
  for (size_t idx = 0; idx < keys.size(); idx++) {
    auto key = keys[idx];
    auto cur_res = cur_get_(key, tranc_id);
    if (cur_res.is_valid()) {
      // 值存在且不为空
      results.emplace_back(
          key, std::make_pair(cur_res.get_value(), cur_res.get_tranc_id()));
    } else {
      // 如果活跃表中未找到，先占位
      results.emplace_back(key, std::nullopt);
    }
  }

  // 2. 如果某些键在活跃表中未找到，还需要查找冻结表
  if (!std::any_of(results.begin(), results.end(), [](const auto &result) {
        return !result.second.has_value();
      })) {
    return results;
  }

  slock1.unlock(); // 释放活跃表的锁
  std::shared_lock<std::shared_mutex> slock2(frozen_mtx); // 获取冻结表的锁
  for (size_t idx = 0; idx < keys.size(); idx++) {
    if (results[idx].second.has_value()) {
      continue; // 如果在活跃表中已经找到，则跳过
    }
    auto key = keys[idx];
    auto frozen_result = frozen_get_(key, tranc_id);
    if (frozen_result.is_valid()) {
      // 值存在且不为空
      results[idx] =
          std::make_pair(key, std::make_pair(frozen_result.get_value(),
                                             frozen_result.get_tranc_id()));
    } else {
      results[idx] = std::make_pair(key, std::nullopt);
    }
  }

  return results;
}

void MemTable::remove_(const std::string &key, uint64_t tranc_id) {
  // Lab2.1 无锁版本的remove
  // ? 在 LSM 中, 删除操作是写入空值, 调用 current_table->put(key, "", tranc_id)
  spdlog::trace("MemTable--remove_({}, {})", key, tranc_id);
  current_table->put(key, "", tranc_id);
}

void MemTable::remove(const std::string &key, uint64_t tranc_id) {
  // Lab2.1 有锁版本的remove
  // ? 加 cur_mtx 写锁后调用 remove_()
  // ? 若超限则冻结当前表
  spdlog::trace("MemTable--remove({}, {})", key, tranc_id);
  std::lock_guard<std::shared_mutex> write_lock(cur_mtx);
  remove_(key, tranc_id);
  if (current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    std::lock_guard<std::shared_mutex> frozen_lock(frozen_mtx);
    frozen_cur_table_();
  }
}

void MemTable::remove_batch(const std::vector<std::string> &keys,
                            uint64_t tranc_id) {
  // Lab2.1 有锁版本的remove_batch
  // ? 加 cur_mtx 写锁后遍历 keys 依次调用 remove_()
  // ? 结束后若超限则冻结当前表
  spdlog::trace("MemTable--remove_batch with {} keys", keys.size());
  std::lock_guard<std::shared_mutex> write_lock(cur_mtx);
  for (const auto &key : keys) {
    remove_(key, tranc_id);
  }
  if (current_table->get_size() >=
      TomlConfig::getInstance().getLsmPerMemSizeLimit()) {
    std::lock_guard<std::shared_mutex> frozen_lock(frozen_mtx);
    frozen_cur_table_();
  }
}

void MemTable::clear() {
  spdlog::info("MemTable--clear(): Clearing all tables");

  std::unique_lock<std::shared_mutex> lock1(cur_mtx);
  std::unique_lock<std::shared_mutex> lock2(frozen_mtx);
  frozen_tables.clear();
  current_table->clear();
}

// 将最老的 memtable 写入 SST, 并返回控制类
std::shared_ptr<SST>
MemTable::flush_last(SSTBuilder &builder, std::string &sst_path, size_t sst_id,
                     std::vector<uint64_t> &flushed_tranc_ids,
                     std::shared_ptr<BlockCache> block_cache) {
  spdlog::debug("MemTable--flush_last(): Starting to flush memtable to SST{}",
                sst_id);

  // 由于 flush 后需要移除最老的 memtable, 因此需要加写锁
  std::unique_lock<std::shared_mutex> lock(frozen_mtx);

  uint64_t max_tranc_id = 0;
  uint64_t min_tranc_id = UINT64_MAX;

  if (frozen_tables.empty()) {
    // 如果当前表为空，直接返回nullptr
    if (current_table->get_size() == 0) {
      spdlog::debug(
          "MemTable--flush_last(): Current table is empty, returning null");

      return nullptr;
    }
    // 将当前表加入到frozen_tables头部
    frozen_tables.push_front(current_table);
    frozen_bytes += current_table->get_size();
    // 创建新的空表作为当前表
    current_table = std::make_shared<SkipList>();
  }

  // 将最老的 memtable 写入 SST
  std::shared_ptr<SkipList> table = frozen_tables.back();
  frozen_tables.pop_back();
  frozen_bytes -= table->get_size();

  std::vector<std::tuple<std::string, std::string, uint64_t>> flush_data =
      table->flush();
  for (auto &[k, v, t] : flush_data) {
    if (k == "" && v == "") {
      flushed_tranc_ids.push_back(t);
    }
    max_tranc_id = (std::max)(t, max_tranc_id);
    min_tranc_id = (std::min)(t, min_tranc_id);
    builder.add(k, v, t);
  }
  auto sst = builder.build(sst_id, sst_path, block_cache);

  spdlog::info("MemTable--flush_last(): SST{} built successfully at '{}'",
               sst_id, sst_path);

  return sst;
}

void MemTable::frozen_cur_table_() {
  // Lab2.1 冻结活跃表（无锁版本）
  // ? 将 current_table 移入 frozen_tables 头部, 并更新 frozen_bytes
  // ? 创建新的空 SkipList 作为 current_table
  // 无锁版本：直接冻结活跃表
  // 注意链表最前面是最新的
  frozen_tables.push_front(current_table);
  frozen_bytes += current_table->get_size();
  current_table = std::make_shared<SkipList>();
}

void MemTable::frozen_cur_table() {
  // Lab2.1 冻结活跃表（有锁版本）
  // ? 加 cur_mtx 和 frozen_mtx 写锁后调用 frozen_cur_table_()
  std::lock_guard<std::shared_mutex> table_lock(cur_mtx),
      frozen_lock(frozen_mtx);
  frozen_cur_table_();
}

size_t MemTable::get_cur_size() {
  std::shared_lock<std::shared_mutex> slock(cur_mtx);
  return current_table->get_size();
}

size_t MemTable::get_frozen_size() {
  std::shared_lock<std::shared_mutex> slock(frozen_mtx);
  return frozen_bytes;
}

size_t MemTable::get_total_size() {
  std::shared_lock<std::shared_mutex> slock1(cur_mtx);
  std::shared_lock<std::shared_mutex> slock2(frozen_mtx);
  return get_frozen_size() + get_cur_size();
}

// 需要进一步判断这里的 HeapIterator 能否跳过删除元素
HeapIterator MemTable::begin(uint64_t tranc_id) {
  // Lab2.2 MemTable 的迭代器
  // ? 加 cur_mtx 和 frozen_mtx 读锁, 遍历所有表收集 SearchItem
  // ? 每个 item 包含 key, value, table_idx, 0, tranc_id
  // ? 过滤 tranc_id 不可见的记录 (tranc_id != 0 && iter.get_tranc_id() >
  // tranc_id) ? 返回 HeapIterator(item_vec, tranc_id)
  std::vector<SearchItem> items;
  // 先 curr 后 frozen mtx，都是读锁
  std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);

  // idx 约定： 表越新 idx 越大
  //  取现现在 frozen_tables 个数就是 现在最新的 table index
  int idx = static_cast<int>(frozen_tables.size());

  // 1. 取活跃表中的元素放入到heap中
  //  tranc_id 过滤：tranc_id != 0 && 条目tranc_id > tranc_id 才跳过
  //  tranc_id == 0 表示"非事务读"，
  //  事务读只滤掉比自己新的版本。注意这个过滤是"收集时"做的，比灌进堆再滤便宜
  for (auto it = current_table->begin(); it != current_table->end(); ++it) {
    // 事务太新，对现版本不可见，跳过
    if (!tranc_visible(it.get_tranc_id(), tranc_id))
      continue;
    items.emplace_back(it.get_key(), it.get_value(), idx, 0, it.get_tranc_id());
  }

  // 2. 从冻结表取元素放到heap中: front 新 back 旧, idx 从大到小递减
  for (const auto &table : frozen_tables) {
    --idx;
    for (auto it = table->begin(); it != table->end(); ++it) {
      // 事务太新，对现版本不可见，跳过
      if (!tranc_visible(it.get_tranc_id(), tranc_id))
        continue;
      items.emplace_back(it.get_key(), it.get_value(), idx, 0,
                         it.get_tranc_id());
    }
  }

  return HeapIterator(items, tranc_id); // skip_delete 默认为true
}

HeapIterator MemTable::end() {
  // Lab2.2 MemTable 的迭代器
  // ? 加读锁后返回空 HeapIterator
  return HeapIterator{};
}

// 它就是 begin() 的区间版——锁、idx 约定、tranc 过滤、收集进堆全套复用
// 唯一变化是每张表从全量 [begin, end) 换成前缀区间 [begin_preffix, end_preffix)
HeapIterator MemTable::iters_preffix(const std::string &preffix,
                                     uint64_t tranc_id) {
  // Lab2.3 MemTable 的前缀迭代器
  // ? 加读锁, 对所有表调用 begin_preffix/end_preffix 遍历前缀范围
  // ? 过滤事务可见性, 同 key 只保留最新版本
  std::vector<SearchItem> items;
  // 加curr 和 frozen 读锁
  std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);

  // idx 约定同 begin(): 表越新 idx 越大，current 最大
  int idx = static_cast<int>(frozen_tables.size());

  // 1. 活跃表：左开右闭 [begin_preffix, end_preffix)
  for (auto it = current_table->begin_preffix(preffix);
       it != current_table->end_preffix(preffix); ++it) {
    // 事务不可见，跳过
    if (!tranc_visible(it.get_tranc_id(), tranc_id))
      continue;
    items.emplace_back(it.get_key(), it.get_value(), idx, 0, it.get_tranc_id());
  }

  // 2. 冻结表：靠前的新，靠后的旧，idx递减
  for (const auto &table : frozen_tables) {
    --idx;
    for (auto it = table->begin_preffix(preffix);
         it != table->end_preffix(preffix); ++it) {
      // 事务不可见，跳过
      if (!tranc_visible(it.get_tranc_id(), tranc_id))
        continue;
      items.emplace_back(it.get_key(), it.get_value(), idx, 0,
                         it.get_tranc_id());
    }
  }

  // skip_delete 默认 true
  return HeapIterator(items, tranc_id);
}

std::optional<std::pair<HeapIterator, HeapIterator>>
MemTable::iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // Lab2.3 MemTable 的谓词查询迭代器起始范围
  // ? 加读锁, 对所有表调用 iters_monotony_predicate 获取结果
  // ? 过滤事务可见性, 同 key 只保留最新版本
  // ? 若结果为空返回 nullopt;
  // 否则返回 make_pair(HeapIterator(item_vec,ctranc_id, true), HeapIterator{})

  std::vector<SearchItem> items;
  // 加curr 和 frozen 读锁
  std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);

  int idx = static_cast<int>(frozen_tables.size());

  // 1. 收集一张表谓词命中区间（单调谓词 -> 命中集连续 -> 每表一段）
  auto collect = [&](SkipList &table, const int &table_idx) {
    auto range = table.iters_monotony_predicate(predicate);
    // 本表没命中不算错误，别的表可能有
    if (!range.has_value())
      return;

    for (auto it = range->first; it != range->second; ++it) {
      // 事务不可见，跳过
      if (!tranc_visible(it.get_tranc_id(), tranc_id))
        continue;
      items.emplace_back(it.get_key(), it.get_value(), table_idx, 0,
                         it.get_tranc_id());
    }
  };

  // 现在在curr, frozen tables 里面都收集
  collect(*current_table, idx);
  for (const auto &table : frozen_tables) {
    --idx;
    collect(*table, idx);
  }

  // 所有表都没命中, 整体无结果
  if (items.empty()) {
    return std::nullopt;
  }

  // 命中: (归并迭代器, 空哨兵), 与 begin()/end() 同款的用法
  return std::make_pair(HeapIterator(items, tranc_id), HeapIterator{});
}
} // namespace tiny_lsm
