#include "lsm/engine.h"
#include "block/block_cache.h"
#include "config/config.h"
#include "iterator/iterator.h"
#include "logger/logger.h"
#include "lsm/level_iterator.h"
#include "lsm/two_merge_iterator.h"
#include "spdlog/spdlog.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tiny_lsm {

// *********************** LSMEngine ***********************

// Lab 4.2 引擎初始化
LSMEngine::LSMEngine(std::string path) : data_dir(path) {
  // 1. 初始化日志: init_spdlog_file()
  init_spdlog_file();

  // 2. 初始化 block_cache (容量和 K 值从 TomlConfig 读取)
  block_cache = std::make_shared<BlockCache>(
      TomlConfig::getInstance().getLsmBlockCacheCapacity(),
      TomlConfig::getInstance().getLsmBlockCacheK());

  // 3. 若目录不存在则创建
  if (!std::filesystem::exists(path)) {
    spdlog::warn("LSMEngine::LSMEngine(): directory {} not exist, create it",
                 path);
    std::filesystem::create_directory(path);
  }

  // 4. vlog 常驻打开 (开销小; 即使没开 WiscKey 也无害, clear() 也假设它在)
  //    初始化 VLog: vlog_ = VLog::open(data_dir + "/vlog.data")
  vlog_ = VLog::open(data_dir + "/vlog.data");

  // 5. 遍历目录加载所有已存在的 SST 文件 (文件名格式: sst_{id}.{level})
  //      - 文件名格式: sst_{id}.{level}
  //      - 调用 SST::open 并记录到 ssts 和 level_sst_ids
  //      - 维护 next_sst_id 和 cur_max_level
  for (const auto &entry : std::filesystem::directory_iterator(path)) {

    // 检查文件类型
    if (!entry.is_regular_file())
      continue;

    // 检查文件名字格式
    std::string filename = entry.path().filename().string();
    if (!filename.starts_with("sst_"))
      continue;

    auto dot_pos = filename.find('.');
    if (dot_pos == std::string::npos || dot_pos == filename.size() - 1)
      continue;

    // len = dot_pos - 4 因为 4 = "sst_"
    size_t sst_id = std::stoull(filename.substr(4, dot_pos - 4));
    size_t lvl = std::stoull(filename.substr(dot_pos + 1));

    // 只打开不要创建，所以 open() 直接设置为 false
    auto sst = SST::open(sst_id, FileObj::open(entry.path().string(), false),
                         block_cache, vlog_);

    // 写锁 (构造函数里其实还没有竞争者, 参考实现的防御写法, 保留)
    std::unique_lock<std::shared_mutex> lock(ssts_mtx);
    ssts[sst_id] = sst;
    level_sst_ids[lvl].push_back(sst_id);

    // 记录目前最大的 sst_id
    next_sst_id = (std::max)(sst_id, next_sst_id);
    cur_max_level = (std::max)(lvl, cur_max_level);
  }

  // 6. 现有的最大 sst_id 自增后才是下一个分配的 sst_id
  next_sst_id++;

  // 7. 各层 sst_id_list 排序; L0 需要 reverse (id 越大越新, 要优先查询)
  for (auto &[level, sst_id_list] : level_sst_ids) {
    std::sort(sst_id_list.begin(), sst_id_list.end());
    if (level == 0)
      std::reverse(sst_id_list.begin(), sst_id_list.end());
  }
}

LSMEngine::~LSMEngine() = default;

// Lab 4.2 查询
std::optional<std::pair<std::string, uint64_t>>
LSMEngine::get(const std::string &key, uint64_t tranc_id) {
  // 1. 先查 memtable.get(key, tranc_id), 命中则返回 (value 非空) 或
  // nullopt(value 为空=删除)
  auto mem_ret = memtable.get(key, tranc_id);
  if (mem_ret.is_valid()) {
    if (mem_ret.get_value().empty())
      return std::nullopt;
    return std::make_pair(mem_ret.get_value(), mem_ret.get_tranc_id());
  }

  // 2. memtable 没有 -> 加读锁查 SST
  //    参考实现这里把 SST 查询逻辑原样复制了一遍, sst_get_ 沦为死代码;
  //    我们委托消重 (语义逐行核对过, 等价; sst_get_ 就是为此存在的)
  std::shared_lock<std::shared_mutex> lock(ssts_mtx);
  auto sst_ret = sst_get_(key, tranc_id);

  // 对普通查询而言，墓碑表示 key 已删除x
  // 先确认 optional 有值，再访问其中的记录
  if (sst_ret.has_value() && sst_ret->first.empty())
    return std::nullopt;

  return sst_ret;
}

// Lab 4.2 批量查询
std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
LSMEngine::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  // 1. 先从 memtable 批量查询: memtable.get_batch(keys, tranc_id)
  auto results = memtable.get_batch(keys, tranc_id);

  // 2. 全部命中直接返回, 不碰 SST
  bool need_search_sst = false;
  for (auto &[key, value] : results) {
    if (!value.has_value()) {
      need_search_sst = true;
      break;
    }
  }

  if (!need_search_sst) {
    // MemTable 的批量结果也会保留墓碑。
    // 这条路径不再查询 SST，因此可以直接转换。
    for (auto &[key, value] : results) {
      if (value.has_value() && value->first.empty())
        value.reset();
    }
    return results;
  }

  std::shared_lock<std::shared_mutex> rlock(ssts_mtx);

  // 3. 进入 L0: 对每个未命中的 key, 从新到旧逐文件补
  if (level_sst_ids.find(0) != level_sst_ids.end()) {
    for (auto &[key, value] : results) {
      if (value.has_value())
        continue;

      for (auto &sst_id : level_sst_ids[0]) {
        // 从 engine 中加载 sst handle 用于之后的 key 查找
        auto &sst = ssts[sst_id];
        auto sst_it = sst->get(key, tranc_id);

        if (sst_it != sst->end()) {
          // 墓碑也记录为 {"", 版本号}
          // 此时 has_value() 为 true，后续层就会跳过这个 key
          value = std::make_pair(sst_it->second, sst_it.get_cur_tranc_id());
          break;
        }
      }
    }
  }

  // 4. L1+: 对每个仍未命中 key, 每层二分定位候选文件补
  for (size_t level = 1; level <= cur_max_level; ++level) {
    if (level_sst_ids.find(level) == level_sst_ids.end())
      continue;

    const auto &id_list = level_sst_ids[level];
    for (auto &[key, value] : results) {
      if (value.has_value())
        continue; // 现在普通记录和墓碑都会跳过

      size_t left = 0, right = id_list.size();
      while (left < right) {
        size_t mid = (left + right) / 2;
        auto &sst = ssts[id_list[mid]];

        // sst 命中， 进入查找
        if (sst->get_first_key() <= key && key <= sst->get_last_key()) {
          auto sst_it = sst->get(key, tranc_id);

          // 命中就记录，包括墓碑，阻止后续更深层补入旧值。
          if (sst_it != sst->end())
            value = std::make_pair(sst_it->second, sst_it.get_cur_tranc_id());
          break;
        } else if (sst->get_last_key() < key)
          left = mid + 1;
        else
          right = mid;
      }
    }
  }

  // 查询已经结束，可以将墓碑转换为对外的“不存在”
  // 墓碑就是有 ID 但是没有string，我们直接过滤掉
  for (auto &[key, value] : results) {
    if (value.has_value() && value->first.empty())
      value.reset();
  }
  return results;
}

// Lab 4.2 sst 内部查询 (不查 memtable)
std::optional<std::pair<std::string, uint64_t>>
LSMEngine::sst_get_(const std::string &key, uint64_t tranc_id) {
  // 不加锁: 约定调用方已持有 ssts_mtx (get 的读锁 / compact 的写锁)

  // 1. L0: 各 SST key 范围重叠, 逐个查; 队列头部 id 最大 = 最新, 先查
  if (level_sst_ids.find(0) != level_sst_ids.end()) {

    for (auto &sst_id : level_sst_ids[0]) {
      // 取出 sst 和 sst_it 一个一个查找
      // get_cur_tranc_id()：永远返回当前 entry 的真实写入 id
      auto &sst = ssts[sst_id];
      auto sst_it = sst->get(key, tranc_id);
      if (sst_it != sst->end()) {
        // 保留墓碑及其版本号，供事务提交时检查冲突。
        // 找到墓碑也立即返回，不能继续查更旧的值。
        return std::make_pair(sst_it->second, sst_it.get_cur_tranc_id());
      }
    }
  }

  // 2. L1+: 每层内 SST 不重叠且按 key 有序, 二分定位唯一候选文件
  for (size_t lvl = 1; lvl <= cur_max_level; ++lvl) {
    if (level_sst_ids.find(lvl) == level_sst_ids.end())
      continue;

    const auto &id_list = level_sst_ids[lvl];
    size_t left = 0, right = id_list.size();

    while (left < right) {
      size_t mid = (left + right) / 2;
      auto &sst = ssts[id_list[mid]];

      // 找到目标 sst
      if (sst->get_first_key() <= key && key <= sst->get_last_key()) {
        auto sst_it = sst->get(key, tranc_id);
        // 检查是否为有效 sst, 而不是尾后 sst
        if (sst_it != sst->end()) {
          // 普通记录和墓碑都返回，保留真实版本号。
          return std::make_pair(sst_it->second, sst_it.get_cur_tranc_id());
        }
        // 本层只有这一个文件可能含 key, 不在就换更旧的一层
        break;
      } else if (sst->get_last_key() < key)
        left = mid + 1;
      else
        right = mid;
    }
  }

  spdlog::trace("LSMEngine::sst_get_({}, {}): key not exist", key, tranc_id);
  return std::nullopt;
}

// Lab 4.1 插入
uint64_t LSMEngine::put(const std::string &key, const std::string &value,
                        uint64_t tranc_id) {
  spdlog::trace("LSMEngine::put({}, {}, tranc_id={})", key, value, tranc_id);
  memtable.put(key, value, tranc_id);

  // 先写后查阈值: 单条超大 value 也能进, memtable 允许短暂超限
  // 若 memtable 总大小 >= LsmTolMemSizeLimit 则调用 flush() 并返回其结果
  // 否则返回 0
  if (memtable.get_total_size() >=
      TomlConfig::getInstance().getLsmTolMemSizeLimit())
    return flush();
  return 0;
}

// Lab 4.1 批量插入
uint64_t LSMEngine::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs,
    uint64_t tranc_id) {
  // ? 调用 memtable.put_batch(kvs, tranc_id)
  // ? 若超限则 flush() 并返回其结果
  spdlog::trace("LSMEngine::put_batch(tranc_id={})", tranc_id);
  memtable.put_batch(kvs, tranc_id);

  // 先写后查阈值: 单条超大 value 也能进, memtable 允许短暂超限
  if (memtable.get_total_size() >=
      TomlConfig::getInstance().getLsmTolMemSizeLimit())
    return flush();
  return 0;
}

// Lab 4.1 删除
uint64_t LSMEngine::remove(const std::string &key, uint64_t tranc_id) {
  spdlog::trace("LSMEngine::remove({}, tranc_id={})", key, tranc_id);
  // LSM 的删除 = 插一个空值墓碑, 墓碑本体在 memtable.remove 里完成
  memtable.remove(key, tranc_id);

  // 若超限则 flush() 并返回其结果
  if (memtable.get_total_size() >=
      TomlConfig::getInstance().getLsmTolMemSizeLimit())
    return flush();
  return 0;
}

// Lab 4.1 批量删除
uint64_t LSMEngine::remove_batch(const std::vector<std::string> &keys,
                                 uint64_t tranc_id) {
  spdlog::trace("LSMEngine::remove_batch(tranc_id={})", tranc_id);
  memtable.remove_batch(keys, tranc_id);

  // 先写后查阈值: 单条超大 value 也能进, memtable 允许短暂超限
  // 若超限则 flush() 并返回其结果
  if (memtable.get_total_size() >=
      TomlConfig::getInstance().getLsmTolMemSizeLimit())
    return flush();
  return 0;
}

void LSMEngine::clear() {
  memtable.clear();
  level_sst_ids.clear();
  ssts.clear();
  // 清空当前文件夹的所有内容
  try {
    for (const auto &entry : std::filesystem::directory_iterator(data_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      std::filesystem::remove(entry.path());

      spdlog::info("LSMEngine--"
                   "clear file {} successfully.",
                   entry.path().string());
    }
  } catch (const std::filesystem::filesystem_error &e) {
    // 处理文件系统错误
    spdlog::error("Error clearing directory: {}", e.what());
  }

  // Re-create the vlog so new writes go to a fresh file
  if (vlog_) {
    vlog_->del_vlog();
    vlog_ = VLog::open(data_dir + "/vlog.data");
  }
}

// Lab 4.1 刷盘形成sst文件
uint64_t LSMEngine::flush() {

  // 0. 若 memtable 为空直接返回 0
  if (memtable.get_total_size() == 0)
    return 0;

  // 1. 加 ssts_mtx 写锁
  //    写锁: 要改 ssts / level_sst_ids, 与未来的读路径 get() 互斥
  std::unique_lock<std::shared_mutex> lock(ssts_mtx);

  // 2. 若 L0 层 SST 数量 >= LsmSstLevelRatio, 先触发 full_compact(0)
  //    L0 堆满 -> 先 compact (Lab 4.5 才实现, 现在是空桩, 调用无害先挂着)
  //    步骤 2 的 find(0) != end() 守卫：防 map 的 operator[] 凭空造出空 L0
  //    队列参与比较
  if (level_sst_ids.find(0) != level_sst_ids.end() &&
      level_sst_ids[0].size() >=
          TomlConfig::getInstance().getLsmSstLevelRatio())
    full_compact(0);

  // 3. 分配新的 sst_id: next_sst_id++
  size_t new_sst_id = next_sst_id++;
  auto sst_path = get_sst_path(new_sst_id, 0);

  // 4+5. 选 builder 模式, 把最老的冻结表刷成 L0 SST
  // 4. 构造 SSTBuilder:
  //    - 若 WiscKey 阈值 > 0 且 vlog_ 存在, 使用 WiscKey 模式的构造函数
  //    - 否则使用普通模式
  // 5. 调用 memtable.flush_last() 生成 SST 文件
  std::vector<uint64_t> flushed_tranc_ids;
  std::shared_ptr<SST> new_sst;
  size_t wk = TomlConfig::getInstance().getWisckeyValueThreshold();
  if (wk > 0 && vlog_) {
    SSTBuilder builder(TomlConfig::getInstance().getLsmBlockSize(), true, vlog_,
                       wk);
    new_sst = memtable.flush_last(builder, sst_path, new_sst_id,
                                  flushed_tranc_ids, block_cache);
  } else {
    SSTBuilder builder(TomlConfig::getInstance().getLsmBlockSize(), true);
    new_sst = memtable.flush_last(builder, sst_path, new_sst_id,
                                  flushed_tranc_ids, block_cache);
  }

  // 6. 更新 ssts 和 level_sst_ids[0] (push_front 保证新的在前)
  //    登记: id->SST 映射 + L0 队列头插 (新的在前, 查询从新到旧)
  ssts[new_sst_id] = new_sst;
  level_sst_ids[0].push_front(new_sst_id);

  // 7. 将 flushed_tranc_ids 通知给 tran_manager
  //    通知事务管理器哪些 tranc 已落盘
  //    参考实现这里没判空, 不炸纯属侥幸: flush_last 只给
  //    "空key+空value" 的 checkpoint 标记 entry 收集 id, 普通写入恒空
  //    我们补判空 (防御性偏离, 明说)
  if (auto tm = tran_manager.lock()) {
    for (auto &id : flushed_tranc_ids)
      tm->add_flushed_tranc_id(id);
  }

  // 8. 返回本次刷入 SST 的最大 tranc_id
  //    返回新 SST 的 max_tranc_id
  //    为什么返回 max？因为它的语义是水位线（watermark）：
  //      回答"这次刷盘把数据 durable 到哪了"
  return new_sst->get_tranc_id_range().second;
}

std::string LSMEngine::get_sst_path(size_t sst_id, size_t target_level) {
  // sst的文件路径格式为: data_dir/sst_<sst_id>，sst_id格式化为32位数字
  std::stringstream ss;
  ss << data_dir << "/sst_" << std::setfill('0') << std::setw(32) << sst_id
     << '.' << target_level;
  return ss.str();
}

  // Lab 4.7 谓词查询
std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
LSMEngine::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // ? 3. 构造 TwoMergeIterator 合并 memtable 结果和 sst 结果
  // ? 4. 若均为空返回 nullopt

  // 收集所有来源的候选记录
  // 此时必须保留墓碑，等跨来源比较完成后才能过滤
  std::vector<SearchItem> items; 

  {
    // 收集期间保护 SST 列表和文件，避免 flush/compact 改动它们
    // 收集完成后，items 自己持有 key/value，可以释放锁
    std::shared_lock<std::shared_mutex> lock(ssts_mtx);

    // 只有 key 和版本号都相同时，才比较来源优先级:
    //  1.  MemTable 最高
    //  2. 随后按层从浅到深分配递减的优先级
    size_t priority = ssts.size(); 

    // 1. 收集 MemTable 中满足谓词的记录
    //    从 memtable 查询: memtable.iters_monotony_predicate(tranc_id, predicate)
    // false 是 skip_delete=false：保留删除标记
    auto mem_it = memtable.begin(tranc_id, false); 
    for (; mem_it.is_valid(); ++mem_it){
      auto [key, value] = *mem_it; 
      const int pos = predicate(key); 

      // 单调谓词: 正数在范围左侧，0 命中，负数在范围右侧
      // 单调谓词规定:
      //    0：满足条件
      //    >0：需要向右找
      //    <0：需要向左找
      if (pos < 0) break; 
      if (pos > 0) continue; 

      items.emplace_back(
        std::move(key), 
        std::move(value), 
        priority, 0,
        mem_it.get_cur_tranc_id()
      ); 
    }


    // 2. 遍历所有 SST, 对每个 SST 调用 sst_iters_monotony_predicate
    //     将所有结果合并到 item_vec (注意过滤事务可见性和相同 key 只保留最新版本)
    //     level_sst_ids 按层号递增；L0 文件按新到旧排列
    for (const auto &[level, ids] : level_sst_ids){
      for(size_t id : ids){
        --priority; 
        auto range = sst_iters_monotony_predicate(ssts.at(id), tranc_id, predicate); 
        if (!range.has_value()) continue; 

        auto [it, range_end] = std::move(*range); 
        for (; it != range_end && !it.is_end(); ++it){
          //  可见性过滤可能让区间起点落在某个块的末尾
          //  此时不能解引用，让循环的 ++ 推进到后续块
          //  为什么？
          //  你当前的范围查询在 sst_iterator.cpp 会把这个块迭代器直接装入 SST 迭代器
          //  因此可能出现：
          //  it.is_valid() == false  // 当前没有可读取的记录
          //  it.is_end()   == false  // 仍持有块迭代器，尚未归一化为 SST 结束状态
          //  所以先 continue，避免执行 *it，
          //  for 循环的 continue 仍然会执行末尾的 ++it，
          //  由 SST 迭代器推进到下一个可见的 Block 
          if (!it.is_valid()) continue; 
          
          //  重新执行谓词
          //  是在保护查询范围边界；跨块推进也可能跨过块尾形式的 range_end，
          //  不只与不可见记录有关
          //  假设查询范围是 [key20, key60]，读取版本上限是 5：
          //  Block 0：key20@2   ← 可见，在范围内
          //  Block 1：key50@9   ← 不可见，在范围内
          //  Block 2：key70@2   ← 可见，但已超出范围
          //  这时范围函数可能把 range_end 设置在 Block 1 的尾后位置
          //  但从 key20@2 执行一次 ++it 时: 
          //  就出现问题: 迭代器直接跨过了 stop 所代表的位置，因此 it != stop 仍然成立
          //  重新执行谓词就能发现，解决问题
          auto [key, value] = *it; 
          const int pos = predicate(key); 

          // ++ 可能因不可见记录而跨过区间边界，再确认一次
          if (pos < 0) break; 
          if (pos > 0) continue; 
          
          // 不在收集阶段按 key 去重，也不删除墓碑
          // 使用真实版本号，让全局堆决定同 key 的胜者
          items.emplace_back(
            std::move(key), std::move(value), priority, 
            static_cast<int>(level), it.get_cur_tranc_id()
          ); 
        }
      }
    }
  }

  // 3. 所有来源进入同一个堆，统一处理：
  //    key 升序 → 版本降序 → 同版本按来源优先级
  //
  //    skip_delete=true：最新可见版本若为墓碑，整组 key 都跳过
  //    keep_all_versions=false：每个 key 只输出最新可见版本
  auto results = std::make_shared<HeapIterator>(
    std::move(items), tranc_id, true, false
  );

  // 候选记录可能全部被墓碑或可见性规则过滤
  // 因此要检查最终迭代器，不能只检查 items 是否为空
  if (!results->is_valid())
    return std::nullopt; 


  // 4. 接口要求返回 TwoMergeIterator
  //    全局归并已由上面的 HeapIterator 完成，
  //    这里用“结果流 + 空流”适配返回类型
  return std::make_pair(
  TwoMergeIterator(
      results,
      std::make_shared<HeapIterator>(), 
      tranc_id), 
  TwoMergeIterator{}
  ); 

}

// Lab 4.7: 返回 Level_Iterator(shared_from_this(), tranc_id)
Level_Iterator LSMEngine::begin(uint64_t tranc_id) {
  // shared_from_this() 获取当前引擎的共享指针，
  // 让迭代器持有引擎，保证遍历期间引擎仍然存在。
  //
  // tranc_id 是读取的版本上限；0 表示不限制版本。
  //
  // Level_Iterator 的构造函数已经负责：
  // 合并 MemTable 和各层 SST，选择最新可见版本，
  // 跳过墓碑，并定位到第一个有效 key；没有结果则处于结束状态。
  return Level_Iterator(shared_from_this(), tranc_id);
}

// Lab 4.7: 返回空的 Level_Iterator{}
Level_Iterator LSMEngine::end() {
  // 默认构造的迭代器没有子迭代器，iter_vec 为空，
  // 因此 is_end() 返回 true，用它表示遍历结束。
  //
  // 这里只创建结束标记，不需要读取数据或持有引擎。
  // Level_Iterator() 是函数声明， 需要使用{}
  return Level_Iterator{};
}

// Lab 4.5 整个 full compact: 将 src_level 的 sst 全体压缩到 src_level + 1
void LSMEngine::full_compact(size_t src_level) {
  // 锁纪律: 本函数不加锁, 调用方 (flush) 已持有 ssts_mtx 写锁

  // 1. 递归判断下一级 level 是否需要 compact
  //    level_sst_ids[src_level+1].size() >= ratio
  if (level_sst_ids[src_level + 1].size() >=
      TomlConfig::getInstance().getLsmSstLevelRatio())
    full_compact(src_level + 1);

  spdlog::debug("LSMEngine--Compaction: Starting full compaction from level{} "
                "to level{}",
                src_level, src_level + 1);

  // 2. 拷出源层/目标层的 sst_id (deque -> vector)
  //    拷贝是因为: 子函数签名要 vector&; 且第 4 步要清空 deque,
  //    手里的旧 id 列表必须独立存活
  auto old_level_id_x = level_sst_ids[src_level];
  auto old_level_id_y = level_sst_ids[src_level + 1];
  std::vector<size_t> lx_ids(old_level_id_x.begin(), old_level_id_x.end());
  std::vector<size_t> ly_ids(old_level_id_y.begin(), old_level_id_y.end());

  // 3. 根据 src_level 是否为 0 分别调用 full_l0_l1_compact 或
  // full_common_compact
  std::vector<std::shared_ptr<SST>> new_ssts;
  if (src_level == 0)
    new_ssts = full_l0_l1_compact(lx_ids, ly_ids);
  else
    new_ssts = full_common_compact(lx_ids, ly_ids, src_level + 1);

  // 4. 删除旧 SST 文件并从 ssts/level_sst_ids 中移除记录
  //    删旧: 磁盘文件 + ssts 映射 + 两层 deque
  //    时机: compact 函数已返回 (合并迭代器全部析构), 此时删文件安全
  for (auto &old_sst_id : old_level_id_x) {
    ssts[old_sst_id]->del_sst();
    ssts.erase(old_sst_id);
  }

  for (auto &old_sst_id : old_level_id_y) {
    ssts[old_sst_id]->del_sst();
    ssts.erase(old_sst_id);
  }

  level_sst_ids[src_level].clear();
  level_sst_ids[src_level + 1].clear();

  // 5. 将新的 SST 加入 level_sst_ids[src_level+1] 并排序
  //    登记新 SST 并按 id 排序
  //    为什么按 id 排 = 按 key 排: next_sst_id 单调递增, 且 gen_sst_from_iter
  //    按 key 升序生成 -> id 序就是 key 序 (参考原话: "此处没必要reverse了")
  for (auto &new_sst : new_ssts) {
    level_sst_ids[src_level + 1].push_back(new_sst->get_sst_id());
    ssts[new_sst->get_sst_id()] = new_sst;
  }

  // Q: 为什么"按 id 排"就等价"按 key 排"?
  // A: full_common_compact 和 full_l0_l1_compact 中的 gen_sst_from_iter 按 key
  // 升序产出 SST（合并迭代器全局有序）
  //    其中 gen_sst_from_iter 中 next_sst_id++ 全局单调递增 → 后生成的 SST id
  //    一定更大
  std::sort(level_sst_ids[src_level + 1].begin(),
            level_sst_ids[src_level + 1].end());

  spdlog::debug("LSMEngine--Compaction: Finished compaction. New SSTs added "
                "at level{}",
                src_level + 1);

  // 5. 更新最大层级
  cur_max_level = (std::max)(cur_max_level, src_level + 1);
}

// 负责完成 l0 和 l1 的 full compact
std::vector<std::shared_ptr<SST>>
LSMEngine::full_l0_l1_compact(std::vector<size_t> &l0_ids,
                              std::vector<size_t> &l1_ids) {
  std::vector<SstIterator> l0_iters;
  std::vector<std::shared_ptr<SST>> l1_ssts;

  // 1. L0 每张表各开一个全版本迭代器
  for (auto id : l0_ids) {
    auto sst_it = ssts[id]->begin(0, true);
    l0_iters.push_back(sst_it);
  }

  for (auto id : l1_ids)
    l1_ssts.push_back(ssts[id]);

  // 2. 把 L0 所有 SST 的全部 entry
  // 抽干，灌进一个大顶堆，得到一个全局有序的迭代器
  //    L0 各 SST 的 key 有重叠, 需要先通过 SstIterator::merge_sst_iterator 合并
  //    ConcactIterator 的前提是本层 SST 有序不重叠（首尾相接直接拼）, L0 不满足
  //    堆内 SearchItem 的排序规则是 key 升序 → tranc_id 降序 → 还分不出
  //    skip_delete=false 保墓碑;
  //    value 已 resolve_value 解过 WiscKey 引用
  auto [l0_begin, l0_end] = SstIterator::merge_sst_iterator(l0_iters, 0, true);

  // 3. TwoMergeIterator 只收 shared_ptr<BaseIterator>,
  //    而 merge_sst_iterator 给的是值 -> 堆上拷贝一份
  std::shared_ptr<HeapIterator> l0_begin_ptr =
      std::make_shared<HeapIterator>(l0_begin);

  // 4. L1 有序不重叠 -> ConcactIterator 直接串联
  //    再用 TwoMergeIterator 与 L1 的 ConcactIterator 合并
  std::shared_ptr<ConcactIterator> old_l1_begin_ptr =
      std::make_shared<ConcactIterator>(l1_ssts, 0, true);

  // 5. a 路 = L0 堆 (较新), b 路 = L1
  TwoMergeIterator l0_l1_begin(l0_begin_ptr, old_l1_begin_ptr, 0, true);

  // 6. 目标大小 = PerMemSizeLimit * ratio, 即 get_sst_size(1)
  //    最后调用 gen_sst_from_iter 生成新的 SST 文件
  //    目标大小 = PerMemSizeLimit * SstLevelRatio
  return gen_sst_from_iter(l0_l1_begin,
                           TomlConfig::getInstance().getLsmPerMemSizeLimit() *
                               TomlConfig::getInstance().getLsmSstLevelRatio(),
                           1);
}

// Lab 4.5 负责完成其他相邻 level 的 full compact
std::vector<std::shared_ptr<SST>>
LSMEngine::full_common_compact(std::vector<size_t> &lx_ids,
                               std::vector<size_t> &ly_ids, size_t level_y) {

  // 1. id -> SST handle
  std::vector<std::shared_ptr<SST>> lx_ssts;
  std::vector<std::shared_ptr<SST>> ly_ssts;

  for (auto id : lx_ids)
    lx_ssts.push_back(ssts[id]);
  for (auto id : ly_ids)
    ly_ssts.push_back(ssts[id]);

  // 2. 两层内部都有序不重叠 -> 各用一个 ConcactIterator 串联整层
  //    tranc_id=0: compact 不做可见性过滤; keep_all_versions=true: 全版本保留
  std::shared_ptr<ConcactIterator> old_lx_begin_ptr =
      std::make_shared<ConcactIterator>(lx_ssts, 0, true);
  std::shared_ptr<ConcactIterator> old_ly_begin_ptr =
      std::make_shared<ConcactIterator>(ly_ssts, 0, true);

  // 3. a 路 = lx (较新层, 同 key 时赢), b 路 = ly
  //    通过 TwoMergeIterator 合并后调用 gen_sst_from_iter
  TwoMergeIterator lx_ly_begin(old_lx_begin_ptr, old_ly_begin_ptr, 0, true);

  // 4. 目标层单 SST 容量 = get_sst_size(level_y) = PerMemSizeLimit *
  // ratio^level_y
  return gen_sst_from_iter(lx_ly_begin, LSMEngine::get_sst_size(level_y),
                           level_y);
}

// Lab 4.5 实现从迭代器构造新的 SST
std::vector<std::shared_ptr<SST>>
LSMEngine::gen_sst_from_iter(BaseIterator &iter, size_t target_sst_size,
                             size_t target_level) {
  // 参数的 SSTBuilder 构造函数

  std::vector<std::shared_ptr<SST>> new_ssts;

  // 0. WiscKey 分支照抄 flush(): 阈值 > 0 且 vlog_ 存在才走 vlog 模式
  //    注意: WiscKey 模式下需使用带 vlog
  size_t wk = TomlConfig::getInstance().getWisckeyValueThreshold();
  auto new_sst_builder =
      (wk > 0 && vlog_)
          ? SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true, vlog_,
                       wk)
          : SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true);

  // 1. 循环从迭代器取 key-value 写入 SSTBuilder
  while (iter.is_valid() && !iter.is_end()) {
    std::string cur_key = (*iter).first;
    // 三件套: key / value / 真实版本号
    // (keep_all_versions 模式下 get_tranc_id() 返回 entry 的真实 tranc_id)
    new_sst_builder.add(cur_key, (*iter).second, iter.get_tranc_id());
    ++iter;

    // 版本切分禁令:
    //  同 key 的不同版本不许被切到两个 SST
    //    否则 L1+ 相邻 SST 的 [first_key,last_key] 在边界 key 处重叠,
    //    engine 点查每层只二分命中一个候选 SST -> 老快照读漏版本
    bool next_is_same_key =
        iter.is_valid() && !iter.is_end() && (*iter).first == cur_key;

    // 2. 到阈值且不在版本中间 -> 落盘一个 SST, 重置 builder
    //    当 estimated_size >= target_sst_size 时 (注意不能在相同 key
    //    的不同版本之间切分)
    if (!next_is_same_key &&
        new_sst_builder.estimated_size() >= target_sst_size) {
      size_t sst_id = next_sst_id++;
      std::string sst_path = get_sst_path(sst_id, target_level);
      auto new_sst = new_sst_builder.build(sst_id, sst_path, block_cache);
      new_ssts.push_back(new_sst);

      spdlog::debug(
          "LSMEngine--Compaction: Generated new SST file with sst_id={} at "
          "level{}",
          sst_id, target_level);

      // 调用 builder.build() 生成 SST 之后并重置 builder
      // 迭代结束后若 builder 非空则再次 build
      new_sst_builder =
          (wk > 0 && vlog_)
              ? SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true,
                           vlog_, wk)
              : SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true);
    }
  }

  // 3. 收尾: builder 里还有没落盘的数据 -> 再 build 一个
  //    real_size() = 已完成的 block + 进行中的 block, 比 estimated_size 更全
  if (new_sst_builder.real_size() > 0) {
    size_t sst_id = next_sst_id++;
    std::string sst_path = get_sst_path(sst_id, target_level);
    auto new_sst = new_sst_builder.build(sst_id, sst_path, block_cache);
    new_ssts.push_back(new_sst);

    spdlog::debug(
        "LSMEngine--Compaction: Generated new SST file with sst_id={} at "
        "level{}",
        sst_id, target_level);
  }

  return new_ssts;
}

size_t LSMEngine::get_sst_size(size_t level) {
  if (level == 0) {
    return TomlConfig::getInstance().getLsmPerMemSizeLimit();
  } else {
    return TomlConfig::getInstance().getLsmPerMemSizeLimit() *
           static_cast<size_t>(std::pow(
               TomlConfig::getInstance().getLsmSstLevelRatio(), level));
  }
}

void LSMEngine::set_tran_manager(std::shared_ptr<TranManager> tran_manager) {
  this->tran_manager = tran_manager;
}

// *********************** LSM ***********************
LSM::LSM(std::string path)
    : engine(std::make_shared<LSMEngine>(path)),
      tran_manager_(std::make_shared<TranManager>(path)) {
  // TODO: Lab 5.5 控制WAL重放与组件的初始化
  // ? 1. 绑定 tran_manager 与 engine: 互相 set
  // ? 2. 调用 tran_manager_->check_recover() 获取需要重放的事务记录
  // ? 3. 遍历返回的 map<tranc_id, records>:
  // ?    - 若该 tranc_id 已在 flushed_tranc_ids 中则跳过 (已刷盘无需重放)
  // ?    - 否则根据 record.getOperationType() 调用 engine->put() 或
  // engine->remove() ? 4. 调用 tran_manager_->init_new_wal() 开启新的 WAL
  // 文件准备接收新写入
}

LSM::~LSM() {
  flush_all();
  tran_manager_->write_tranc_id_file();
}

std::optional<std::string> LSM::get(const std::string &key) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  auto res = engine->get(key, tranc_id);

  if (res.has_value()) {
    return res.value().first;
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, std::optional<std::string>>>
LSM::get_batch(const std::vector<std::string> &keys) {
  // 1. 获取事务ID
  auto tranc_id = tran_manager_->getNextTransactionId();

  // 2. 调用 engine 的批量查询接口
  auto batch_results = engine->get_batch(keys, tranc_id);

  // 3. 构造最终结果
  std::vector<std::pair<std::string, std::optional<std::string>>> results;
  for (const auto &[key, value] : batch_results) {
    if (value.has_value()) {
      results.emplace_back(key, value->first); // 提取值部分
    } else {
      results.emplace_back(key, std::nullopt); // 键不存在
    }
  }

  return results;
}

void LSM::put(const std::string &key, const std::string &value) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  engine->put(key, value, tranc_id);
}

void LSM::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  engine->put_batch(kvs, tranc_id);
}
void LSM::remove(const std::string &key) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  engine->remove(key, tranc_id);
}

void LSM::remove_batch(const std::vector<std::string> &keys) {
  auto tranc_id = tran_manager_->getNextTransactionId();
  engine->remove_batch(keys, tranc_id);
}

void LSM::clear() { engine->clear(); }

void LSM::flush() { auto max_tranc_id = engine->flush(); }

void LSM::flush_all() {
  while (engine->memtable.get_total_size() > 0) {
    auto max_tranc_id = engine->flush();
    // tran_manager_->update_checkpoint_tranc_id(max_tranc_id);
  }
}

LSM::LSMIterator LSM::begin(uint64_t tranc_id) {
  return engine->begin(tranc_id);
}

LSM::LSMIterator LSM::end() { return engine->end(); }

std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
LSM::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  return engine->lsm_iters_monotony_predicate(tranc_id, predicate);
}

// 开启一个事务
std::shared_ptr<TranContext>
LSM::begin_tran(const IsolationLevel &isolation_level) {
  auto tranc_context = tran_manager_->new_tranc(isolation_level);

  spdlog::info("LSM--"
               "lsm_iters_monotony_predicate: Starting query for tranc_id={}",
               tranc_context->tranc_id_);

  return tranc_context;
}

void LSM::set_log_level(const std::string &level) { reset_log_level(level); }
} // namespace tiny_lsm
