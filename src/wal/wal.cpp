// src/wal/wal.cpp

#include "wal/wal.h"
#include "spdlog/spdlog.h"
#include "utils/files.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tiny_lsm {

// Lab 5.4 实现WAL的初始化流程
// 从零开始的初始化流程
WAL::WAL(const std::string &log_dir, size_t buffer_size,
         uint64_t checkpoint_tranc_id, uint64_t clean_interval,
         uint64_t file_size_limit)
    : buffer_size_(buffer_size), checkpoint_tranc_id_(checkpoint_tranc_id),
      stop_cleaner_(false), clean_interval_(clean_interval),
      file_size_limit_(file_size_limit) {

  // 1. 准备日志目录
  // 空字符串按当前目录处理，避免拼接成 "/wal.0"
  // 目录已经存在时无需重新创建；创建失败则直接抛出异常
  const std::filesystem::path dir = log_dir.empty() ? "." : log_dir;
  std::filesystem::create_directories(dir);

  // 2. 找到新日志应使用的序号
  // 没有旧日志时使用 wal.0
  // 例如已有 wal.2、wal.10，则使用 wal.11
  //
  // 必须比较数字大小，不能按文件名字典序比较
  // 这里仅检查文件名，不读取或重放旧日志
  uint64_t next_seq = 0;

  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file())
      continue;

    const std::string name = entry.path().filename().string();
    if (!name.starts_with("wal."))
      continue;

    // 只把 "wal." 后面全部为数字的文件识别为日志
    // 例如 wal.backup 不参与序号计算，也不会被修改
    const std::string suffix = name.substr(4);
    if (suffix.empty() ||
        !std::all_of(suffix.begin(), suffix.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; }))
      continue;

    // stoull 将十进制字符串转换成整数
    // 数字超过它能表示的范围时，会直接抛出异常
    const uint64_t seq = std::stoull(suffix);

    // 最大序号不能再加 1，否则会回绕到 0
    // 导致后面重新选择已经存在的 wal.0
    if (seq == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("WAL: log file sequence exhausted");

    next_seq = std::max(next_seq, seq + uint64_t{1});
  }

  // 3. 创建本次使用的新日志文件
  //
  // FileObj::open(path, true) 会截断已有文件
  // 所以前面先选出超过所有旧日志的序号，避免覆盖旧内容
  // 打开失败时异常直接向上传递，构造不会假装成功
  active_log_path_ = (dir / ("wal." + std::to_string(next_seq))).string();
  log_file_ = FileObj::open(active_log_path_, true);

  // 4. 文件准备好之后，自动创建并启动新线程
  // clean_interval == 0 表示关闭周期清理
  //
  // 把线程启动放在最后：
  // 前面的目录操作或文件打开若失败，此时还没有线程需要回收
  if (clean_interval_ != 0)
    cleaner_thread_ = std::thread(&WAL::cleaner, this);
}

// Lab 5.4 实现WAL的清理流程
WAL::~WAL() {
  // 1. 告诉清理线程：准备退出，不要再开始下一轮清理
  //
  // 后续 cleaner() 会在同一把 mutex_ 下检查停止标记并进入等待
  // 修改标记时也持有这把锁，避免线程错过停止通知
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_cleaner_ = true;
  }

  // 2. 唤醒可能正在等待的清理线程
  // 修改标记只是改变状态；通知才能让等待提前结束
  // 如果线程尚未开始等待，它之后也会通过停止标记发现需要退出
  cleaner_cv_.notify_one();

  // 3. 等待清理线程真正退出，再继续关闭文件
  //
  // clean_interval == 0 时没有创建线程，不需要 join
  // 线程函数已经执行完、但尚未 join 时，仍然需要回收
  //
  // 注意：这里不能持有 mutex_
  // 否则清理线程可能正在等待这把锁，而我们又在等待它退出
  // 双方互相等待，导致死锁
  if (cleaner_thread_.joinable())
    cleaner_thread_.join();

  // 4. 尝试将尚在缓冲区中的日志全部写入文件
  // flush() 自己负责加锁，这里不再持有 mutex_
  //
  // 析构期间不能让刷盘异常直接逃出去，否则可能终止程序
  // 失败时记录错误，随后仍然关闭文件
  // 这不表示日志保存成功，调用方需要检查结果时应显式调用 flush()
  try {
    flush();
  } catch (const std::exception &e) {
    spdlog::error("WAL: failed to flush during destruction: {}", e.what());
  } catch (...) {
    spdlog::error("WAL: unknown flush error during destruction");
  }

  // 5. 清理线程已经退出，最后释放文件资源
  log_file_.close();
}

std::map<uint64_t, std::vector<Record>>
WAL::recover(const std::string &log_dir, uint64_t checkpoint_tranc_id) {
  // TODO: Lab 5.5 检查需要重放的WAL日志
  // ? 1. 若 log_dir 不存在则直接返回空 map
  // ? 2. 遍历目录找到所有 "wal." 前缀的文件
  // ? 3. 按 seq 升序排序
  // ? 4. 逐文件读取所有 Record (Record::decode)
  // ?    仅保留 tranc_id > checkpoint_tranc_id 的记录
  // ? 5. 返回 map<tranc_id, records>
  return {};
}

// Lab 5.4 强制刷盘
// commit 时强制写入
void WAL::flush() {
  // 本次不新增记录，只要求写出缓冲区中已有的记录
  // {}   ：传入空的记录数组
  // true ：强制写入，不再等待缓冲达到条数阈值
  //
  // log() 已经负责加锁、编码、写入、刷新和成功后清空缓冲
  // 这里不能再加 mutex_，否则 log() 会重复获取同一把锁而死锁
  //
  // 写入失败时，让异常自然传给调用者
  // 析构函数调用 flush() 时，已有自己的异常捕获
  log({}, true);
}

void WAL::set_checkpoint_tranc_id(uint64_t checkpoint_tranc_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  checkpoint_tranc_id_ = checkpoint_tranc_id;
}

// Lab 5.4 实现WAL的写入流程
void WAL::log(const std::vector<Record> &records, bool force_flush) {
  // 1. 缓冲区、文件写入和轮换使用同一把锁
  // 防止两个线程同时修改缓冲，或者同时向同一个文件追加数据
  std::lock_guard<std::mutex> lock(mutex_);

  // 之前发生过文件操作错误，不能继续向不确定的文件尾部追加
  // 这个判断也让后续 flush 能报告此前的失败
  if (write_failed_)
    throw std::runtime_error("WAL: previous file operation failed");

  // 2. 将本次记录追加到已有缓冲后面，保持输入顺序
  // records 为空时不追加，但 force_flush 仍可要求写出旧缓冲
  log_buffer_.insert(log_buffer_.end(), records.begin(), records.end());

  // 3. 没有记录就不需要写文件
  // 未达到条数阈值且没有强制要求时，继续保留在缓冲中
  // buffer_size_ == 0 时，非空缓冲会直接进入写入流程
  if (log_buffer_.empty() ||
      (!force_flush && log_buffer_.size() < buffer_size_))
    return;

  // 4. 先在内存中编码完整批次，再开始操作文件
  // 这样编码或分配内存失败时，文件还没有被修改
  // 合并成一份字节数组，只调用一次 append，减少文件操作次数
  std::vector<uint8_t> encoded;
  for (const auto &record : log_buffer_) {
    const auto bytes = record.encode();
    encoded.insert(encoded.end(), bytes.begin(), bytes.end());
  }

  try {
    // 5. 如果旧文件已经达到轮换阈值，在本次写入前换文件
    // file_size_limit_ == 0 表示关闭轮换
    //
    // 允许一批记录写完后稍微超过阈值
    // 等下一次真正写入时再换文件，避免“日志已经写成功
    // 却因为随后创建新文件失败而报告本次写入失败”
    if (file_size_limit_ != 0) {
      const auto current_size = log_file_.size();

      // 当前 FileObj 底层使用 tellg() 获取大小
      // 获取失败返回 -1，转换成 size_t 后成为其最大值
      // 不能把它当作正常的大文件而继续轮换
      if (current_size == std::numeric_limits<size_t>::max())
        throw std::runtime_error("WAL: failed to read log file size");

      if (current_size >= file_size_limit_)
        reset_file();
    }

    // 6. 追加字节，并检查写入结果
    // append 返回 false 时，文件可能已经写入了部分内容
    if (!log_file_.append(encoded))
      throw std::runtime_error("WAL: failed to append log records");

    // 7. 刷新文件流，并检查刷新结果
    // append 成功不代表后续刷新也成功，两步都必须检查
    // 这里沿用 FileObj::sync 的流刷新语义
    if (!log_file_.sync())
      throw std::runtime_error("WAL: failed to sync log file");
  } catch (...) {
    // 文件操作失败：保留缓冲，标记故障，原样抛出异常
    // 后续调用不能把这批记录再次追加到可能已写入一半的文件中
    write_failed_ = true;
    throw;
  }

  // 8. 只有追加和刷新全部成功，才能移除已写出的记录
  // clear 保留 vector 的容量，方便下一批记录复用
  log_buffer_.clear();
}

// TODO: Lab 5.4 实现WAL的清理线程
void WAL::cleaner() {
  // 构造函数只在 clean_interval_ != 0 时启动此线程
  // 将配置中的秒数转换成 wait_for 接收的时间间隔
  const auto interval = std::chrono::seconds(clean_interval_);

  // 等待结束的条件：析构函数已经请求停止
  // 放在循环外定义，后续每次等待都使用同一个判断
  const auto should_stop = [this]() { return stop_cleaner_.load(); };

  // unique_lock 支持暂时解锁和重新加锁
  // 条件变量等待期间需要释放锁，所以这里使用 unique_lock
  std::unique_lock<std::mutex> lock(mutex_);

  while (true) {
    // 1. 等待清理间隔，或者等待停止请求
    //
    // wait_for 等待时会自动释放 mutex_，其他线程仍能调用 log()
    // 醒来检查条件时，它会重新获取 mutex_
    //
    // 返回 true：停止条件成立，立即退出
    // 返回 false：等待时间已到，停止条件仍不成立，可以清理
    //
    // 即使停止通知发生在线程进入等待之前
    // should_stop 也会读到 true，不会再睡满整个间隔
    if (cleaner_cv_.wait_for(lock, interval, should_stop))
      return;

    // 2. cleanWALFile() 内部会自行加锁
    // 调用前必须释放当前锁，避免重复获取 mutex_ 而死锁
    lock.unlock();

    try {
      cleanWALFile();
    } catch (const std::exception &e) {
      // 清理可能因文件读取、解析或删除失败而抛异常。
      // 不能让异常逃出线程入口，否则会终止整个进程。
      // 记录本次失败，下一周期再尝试。
      spdlog::error("WAL: failed to clean old log files: {}", e.what());
    } catch (...) {
      spdlog::error("WAL: unknown error while cleaning old log files");
    }

    // 3. 重新取锁，进入下一轮等待
    // 如果清理期间收到停止请求，下一次 wait_for 会先检查条件
    // 直接返回 true，不会再等待完整的清理间隔
    lock.lock();
  }
}

void WAL::cleanWALFile() {
  // 1. 清理期间保护活动文件和检查点。
  // 避免判断文件能否删除时，另一个线程同时轮换文件或修改检查点。
  //
  // cleaner() 调用前已经解锁，因此这里可以正常获取 mutex_。
  // 这个简单方案会让清理期间的 log() 暂时等待；
  // 清理线程睡眠期间仍会释放锁，不影响前台写入。
  std::lock_guard<std::mutex> lock(mutex_);

  const std::filesystem::path active_path(active_log_path_);
  const uint64_t active_seq =
      std::stoull(active_path.filename().string().substr(4));

  // 每个文件是否能删除可以独立判断，不需要先排序。
  for (const auto &entry :
       std::filesystem::directory_iterator(active_path.parent_path())) {
    if (!entry.is_regular_file())
      continue;

    const auto path = entry.path();
    const std::string name = path.filename().string();

    // 2. 只处理 wal.<纯数字序号>，忽略其他文件。
    if (!name.starts_with("wal.")) {
      continue;
    }

    const std::string suffix = name.substr(4);
    if (suffix.empty() ||
        !std::all_of(suffix.begin(), suffix.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; })) {
      continue;
    }

    try {
      const uint64_t seq = std::stoull(suffix);

      // 3. 只处理比当前活动文件更早的日志。
      // 活动文件仍可能继续写入，不能删除。
      // 如果发现更大的序号，也保守保留。
      if (seq >= active_seq) {
        continue;
      }

      // 4. 完整读取并解码，再判断能否删除。
      // 复用 Record::decode 的长度、类型和边界检查，
      // 避免把截断或损坏的日志误认为可以删除。
      auto file = FileObj::open(path.string(), false);
      const auto file_size = file.size();

      if (file_size == std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("WAL: failed to read log file size");
      }

      const auto records = Record::decode(file.read_to_slice(0, file_size));

      // 5. 只要有一条记录尚未被安全检查点覆盖，就保留整个文件。
      //
      // 例如 checkpoint 为 10：
      //   记录 ID 为 3、8  → 可以删除。
      //   记录 ID 为 3、12 → 必须保留。
      //
      // 文件序号与事务 ID 是两回事，不能拿 wal.后面的序号代替判断。
      bool can_delete = true;
      for (const auto &record : records) {
        if (record.getTrancId() > checkpoint_tranc_id_) {
          can_delete = false;
          break;
        }
      }

      if (!can_delete) {
        continue;
      }

      // 6. 确认安全后，关闭读取句柄，再删除文件。
      // 空的历史日志没有需要保留的记录，也可以删除。
      //
      // 使用能报告文件系统错误的删除接口；
      // 当前 FileObj::del_file() 没有向上传递删除失败。
      file.close();
      std::filesystem::remove(path);
    } catch (const std::exception &e) {
      // 无法确认内容完整或删除失败时，记录错误。
      // 单个文件处理失败，不妨碍继续检查其他文件。
      spdlog::error("WAL: failed to clean {}: {}", path.string(), e.what());
    }
  }
}

void WAL::reset_file() {
  // 调用方 log() 已经持有 mutex_，这里不能再次加锁
  // 本函数只负责切换文件，缓冲中的记录仍由 log() 写入

  // 1. 取出当前日志序号
  // active_log_path_ 由构造函数或本函数生成，
  // 文件名格式已保证为 wal.<数字>，不需要重新扫描目录
  //
  // 例如 data/wal.3：
  // filename() 得到 wal.3，substr(4) 得到 "3"
  const std::filesystem::path current_path(active_log_path_);
  const uint64_t seq = std::stoull(current_path.filename().string().substr(4));

  // 2. 序号达到上限时，不能继续加一，避免回绕到 wal.0。
  if (seq == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("WAL: log file sequence exhausted");

  // 3. 在同一个目录下生成下一日志文件的路径
  // 例如 data/wal.3 → data/wal.4
  std::string next_path = (current_path.parent_path() /
                           ("wal." + std::to_string(seq + uint64_t{1})))
                              .string();

  // create_and_write 会截断已有文件
  // 如果目标路径已经存在，明确报错，不能覆盖其中的内容
  if (std::filesystem::exists(next_path))
    throw std::runtime_error("WAL: next log file already exists: " + next_path);

  // 4. 先用局部变量创建新文件，暂时不修改成员
  // {} 表示新文件初始内容为空
  //
  // 如果创建失败，异常直接交给 log() 处理
  // 当前路径和文件句柄仍然指向旧文件
  auto next_file = FileObj::create_and_write(next_path, {});

  // 5. 创建成功后，再切换成员
  //
  // FileObj 的移动赋值已声明 noexcept，接管新文件句柄
  // 同时释放旧句柄；释放句柄不会删除旧日志文件
  log_file_ = std::move(next_file);

  // swap 不需要重新分配字符串内存
  // 避免已经换好文件句柄，却在复制新路径时因分配失败而中断
  // 交换后，active_log_path_ 保存新路径，next_path 保存旧路径
  active_log_path_.swap(next_path);
}
} // namespace tiny_lsm
