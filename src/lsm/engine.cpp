#include "lsm/engine.h"
#include "block/block_cache.h"
#include "config/config.h"
#include "iterator/iterator.h"
#include "logger/logger.h"
#include "lsm/level_iterator.h"
#include "lsm/transaction.h"
#include "lsm/two_merge_iterator.h"
#include "spdlog/spdlog.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include "wal/record.h"
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
LSMEngine::get(const std::string &key, uint64_t tranc_id,
               bool require_committed) {
  // 1. 从弱引用取得管理器的 shared_ptr
  //    后面由回调持有它，保证本次查询期间回滚名单仍然可用
  auto man = tran_manager.lock();

  // 查询需要有效的事务管理器来检查回滚状态
  // 从未绑定或管理器已经销毁时，lock() 返回空，下面直接报错
  if (!man)
    throw std::runtime_error("LSMEngine::get: transaction manager has expired");

  // 2. 为本次查询构造统一的事务状态判断。
  //
  // 底层传入的 id 是候选记录的真实写事务 ID
  // 它与本函数参数 tranc_id（读取版本上限）是两回事
  //
  // false：排除已回滚事务，允许活跃事务
  // true ：同时排除已回滚和仍活跃的事务
  //
  // 底层已有的回调约定是“返回 true 就跳过当前版本”
  // 因此可以复用现有传递路径，无需重写各层查找逻辑
  std::function<bool(uint64_t)> should_skip = [man = std::move(man),
                                               require_committed](uint64_t id) {
    return man->should_skip_read(id, require_committed);
  };

  // 3. 在内存表查找过程中就过滤不符合要求的版本
  //
  // 例如 k@5 尚未提交、k@3 已提交：
  // 严格读取应跳过 k@5，继续找到 k@3
  // 如果等查找结束才丢弃 k@5，就会漏掉被它遮住的 k@3
  //
  // get_record 在 MemTable 的锁内复制结果
  // 有效墓碑也先保留，后面还要与 SST 中的版本比较
  std::optional<std::pair<std::string, uint64_t>> best;

  // 3. 查询内存中的最大有效版本
  //    get_record 在 MemTable 自己的锁内查找、复制结果
  //    暂时保留墓碑，因为还需要与 SST 中的版本比较
  // best 是独立结果，后续原节点被更新不会影响它
  // nullopt 表示未命中；{"", id} 表示墓碑
  best = memtable.get_record(key, tranc_id, should_skip);

  // 4. SST 使用同一套读取条件，并与内存候选比较真实版本
  // 这里只读取 SST 集合，使用读锁；
  // sst_get_ 不自行加锁，由这里保护查询过程
  {
    std::shared_lock<std::shared_mutex> lock(ssts_mtx);
    auto sst_ret = sst_get_(key, tranc_id, should_skip);

    // 磁盘版本更大时才替换
    // 版本相同则保留内存来源，沿用原有优先级
    if (sst_ret.has_value() &&
        (!best.has_value() || sst_ret->second > best->second)) {
      best = std::move(sst_ret);
    }
  }

  // 5. 全部候选比较结束后，再处理最终胜出的墓碑
  // 有效墓碑表示该 key 已删除，不能退回其他来源的旧值
  if (!best.has_value() || best->first.empty())
    return std::nullopt;

  return best;
}

// Lab 4.2 批量查询
std::vector<
    std::pair<std::string, std::optional<std::pair<std::string, uint64_t>>>>
LSMEngine::get_batch(const std::vector<std::string> &keys, uint64_t tranc_id) {
  // 1. 批量获取内存候选
  // 每项都保留真实版本号，包括墓碑；结果顺序与输入一致
  // 与单条 get 使用相同的前置要求：必须有有效管理器。
  // 未绑定或已经销毁时，无法判断哪些版本已回滚，直接报错
  auto man = tran_manager.lock();
  if (!man)
    throw std::runtime_error(
        "LSMEngine::get_batch: transaction manager is unavailable");

  // 空输入没有需要查询的 key，返回空数组
  // 放在管理器检查之后，使空批次也遵守相同的初始化要求
  if (keys.empty())
    return {};

  // 2. 整个批次共用一个回滚判断
  //    回调持有 shared_ptr，保证批量查询期间管理器不会销毁
  //    tranc_id == 0 只取消版本上限，仍然需要过滤回滚版本
  std::function<bool(uint64_t)> is_aborted =
      [man = std::move(man)](uint64_t id) { return man->is_aborted(id); };

  // 3. 批量获取内存候选。
  //    MemTable 在内部持锁查找并复制结果，过滤已回滚版本。
  //    每个输入 key 都保留一个位置；未命中为 nullopt。
  //    有效墓碑暂时保留，随后还要与 SST 中的版本比较。
  auto results = memtable.get_batch(keys, tranc_id, is_aborted);

  // 4. 整个批次共用一次 SST 读锁
  // sst_get_ 不自行加锁，调用期间由这里保护 SST 集合
  {
    std::shared_lock<std::shared_mutex> lock(ssts_mtx);

    for (auto &[key, best] : results) {
      // 即使内存命中，也继续查 SST：
      // 过滤之后，磁盘仍可能存在版本号更大的有效记录
      auto sst_ret = sst_get_(key, tranc_id, is_aborted);

      // 只有磁盘版本更大才替换；相同版本保留内存来源
      if (sst_ret.has_value() &&
          (!best.has_value() || sst_ret->second > best->second))
        best = std::move(sst_ret);

      // 当前 key 的全部来源比较完，才能处理最终胜出的墓碑
      // 例如内存 k@5 是墓碑，SST k@7 是普通值，应返回 k@7
      // reset 只清空结果，不删除这个 key 在数组中的位置
      if (best.has_value() && best->first.empty())
        best.reset();
    }
  }

  // 原地更新结果，因此输入顺序和重复 key 都会保留
  return results;
}

void LSMEngine::update_best_from_sst(
    const std::shared_ptr<SST> &sst, const std::string &key, uint64_t tranc_id,
    std::optional<std::pair<std::string, uint64_t>> &best,
    const std::function<bool(uint64_t)> &is_aborted) {

  // 1. 从这张 SST 中选出满足读取上限、且没有回滚的最大版本
  // 例如 k@9 已回滚、k@5 有效，SST::get 应返回 k@5
  // 必须在 SST 内筛选，才能在排除 k@9 后继续找到同 key 的旧版本
  auto candidate = sst->get(key, tranc_id, is_aborted);

  // 2. 当前 SST 没有有效候选，保留之前其他 SST 提供的 best
  // 这里不能清空 best，因为“这张表没有”不代表“其他表也没有”
  if (!candidate.is_valid())
    return;

  // 3. 比较实际命中记录的写入版本，而不是本次读取的版本上限
  // 例如读取上限为 10、命中 k@5，这里取得的是 5
  const uint64_t version = candidate.get_cur_tranc_id();

  // 4. best 为空，或者当前候选版本更大，才更新结果
  // 相同版本保留先遇到的来源，沿用已有的来源优先级
  if (!best.has_value() || version > best->second) {
    // 有效墓碑也必须保存
    // 例如墓碑 k@9 胜过其他表的旧值 k@5，不能因为 value 为空就丢掉它
    best = std::make_pair(candidate->second, version);
  }
}

// Lab 4.2 sst 内部查询 (不查 memtable)
//         由于 sst 中无法保证先查询到的就是 tranc_id 最大的
//         所以我们需要用 best
//         保存目前找到的最大可见版本，查完其他候选来源再返回
std::optional<std::pair<std::string, uint64_t>>
LSMEngine::sst_get_(const std::string &key, uint64_t tranc_id,
                    const std::function<bool(uint64_t)> &is_aborted) {
  // 不加锁: 约定调用方已持有 ssts_mtx (get 的读锁 / compact 的写锁)
  // 沿用原来的锁约定：调用方已经持有 ssts_mtx
  std::optional<std::pair<std::string, uint64_t>> best;

  // 1. L0 的不同 SST 可以包含相同 key，因此逐张查询
  // 即使已经找到候选，也继续比较后面的来源
  if (level_sst_ids.find(0) != level_sst_ids.end()) {
    for (auto &sst_id : level_sst_ids[0]) {
      // 取出 sst 和 sst_it 一个一个查找
      // 回调传到底层：先在这张 SST 内排除回滚版本，
      // 再用选出的有效版本与 best 比较
      auto &sst = ssts.at(sst_id);
      update_best_from_sst(sst, key, tranc_id, best, is_aborted);
    }
  }

  // 2. L1 及更深层：同一层的 SST 按 key 有序、范围不重叠
  // 因此每层最多只有一张 SST 可能包含目标 key，用二分定位
  for (const auto &[lvl, ids] : level_sst_ids) {
    if (lvl == 0)
      continue;

    size_t left = 0, right = ids.size();

    while (left < right) {
      size_t mid = (left + right) / 2;
      auto &sst = ssts.at(ids[mid]);

      // key 落在当前 SST 的范围内，使用同一个回滚判断
      if (sst->get_first_key() <= key && key <= sst->get_last_key()) {
        update_best_from_sst(sst, key, tranc_id, best, is_aborted);
        // 本层其他 SST 不可能再包含这个 key，因此结束本层二分
        // 即使当前候选全部回滚，也仍要让外层循环继续检查更深层
        break;
      } else if (sst->get_last_key() < key)
        left = mid + 1;
      else
        right = mid;
    }
  }

  // 3. 所有候选来源都检查完后，再返回最终结果
  if (!best.has_value()) {
    spdlog::trace("LSMEngine::sst_get_({}, {}): key not exist", key, tranc_id);
    return std::nullopt;
  }

  // 有效墓碑也作为 {空字符串, 真实版本号} 返回
  // 上层还需要用它参与跨 MemTable/SST 比较或提交冲突检查
  return best;
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

// 清空引擎保存的数据：
// 1. 清空活跃和冻结 MemTable 中的记录
// 2. 清空各层 SST 的编号列表，以及引擎持有的 SST 对象
// 3. 删除数据库目录中的普通文件，包括 SST、WAL 和事务状态文件
// 4. 如果启用了 VLog，清理后重新创建空的 VLog
//
// 这是需要独占执行的维护操作：调用前应结束活跃事务、销毁迭代器
// 并停止旧 WAL 及其清理线程，避免删除仍在使用的日志文件
//
// 本函数负责存储层的数据清空；清空后的事务状态重置、状态文件保存
// 以及新 WAL 的初始化，需要由上层 LSM::clear() 流程继续完成
void LSMEngine::clear() {
  // 1. 调用方必须先停止旧 WAL，并保证没有并发操作和存活的迭代器
  // 清空内存中的记录，以及引擎维护的 SST 目录信息
  memtable.clear();
  level_sst_ids.clear();
  ssts.clear();

  // 2. 删除数据库目录中的普通文件
  // 包括旧 SST、WAL 和事务状态文件；子目录不在当前清理范围内
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
    // 删除失败时，旧文件可能仍然存在
    // 必须让上层知道清空没有完成，停止后续事务状态重置和新 WAL 初始化
    //
    // 只打印日志然后返回，会让上层误以为清空成功
    // throw; 保留原异常，让调用者处理失败原因
    spdlog::error("Error clearing directory: {}", e.what());
    throw;
  }

  // 3. 沿用现有 VLog 的清理与重新打开流程
  // 重新打开失败时，异常自然向上传播，上层同样不能继续按成功处理
  if (vlog_) {
    vlog_->del_vlog();
    vlog_ = VLog::open(data_dir + "/vlog.data");
  }

  // 4. 清空成功后，由上层 LSM::clear() 继续重置、保存事务状态
  // 然后初始化新 WAL
}

// Lab 4.1 刷盘形成sst文件
uint64_t LSMEngine::flush() {

  // 1. 内存表完全为空，没有业务数据或完成标记需要处理
  if (memtable.get_total_size() == 0)
    return 0;

  // 2. 加 ssts_mtx 写锁
  //    刷盘会修改 SST 映射和层级列表(ssts / level_sst_ids) 与未来的读路径 get()
  //    互斥 需要获取写锁 同时让多个线程的刷盘操作依次执行，避免同时修改这些结构
  std::unique_lock<std::shared_mutex> lock(ssts_mtx);

  //    L0 文件数量达到阈值时，先执行合并; 若 L0 层 SST 数量 >=
  //    LsmSstLevelRatio, 先触发 full_compact(0)
  //
  //    步骤 2 的 find(0) != end() 守卫：防 map 的 operator[] 凭空造出空 L0
  //    队列参与比较
  if (level_sst_ids.find(0) != level_sst_ids.end() &&
      level_sst_ids[0].size() >=
          TomlConfig::getInstance().getLsmSstLevelRatio())
    full_compact(0);

  // 3. 为本次可能生成的 SST 准备编号和路径: 分配新的 sst_id: next_sst_id++
  size_t new_sst_id = next_sst_id++;
  auto sst_path = get_sst_path(new_sst_id, 0);

  // 4. 处理最老的内存表, 选 builder 模式, 把最老的冻结表刷成 L0 SST
  // 构造 SSTBuilder:
  //    - 若 WiscKey 阈值 > 0 且 vlog_ 存在, 使用 WiscKey 模式的构造函数
  //    - 否则使用普通模式
  //
  // 调用 memtable.flush_last() 生成 SST 文件
  //
  // flush_last 提供两个结果：
  //   new_sst：本次生成的 SST，可能为空；
  //   flushed_tranc_ids：本次可以登记刷盘完成的事务 ID。
  //
  // 例如 A@5 已经在之前刷盘，当前表只剩完成标记@5：
  // 此时无需再创建 SST，但仍需要返回 ID 5，供下面通知管理器
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

  // 5. 只有生成了 SST，才登记文件并读取其中的最大版本号
  //
  // 没有 SST 时不能直接 return：
  // 本次仍可能处理了只有完成标记的内存表，需要继续登记事务状态
  uint64_t max_processed_id = 0;

  if (new_sst) {
    // 先让新 SST 被引擎正常管理，再报告事务刷盘完成
    // 更新 ssts 和 level_sst_ids[0] (push_front 保证新的在前)
    // 登记: id->SST 映射 + L0 队列头插 (新的在前, 查询从新到旧)
    ssts[new_sst_id] = new_sst;
    level_sst_ids[0].push_front(new_sst_id);

    max_processed_id = new_sst->get_tranc_id_range().second;
  }

  // 6. 将 flushed_tranc_ids 通知给 tran_manager
  //    通知事务管理器哪些 tranc 已落盘
  //    参考实现这里没判空, 不炸纯属侥幸: flush_last 只给
  //    flush_last 必须保证：对应业务数据成功刷盘后，才输出这些 ID
  //    这里收到的是完成通知，不需要重新扫描 SST 验证
  //
  //    无论是否生成 SST，都处理返回的完成 ID， 这是新设计为这样的原因
  //    返回不依赖 new_sst
  //    flush_last 保证：对应业务数据成功刷盘后，才输出这些 ID
  auto manager = tran_manager.lock();

  for (uint64_t id : flushed_tranc_ids) {
    // 只有完成标记、没有 SST 时，也要把标记 ID 计入返回值。
    // 这个计算不依赖管理器是否存在。
    max_processed_id = std::max(max_processed_id, id);

    if (manager) {
      manager->add_flushed_tranc_id(id);
    }
  }

  // 7. 返回本次刷入 SST 的最大 tranc_id
  //    返回新 SST 的 max_tranc_id
  //    为什么返回 max？因为它的语义是水位线：
  //    回答"这次刷盘把数据持久化到哪了"
  //
  //    这个最大值不是连续检查点：
  //    返回 10，不代表事务 1～10 都已全部刷盘
  //    连续检查点仍由事务管理器根据完成集合维护
  //    如果本次什么都没有处理，返回值自然保持为 0
  return max_processed_id;
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

  // 1. 与 get、begin 一样，先取得有效的事务管理器。
  // 无法查询回滚名单时，应报错，不能当作“没有事务被回滚”。
  auto manager = tran_manager.lock();
  if (!manager) {
    throw std::runtime_error("LSMEngine::lsm_iters_monotony_predicate: "
                             "transaction manager is unavailable");
  }

  // 本次查询的所有来源使用同一回滚判断。
  // 回调持有 shared_ptr，保证收集期间管理器仍然存在。
  std::function<bool(uint64_t)> is_aborted =
      [manager = std::move(manager)](uint64_t id) {
        return manager->is_aborted(id);
      };

  // 收集所有来源的候选记录
  // 此时必须保留墓碑，等跨来源比较完成后才能过滤
  std::vector<SearchItem> items;

  // 保存原来的用户谓词，再用新的函数包装它
  // 因为我们需要在调用原本谓词之前，过滤空key空value
  // 后续调用 predicate，以及传给 SST/Block 的 predicate，
  // 都会先经过下面的检查
  predicate = [user_predicate =
                   std::move(predicate)](const std::string &key) -> int {
    // 本项目将空 key 保留给内部事务标记
    // 遇到它时直接跳过，不调用用户的谓词
    //
    // 空 key 排在所有普通 key 前面
    // 返回 1 表示“继续向右查找”，不能返回 -1 提前结束查询
    if (key.empty())
      return 1;

    // 普通 key 才交给原来的用户谓词处理
    // 不检查 value：普通墓碑仍需参与归并，才能遮住旧值
    return user_predicate(key);
  };

  {
    // 收集期间保护 SST 列表和文件，避免 flush/compact 改动它们
    // 收集完成后，items 自己持有 key/value，可以释放锁
    std::shared_lock<std::shared_mutex> lock(ssts_mtx);

    // 只有 key 和版本号都相同时，才比较来源优先级:
    //  1.  MemTable 最高
    //  2. 随后按层从浅到深分配递减的优先级
    size_t priority = ssts.size();

    // 1. 收集 MemTable 中满足谓词的记录
    //    从 memtable 查询: memtable.iters_monotony_predicate(tranc_id,
    //    predicate)
    //
    // false 是 skip_delete=false：有效墓碑必须保留，
    // 否则磁盘中的旧值可能在后面的归并中重新出现。
    //
    // 先传入回滚判断，让 MemTable 找到有效版本：
    // 例如 k@9 已回滚、k@5 有效，这里应取得 k@5
    auto mem_it = memtable.begin(tranc_id, false, is_aborted);
    for (; mem_it.is_valid(); ++mem_it) {
      auto [key, value] = *mem_it;
      const int pos = predicate(key);

      // 单调谓词: 正数在范围左侧，0 命中，负数在范围右侧
      // 单调谓词规定:
      //     0：满足条件
      //    >0：需要向右找
      //    <0：需要向左找
      if (pos < 0)
        break;
      if (pos > 0)
        continue;

      items.emplace_back(std::move(key), std::move(value), priority, 0,
                         mem_it.get_cur_tranc_id());
    }

    // 2. 遍历所有 SST, 对每个 SST 调用 sst_iters_monotony_predicate
    //     将所有结果合并到 item_vec (注意过滤事务可见性和相同 key
    //     只保留最新版本) level_sst_ids 按层号递增；L0 文件按新到旧排列
    for (const auto &[level, ids] : level_sst_ids) {
      for (size_t id : ids) {
        --priority;
        auto range = sst_iters_monotony_predicate(ssts.at(id), tranc_id,
                                                  predicate, is_aborted);
        if (!range.has_value())
          continue;

        auto [it, range_end] = std::move(*range);
        for (; it != range_end && !it.is_end(); ++it) {
          //  可见性过滤可能让区间起点落在某个块的末尾
          //  此时不能解引用，让循环的 ++ 推进到后续块
          //  为什么？
          //
          //  你当前的范围查询在 sst_iterator.cpp 会把这个块迭代器直接装入 SST
          //  迭代器 因此可能出现： it.is_valid() == false  //
          //  当前没有可读取的记录 it.is_end()   == false  //
          //  仍持有块迭代器，尚未归一化为 SST 结束状态 所以先
          //  continue，避免执行 *it， for 循环的 continue 仍然会执行末尾的
          //  ++it， 由 SST 迭代器推进到下一个可见的 Block
          if (!it.is_valid())
            continue;

          //  重新执行谓词
          //  是在保护查询范围边界；跨块推进也可能跨过块尾形式的 range_end，
          //  不只与不可见记录有关
          //  假设查询范围是 [key20, key60]，读取版本上限是 5：
          //  Block 0：key20@2   ← 可见，在范围内
          //  Block 1：key50@9   ← 不可见，在范围内
          //  Block 2：key70@2   ← 可见，但已超出范围
          //  这时范围函数可能把 range_end 设置在 Block 1 的尾后位置
          //  但从 key20@2 执行一次 ++it 时:
          //  就出现问题: 迭代器直接跨过了 stop 所代表的位置，因此 it != stop
          //  仍然成立 重新执行谓词就能发现，解决问题
          auto [key, value] = *it;
          const int pos = predicate(key);

          // ++ 可能因不可见记录而跨过区间边界，再确认一次
          if (pos < 0)
            break;
          if (pos > 0)
            continue;

          // 不在收集阶段按 key 去重，也不删除墓碑
          // 使用真实版本号，让全局堆决定同 key 的胜者
          items.emplace_back(std::move(key), std::move(value), priority,
                             static_cast<int>(level), it.get_cur_tranc_id());
        }
      }
    }
  }

  // 3. 所有来源进入同一个堆，统一处理：
  //    key 升序 → 版本降序 → 同版本按来源优先级
  //
  //    skip_delete=true：最新可见版本若为墓碑，整组 key 都跳过
  //    keep_all_versions=false：每个 key 只输出最新可见版本
  auto results =
      std::make_shared<HeapIterator>(std::move(items), tranc_id, true, false);

  // 候选记录可能全部被墓碑或可见性规则过滤
  // 因此要检查最终迭代器，不能只检查 items 是否为空
  if (!results->is_valid())
    return std::nullopt;

  // 4. 接口要求返回 TwoMergeIterator
  //    全局归并已由上面的 HeapIterator 完成，
  //    这里用“结果流 + 空流”适配返回类型
  return std::make_pair(
      TwoMergeIterator(results, std::make_shared<HeapIterator>(), tranc_id),
      TwoMergeIterator{});
}

// Lab 4.7: 返回 Level_Iterator(shared_from_this(), tranc_id)
Level_Iterator LSMEngine::begin(uint64_t tranc_id) {

  // 1. 从弱引用取得事务管理器
  // 与 get/get_batch 的约定一致：公开查询需要有效管理器
  // 才能判断哪些版本已经回滚
  auto manager = tran_manager.lock();
  if (!manager) {
    throw std::runtime_error(
        "LSMEngine::begin: transaction manager is unavailable");
  }

  // 2. 创建本次遍历共用的回滚判断
  // 按值捕获 shared_ptr，让保存此回调的子迭代器持有管理器
  // 这样 begin 返回后，后续跨块、跨 SST 读取仍能查询回滚状态
  //
  // tranc_id == 0 只表示没有版本上限，仍然要过滤已回滚版本
  std::function<bool(uint64_t)> is_aborted =
      [manager = std::move(manager)](uint64_t id) {
        return manager->is_aborted(id);
      };

  // 3. shared_from_this() 获取当前引擎的共享指针，
  // 让迭代器持有引擎，保证遍历期间引擎仍然存在。
  //
  // tranc_id 是读取的版本上限；0 表示不限制版本。
  //
  // Level_Iterator 的构造函数已经负责：
  // 合并 MemTable 和各层 SST，选择最新可见版本，
  // 跳过墓碑，并定位到第一个有效 key；没有结果则处于结束状态。
  //
  // Level 构造会把回调传给 MemTable、L0 和 L1+，
  // 完成归并并定位到第一条可返回的记录。
  //
  // SST 集合的读锁由 Level 自己持有，这里无需再获取同一把锁
  return Level_Iterator(shared_from_this(), tranc_id, is_aborted);
}

// Lab 4.7: 返回空的 Level_Iterator{}
Level_Iterator LSMEngine::end() {
  // 默认构造的迭代器没有子迭代器，iter_vec 为空，
  // 因此 is_end() 返回 true，用它表示遍历结束。
  //
  // 这里只创建结束标记，不需要读取数据或持有引擎
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

  // 当前 builder 是否保存着尚未生成 SST 的记录
  // 空 builder 的 real_size() 也包含 2 字节编码开销
  // 因此不能用 real_size() > 0 判断是否存在记录
  bool has_pending_entries = false;

  // 1. 循环从迭代器取 key-value 写入 SSTBuilder
  while (iter.is_valid() && !iter.is_end()) {
    std::string cur_key = (*iter).first;

    // 三件套: key / value / 真实版本号
    // 使用 get_cur_tranc_id() 保留当前记录的真实写入版本
    // Compaction 使用保留全部版本的迭代器
    new_sst_builder.add(cur_key, (*iter).second, iter.get_cur_tranc_id());
    has_pending_entries = true;
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
      // 这批记录已经输出。换成空 builder，等待接收下一批记录，
      // 同时清除标志，表示现在没有待输出的数据。
      new_sst_builder =
          (wk > 0 && vlog_)
              ? SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true,
                           vlog_, wk)
              : SSTBuilder(TomlConfig::getInstance().getLsmBlockSize(), true);
      has_pending_entries = false;
    }
  }

  // 3. 收尾: builder 里还有没落盘的数据 -> 再 build 一个
  // 1. 最后一批不足阈值：标志为 true，需要生成 SST。
  // 2. 最后一批已在循环内输出：标志为 false，不再生成。
  // 3. 输入原本为空：标志始终为 false，也不生成 SST。
  if (has_pending_entries) {
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
// Lab 5.5 控制WAL重放与组件的初始化
LSM::LSM(std::string path)
    : engine(std::make_shared<LSMEngine>(path)),
      tran_manager_(std::make_shared<TranManager>(path)) {
  // 初始化列表已经创建了两个组件：
  // Engine 加载已有 SST，TranManager 读取已有事务状态
  // 此时还没有对外接收操作，也没有启动新的 WAL 清理线程

  // 1. 绑定组件
  // 绑定 tran_manager 与 engine: 互相 set
  //
  // 管理器创建事务时需要 Engine
  // Engine 查询回滚状态、报告刷盘结果时需要管理器
  tran_manager_->set_engine(engine);
  engine->set_tran_manager(tran_manager_);

  // 2. 读取候选日志，再恢复管理器状态
  // 调用 tran_manager_->check_recover() 获取需要重放的事务记录
  //
  // 先记住哪些事务已取消，以及下一个可用 ID
  // 后面才能正确决定哪些数据需要重放
  const auto recovery_records = tran_manager_->check_recover();
  tran_manager_->restore_recovery_state(recovery_records);

  // 3. 按事务 ID 依次处理候选记录
  // 遍历返回的 map<tranc_id, records>:
  // - 若该 tranc_id 已在 flushed_tranc_ids 中则跳过 (已刷盘无需重放)
  // - 否则根据 record.getOperationType() 调用 engine->put() 或 engine->remove()
  for (const auto &[tranc_id, records] : recovery_records) {
    // 已经处理完成或已经取消的事务不重放
    //
    // 这里同时检查连续检查点和零散刷盘集合：
    // 例如 flushed 原来为 {2, 4, 5}，重放事务 3 时可能触发刷盘
    // 集合随后压缩成 {5}，此时虽然 contains(4) 为 false
    // 但 4 <= checkpoint(5)，仍然应当跳过
    if (tranc_id <= tran_manager_->get_checkpoint_tranc_id() ||
        tran_manager_->get_flushed_tranc_ids().contains(tranc_id) ||
        tran_manager_->is_aborted(tranc_id))
      continue;

    // 4. 按原来的操作顺序，把修改重新应用到数据库
    //
    // 使用底层 Engine 接口，沿用日志中的原事务 ID
    // 不能调用 LSM::put/remove，因为普通入口会分配新的 ID
    // Engine 内部已有容量检查，需要时会冻结内存表、触发刷盘
    for (const auto &record : records) {
      const auto type = record.getOperationType();

      if (type == OperationType::OP_PUT) {
        engine->put(record.getKey(), record.getValue(), tranc_id);
      } else if (type == OperationType::OP_DELETE) {
        engine->remove(record.getKey(), tranc_id);
      }
      // CREATE、COMMIT 等记录用于判断事务状态
      // 不作为用户 key-value 写入
    }
    // 5. 所有数据操作都成功后，补上本事务的完成标记
    //
    // 与正常 commit 一样，空 key + 空 value 是内部标记
    // 后续刷盘遇到它，才会通知管理器：这笔事务已经刷盘完成
    // 必须放在该事务全部数据操作之后，不能提前报告完成
    engine->put("", "", tranc_id);

    spdlog::debug("LSM--Recover: Recovered transaction with tranc_id={}",
                  tranc_id);
  }

  // 6. 保存恢复后的事务状态
  // 包括新的 ID 进度、回滚名单，以及重放期间可能更新的刷盘进度
  // 保存失败直接抛异常，不能继续启动 WAL 清理线程
  tran_manager_->write_tranc_id_file();

  // 7. 最后创建本次运行使用的 WAL
  // 清理线程从此开始工作，使用的是刚才已经保存成功的检查点
  // 尚未刷盘的恢复数据，仍由保留下来的旧 WAL 保障后续恢复
  tran_manager_->init_new_wal();
}

LSM::~LSM() {
  try {
    // 1. 正常关闭时，先将剩余内存数据写入 SST
    // 再保存事务状态和实际的刷盘进度
    flush_all();
    tran_manager_->write_tranc_id_file();
  } catch (const std::exception &e) {
    // 2. 析构可能发生在处理其他异常的过程中
    // 不能让这里的刷盘或保存异常逃出析构，导致进程终止
    //
    // 析构没有返回值，因此通过日志记录具体原因
    // 记录错误不代表数据或事务状态已经保存成功
    spdlog::error("LSM: failed to flush or save state during destruction: {}",
                  e.what());
  } catch (...) {
    // 3. 非标准异常没有 what() 信息，也需要阻止它逃出析构。
    spdlog::error(
        "LSM: unknown error while flushing or saving state during destruction");
  }
}

std::optional<std::string> LSM::get(const std::string &key) {
  // 1. 普通查询不创建事务，也不分配新的事务 ID
  //
  // 0：不限制版本号大小，由 Engine 选择最大的有效版本
  // false：沿用普通查询允许读取其他事务未提交修改的约定
  // 无论是否允许读未提交，Engine 都会排除已回滚版本
  auto result = engine->get(key, 0, false);

  // 2. Engine 已经完成版本比较和墓碑处理
  // 没有找到有效值时，直接向用户返回“不存在”
  if (!result.has_value())
    return std::nullopt;

  // 3. Engine 返回的是 {value, 事务版本号}。
  // 普通用户接口只返回 value，将临时结果中的字符串移出去，
  // 避免再复制一份；版本号留在存储层内部使用。
  return std::move(result->first);
}

std::vector<std::pair<std::string, std::optional<std::string>>>
LSM::get_batch(const std::vector<std::string> &keys) {
  // 1. 普通批量查询不创建事务，也不分配新的事务 ID
  // 0 表示不限制版本号大小
  // 底层仍会排除回滚版本，并将有效墓碑处理为“不存在”
  auto batch_results = engine->get_batch(keys, 0);

  // 2. 将底层的 {key, optional<{value, version}>}
  // 转换成用户接口需要的 {key, optional<value>}
  //
  // 每个输入 key 都对应一个结果，先预留容量
  // 避免逐项添加时反复扩容
  std::vector<std::pair<std::string, std::optional<std::string>>> results;
  results.reserve(batch_results.size());

  // 3. 按底层返回顺序逐项转换
  // 底层已经保留输入顺序和重复 key，这里不重新排序或去重
  for (auto &[key, record] : batch_results) {
    if (record.has_value()) {
      // batch_results 是本次查询的临时结果
      // 将其中的 key 和 value 移入返回数组，减少字符串复制
      results.emplace_back(std::move(key), std::move(record->first));
    } else {
      // 没找到也保留这一项，用户才能知道哪个 key 不存在
      results.emplace_back(std::move(key), std::nullopt);
    }
  }

  // 空输入会自然得到空结果，不需要额外分支
  return results;
}

void LSM::put(const std::string &key, const std::string &value) {
  // 1. 先检查公开接口参数，非法调用不分配事务 ID。
  // 空 key 用作内部事务标记；空 value 表示墓碑。
  // 用户需要删除数据时，应调用 remove。
  if (key.empty()) {
    throw std::invalid_argument("LSM::put: key must not be empty");
  }
  if (value.empty()) {
    throw std::invalid_argument(
        "LSM::put: value must not be empty; use remove()");
  }

  // 2. 一次普通写入使用一个独立的新版本号
  // 提前预留三条记录的位置；如果这里分配失败，还没有消耗 ID
  std::vector<Record> records;
  records.reserve(3);
  const auto tranc_id = tran_manager_->getNextTransactionId();
  try {
    // 3. 这次写入的内容和结束决定已经确定
    // 因此可以一起准备 CREATE、PUT、COMMIT
    // 三条记录共用同一个 ID，恢复时属于同一组
    records.emplace_back(Record::createRecord(tranc_id));
    records.emplace_back(Record::putRecord(tranc_id, key, value));
    records.emplace_back(Record::commitRecord(tranc_id));
  } catch (...) {
    // 例如 key/value 超出 Record 编码长度。
    // 此时还没有调用 WAL，也没有修改数据，可以登记取消。
    //
    // 这里只是结算已经分配的 ID，避免检查点被这个编号挡住；
    // 没有旧值需要恢复，也不需要写回滚日志。
    tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                               TransactionState::ABORTED);
    throw;
  }

  // 4. 先将整组日志强制写出，再修改数据
  // write_to_wal 内部调用 WAL::log(records, true)
  //
  // 写日志失败可能已经留下部分内容，甚至包含 COMMIT
  // 因此这里仅报告失败，不能套用上面的“登记取消”分支
  if (!tran_manager_->write_to_wal(records))
    throw std::runtime_error("LSM::put: failed to write WAL");

  // 5. 日志已经确认 COMMIT，现在应用业务数据
  // Engine 自己处理锁、冻结和容量检查
  //
  // 这一步即使触发刷盘，也尚无本事务的完成标记
  // 因此不会误报“本事务已经全部刷盘”
  engine->put(key, value, tranc_id);

  // 6. 数据应用成功后，登记已经提交、等待刷盘确认
  // 必须在写完成标记之前登记：
  // 下一步可能立即触发刷盘，刷盘通知需要查到这份状态
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);

  // 7. 最后写内部完成标记
  // 数据先写、标记后写；结合最老表优先刷盘的顺序
  // 刷出标记时，才能确认该次写入的数据已经刷盘
  //
  // 从 WAL 确认 COMMIT 开始，后续异常都直接向上传播
  // 不能再把这次写入改成取消；恢复仍依据保留的日志
  engine->put("", "", tranc_id);
}

void LSM::put_batch(
    const std::vector<std::pair<std::string, std::string>> &kvs) {
  // 1. 空批次没有任何修改，不分配 ID，也不写日志。
  if (kvs.empty())
    return;

  // 2. 先检查整批参数，再开始处理
  // 例如第三项的 value 为空，前两项也不能提前写入
  // 空 key 保留给内部标记；删除操作使用 remove/remove_batch
  for (const auto &[key, value] : kvs) {
    if (key.empty())
      throw std::invalid_argument("LSM::put_batch: key must not be empty");

    if (value.empty())
      throw std::invalid_argument(
          "LSM::put_batch: value must not be empty; use remove_batch()");
  }

  // 3. 整批需要：一条 CREATE、每项一条 PUT、一条 COMMIT
  // 先预留容量，再分配 ID；预留失败时还没有消耗编号
  std::vector<Record> records;
  records.reserve(kvs.size() + 2);
  const auto tranc_id = tran_manager_->getNextTransactionId();

  try {
    records.emplace_back(Record::createRecord(tranc_id));

    // 保留输入顺序，不在这里按 key 去重
    // 同一批出现 {"a", "first"}、{"a", "last"} 时
    // 底层同 key、同版本的更新规则会让最后一项生效
    // WAL 也保持这个顺序，恢复才能得到相同结果
    for (const auto &[key, value] : kvs)
      records.emplace_back(Record::putRecord(tranc_id, key, value));

    records.emplace_back(Record::commitRecord(tranc_id));
  } catch (...) {
    // 任意一项超出 Record 编码限制时，整组记录准备失败。
    // 此时尚未调用 WAL，也没有把前面的项目写进引擎。
    // 登记取消，结算已经分配的 ID，再继续抛出异常。
    tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                               TransactionState::ABORTED);
    throw;
  }

  // 4. 全部记录准备成功后，一次写出完整日志
  // WAL 写入失败可能留下部分内容，不能直接登记取消
  if (!tran_manager_->write_to_wal(records))
    throw std::runtime_error("LSM::put_batch: failed to write WAL");

  // 5. 日志已确认 COMMIT，再将整批数据交给 Engine
  // 底层批量接口统一加锁、按顺序写入，并处理冻结和刷盘
  // LSM 不额外加锁，也不拆成多次普通 put
  engine->put_batch(kvs, tranc_id);

  // 6. 整批数据应用成功后，先登记已提交、等待刷盘确认
  // 下一步写完成标记可能立即触发刷盘，因此登记必须在前
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);

  // 7. 整批只需要一个完成标记，放在所有业务数据之后
  // 刷盘识别这个标记后，才能通知管理器本批写入已全部刷盘
  engine->put("", "", tranc_id);
}
void LSM::remove(const std::string &key) {
  // 1. 空 key 保留给内部事务标记，用户不能删除它
  // 在分配 ID 前检查，非法调用不消耗编号
  if (key.empty())
    throw std::invalid_argument("LSM::remove: key must not be empty");

  // 2. 一次普通删除使用一个独立的新版本号
  // 日志由 CREATE、DELETE、COMMIT 三条记录组成
  // 先预留容量，再分配 ID
  std::vector<Record> records;
  records.reserve(3);
  const auto tranc_id = tran_manager_->getNextTransactionId();

  try {
    records.emplace_back(Record::createRecord(tranc_id));

    // DELETE 只记录待删除的 key，不需要携带 value
    // deleteRecord 自己检查 key 和整条记录的编码长度
    records.emplace_back(Record::deleteRecord(tranc_id, key));

    records.emplace_back(Record::commitRecord(tranc_id));
  } catch (...) {
    // 构建记录失败时，还没有写 WAL，也没有删除数据
    // 登记取消，结算已经分配的 ID，避免挡住检查点推进
    tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                               TransactionState::ABORTED);
    throw;
  }

  // 3. 先确认完整删除日志已经写出，再修改存储层
  // WAL 报错可能已经留下部分内容，因此这里仅报告失败
  // 不能直接把可能已经写出 COMMIT 的操作登记为取消
  if (!tran_manager_->write_to_wal(records))
    throw std::runtime_error("LSM::remove: failed to write WAL");

  // 4. 删除不是物理移除旧记录，而是写入本版本的墓碑：
  // {key, "", tranc_id}。旧版本仍可供较早的读取上限查询
  //
  // key 当前不存在也允许删除，不需要先调用 get
  // 使用底层 remove，不能调用会拒绝空 value 的公开 LSM::put
  engine->remove(key, tranc_id);

  // 5. 墓碑应用成功后，登记已提交、等待刷盘确认
  // 必须先登记，再写可能立即触发刷盘的完成标记
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);

  // 6. 最后写完成标记，供刷盘流程确认本次删除已全部刷盘
  // WAL 确认 COMMIT 后，后续异常继续向上传播
  // 不再登记取消；保留日志供恢复完成这次删除
  engine->put("", "", tranc_id);
}

void LSM::remove_batch(const std::vector<std::string> &keys) {
  // 1. 空批次没有删除操作，不分配 ID，也不写日志。
  if (keys.empty()) {
    return;
  }

  // 2. 先检查全部 key，再开始处理。
  // 例如第三个 key 为空，前两个 key 也不能提前删除。
  // 空 key 保留给内部事务标记，不能由用户删除。
  for (const auto &key : keys) {
    if (key.empty()) {
      throw std::invalid_argument("LSM::remove_batch: key must not be empty");
    }
  }

  // 3. 整批日志：一条 CREATE、每个 key 一条 DELETE、一条 COMMIT。
  // 先预留容量，再为整批分配一个独立的新版本号。
  std::vector<Record> records;
  records.reserve(keys.size() + 2);
  const auto tranc_id = tran_manager_->getNextTransactionId();

  try {
    records.emplace_back(Record::createRecord(tranc_id));

    // 保留输入顺序，重复 key 也允许。
    // 同 key、同版本的重复删除只会更新同一个墓碑，
    // 不需要额外创建去重容器，也不需要先查询 key 是否存在。
    for (const auto &key : keys) {
      records.emplace_back(Record::deleteRecord(tranc_id, key));
    }

    records.emplace_back(Record::commitRecord(tranc_id));
  } catch (...) {
    // 任意 key 超出 Record 编码长度时，整组记录准备失败。
    // 此时尚未写 WAL，也没有删除前面那些合法 key。
    // 登记取消，结算已经分配的 ID，再继续抛出异常。
    tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                               TransactionState::ABORTED);
    throw;
  }

  // 4. 全部记录准备成功后，一次确认整组日志写出。
  // WAL 报错可能已经留下部分内容，不能直接登记取消。
  if (!tran_manager_->write_to_wal(records)) {
    throw std::runtime_error("LSM::remove_batch: failed to write WAL");
  }

  // 5. 日志已确认 COMMIT，再批量写入墓碑。
  // 底层统一加锁、按顺序删除，并处理冻结和自动刷盘。
  //
  // 不能循环调用公开 LSM::remove：
  // 那会为每个 key 分配不同 ID，拆成多笔独立删除。
  engine->remove_batch(keys, tranc_id);

  // 6. 全部墓碑写入成功后，登记已提交、等待刷盘确认。
  // 必须在完成标记之前登记，避免标记已经刷出却查不到状态。
  tran_manager_->add_ready_to_flush_tranc_id(tranc_id,
                                             TransactionState::OP_COMMITTED);

  // 7. 整批只写一个完成标记，放在所有墓碑之后。
  // WAL 确认 COMMIT 后的异常继续向上传播，
  // 不再登记取消；保留日志供恢复补齐这批删除。
  engine->put("", "", tranc_id);
}

void LSM::clear() {
  // 调用方必须保证整个清空过程没有并发数据库操作
  // 并已经销毁数据库迭代器；遍历结束不等于释放迭代器持有的锁
  //
  // 1. 先停止旧 WAL，等待清理线程退出并关闭日志文件
  // 如果还有活跃事务，stop_wal 会先抛异常
  // 此时还没有开始删除数据，调用者可以先结束事务再重试
  tran_manager_->stop_wal();

  // 2. 清空存储层的数据和旧文件
  // 旧 WAL 已关闭，现在删除它不会留下继续写入的旧句柄
  // 删除失败会抛异常，下面的状态重置不会执行
  engine->clear();

  // 3. 存储确认清空后，再重置事务管理器的内存状态
  // 清理旧的待刷盘记录和回滚名单，检查点覆盖已被主动清除的旧编号
  // 下一个事务 ID 保持递增，不重新从 1 开始
  tran_manager_->reset_after_clear();

  // 4. 保存清空后的检查点和下一个事务 ID
  // 即使清空后没有立刻写入新数据，关闭重开也能延续正确的编号
  // 保存失败时不能继续启动新 WAL
  tran_manager_->write_tranc_id_file();

  // 5. 最后创建并打开新的 WAL，启动新的清理线程
  // 后续 put/remove 会使用这个新日志文件
  // 不会再向已经删除的旧文件句柄写入
  tran_manager_->init_new_wal();
}

void LSM::flush() { auto max_tranc_id = engine->flush(); }

void LSM::flush_all() {
  while (engine->memtable.get_total_size() > 0) {
    auto max_tranc_id = engine->flush();
    // tran_manager_->update_checkpoint_tranc_id(max_tranc_id);
  }
}

// !
// SERIALIZABLE（未完整实现）：此遍历入口不会向事务登记读取范围，不提供幻读保护。
LSM::LSMIterator LSM::begin(uint64_t tranc_id) {
  return engine->begin(tranc_id);
}

LSM::LSMIterator LSM::end() { return engine->end(); }

// !
// SERIALIZABLE（未完整实现）：谓词只用于筛选结果，未登记为事务的范围读取依赖。
std::optional<std::pair<TwoMergeIterator, TwoMergeIterator>>
LSM::lsm_iters_monotony_predicate(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  return engine->lsm_iters_monotony_predicate(tranc_id, predicate);
}

// 开启一个事务
// ! SERIALIZABLE（未完整实现）：这里只传递所选级别，不会补齐完整串行化机制。
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
