#include "config/config.h"
#include "lsm/engine.h"
#include "lsm/transaction.h"
#include "memtable/memtable.h"
#include "spdlog/spdlog.h"
#include "utils/files.h"
#include "utils/set_operation.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

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

// Lab 5.2 put 实现
// 读未提交：使用本事务 ID 直接写入引擎，不再备份旧值。
// 其他隔离级别： 暂存在本事务的 temp_map_，等 commit() 再写入引擎
void TranContext::put(const std::string &key, const std::string &value) {
  spdlog::trace("TranContext--put({}, {}), tranc_id={}", key, value, tranc_id_);

  // 1. 检查开关标记
  //    已经结束的事务不能继续接受写入
  if (isCommited || isAborted)
    throw std::logic_error(
        "TranContext::put: transaction has already finished");

  // 2. 设置写开关，确认是否为读未提交
  const bool write_immediately =
      (isolation_level_ == IsolationLevel::READ_UNOP_COMMITTED);

  // 3. 所有隔离级别都记录本次操作，供后续 WAL 使用
  //    0这里只追加内存记录，不执行日志持久化
  operations.emplace_back(Record::putRecord(tranc_id_, key, value));

  // 4. 开始写入，有两种方式：立即写入 vs 暂存于上下文
  if (write_immediately) {
    // 使用本事务的 ID 写入；底层查询此时就可能读到它
    engine_->put(key, value, tranc_id_);

    spdlog::trace("TranContext--put: applied to engine, tranc_id={}",
                  tranc_id_);
  } else {
    // 暂存在自己的上下文中，其他事务无法通过引擎读取这次修改
    // 重复修改同一个 key 时，暂存表保留最后一次写入的值
    temp_map_.insert_or_assign(key, value);

    spdlog::trace("TranContext--put: stored in temp map, tranc_id={}",
                  tranc_id_);
  }
}

void TranContext::remove(const std::string &key) {
  spdlog::trace("TranContext--remove({}) called, tranc_id={}", key, tranc_id_);

  // 1. 已结束的事务不能继续删除。
  // 必须在追加日志记录、修改暂存表或引擎之前检查，
  // 避免 abort 后又产生新的操作。
  if (isCommited || isAborted) {
    throw std::logic_error(
        "TranContext::remove: transaction has already finished");
  }

  // 2. 所有隔离级别都需要先写入 operations 中
  operations.emplace_back(Record::deleteRecord(this->tranc_id_, key));

  // 3. 按隔离级别决定立即写入墓碑还是暂存墓碑
  auto isolation_level = get_isolation_level();
  if (isolation_level == IsolationLevel::READ_UNOP_COMMITTED) {
    // Engine::remove 会写入 {key, "", tranc_id_}
    // 这是新增或更新本事务的删除版本，不是物理删除历史节点
    // 不需要备份旧值：取消事务后，由查询过滤排除这个墓碑版本
    engine_->remove(key, tranc_id_);

    spdlog::trace(
        "TranContext--READ_UNOP_COMMITTED: remove({}) applied to memtable",
        key);

    return;
  }

  // 空字符串表示“本事务准备删除这个 key”
  // 如果此前 put 过这个 key，就用墓碑覆盖暂存值
  //
  // 不能用 temp_map_.erase(key)：
  // 那会丢失删除意图，随后 get 可能重新读到数据库中的旧值。
  // 保留墓碑后，本事务 get 返回不存在，commit 再应用这次删除。
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

  // 2. 优先读取本事务自己的修改
  //    即使之前缓存过旧值，后续 put/remove 的结果也应优先
  auto pending = temp_map_.find(key);
  if (pending != temp_map_.end()) {
    spdlog::trace("TranContext--get({}): found in temp map", key);

    // 空值是删除标记：返回“没查到”，不能返回一个有值的空字符串
    if (pending->second.empty())
      return std::nullopt;

    return pending->second;
  }

  std::optional<std::pair<std::string, uint64_t>> query;

  if (isolation_level_ == IsolationLevel::READ_UNOP_COMMITTED ||
      isolation_level_ == IsolationLevel::READ_OP_COMMITTED) {
    // 3. 这两种级别不复用上一次查询结果，每次重新读取当前版本
    //    因为这两个级别都允许后一次查询看到新的修改，不要求一直返回第一次读到的值
    //    读已提交不能一直使用事务开始时的 ID，否则会挡住后续的新版本
    //    因为 tranc_id_ 在事务创建时就确定了。如果一直拿它作为查询上限，更大 ID
    //    的版本就一直查不到，即使那个版本已经提
    //
    //    注意：0 仅表示不限制版本上限，并不表示“只读取已提交记录”。
    //    混合隔离级别下的提交状态过滤，还需要事务模块进一步支持。
    query = engine_->get(key, 0);
  } else {
    // 4. 可重复读：先查读缓存，命中后直接复用第一次的结果
    //    SERIALIZABLE 的单键读缓存复用这里
    //    完整串行化还需要额外的并发控制，不能仅凭缓存实现
    auto cached = read_map_.find(key);

    if (cached != read_map_.end()) {
      query = cached->second;
    } else {
      // 首次读取使用本事务的版本上限
      query = engine_->get(key, tranc_id_);

      // 即使 query 是 nullopt，也必须缓存
      // “第一次没查到”也是需要保持稳定的查询结果
      read_map_.emplace(key, query);
    }
  }

  // 5. 检查我们的返回值是否有效
  //    Engine::get 已将胜出的墓碑转换成 nullopt
  //    对外只返回 value，真实版本号仍保留在读缓存中
  if (!query.has_value()) {
    spdlog::trace("TranContext--get({}): returned no value", key);
    return std::nullopt;
  }

  spdlog::trace("TranContext--get({}): returned value={}", key, query->first);
  return query->first;
}

// Lab 5.2 commit 实现
// commit() 提交流程：
// 检查是否允许提交 -> 应用修改 -> 登记提交完成 -> 按容量冻结、刷盘。
// 冲突时取消整笔事务；暂存修改尚未写入，因此不需要恢复数据库旧值。
// 当前阶段不写 WAL；同一个 TranContext 的操作由调用者串行执行。
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
        "TranContext::commit: test_fail requires WAL support");

  auto manager = tranManager_.lock();
  if (!manager || !engine_)
    throw std::logic_error(
        "TranContext::commit: missing transaction manager or engine");

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

  // 5. SERIALIZABLE 额外验证已记录的单键读取结果
  //    例如读 A 后决定修改 B，如果 A 已变化，就不能继续按旧结果提交
  //    这里检查的是 read_map_ 中的单键记录，不涵盖范围查询的幻读
  //    read_map_ 是当前事务自己的“读取记录表”，保存第一次查询某个 key
  //    时看到的结果
  //    1. 实现可重复读, 2. 给 SERIALIZABLE 的提交检查提供依据
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

  // 6. 检查全部通过后，才开始应用修改
  // operations 是内存中的操作记录，目前不会调用 write_to_wal
  operations.emplace_back(Record::commitRecord(tranc_id_));

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
  isCommited = true;

  try {
    // 7. 写完整笔事务及结束标记后，检查单表容量
    //    put_ 不会自动冻结，因此这里主动处理
    //    冻结只把旧表放入内存队列、换上新表，此时尚未写 SST
    if (mem.current_table->get_size() >= per_mem_limit)
      mem.frozen_cur_table_();

    // 8. 按逆序释放锁，然后才能调用自行加锁的容量查询和刷盘接口
    frozen_lock.unlock();
    curr_lock.unlock();
    sst_lock.unlock();

    // 9. 总内存达到阈值就刷盘；一次只刷一张表，所以每次重新检查
    //    空表直接结束，也避免阈值为 0 时反复尝试刷空表
    for (;;) {
      const auto &total_size = mem.get_total_size();

      if (total_size == 0 || total_size < total_mem_limit)
        break;

      engine_->flush();
    }
  } catch (const std::exception &e) {
    // 到这里事务已经提交。冻结或刷盘失败，不会撤销已应用的修改
    // 保留 isCommited == true，并原样抛出异常，供调用者处理刷盘故障
    // 若冻结时就失败，尚未手动释放的锁会在函数退出时自动释放
    spdlog::error(
        "TranContext--commit(): Transaction ID={} is already committed, "
        "but memtable maintenance failed: {}",
        tranc_id_, e.what());

    // throw;：重新抛出当前异常，让调用者处理
    throw;
  }

  spdlog::info(
      "TranContext--commit(): Transaction ID={} committed successfully",
      tranc_id_);

  return true;
}

// 请求管理器登记回滚，并结束上下文、清理本事务的暂存数据
// 查询侧已接入回滚过滤；
bool TranContext::abort() {
  // 同一个 TranContext 的操作由调用者串行执行
  // 已经提交或回滚的事务，不能再次取消
  if (isCommited || isAborted)
    return false;

  // 上下文持有的是 weak_ptr，管理器可能已经销毁
  // 回滚必须让管理器记住这个 ID，不能只修改本地标志
  auto manager = tranManager_.lock();
  if (!manager)
    throw std::logic_error(
        "TranContext::abort: transaction manager has expired");

  // 先登记回滚，再修改上下文状态
  // 如果登记抛异常，下面的代码不会执行
  // 上下文仍保留原来的状态和暂存内容，不会假装回滚成功
  //
  // 两类事务都登记同一个回滚 ID：
  // 1. 暂存型事务：修改尚未写入存储层，随后丢弃暂存内容
  // 2. 读未提交事务：记录可能已在 MemTable 或 SST 中
  //    后续查询根据回滚名单跳过这些版本，不在这里回写旧值
  //
  // 登记函数内部负责加锁，这里无需再次获取管理器的锁
  manager->add_ready_to_flush_tranc_id(tranc_id_, TransactionState::ABORTED);

  // 登记成功后，正式结束这个上下文
  // 前面的检查已保证 isCommited 为 false，无需重复赋值
  isAborted = true;

  // 丢弃尚未提交的修改，以及这个事务自己的读取缓存
  // clear 只清理上下文容器，不修改数据库里的记录
  temp_map_.clear();
  read_map_.clear();

  // 当前阶段没有把这些操作记录写入 WAL，可以直接丢弃
  // 后续接入 WAL 时，再处理回滚日志
  operations.clear();
  return true;
}

enum IsolationLevel TranContext::get_isolation_level() {
  return isolation_level_;
}

// *********************** TranManager ***********************
TranManager::TranManager(std::string data_dir) : data_dir_(data_dir) {
  auto file_path = get_tranc_id_file_path();

  // 判断文件是否存在

  if (!std::filesystem::exists(file_path)) {
    tranc_id_file_ = FileObj::open(file_path, true);
    flushedTrancIds_.insert(0);
  } else {
    tranc_id_file_ = FileObj::open(file_path, false);
    read_tranc_id_file();
  }
}

void TranManager::init_new_wal() {
  spdlog::info("TranManager--init_new_wal(): Cleaning up old WAL files");

  // TODO: 1 和 4096 应该统一用宏定义
  // 先清理掉所有 wal. 开头的文件, 因为其已经被重放过了
  for (const auto &entry : std::filesystem::directory_iterator(data_dir_)) {
    if (entry.path().filename().string().find("wal.") == 0) {
      std::filesystem::remove(entry.path());
    }
  }
  wal = std::make_shared<WAL>(data_dir_, 128, get_max_flushed_tranc_id(), 1,
                              4096);
  flushedTrancIds_.clear();
  flushedTrancIds_.insert(nextTransactionId_.load() - 1);
  spdlog::info("TranManager--init_new_wal(): New WAL initialized");
}

void TranManager::set_engine(std::shared_ptr<LSMEngine> engine) {
  engine_ = std::move(engine);
}

TranManager::~TranManager() { write_tranc_id_file(); }

// Lab 5.2 持久化事务状态信息
// 保存事务管理器的元信息
// 本函数不刷 MemTable，也不写 WAL；只保存当前的事务状态记录
void TranManager::write_tranc_id_file() {
  // 保存期间不能让结束登记或刷盘登记修改这些集合
  // 同时让多个保存请求串行执行，避免互相覆盖文件
  std::lock_guard<std::mutex> lock(mutex_); 
}

// TODO: Lab 5.2 读取持久化的事务状态信息
void TranManager::read_tranc_id_file() {}

bool TranManager::is_aborted(uint64_t tranc_id) const {
  // 查询期间，其他线程可能正在登记回滚并修改集合
  // 读取和写入必须使用同一把锁，避免同时访问正在变化的容器
  std::lock_guard<std::mutex> lock(mutex_);

  // contains 只查询，不插入元素，也不会复制整个集合
  // 空集合或没有找到该 ID 时，自然返回 false，无需额外判空
  return aborted_tranc_ids_.contains(tranc_id);
}

// Lab 5.2 负责登记事务的结束状态（已提交或已回滚），供后续刷盘时处理
// 如果是回滚，还会把事务 ID 加入回滚名单
void TranManager::add_ready_to_flush_tranc_id(uint64_t tranc_id, TransactionState state) {
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
  try{
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
void TranManager::add_flushed_tranc_id(uint64_t tranc_id){
  std::lock_guard<std::mutex> lock(mutex_); 

  // ready 表保存已经结束、等待结算的事务，按 ID 从小到大排列
  // 沿用作者的规则：处理本次通知的 ID，以及更小的已回滚事务
  // 大于本次 ID 的条目，留给后续处理
  auto it = readyToFlushTrancIds_.begin(); 
  while (it != readyToFlushTrancIds_.end() && it->first <= tranc_id){
    const uint64_t ready_id = it->first; 

    // 例如 ready 中有：
    //   1：已提交，2：已回滚，3：已提交
    // 本次收到的是事务 3 的刷盘通知：
    //   3 可以登记完成；
    //   2 已回滚，不需要保留其写入结果，可以一起结算
    //   1 虽然已提交，却可能仍在 MemTable，必须等自己的刷盘通知
    if (ready_id != tranc_id && it->second != TransactionState::ABORTED){
      ++it; 
      continue; 
    } 

    // 集合初始化为 {0}，压缩后首项表示 检查点 ：从开头到哪个 ID，已经连续处理完成
    // flushed 集合首项表示“从开头到这里，已经连续处理完成”
    // 例如 {3, 5} 表示 1～3 已完成，5 也完成，但 4 尚未确认
    //
    // 再次遇到 1 时，不应插回去：
    // {3, 5} 若变成 {1, 3, 5}，首项就错误地退回了 1
    // 因此只插入检查点之后的 ID
    if(ready_id > *flushedTrancIds_.begin()){
      // 先记入完成集合，最后才删除 ready 条目。
      // 如果插入时内存分配失败，当前事务的待处理登记仍然保留。
      flushedTrancIds_.insert(ready_id); 

      
      // 找到从当前检查点开始，最后一个连续的 ID
      // 例如 {0, 1, 2, 4}：
      // 0→1→2 连续，但缺少 3，所以必须停在 2，不能推进到 4
      auto last_contiguous = flushedTrancIds_.begin(); 
      auto next = last_contiguous; 
      ++next; 

      while (next != flushedTrancIds_.end() && *next - *last_contiguous == 1){
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

std::map<uint64_t, std::vector<Record>> TranManager::check_recover() {
  spdlog::info("TranManager--check_recover(): Starting recovery from WAL");

  std::map<uint64_t, std::vector<Record>> wal_records =
      WAL::recover(data_dir_, *flushedTrancIds_.begin());

  spdlog::info("TranManager--check_recover(): Recovered {} transactions",
               wal_records.size());

  return wal_records;
}

bool TranManager::write_to_wal(const std::vector<Record> &records) {
  spdlog::trace("TranManager--write_to_wal(): Writing {} records to WAL",
                records.size());

  try {
    wal->log(records, true);
  } catch (const std::exception &e) {
    spdlog::error("TranManager--write_to_wal(): Exception occurred: {}",
                  e.what());

    return false;
  }

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