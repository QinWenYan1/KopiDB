// src/wal/wal.cpp

#include "wal/wal.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
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

// commit 时强制写入
void WAL::flush() {
  // TODO: Lab 5.4 强制刷盘
  // ? 当前实现仅需加锁保证当前写入完成即可
  // ? 若 log() 中使用了缓冲区, 这里需要确保缓冲区内容全部落盘
  std::lock_guard<std::mutex> lock(mutex_);
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
  if (log_buffer_.empty() || (!force_flush && log_buffer_.size() < buffer_size_))
    return; 

  // 4. 先在内存中编码完整批次，再开始操作文件
  // 这样编码或分配内存失败时，文件还没有被修改
  // 合并成一份字节数组，只调用一次 append，减少文件操作次数
  std::vector<uint8_t> encoded; 
  for (const auto &record : log_buffer_){
    const auto bytes = record.encode(); 
    encoded.insert(encoded.end(), bytes.begin(), bytes.end()); 
  }

  try{
    // 5. 如果旧文件已经达到轮换阈值，在本次写入前换文件
    // file_size_limit_ == 0 表示关闭轮换
    //
    // 允许一批记录写完后稍微超过阈值
    // 等下一次真正写入时再换文件，避免“日志已经写成功
    // 却因为随后创建新文件失败而报告本次写入失败”
    if (file_size_limit_ != 0){
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

void WAL::cleaner() {
  // TODO: Lab 5.4 实现WAL的清理线程
  // ? 循环:
  // ?   1. sleep clean_interval_ 秒
  // ?   2. 若 stop_cleaner_ 为 true 则退出
  // ?   3. 调用 cleanWALFile() 清理已可以删除的旧 WAL 文件
}

void WAL::cleanWALFile() {
  // 遍历log_file_所在的文件夹
  std::string dir_path;

  std::unique_lock<std::mutex> lock(mutex_); // 只在获取当前文件路径时获取锁
  if (active_log_path_.find("/") != std::string::npos) {
    dir_path =
        active_log_path_.substr(0, active_log_path_.find_last_of("/")) + "/";
  } else {
    dir_path = "./";
  }
  lock.unlock();

  // wal文件格式为:
  // wal.seq

  std::vector<std::pair<size_t, std::string>> wal_paths;

  for (const auto &entry : std::filesystem::directory_iterator(dir_path)) {
    if (entry.is_regular_file() &&
        entry.path().filename().string().substr(0, 4) == "wal.") {
      std::string filename = entry.path().filename().string();
      size_t dot_pos = filename.find_last_of(".");
      std::string seq_str = filename.substr(dot_pos + 1);
      uint64_t seq = std::stoull(seq_str);
      wal_paths.push_back({seq, entry.path().string()});
    }
  }

  // 按照seq升序排序
  std::sort(wal_paths.begin(), wal_paths.end(),
            [](const std::pair<size_t, std::string> &a,
               const std::pair<size_t, std::string> &b) {
              return a.first < b.first;
            });

  // 判断是否可以删除
  std::vector<FileObj> del_paths;
  for (int idx = 0; idx < (int)wal_paths.size() - 1; idx++) {
    auto cur_path = wal_paths[idx].second;
    auto cur_file = FileObj::open(cur_path, false);
    // 遍历文件记录, 读取所有的tranc_id,
    // 判断是否都小于等于checkpoint_tranc_id_
    size_t offset = 0;
    bool has_unfinished = false;
    while (offset + sizeof(uint16_t) < cur_file.size()) {
      uint16_t record_size = cur_file.read_uint16(offset);
      uint64_t tranc_id = cur_file.read_uint64(offset + sizeof(uint16_t));
      if (tranc_id > checkpoint_tranc_id_) {
        has_unfinished = true;
        break;
      }
      offset += record_size;
    }
    if (!has_unfinished) {
      del_paths.push_back(std::move(cur_file));
    }
  }

  for (auto &del_file : del_paths) {
    del_file.del_file();
  }
}

void WAL::reset_file() {
  // wal文件格式为:
  // wal.seq
  // 当当前wal文件容量超出阈值后, 创建新的文件, 将seq自增

  auto old_path = active_log_path_;
  // 字符串处理获取seq
  auto seq = std::stoi(old_path.substr(old_path.find_last_of(".") + 1));
  seq++;

  active_log_path_ = old_path.substr(0, old_path.find_last_of(".")) + "." +
                     std::to_string(seq);

  // 创建新的文件
  log_file_ = FileObj::create_and_write(active_log_path_, {});
}
} // namespace tiny_lsm
