#include "config/config.h"
#include "lsm/engine.h"
#include "lsm/transaction.h"
#include "memtable/memtable.h"
#include "spdlog/spdlog.h"
#include "utils/files.h"
#include "wal/record.h"
#include "wal/wal.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <algorithm>
#include <limits>

namespace tiny_lsm {

inline std::string isolation_level_to_string(const IsolationLevel &level) {
  switch (level) {
  case IsolationLevel::READ_UNOP_COMMITTED:
    return "READ_UNOP_COMMITTED";
  case IsolationLevel::READ_OP_COMMITTED:
    return "READ_OP_COMMITTED";
  case IsolationLevel::REPEATABLE_READ:
    return "REPEATABLE_READ";
  case IsolationLevel::SERIALIZABLE:
    // ! SERIALIZABLE（未完整实现）：仅返回级别名称，不表示已具备完整隔离保证。
    return "SERIALIZABLE";
  default:
    return "UNKNOWN";
  }
}

// *********************** TranContext ***********************
// Lab 5.2 构造函数初始化
// 初始化 engine, trancManager,
// isolation level, tranc_id, 和 operation
// isCommited、isAborted 已在头文件中默认初始化为 false
// 几个容器也会自动构造为空
// ! SERIALIZABLE（未完整实现）：这里只保存所选级别，相关分支仍为实验性实现。
TranContext::TranContext(uint64_t tranc_id, std::shared_ptr<LSMEngine> engine,
                         std::shared_ptr<TranManager> tranManager,
                         const enum IsolationLevel &isolation_level)
    // 按头文件中成员的声明顺序初始化
    // 保存引擎的 shared_ptr，让引擎在上下文存活期间保持有效
    : engine_(std::move(engine)),
      // 成员是 weak_ptr：管理器持有上下文，上下文弱引用管理器
      // 避免双方通过 shared_ptr 互相持有，导致无法释放
      // 这里即使使用移动语意依然调用 weak_ptr(const shared_ptr<Y>& r) noexcept
      // 构造函数 没有额外的移动收益
      tranManager_(tranManager),
      // ID 已由管理器分配，这里只保存
      tranc_id_(tranc_id), isolation_level_(isolation_level) {
  // 记录“事务开始”，后续 put/remove/commit 的操作记录追加在它后面
  // 此时只保存在内存中的 operations 中，没有写入 WAL 或 MemTable
  operations.emplace_back(Record::createRecord(tranc_id_));
}

void TranContext::persist_pending_wal_() {
  const auto record_count = operations.size();

  // 1. 已写条数不能大于当前记录总数
  // 例如清空 operations 却忘记同步计数，就会破坏这个关系
  // 明确报告内部状态错误，避免后面构造越界的迭代器区间
  if (wal_written_count_ > record_count)
    throw std::logic_error(
        "TranContext::persist_pending_wal_: invalid WAL record count");

  // 2. 没有新增记录，不需要再次发送
  // operations 为空且计数为 0 时，也自然在这里返回
  if (wal_written_count_ == record_count)
    return;

  // 3. 上下文保存的是管理器的 weak_ptr
  // 取得 shared_ptr，保证本次日志调用期间管理器仍然有效
  auto manager = tranManager_.lock();
  if (!manager)
    throw std::logic_error(
        "TranContext::persist_pending_wal_: transaction manager has expired");
  bool success = false;

  if (wal_written_count_ == 0) {
    // 4. 首次发送：全部记录都尚未写出
    // 直接传原数组，避免额外复制整批记录
    success = manager->write_to_wal(operations);
  } else {
    // 5. 后续发送：只取尚未写出的后缀
    //
    // 例如总共 3 条、已经写出 2 条
    // 就取 [begin + 2, end)，仅发送最后一条
    // 保留原 operations，通过副本适配现有 vector 接口
    const std::vector<Record> pending(operations.begin() + wal_written_count_,
                                      operations.end());
    success = manager->write_to_wal(pending);
  }

  // 6. 写入没有确认成功，停止当前流程
  // 计数保持不变，不能把失败的部分标记为已经写出
  if (!success)
    throw std::runtime_error(
        "TranContext::persist_pending_wal_: failed to write WAL");

  // 7. 全部新增记录写入成功，才推进已写位置
  // 这里只记录日志进度，不改变事务的提交或回滚状态
  wal_written_count_ = record_count;
}

// Lab 5.2 put 实现
// 读未提交：使用本事务 ID 直接写入引擎，不再备份旧值。
// 其他隔离级别： 暂存在本事务的 temp_map_，等 commit() 再写入引擎
void TranContext::put(const std::string &key, const std::string &value) {
  spdlog::trace("TranContext--put({}, {}), tranc_id={}", key, value, tranc_id_);

  // 1. 已经提交或回滚的事务不能继续写入
  // 必须先检查，避免给已结束的事务追加记录
  if (isCommited || isAborted)
    throw std::logic_error(
        "TranContext::put: transaction has already finished");

  // 2. 设置写开关，确认是否为读未提交
  const bool write_immediately =
      (isolation_level_ == IsolationLevel::READ_UNOP_COMMITTED);

  // 3. 所有隔离级别都记录本次操作，供后续 WAL 使用
  // RU 在下面立即写 WAL；其他级别等 commit 再统一写出
  operations.emplace_back(Record::putRecord(tranc_id_, key, value));

  // 4. 开始写入，有两种方式：立即写入 vs 暂存于上下文
  if (write_immediately) {
    // RU 会立即修改引擎，因此必须先写出对应日志。
    //
    // 第一次会连同 CREATE 一起发送；
    // 后续只发送新增记录，已写条数由辅助函数维护。
    //
    // 日志写入失败时，辅助函数抛异常，本次 put 在这里结束，
    // 后面的 engine_->put 不会执行。
    persist_pending_wal_();
    // 使用本事务的 ID 写入；底层查询此时就可能读到它
    engine_->put(key, value, tranc_id_);

    spdlog::trace("TranContext--put: applied to engine, tranc_id={}",
                  tranc_id_);
  } else {
    // 暂存在自己的上下文中，其他事务无法通过引擎读取这次修改
    // 重复修改同一个 key 时，暂存表保留最后一次写入的值
    // ! SERIALIZABLE（未完整实现）：同样走暂存路径，不为读过的 key
    // 或范围持有事务锁。
    temp_map_.insert_or_assign(key, value);

    spdlog::trace("TranContext--put: stored in temp map, tranc_id={}",
                  tranc_id_);
  }
}

void TranContext::remove(const std::string &key) {
  spdlog::trace("TranContext--remove({}) called, tranc_id={}", key, tranc_id_);

  // 1. 已结束的事务不能继续删除
  // 必须在追加日志记录、修改暂存表或引擎之前检查
  // 避免 abort 后又产生新的操作
  if (isCommited || isAborted) {
    throw std::logic_error(
        "TranContext::remove: transaction has already finished");
  }

  // 2. 所有隔离级别都需要先写入 operations 中
  // DELETE 只需要记录 key，删除发生在哪个版本由 tranc_id_ 标识
  operations.emplace_back(Record::deleteRecord(this->tranc_id_, key));

  // 3. 按隔离级别决定立即写入墓碑还是暂存墓碑
  auto isolation_level = get_isolation_level();
  if (isolation_level == IsolationLevel::READ_UNOP_COMMITTED) {
    // RU 会立即修改引擎，因此先写出删除日志。
    // 首次发送会包含 CREATE；后续只发送尚未写出的记录。
    //
    // 日志失败时，辅助函数抛异常，下面的引擎删除不会执行。
    persist_pending_wal_();

    // Engine::remove 会写入 {key, "", tranc_id_}
    // 这是新增或更新本事务的删除版本，不是物理删除历史节点
    // 不需要备份旧值：取消事务后，由查询过滤排除这个墓碑版本
    engine_->remove(key, tranc_id_);

    spdlog::trace(
        "TranContext--READ_UNOP_COMMITTED: remove({}) applied to memtable",
        key);

    return;
  }

  // 4. 空字符串表示“本事务准备删除这个 key”
  // 如果此前 put 过这个 key，就用墓碑覆盖暂存值
  //
  // 不能用 temp_map_.erase(key)：
  // 那会丢失删除意图，随后 get 可能重新读到数据库中的旧值。
  // 保留墓碑后，本事务 get 返回不存在，commit 再应用这次删除。
  // !
  // SERIALIZABLE（未完整实现）：这里仅暂存删除，不阻塞其他事务对读取范围的修改。
  temp_map_.insert_or_assign(key, std::string{});
  spdlog::trace("TranContext--{}: remove({}) stored in temp map",
                isolation_level_to_string(isolation_level_), key);
}

// Lab 5.2 get 实现
std::optional<std::string> TranContext::get(const std::string &key) {
  spdlog::trace("TranContext--get({}) called, tranc_id={}", key, tranc_id_);

  // 1. 已结束的事务不能继续使用
  if (isCommited || isAborted)
    throw std::logic_error(
        "TranContext::get: transaction has already finished");

  // 2. 优先返回本事务自己的暂存修改
  // 即使读缓存里保存了旧值，自己后来的 put/remove 也应该优先
  // 这些修改属于当前事务，因此不需要交给 Engine 排除未提交版本
  auto pending = temp_map_.find(key);
  if (pending != temp_map_.end()) {
    spdlog::trace("TranContext--get({}): found in temp map", key);

    // 空值是删除标记：返回“没查到”，不能返回一个有值的空字符串
    if (pending->second.empty())
      return std::nullopt;

    return pending->second;
  }

  // 3. 根据“正在读取的事务”的隔离级别确定要求
  //
  // 读未提交允许读取其他活跃事务的记录，因此传 false
  // 读已提交、可重复读及当前 SERIALIZABLE 分支传 true
  // 让 Engine 在查找过程中排除活跃事务的记录
  // 无论传什么，已经回滚的记录都会被排除
  // !
  // SERIALIZABLE（未完整实现）：排除未提交版本只是读取条件，不等于完整串行化。
  const bool require_committed =
      isolation_level_ != IsolationLevel::READ_UNOP_COMMITTED;

  std::optional<std::pair<std::string, uint64_t>> query;

  if (isolation_level_ == IsolationLevel::READ_UNOP_COMMITTED ||
      isolation_level_ == IsolationLevel::READ_OP_COMMITTED) {
    // 4. RU / RC 每次重新查询，不复用此前的读取结果。
    //
    // 版本上限传 0，允许看到后来产生的版本：
    // 例如本事务 ID 为 5，事务 6 后来提交，
    // RC 再次查询时应该允许读到事务 6 的修改。
    //
    // 0 只是不限制版本号；是否允许读取未提交记录，
    // 由第三个参数 require_committed 单独控制。
    query = engine_->get(key, 0, require_committed);
  } else {
    // 5. RR 保持同一个 key 的首次读取结果
    // ! SERIALIZABLE（未完整实现）：当前也复用这套单键缓存。
    // 不记录范围查询条件，不能据此保证范围结果不出现幻读。
    auto cached = read_map_.find(key);
    if (cached != read_map_.end()) {
      query = cached->second;
    } else {
      // 首次读取沿用本事务的版本上限，并排除未提交记录
      // 必须先取得合法结果再缓存，否则脏值或错误的“不存在”
      // 会一直留在本事务的读缓存中
      query = engine_->get(key, tranc_id_, require_committed);

      // nullopt 也要缓存：
      // “第一次没查到”同样是需要保持稳定的读取结果
      read_map_.emplace(key, query);
    }
  }

  // 6. Engine 已将最终胜出的墓碑转换成 nullopt
  // 对外只返回 value，真实版本号仍保留在读缓存中
  // 供需要验证读取版本的提交逻辑使用
  if (!query.has_value()) {
    spdlog::trace("TranContext--get({}): returned no value", key);
    return std::nullopt;
  }

  spdlog::trace("TranContext--get({}): returned value={}", key, query->first);
  return query->first;
}

// Lab 5.2 commit 实现
// commit() 提交流程：
// 检查冲突 → 写出 COMMIT → 应用数据 → 登记完成 → 按容量冻结、刷盘
// 冲突时取消整笔事务；暂存修改尚未写入，因此不需要恢复数据库旧值
// 同一个 TranContext 的操作由调用者串行执行
bool TranContext::commit(bool test_fail) {
  spdlog::info("TranContext--commit(): Starting commit for transaction ID={}",
               tranc_id_);

  // 1. 已结束的事务不能再次提交
  if (isCommited || isAborted)
    return false;

  // 这个开关用于后续 WAL 恢复测试，当前阶段尚不支持
  // 必须抛出异常；仅构造一个 logic_error 对象不会阻止后续执行
  if (test_fail)
    throw std::logic_error(
        "TranContext::commit: test_fail is reserved for recovery tests");

  auto manager = tranManager_.lock();
  if (!manager || !engine_)
    throw std::logic_error(
        "TranContext::commit: missing transaction manager or engine");

  // 如果上次结束流程异常退出，operations 中可能已经留下结束记录
  // 写日志报错不等于文件中一个字节也没写入
  // 因此不能自动再次提交，或者在已有回滚记录后追加 COMMIT
  for (const auto &record : operations) {
    const auto type = record.getOperationType();
    if (type == OperationType::OP_COMMIT ||
        type == OperationType::OP_ROLLBACK) {
      throw std::logic_error(
          "TranContext::commit: previous ending attempt did not finish");
    }
  }

  // 准备常用数据
  const auto &config = TomlConfig::getInstance();
  const auto &per_mem_limit = config.getLsmPerMemSizeLimit();
  const auto &total_mem_limit = config.getLsmTolMemSizeLimit();
  MemTable &mem = engine_->memtable;

  // 2. 加锁顺序必须与 Engine::flush 一致：
  //    SST -> 活跃表 -> 冻结表
  //
  //    如果一个线程 commit 先锁内存表，再等 SST
  //    而另外一个线程 flush 已经锁住 SST，再等内存表，两边就会互相等待
  //
  //    SST 这里只查询，使用读锁，阻止刷盘和压缩修改 SST 集合
  //    内存表的锁覆盖“检查冲突 + 写入”，防止检查后又插入冲突版本
  std::shared_lock<std::shared_mutex> sst_lock(engine_->ssts_mtx);
  std::unique_lock<std::shared_mutex> curr_lock(mem.cur_mtx);
  std::unique_lock<std::shared_mutex> frozen_lock(mem.frozen_mtx);

  using VersionValue = std::optional<std::pair<std::string, uint64_t>>;
  // 同一次提交中的所有冲突查询，共用这个回滚判断
  // 复制 shared_ptr 即可：后面还要用 manager 登记提交或取消
  // 因此这里不能把 manager move 走
  std::function<bool(uint64_t)> is_aborted = [manager](uint64_t id) {
    return manager->is_aborted(id);
  };

  // 3. 查询某个 key 当前真实的最新版本，保留墓碑
  //    0 表示不限制版本上限，才能发现比当前事务更新的记录
  //
  //    这里不能调用 Engine::get：
  //    ① 已经持有内存表的锁，再调用自行加锁的接口会重复加锁；
  //    ② Engine::get 会把墓碑转换成 nullopt，丢失删除操作的版本号
  //
  //    例如库里存在 k@12 的墓碑，事务 10 想写 k：
  //    删除也是一次更新，必须识别出 12 > 10，拒绝旧事务写入
  //    但已回滚版本必须排除，不能让它们造成错误冲突
  auto latest_record = [&](const std::string &key) -> VersionValue {
    auto latest = engine_->sst_get_(key, 0, is_aborted);
    auto memory = mem.get_(key, 0, is_aborted);

    if (memory.is_valid() &&
        (!latest || memory.get_cur_tranc_id() >= latest->second)) {
      latest = std::make_pair(memory.get_value(), memory.get_cur_tranc_id());
    }

    return latest;
  };

  const bool write_immediately =
      (isolation_level_ == IsolationLevel::READ_UNOP_COMMITTED);
  bool conflict = false;

  // 4. 所有暂存型事务先检查全部写入，再应用任何一条修改
  //
  //    例如事务 5 想写 k，但库里已有 k@6（包括墓碑），就拒绝提交
  //    否则新写入的 k@5 会被 k@6 遮住
  //
  //    这是针对本项目“开始 ID 就是写入版本”的保守冲突策略
  //    并非读已提交隔离级别本身要求必须这样检查
  //    这条策略适配本项目的版本编号方式，也用于读已提交事务。
  //    读未提交此前已直接写入引擎，不经过这里的暂存写入检查。
  if (!write_immediately) {
    for (const auto &[key, value] : temp_map_) {
      auto latest = latest_record(key);

      if (latest && latest->second > tranc_id_) {
        conflict = true;
        spdlog::warn("TranContext--commit(): Write conflict on key={}, "
                     "latest version={}, transaction ID={}",
                     key, latest->second, tranc_id_);
        break;
      }
    }
  }

  // 5. 单键读取校验
  // ! SERIALIZABLE（未完整实现）：当前只有以下校验，不具备完整可串行化保证。
  // read_map_ 保存本事务第一次读取某个 key 的结果。
  // 提交时若发现该结果已变化，就取消本事务；不会在读取后一直锁住该 key。
  //
  // ! 这里没有记录、验证范围查询条件，不能保证避免幻读。
  // 例如查询“年龄 > 30”得到 10 条记录，其他事务插入一名 31 岁员工：
  // 仅检查原先读过的 key，无法发现新增记录使结果变成了 11 条。
  // TODO: 后续独立补齐范围冲突处理等串行化保证，并验证完整隔离语义。
  if (!conflict && isolation_level_ == IsolationLevel::SERIALIZABLE) {
    for (const auto &[key, expected] : read_map_) {
      auto current = latest_record(key);

      // get() 将墓碑缓存为 nullopt，这里采用相同表示再比较
      // expected 为 nullopt 也参与比较，
      // 才能发现“原来不存在的 key，后来被创建了”
      if (current && current->first.empty())
        current.reset();

      if (current != expected) {
        conflict = true;
        spdlog::warn("TranContext--commit(): Read conflict on key={}, "
                     "transaction ID={}",
                     key, tranc_id_);

        break;
      }
    }
  }

  if (conflict) {
    // 冲突时终止整笔事务，取消整笔事务，不应用任何暂存修改
    // 此时数据仍在 temp_map_ 中，尚未写入数据库，因此无需恢复旧值。
    // 只需向管理器登记取消状态，并禁止这个事务上下文继续使用。
    manager->add_ready_to_flush_tranc_id(tranc_id_, TransactionState::ABORTED);

    isAborted = true;
    return false;
  }

  // 6. 检查通过后，追加 COMMIT 并强制写出尚未发送的日志。
  //
  // 暂存型事务：发送 CREATE、数据操作和 COMMIT
  // RU：之前的数据操作已写出，这里通常只发送新增的 COMMIT
  //
  // 写入未确认成功时，辅助函数抛异常；
  // 后面的数据应用、结束标记和提交登记都不会执行。
  operations.emplace_back(Record::commitRecord(tranc_id_));
  persist_pending_wal_();

  // 7. COMMIT 已确认写出，先记住这个提交决定
  // 后续即使内存处理失败，也不能再允许 abort 改成回滚
  //
  // 其他事务的严格读取仍由管理器的活跃表控制：
  // 下面写完数据并登记完成之后，才从活跃表移出
  // commit() 也只有正常走完整个流程才返回 true
  isCommited = true;

  try {
    // 7. 应用暂存修改。RU 已经写过数据，不重复应用
    // 已经持有内存表锁，使用不自行加锁的 put_
    // 空 value 表示删除，也必须写入
    if (!write_immediately) {
      for (const auto &[key, value] : temp_map_) {
        // 外层已经持锁，使用不自行加锁的 put_
        // 空 value 也写入，表示删除这个 key
        mem.put_(key, value, tranc_id_);
      }
    }

    // 所有隔离级别都补上结束标记
    // 后续刷盘识别这个标记，通过 Engine 通知管理器对应事务已刷盘
    mem.put_("", "", tranc_id_);

    // 先登记提交完成，再允许刷盘线程处理这些数据
    // 否则标记可能已经刷出，管理器却还没有这笔事务的结束登记
    manager->add_ready_to_flush_tranc_id(tranc_id_,
                                         TransactionState::OP_COMMITTED);

    // 8. 写完整笔事务及结束标记后，检查单表容量
    //    put_ 不会自动冻结，因此这里主动处理
    //    冻结只把旧表放入内存队列、换上新表，此时尚未写 SST
    if (mem.current_table->get_size() >= per_mem_limit)
      mem.frozen_cur_table_();

    // 9. 按逆序释放锁，然后才能调用自行加锁的容量查询和刷盘接口
    frozen_lock.unlock();
    curr_lock.unlock();
    sst_lock.unlock();

    // 10. 总内存达到阈值就刷盘；一次只刷一张表，所以每次重新检查
    //    空表直接结束，也避免阈值为 0 时反复尝试刷空表
    for (;;) {
      const auto &total_size = mem.get_total_size();

      if (total_size == 0 || total_size < total_mem_limit)
        break;

      engine_->flush();
    }

  } catch (const std::exception &e) {
    // WAL 已确认 COMMIT；后续数据应用、状态登记或内存维护仍可能失败。
    // 保留提交决定并向调用方报告，不能改成回滚。
    // 尚未手动释放的锁，会在函数退出时自动释放。
    spdlog::error("TranContext--commit(): WAL confirmed transaction ID={}, "
                  "but applying changes, registering state or memtable "
                  "maintenance failed: {}",
                  tranc_id_, e.what());

    // throw;：重新抛出当前异常，让调用者处理
    throw;
  }

  spdlog::info(
      "TranContext--commit(): Transaction ID={} committed successfully",
      tranc_id_);

  return true;
}

// 取消事务：按需写出 ROLLBACK → 登记回滚 → 清理上下文
// 已经写入存储层的版本由回滚名单过滤，不在这里恢复旧值
// !
// SERIALIZABLE（未完整实现）：同样沿用通用取消流程，不代表已具备完整隔离保证。
bool TranContext::abort() {
  // 1. 同一个上下文由调用者串行使用
  // 已经提交或取消的事务不能再次取消，也不能重复写 ROLLBACK
  if (isCommited || isAborted)
    return false;

  // 回滚必须登记到管理器，才能让其他查询排除本事务的版本
  // 取得 shared_ptr，保证后续操作期间管理器仍然存在
  auto manager = tranManager_.lock();
  if (!manager)
    throw std::logic_error(
        "TranContext::abort: transaction manager has expired");

  // 已写条数必须对应 operations 中实际存在的前缀
  // 在追加 ROLLBACK 前检查，避免追加操作掩盖原有的计数错误
  if (wal_written_count_ > operations.size())
    throw std::logic_error("TranContext::abort: invalid WAL record count");

  const bool write_immediately =
      (isolation_level_ == IsolationLevel::READ_UNOP_COMMITTED);

  // 已经确认发送过日志，需要再记录这笔事务被取消
  bool need_rollback_log = wal_written_count_ != 0;

  // 2. 检查此前是否已经尝试结束，并判断是否需要回滚日志。
  for (const auto &record : operations) {
    const auto type = record.getOperationType();

    // 本地尚未结束，却已经存在结束记录，说明上次结束流程异常退出。
    //
    // 尤其是 COMMIT 写入报错：不能认定磁盘里一定没有 COMMIT，
    // 因此不能随后追加 ROLLBACK，擅自把结果不确定的提交改成取消。
    // 已有 ROLLBACK 时也不自动重试，避免重复发送结束记录。
    if (type == OperationType::OP_COMMIT ||
        type == OperationType::OP_ROLLBACK) {
      throw std::logic_error(
          "TranContext::abort: previous ending attempt did not finish");
    }

    // RU 的 PUT/DELETE 会立即尝试写 WAL。
    // 即使第一次写入报错、已写计数仍为 0，也不能认定日志文件为空。
    // 因此这种情况仍走写 ROLLBACK 的流程；
    // 如果 WAL 已进入故障状态，辅助函数会抛异常，不假装日志写入成功。
    if (write_immediately &&
        (type == OperationType::OP_PUT || type == OperationType::OP_DELETE)) {
      need_rollback_log = true;
    }
  }

  // 3. 先记录取消决定，再向管理器登记取消。
  //
  // 例如 RU 已写出 CREATE、PUT、DELETE：
  // 这里只发送新追加的 ROLLBACK，之前的记录不会重复发送。
  //
  // 没有发送过日志的暂存事务，以及没有修改的只读事务，
  // 可以直接丢弃上下文，不必专门写一组 CREATE/ROLLBACK。
  if (need_rollback_log) {
    operations.emplace_back(Record::rollbackRecord(tranc_id_));
    persist_pending_wal_();
  }

  // 4. 登记回滚 ID
  // RU 的记录可能已经进入 MemTable 或 SST，查询会根据此名单跳过它们
  // 暂存事务还没有把数据写入存储层，后面直接清理暂存内容即可
  //
  // 登记函数内部负责加锁；这里不修改存储数据，无需获取内存表或 SST 锁
  // 如果登记失败，异常直接向上传播，不提前设置 isAborted 或清空现场
  manager->add_ready_to_flush_tranc_id(tranc_id_, TransactionState::ABORTED);

  // 登记成功后，正式结束这个上下文
  // 前面的检查已保证 isCommited 为 false，无需重复赋值
  isAborted = true;

  // 丢弃尚未提交的修改，以及这个事务自己的读取缓存
  // clear 只清理上下文容器，不修改数据库里的记录
  temp_map_.clear();
  read_map_.clear();

  // 取消已登记完成，清理上下文保存的日志历史
  // 记录数组与已写计数同时清空；已经写入的 WAL 文件内容不受影响
  operations.clear();
  wal_written_count_ = 0;
  spdlog::info("TranContext--abort(): Transaction ID={} aborted", tranc_id_);
  return true;
}

enum IsolationLevel TranContext::get_isolation_level() {
  // ! SERIALIZABLE（未完整实现）：返回所选级别，不表示相应隔离保证已完整实现。
  return isolation_level_;
}

// *********************** TranManager ***********************
TranManager::TranManager(std::string data_dir)
    : data_dir_(std::move(data_dir)) {
  // data_dir 按值传入，后面不再使用这个参数
  // 将它移动到成员中，避免再复制一次路径字符串
  //
  // 路径函数还负责将空目录参数规范为当前目录 "."
  auto file_path = get_tranc_id_file_path();

  // 1. 已有状态文件：恢复上次保存的事务状态
  if (std::filesystem::exists(file_path)) {
    // 表示打开已有文件，不清空其内容
    tranc_id_file_ = FileObj::open(file_path, false);
    // 恢复下一个 ID、刷盘进度、待处理状态和回滚名单
    // 读取函数内部会加锁，这里不要重复获取同一个 mutex
    //
    // 如果文件损坏或读取失败，直接让异常向上传播
    // 构造失败，调用者不会得到一个状态错误的管理器
    read_tranc_id_file();

    // 恢复成功后结束，不能再执行下面的新库初始化
    // 否则会把检查点 0 混入已经恢复的刷盘集合
    return;
  }

  // 2. 没有状态文件：建立首次启动的初始状态
  //
  // nextTransactionId_ 已在头文件中初始化为 1
  // active、ready 和 aborted 容器默认都是空的
  // flushed 则需要显式放入检查点 0，表示尚无已完成事务
  //
  // 先建立内存中的检查点，再创建文件
  flushedTrancIds_.insert(0);

  // true 表示创建文件；这个分支已经确认状态文件不存在
  // 初始状态及后续变化，由显式保存或正常析构写入文件
  tranc_id_file_ = FileObj::open(file_path, true);
}

void TranManager::init_new_wal() {
  spdlog::info("TranManager--init_new_wal(): Cleaning up old WAL files");

  // 1. 取得连续处理完成的检查点
  // 例如 flushed 为 {2, 4}，这里使用 2
  // 事务 3 尚未确认完成，不能让清理线程提前删除它的日志
  const uint64_t checkpoint = get_checkpoint_tranc_id(); 

  // 2. 沿用当前 WAL 参数，明确每个数字的含义
  constexpr size_t buffer_size = 128;         // 缓冲阈值：128 条记录
  constexpr uint64_t clean_interval = 1;      // 清理线程每隔 1 秒检查一次
  constexpr uint64_t file_size_limit = 4096;  // 文件轮换阈值：4096 字节

  // 3. 创建本次运行使用的 WAL
  //
  // WAL 构造函数已经负责选择新文件序号：
  // 例如已有 wal.2、wal.10，就创建 wal.11，保留旧文件内容
  // 这里直接复用，不再次扫描或删除旧日志
  //
  // 调用前，上层启动流程必须已成功保存与 checkpoint 对应的事务状态
  // 因为构造完成后清理线程就会开始工作，可能删除检查点覆盖的旧日志
  // “先保存状态，再调用本函数”的顺序将在 LSM 构造函数中接通
  wal = std::make_shared<WAL>(data_dir_, buffer_size, checkpoint, clean_interval, file_size_limit); 

  // 此处保持原来的刷盘集合和事务 ID。
  // 数据是否已经进入 SST，由真实的刷盘流程登记。
  spdlog::info(
      "TranManager--init_new_wal(): New WAL initialized, checkpoint={}",
      checkpoint);
}

void TranManager::set_engine(std::shared_ptr<LSMEngine> engine) {
  engine_ = std::move(engine);
}

// 自定义析构函数，是为了在 TranManager 被销毁时，自动做最后一次事务状态保存
TranManager::~TranManager() {
  try {
    write_tranc_id_file();
  } catch (const std::exception &e) {
    // 析构时已经无法让调用者重试，通过日志报告保存失败。
    spdlog::error("TranManager: failed to save state during destruction: {}",
                  e.what());
  } catch (...) {
    spdlog::error(
        "TranManager: unknown error while saving state during destruction");
  }
}

// Lab 5.2 持久化事务状态信息
// 保存事务管理器的元信息
// 本函数不刷 MemTable，也不写 WAL；只保存当前的事务状态记录
void TranManager::write_tranc_id_file() {
  // 保存期间不能让结束登记或刷盘登记修改这些集合
  // 同时让多个保存请求串行执行，避免互相覆盖文件
  std::lock_guard<std::mutex> lock(mutex_);

  const uint64_t next_id = nextTransactionId_.load();

  // 刷盘集合必须保留一个检查点，初始值为 0
  // 正常事务 ID 从 1 开始，next_id 为 0 表示分配器已经回绕
  // 这些状态不合法时明确报错，不把错误状态写进文件
  if (flushedTrancIds_.empty() || next_id == 0)
    throw std::logic_error(
        "TranManager::write_tranc_id_file: invalid transaction state");

  // 1. 先在内存中编码完整内容，再操作文件
  // 如果编码时分配内存失败，原有状态文件还没有被修改
  std::vector<uint8_t> buf;

  // 每个整数固定使用 8 字节，低位字节先写入（小端）：低位字节在前，高位字节在后
  // 例如数字 1 保存为：01 00 00 00 00 00 00 00
  auto append_u64 = [&](uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      buf.push_back(static_cast<uint8_t>(value >> shift) & 0xffu);
    }
  };

  // 2. 先保存下一个 ID，以及三个容器各有多少条记录。
  // 读取时根据数量，确定每一部分的边界。
  append_u64(next_id);
  append_u64(flushedTrancIds_.size());
  append_u64(readyToFlushTrancIds_.size());
  append_u64(aborted_tranc_ids_.size());

  // 3. 保存检查点和检查点之后零散完成的 ID
  // std::set 自带升序，直接按迭代顺序写入即可
  // 例如 {3, 5}：1～3 已连续完成，5 也完成，4 尚未确认
  // set 已按升序排列，直接按迭代顺序保存
  for (uint64_t id : flushedTrancIds_)
    append_u64(id);

  // 4. 保存已经结束、但还留在待处理集合中的事务
  // 每项需要同时保存 ID 和结束状态，才能区分提交与回滚
  // 文件中明确约定：0 表示提交，1 表示回滚
  for (const auto &[id, state] : readyToFlushTrancIds_) {
    append_u64(id);

    switch (state) {
    case tiny_lsm::TransactionState::ABORTED:
      append_u64(1);
      break;

    case tiny_lsm::TransactionState::OP_COMMITTED:
      append_u64(0);
      break;

    default:
      throw std::logic_error(
          "TranManager::write_tranc_id_file: unknown transaction state");
    }
  }

  // 5. 单独保存回滚名单
  // 一个回滚事务即使已经从 ready 中清理，其旧记录仍可能留在 SST
  // 重启后必须记住这个 ID，查询才能继续跳过它的记录
  // unordered_set 的输出顺序不固定，但恢复集合不依赖顺序
  for (uint64_t id : aborted_tranc_ids_)
    append_u64(id);

  // 6. 编码完成后，再覆盖状态文件
  // true 表示以截断模式打开：先清空旧内容，再写入新内容
  // 例如上次保存 96 字节、本次只有 48 字节
  // 必须删除多出来的旧尾部，不能只覆盖前 48 字节
  tranc_id_file_.close();
  tranc_id_file_ = FileObj::open(get_tranc_id_file_path(), true);

  // 检查写入和流缓冲刷新是否成功
  // write() 成功时内容仍可能在流缓冲区，最终刷新也必须单独检查
  if (!tranc_id_file_.write(0, buf) || !tranc_id_file_.sync()) {
    throw std::runtime_error(
        "TranManager::write_tranc_id_file: failed to save transaction state");
  }
}

// Lab 5.2 读取持久化的事务状态信息
// 读取文件 → 检查长度和数量 → 解码到临时容器 → 全部成功后替换成员
// 作者参考只恢复 nextTransactionId_ 和 flushedTrancIds_
// 我们还要对称恢复之前保存的 ready 和回滚名单
void TranManager::read_tranc_id_file() {
  // 与保存、结束登记和刷盘登记使用同一把锁
  // 本函数用于启动恢复，恢复完成后再对外提供事务服务
  std::lock_guard<std::mutex> lock(mutex_);

  // 1. 检查文件是否至少包含四个固定字段：
  // next_id、flushed 数量、ready 数量、aborted 数量
  // 每个字段都是 8 字节，后面的所有记录也由 8 字节整数组成
  const size_t file_size = tranc_id_file_.size();
  if (file_size < 32 || file_size % 8 != 0) {
    throw std::runtime_error(
        "TranManager::read_tranc_id_file: invalid file length");
  }

  const auto buf = tranc_id_file_.read_to_slice(0, file_size);
  size_t offset = 0;

  // 按写入时的小端顺序还原一个整数，同时推进读取位置
  // 先转为 uint64_t 再移位，保证高位字节也能正确参与计算
  //
  // 固定字段已由上面的长度检查保证完整
  // 容器内容则会在进入循环前统一检查长度
  auto read_u64 = [&]() {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      value |= static_cast<uint64_t>(buf[offset++]) << shift;
    }
    return value;
  };

  const uint64_t next_id = read_u64();
  const uint64_t flushed_count = read_u64();
  const uint64_t ready_count = read_u64();
  const uint64_t aborted_count = read_u64();

  // 2. 检查三个数量是否与文件剩余内容一致
  // remaining 表示还剩多少个“8 字节整数”，不是字节数
  //
  // flushed 每项占 1 个整数
  // ready 每项占 2 个整数：ID 和状态
  // aborted 每项占 1 个整数
  size_t remaining = (buf.size() - offset) / 8;

  // flushed 至少保留一个检查点；next_id 必须大于 0
  if (next_id == 0 || flushed_count == 0 || flushed_count > remaining)
    throw std::runtime_error("TranManager::read_tranc_id_file: invalid header");
  remaining -= static_cast<size_t>(flushed_count);

  // 先检查能放下多少对，再乘以 2 扣除
  // 避免文件中的错误大数在乘法或加法中溢出
  if (ready_count > remaining / 2)
    throw std::runtime_error(
        "TranManager::read_tranc_id_file: invalid ready count");
  remaining -= static_cast<size_t>(ready_count) * 2;

  // 最后一段必须刚好占完剩余内容
  // 既不能缺少记录，也不能残留未被这些数量解释的尾部数据
  if (aborted_count != remaining)
    throw std::runtime_error(
        "TranManager::read_tranc_id_file: invalid aborted count");

  // 3. 先恢复到临时容器，暂时不修改管理器成员
  // 后面任何解析或内存分配失败，都不会留下半份新状态
  std::set<uint64_t> flushed;
  std::map<uint64_t, TransactionState> ready;
  std::unordered_set<uint64_t> aborted;

  // 保存时 flushed 来自 set，因此文件里的 ID 应严格递增
  // 重复或降序都说明内容不符合我们的写入格式
  for (uint64_t i = 0; i < flushed_count; ++i) {
    const uint64_t id = read_u64();

    if (!flushed.empty() && id <= *flushed.rbegin())
      throw std::runtime_error(
          "TranManager::read_tranc_id_file: invalid flushed order");

    // 已确认 ID 递增，提示容器在末尾插入，避免重复查找位置
    flushed.emplace_hint(flushed.end(), id);
  }

  // ready 来自 map，同样按 ID 严格递增
  // 状态编码必须与写入函数一致：0 为提交，1 为回滚
  for (uint64_t i = 0; i < ready_count; ++i) {
    const uint64_t id = read_u64();
    const uint64_t state_code = read_u64();

    if (state_code > 1 || (!ready.empty() && id <= ready.rbegin()->first))
      throw std::runtime_error(
          "TranManager::read_tranc_id_file: invalid ready entry");

    const auto state = (state_code == 0) ? TransactionState::OP_COMMITTED
                                         : TransactionState::ABORTED;
    ready.emplace_hint(ready.end(), id, state);
  }

  // 回滚名单来自 unordered_set，不要求文件里的 ID 有序
  // 但同一个 ID 不应出现两次，避免自动去重掩盖错误内容
  for (uint64_t i = 0; i < aborted_count; ++i) {
    const uint64_t id = read_u64();

    if (!aborted.insert(id).second)
      throw std::runtime_error(
          "TranManager::read_tranc_id_file: duplicate aborted ID");
  }

  // 4. 所有内容都已成功读取并验证，再替换正式状态
  // 使用 swap 替换整个容器，避免重复加载时混入旧记录
  // 当前这些容器使用标准分配器和默认比较／哈希对象
  // 交换不需要重新分配节点；旧内容随后随临时容器销毁
  flushedTrancIds_.swap(flushed);
  readyToFlushTrancIds_.swap(ready);
  aborted_tranc_ids_.swap(aborted);
  nextTransactionId_.store(next_id);
}

bool TranManager::is_aborted(uint64_t tranc_id) const {
  // 查询期间，其他线程可能正在登记回滚并修改集合
  // 读取和写入必须使用同一把锁，避免同时访问正在变化的容器
  std::lock_guard<std::mutex> lock(mutex_);

  // contains 只查询，不插入元素，也不会复制整个集合
  // 空集合或没有找到该 ID 时，自然返回 false，无需额外判空
  return aborted_tranc_ids_.contains(tranc_id);
}

bool TranManager::should_skip_read(uint64_t tranc_id,
                                   bool require_committed) const {
  // 1. 在同一次加锁中检查回滚集合和活跃集合。
  //
  // 回滚登记会先加入回滚集合，再从活跃集合移除
  // 如果分成两次独立查询，可能发生：
  //   查回滚集合时尚未回滚；
  //   查活跃集合时已经回滚并被移除
  // 这样就会错误地认为该事务“既未回滚，也不活跃”
  // 与登记过程共用同一把锁，可以避免这种状态拼接
  std::lock_guard<std::mutex> lock(mutex_);

  // 2. 所有隔离级别都跳过已回滚事务的记录
  //
  // 回滚记录可能仍在 MemTable 或 SST 中，所以读取时必须排除
  // 这里直接查集合，不调用 is_aborted()：
  // 它内部也会获取 mutex_，重复获取同一把普通 mutex 会死锁
  if (aborted_tranc_ids_.contains(tranc_id))
    return true;

  // 3. 根据本次读取要求，决定是否跳过活跃事务
  //
  // require_committed == true：
  //   读已提交、可重复读等查询必须排除仍未结束的事务记录
  //   即使这条记录的版本号没有超过读取上限，也不能读它
  //
  // require_committed == false：
  //   读未提交允许读取活跃事务的记录，此处返回 false
  //   && 会短路，不再查询 activeTrans_
  //
  // 返回 false 仅表示通过事务状态检查
  // 版本上限、同 key 的版本选择仍由存储层负责
  return require_committed && activeTrans_.contains(tranc_id);
}

// Lab 5.2 负责登记事务的结束状态（已提交或已回滚），供后续刷盘时处理
// 如果是回滚，还会把事务 ID 加入回滚名单
void TranManager::add_ready_to_flush_tranc_id(uint64_t tranc_id,
                                              TransactionState state) {
  // 多个事务可能同时结束，查询线程也可能正在读取回滚名单
  // 用同一把锁保护本次涉及的三个容器，避免其他线程看到更新到一半的状态
  std::lock_guard<std::mutex> lock(mutex_);

  // 1. 回滚事务先加入名单，让查询能够排除它写入的所有版本
  //
  // insert 返回值中的 second 表示“这次是否真的新增了这个 ID”
  // 重复登记时为 false；后面如果失败，不能误删此前已经存在的回滚记录
  bool inserted_abort = false;
  if (state == TransactionState::ABORTED)
    inserted_abort = aborted_tranc_ids_.insert(tranc_id).second;

  // 2. 保存结束状态，交给后续刷盘登记使用
  // 首次登记插入记录；相同结束状态的重复登记更新已有记录
  try {
    readyToFlushTrancIds_.insert_or_assign(tranc_id, state);
  } catch (...) {
    // 例如 map 分配节点时内存不足：
    // 如果刚才已经新增了回滚 ID，就撤销本次新增，恢复调用前的状态
    // 此时尚未移出活跃集合，调用方也不会误以为登记成功
    if (inserted_abort)
      aborted_tranc_ids_.erase(tranc_id);

    throw;
  }

  // 3. 两处登记都成功后，这个事务才不再属于“正在执行的事务”
  // 移除的是管理器持有的 shared_ptr；用户仍持有句柄时，上下文继续有效
  activeTrans_.erase(tranc_id);
}

// Lab 5.2 更新持久化的事务状态信息
// 登记刷盘完成，并更新连续完成的检查点
// 已回滚事务可以结算，但其回滚名单仍须保留，供查询过滤使用
// flush() 写出 SST
//    ↓
// 确认这次刷出了哪些事务的完成标记
//    ↓
// 逐个调用 add_flushed_tranc_id(id)，登记结果
void TranManager::add_flushed_tranc_id(uint64_t tranc_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  // ready 表保存已经结束、等待结算的事务，按 ID 从小到大排列
  // 沿用作者的规则：处理本次通知的 ID，以及更小的已回滚事务
  // 大于本次 ID 的条目，留给后续处理
  auto it = readyToFlushTrancIds_.begin();
  while (it != readyToFlushTrancIds_.end() && it->first <= tranc_id) {
    const uint64_t ready_id = it->first;

    // 例如 ready 中有：
    //   1：已提交，2：已回滚，3：已提交
    // 本次收到的是事务 3 的刷盘通知：
    //   3 可以登记完成；
    //   2 已回滚，不需要保留其写入结果，可以一起结算
    //   1 虽然已提交，却可能仍在 MemTable，必须等自己的刷盘通知
    if (ready_id != tranc_id && it->second != TransactionState::ABORTED) {
      ++it;
      continue;
    }

    // 集合初始化为 {0}，压缩后首项表示 检查点 ：从开头到哪个
    // ID，已经连续处理完成 flushed 集合首项表示“从开头到这里，已经连续处理完成”
    // 例如 {3, 5} 表示 1～3 已完成，5 也完成，但 4 尚未确认
    //
    // 再次遇到 1 时，不应插回去：
    // {3, 5} 若变成 {1, 3, 5}，首项就错误地退回了 1
    // 因此只插入检查点之后的 ID
    if (ready_id > *flushedTrancIds_.begin()) {
      // 先记入完成集合，最后才删除 ready 条目。
      // 如果插入时内存分配失败，当前事务的待处理登记仍然保留。
      flushedTrancIds_.insert(ready_id);

      // 找到从当前检查点开始，最后一个连续的 ID
      // 例如 {0, 1, 2, 4}：
      // 0→1→2 连续，但缺少 3，所以必须停在 2，不能推进到 4
      auto last_contiguous = flushedTrancIds_.begin();
      auto next = last_contiguous;
      ++next;

      while (next != flushedTrancIds_.end() && *next - *last_contiguous == 1) {
        last_contiguous = next;
        ++next;
      }

      // 用最后一个连续 ID 代表前面整段，减少记录数量
      // {0, 1, 2, 4} -> {2, 4}，仍然能看出 3 尚未完成
      // erase 是左闭右开，因此保留 last_contiguous 指向的 2
      flushedTrancIds_.erase(flushedTrancIds_.begin(), last_contiguous);
    }

    // 当前 ID 已新登记完成，或者早已被检查点覆盖，
    // 都不必继续留在 ready 表中。erase 返回下一项，继续遍历
    //
    // 这里只清理待处理登记，不删除回滚名单
    // 回滚数据可能仍在 SST 中，查询依然需要通过名单排除它
    it = readyToFlushTrancIds_.erase(it);
  }
}

uint64_t TranManager::getNextTransactionId() {
  return nextTransactionId_.fetch_add(1);
}

std::set<uint64_t> &TranManager::get_flushed_tranc_ids() {
  return flushedTrancIds_;
}

uint64_t TranManager::get_max_flushed_tranc_id() {
  // 需保证 size 至少为1
  return *flushedTrancIds_.rbegin();
}

uint64_t TranManager::get_checkpoint_tranc_id() {
  // 需保证 size 至少为1
  return *flushedTrancIds_.begin();
}

// Lab 5.2 事务上下文分配
// 分配 ID → 创建上下文 → 登记到管理器 → 返回上下文
// SERIALIZABLE（未完整实现）：允许创建实验性上下文，但未提供完整的串行化调度。
std::shared_ptr<TranContext>
TranManager::new_tranc(const IsolationLevel &isolation_level) {
  spdlog::debug("TranManager--new_tranc(): Creating new transaction with "
                "isolation level={}",
                static_cast<int>(isolation_level));

  // 1. 设置锁:
  //    ID 计数器虽然是原子的，但 activeTrans_ 是普通 map
  //    多个线程可能同时创建事务，因此登记过程需要互斥保护
  std::lock_guard<std::mutex> lock(mutex_);

  // 2. 管理器必须先绑定引擎: 否则创建出的事务无法进行读写
  if (!engine_)
    throw std::logic_error("TranManager::new_tranc: engine is not initialized");

  // 3. 为这个事务分配唯一的 ID
  const auto tranc_id = getNextTransactionId();

  // 4. 创建事务上下文
  //    shared_from_this() 获取共享当前管理器所有权的 shared_ptr 并绑定
  //    TranContext 构造函数会将其保存为 weak_ptr，避免循环引用
  auto context = std::make_shared<TranContext>(
      tranc_id, engine_, shared_from_this(), isolation_level);

  // 5. 登记活跃事务，管理器持有该上下
  activeTrans_.emplace(tranc_id, context);

  spdlog::debug("TranManager--new_tranc(): Created transaction ID={} with "
                "isolation level={}",
                tranc_id, static_cast<int>(isolation_level));

  // 6. 返回同一个上下文，让调用者执行 put/get/commit 等操作
  return context;
}

std::string TranManager::get_tranc_id_file_path() {
  // 未指定数据目录时，使用当前目录
  // 同时规范成员值，供管理器的其他文件操作使用
  if (data_dir_.empty())
    data_dir_ = ".";

  return data_dir_ + "/tranc_id";
}

// 读取检查点之后的候选 WAL 记录，并返回给启动流程。
// 上层继续判断事务状态和刷盘情况，再决定是否重放。
// 本函数不直接把日志中的修改写入数据库。
std::map<uint64_t, std::vector<Record>> TranManager::check_recover() {
  spdlog::info("TranManager--check_recover(): Starting recovery from WAL");

  // 1. 管理器构造时已经读取了事务状态文件。
  // 这里取“连续处理完成”的检查点。
  // 例如 flushed 为 {2, 4}，检查点是 2，不能漏掉尚未完成的事务 3。
  const uint64_t checkpoint = get_checkpoint_tranc_id();

  // 2. 收集检查点之后的日志。
  // recover 是静态函数，所以此时不需要先创建 WAL 对象。
  // 文件读取失败时，异常直接向上传递，不能当作“没有日志”。
  auto records = WAL::recover(data_dir_, checkpoint);

  spdlog::info(
      "TranManager--check_recover(): Found {} transaction groups after "
      "checkpoint {}",
      records.size(), checkpoint);

  // 3. 将候选日志交给上层启动流程。
  // 返回结果仍可能包含回滚、未结束和零散已刷盘的事务，
  // 后续根据事务状态判断哪些需要重放。
  return records;
}


void TranManager::restore_recovery_state(const std::map<uint64_t, std::vector<Record>> &records){
  // 本函数只在启动恢复期间调用，此时没有并发用户事务
  // 没有候选日志时，保留状态文件已经恢复出来的进度
  if(records.empty())
    return; 

  // 1. 找到 WAL 中出现过的最大事务 ID
  // map 按 ID 排序，因此最后一项的 key 就是最大 ID
  //
  // 回滚和未结束事务的 ID 也已经被使用过，不能再次分配
  // 例如日志中最大 ID 为 7，新事务至少要从 8 开始
  const uint64_t max_wal_id = records.rbegin()->first; 

  // 最大整数不能再加 1，否则会回绕到 0，导致重复使用旧 ID
  // 先检查这一点，再开始修改管理器状态
  if (max_wal_id == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error(
        "TranManager::restore_recovery_state: transaction ID exhausted");

  // 状态文件中的进度也可能比 WAL 更靠后，因此只能向前推进
  // 先保留这些已经使用过的编号，再处理下面的事务登记
  // 推进 ID 不代表这些事务已经提交，也不代表数据已经刷盘
  if (nextTransactionId_.load() <= max_wal_id)
      nextTransactionId_.store(max_wal_id + 1); 

}

// Lab 5.4
bool TranManager::write_to_wal(const std::vector<Record> &records) {
  // 1. WAL 应在启动阶段完成初始化
  // 没有 WAL 时不能报告成功，否则上层会误以为日志已经写出
  // 此处也避免直接解引用空 shared_ptr
  if (!wal) {
    spdlog::error("TranManager--write_to_wal(): WAL is not initialized");
    return false;
  }

  try {
    // 2. 强制写入，不等待缓冲达到条数阈值
    //
    // 即使本次只有几条记录，也必须完成写入和文件流刷新
    // 才能向上层报告本次日志操作成功
    //
    // records 为空时也交给 log：它仍可能需要写出已有缓冲
    // WAL::log 内部负责加锁、写入和错误检查
    wal->log(records, true);
  } catch (const std::exception &e) {
    // 3. 将日志调用中的标准异常转换为失败结果
    // 上层收到 false 后，不能直接登记事务提交成功
    spdlog::error("TranManager--write_to_wal(): Exception occurred: {}",
                  e.what());
    return false;
  } catch (...) {
    // 无法取得具体错误信息时，也明确报告失败
    spdlog::error(
        "TranManager--write_to_wal(): Unknown exception while writing WAL");
    return false;
  }

  // 4. log 正常返回，说明它已经完成本次强制写入要求。
  // 这里只报告日志结果，不登记事务提交或推进 SST 检查点。
  spdlog::trace(
      "TranManager--write_to_wal(): Successfully wrote {} records to WAL",
      records.size());
  return true;
}

// void TranManager::flusher() {
//   while (flush_thread_running_.load()) {
//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     write_tranc_id_file();
//   }
// }
} // namespace tiny_lsm
