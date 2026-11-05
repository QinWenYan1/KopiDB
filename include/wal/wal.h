// include/wal/wal.h

#pragma once

#include "utils/files.h"
#include "record.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tiny_lsm {

class WAL {
public:
  WAL(const std::string &log_dir, size_t buffer_size,
      uint64_t checkpoint_tranc_id, uint64_t clean_interval,
      uint64_t file_size_limit);
  ~WAL();

  static std::map<uint64_t, std::vector<Record>>
  recover(const std::string &log_dir, uint64_t checkpoint_tranc_id);

  // 将记录添加到缓冲区
  void log(const std::vector<Record> &records, bool force_flush = false);

  // 强制将缓冲区中的数据写入 WAL 文件
  void flush();

  void set_checkpoint_tranc_id(uint64_t checkpoint_tranc_id);

private:
  void cleaner();
  // 用于唤醒正在等待清理间隔的线程
  // 例如清理间隔为 60 秒，析构时可以立即通知它退出
  // 不必等满剩下的时间。后续 cleaner() 会使用它等待
  std::condition_variable cleaner_cv_; 
  // 文件操作失败后，禁止继续追加日志
  // 因为失败时可能已经写入一部分，重新写整批会造成重复或损坏
  // 由 mutex_ 保护，不需要使用 atomic
  bool write_failed_ = false;
  void cleanWALFile();
  void reset_file();


protected:
  std::string active_log_path_;
  FileObj log_file_;
  size_t file_size_limit_;
  std::mutex mutex_;
  std::vector<Record> log_buffer_;
  size_t buffer_size_;
  std::thread cleaner_thread_;
  uint64_t checkpoint_tranc_id_;
  std::atomic<bool> stop_cleaner_;
  uint64_t clean_interval_;
};
} // namespace tiny_lsm