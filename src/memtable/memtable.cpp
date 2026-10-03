#include "memtable/memtable.h"
#include "config/config.h"
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

// Lab2.1 从冻结跳表中查询
SkipListIterator MemTable::frozen_get_(const std::string &key,
                                       uint64_t tranc_id) {

  // 记录目前找到的最大可见版本；初始为空
  SkipListIterator best{};

  // 本函数不加锁，由调用方保护冻结表
  for (const auto &table : frozen_tables) {
    // SkipList::get 已完成本表内的可见性筛选：
    // tranc_id == 0 时不限制版本，否则只返回 <= tranc_id 的版本
    auto candidate = table->get(key, tranc_id);
    if (!candidate.is_valid())
      continue;

    // 在不同冻结表的候选记录中，选择真实版本号最大的记录。
    // 先判空，再读取 best 的版本号，避免访问空迭代器。
    if (!best.is_valid() ||
        candidate.get_cur_tranc_id() > best.get_cur_tranc_id())
      best = candidate;

    // 版本相同时不替换：冻结表从新到旧遍历，保留先找到的记录
    // 墓碑也参与比较，不能因 value 为空就忽略
  }

  // 全部未命中时，best 仍为空迭代器
  return best;
}

// Lab2.1 查询, 建议复用 cur_get_ 和 frozen_get_
SkipListIterator MemTable::get(const std::string &key, uint64_t tranc_id) {
  spdlog::trace("MemTable--get({}, {})", key, tranc_id);

  // 统一顺序：先活跃表，再冻结表
  // 同时持锁，避免查询过程中发生冻结或移除
  std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
  auto it = get_(key, tranc_id);
  if (!it.is_valid())
    return {};

  // get_ 在活跃表与所有冻结表中选择最大可见版本。
  // 返回指向原节点的迭代器，保留在命中 SkipList 内继续 ++ 的能力。
  //
  // 两把锁只保护本次查找，函数返回后释放。
  // 返回后的读取、遍历如何与并发修改协调，需要由调用方另行保证。
  return it;
}

// Lab2.1 查询, 无锁版本
SkipListIterator MemTable::get_(const std::string &key, uint64_t tranc_id) {
  spdlog::trace("MemTable--get_({}, {})", key, tranc_id);

  // 无锁版本：调用方必须已经持有 cur_mtx 和 frozen_mtx。
  //
  // 两个查询都会先按照 tranc_id 过滤可见性：
  //  best   ：活跃表中的最大可见版本
  //  frozen ：所有冻结表中的最大可见版本
  auto best = cur_get_(key, tranc_id);
  auto frozen = frozen_get_(key, tranc_id);

  // 冻结表存在候选，并且：
  //  1. 活跃表没有候选；或者
  //  2. 冻结表候选的真实版本号更大
  //
  //  || 会短路：best 无效时，不会调用它的 get_cur_tranc_id()
  if (frozen.is_valid() &&
      (!best.is_valid() || frozen.get_cur_tranc_id() > best.get_cur_tranc_id()))
    best = frozen;

  // 相同版本：保留活跃表中的记录，所以这里使用 >，不用 >=
  // 墓碑：也参与版本比较，不能因为 value 为空就丢掉
  return best;
}

// 返回结果中的每一项对应一个输入 key：
//   first：被查询的 key
//   second：optional，表示是否找到可见记录
//     有值时，内部 pair 保存 {value, 记录的真实版本号}
//     无值时，表示没有找到符合读取上限的记录
std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
MemTable::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  spdlog::trace("MemTable--get_batch with {} keys", keys.size());

  // 每个输入 key 都会产生一个结果，因此提前预留空间。
  // reserve 只预留容量，不会创建元素；此时 results.size() 仍然是 0
  std::vector<
      std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
      results;
  results.reserve(keys.size());

  // 2. 按统一顺序获取读锁：先活跃表，再冻结表
  //
  // 两把锁覆盖整个批次，保护查找以及后续 key/value 的复制
  // 避免查完活跃表、再查冻结表的间隙发生冻结或移除
  // 读锁允许其他读者进入，但会阻止需要相应写锁的修改操作。、
  std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);

  for (const auto &key : keys) {
    // 3. 复用 get_，统一单条查询与批量查询的版本选择规则
    //
    // get_ 会先按 tranc_id 筛选可见版本，再跨表选择最大版本：
    //   tranc_id == 0：不限制版本上限
    //   tranc_id != 0：只允许版本号 <= tranc_id 的记录
    //
    // 不能在活跃表命中后直接返回：
    // 例如活跃表是 K@5，冻结表是 K@9，最新查询应该选 K@9
    //
    // 这里已经持有两把锁，必须调用不自行加锁的 get_
    // 如果调用公开 get()，就会重复获取相同的 shared_mutex
    //
    // 为什么不先查cur表，锁一张表，然后要查下一张表了，再锁另外一张？
    // 不能直接认为分开加锁就一定更好，关键在于：两段查询之间，表可能发生变化
    auto it = get_(key, tranc_id);
    if (it.is_valid()) {
      results.emplace_back(
          key, std::make_pair(it.get_value(), it.get_cur_tranc_id()));
    } else {
      // 没有可见记录：可能 key 不存在，也可能所有版本都超过读取上限
      //
      // 保留这个 key 对应的位置，用 nullopt 表示未命中
      results.emplace_back(key, std::nullopt);
    }
  }

  // 按输入顺序逐项追加，因此结果顺序与输入一致
  // 例如输入 {"A", "B", "A"}，会返回三个结果，不会合并重复的 A
  //
  // results 保存的是复制后的值，不依赖原节点
  // 函数返回并释放读锁后，这些结果仍可独立读取
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

  // 清空会修改活跃表和冻结表，因此需要两把写锁
  // 加锁顺序保持一致：先活跃表，再冻结表
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::unique_lock<std::shared_mutex> frozen_lock(frozen_mtx);

  // SkipList::clear() 会重置跳表，并将其内部 size_bytes 清零
  // 因此活跃表的数据和大小统计会一起清空
  current_table->clear();

  // 清空冻结表容器
  // 这个操作不会自动更新 MemTable 自己维护的 frozen_bytes
  frozen_tables.clear();

  // 冻结表已经全部移除，对应的大小统计必须同步归零
  // 否则之后查询总大小、判断是否需要刷盘时仍会算上旧数据
  frozen_bytes = 0;
}

// 将最老的 memtable 写入 SST
// 现版本函数有 3 个问题：
//  1. 只锁冻结表，却可能修改活跃表
//  2. SST 尚未构建成功，就先移除了内存表；构建失败后，数据无法再从 MemTable 查询
//  3. 提前追加已刷盘事务 ID，失败时会留下错误的输出结果
std::shared_ptr<SST>
MemTable::flush_last(SSTBuilder &builder, std::string &sst_path, size_t sst_id,
                     std::vector<uint64_t> &flushed_tranc_ids,
                     std::shared_ptr<BlockCache> block_cache) {
  spdlog::debug("MemTable--flush_last(): Starting to flush memtable to SST{}",
                sst_id);

  // 1. 没有冻结表时，需要冻结活跃表，因此两把写锁都要获取
  //    顺序与其他 MemTable 操作一致：先活跃表，再冻结表
  std::unique_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::unique_lock<std::shared_mutex> frozen_lock(frozen_mtx);

  // 2. 优先刷已有的冻结表；没有时，再冻结当前活跃表
  //    复用刚修好的函数，避免在这里重复实现冻结逻辑
  if (frozen_tables.empty())
    frozen_cur_table_(); 

  // 如果仍然为空，说明活跃表也没有数据，无须构建 SST
  if (frozen_tables.empty()){
    spdlog::debug(
          "MemTable--flush_last(): Current table is empty, returning null");
    return nullptr;
  }

  // 3. 队尾是最早冻结的表
  //    这里只保存引用，构建成功之前不能将它移出队列
  auto table = frozen_tables.back(); 
  const auto &table_size = table->get_size(); 

  //    后续只访问冻结表，释放活跃表锁
  //    不需要冻结的写入可以继续执行；需要冻结时仍会等待 frozen_mtx
  //    冻结表锁继续持有，防止其他线程清除或重复刷出同一张表
  cur_lock.unlock(); 

  // 4. 暂存事务完成标记的 ID，暂时不修改调用方的输出列表
  std::vector<uint64_t> pending_ids;
  for (const auto&[key,value,version] : table->flush()){
    if (key.empty() && value.empty())
      // 本项目用“空 key + 空 value”记录事务完成标记
      pending_ids.push_back(version); 

    // 刷盘必须保留全部版本、墓碑和现有格式中的事务标记
    // 不能像普通查询一样，只保留某个读取上限下的可见版本
    builder.add(key,value,version); 
  }
  
  // 将最老的 memtable 写入 SST 的信息更新了
  frozen_tables.pop_back();
  frozen_bytes -= table->get_size();




  uint64_t max_tranc_id = 0;
  uint64_t min_tranc_id = UINT64_MAX;

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

//  Lab2.1 冻结活跃表（无锁版本 + 新版）
// 将 current_table 移入 frozen_tables 头部, 并更新 frozen_bytes
// 创建新的空 SkipList 作为 current_table
//  老版本有两个问题没有解决：
//  1.  空表也会入队，后续刷盘可能尝试构建空 SST。
//  2.  新活跃表创建得太晚：如果最后的 make_shared 抛异常，旧表已经进入冻结队列
//      但 current_table 仍指向它。同一张表就同时成了“活跃表”和“冻结表”
void MemTable::frozen_cur_table_() {
  // 无锁版本：调用方必须已经持有 cur_mtx 和 frozen_mtx 的写锁
  // 这些锁保护下面的大小读取、冻结队列修改和活跃表切换
  const auto &table_size = current_table->get_size();

  // 空表没有数据需要冻结，直接返回，避免队列中出现空表
  if (table_size == 0)
    return;

  // 先创建下一张活跃表。
  // 如果创建失败，此时还没修改任何成员，原来的状态保持不变
  auto next_table = std::make_shared<SkipList>();

  // 把当前表加入冻结队列头部：越新冻结的表越靠前
  // 这里保存的是 shared_ptr，不会复制整张 SkipList
  frozen_tables.push_front(current_table);

  // 这张表现在属于冻结表集合，将它的大小计入冻结表总量
  frozen_bytes += table_size;

  // 切换到准备好的空表，后续写入进入新表
  // 旧表由 frozen_tables 持有，数据仍然保留
  current_table = std::move(next_table);
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

// 计算现在 memtable 的总体积
// 总大小必须在同时持有 cur_mtx、frozen_mtx 的情况下读取，原因有两个：
//
// 1. 不能持锁后再调用 get_cur_size() / get_frozen_size()。
//    这两个 getter 内部也会加锁，导致同一线程重复获取同一把
//    shared_mutex；shared_mutex 不支持递归加锁。
//
// 2. 也不能去掉外层锁，直接分别调用两个 getter。
//    每个 getter 返回时都会释放自己的锁，两次读取之间可能发生冻结，
//    导致读到的两项统计来自不同时刻，出现漏算或重复计算。
//    例如：先读冻结表大小为 0，随后活跃表的 100 字节被冻结，
//    再读活跃表大小为 0，最终得到 0，但实际总大小始终为 100。
//
// 因此：同时获取两把读锁，直接读取 current_table->get_size()
// 和 frozen_bytes。SkipList::get_size() 本身不会再次获取 MemTable 的锁
size_t MemTable::get_total_size() {
  std::shared_lock<std::shared_mutex> cur_lock(cur_mtx);
  std::shared_lock<std::shared_mutex> frozen_lock(frozen_mtx);
  return current_table->get_size() + frozen_bytes;
}

// 需要进一步判断这里的 HeapIterator 能否跳过删除元素
HeapIterator MemTable::begin(uint64_t tranc_id, bool skip_delete) {
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

  return HeapIterator(items, tranc_id, skip_delete); // skip_delete 默认为true
}

HeapIterator MemTable::end() {
  // Lab2.2 MemTable 的迭代器
  // ? 加读锁后返回空 HeapIterator
  return HeapIterator{};
}

// Lab2.3 MemTable 的前缀迭代器
// 它就是 begin() 的区间版——锁、idx 约定、tranc 过滤、收集进堆全套复用
// 唯一变化是每张表从全量 [begin, end) 换成前缀区间 [begin_preffix, end_preffix)
HeapIterator MemTable::iters_preffix(const std::string &preffix,
                                     uint64_t tranc_id) {
  // 加读锁, 对所有表调用 begin_preffix/end_preffix 遍历前缀范围
  // 过滤事务可见性, 同 key 只保留最新版本
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
