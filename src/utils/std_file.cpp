#include "utils/std_file.h"
#include "spdlog/spdlog.h"
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

#else
#include <unistd.h>
#endif

namespace tiny_lsm {

bool StdFile::open(const std::string &filename, bool create) {
  filename_ = filename;

  if (create) {
    file_.open(filename, std::ios::in | std::ios::out | std::ios::binary |
                             std::ios::trunc);
  } else {
    file_.open(filename, std::ios::in | std::ios::out | std::ios::binary);
  }

  if (!file_.is_open()) {
    // 获取具体的错误信息
    int err = errno;
    std::string error_msg = fmt::format("Failed to open file '{}': {} ({})",
                                        filename, strerror(err), err);

    // 同时输出到日志文件和标准错误
    spdlog::error(error_msg);
    std::cerr << "[ERROR] " << error_msg << std::endl;

    return false;
  }

  return true;
}

bool StdFile::create(const std::string &filename, std::vector<uint8_t> &buf) {
  // 先创建或截断文件；保留原接口在打开失败时抛异常的行为。
  if (!this->open(filename, true)) {
    throw std::runtime_error("Failed to open file for writing");
  }

  // 创建空文件也是合法操作，此时没有初始内容需要写入。
  if (buf.empty()) {
    return true;
  }

  // 将初始内容的写入结果交给调用方。
  // 文件已经打开，不代表内容已经写好；忽略 false 会把不完整文件当成成功。
  // 最终流缓冲刷新仍由调用方执行并检查 sync()。
  return write(0, buf.data(), buf.size());
}

void StdFile::close() {
  if (file_.is_open()) {
    sync();
    file_.close();
  }
}

size_t StdFile::size() {
  file_.seekg(0, std::ios::end);
  return file_.tellg();
}

std::vector<uint8_t> StdFile::read(size_t offset, size_t length) {
  std::vector<uint8_t> buf(length);
  file_.seekg(offset, std::ios::beg);
  if (!file_.read(reinterpret_cast<char *>(buf.data()), length)) {
    throw std::runtime_error("Failed to read from file");
  }
  return buf;
}

bool StdFile::write(size_t offset, const void *data, size_t size) {
  // 1. 从文件开头计算偏移，定位本次写入的位置。
  // seekp 用于定位写入位置，seekg 用于定位读取位置。
  file_.seekp(offset, std::ios::beg);

  // 定位失败时报告失败，不继续尝试写入。
  // 例如文件未打开，或者文件流已经处于失败状态。
  if (!file_) {
    return false;
  }

  // 2. 写入指定数量的字节；底层 I/O 失败会设置文件流的错误状态。
  file_.write(static_cast<const char *>(data), size);

  // 3. 返回真实流状态，不能无条件报告成功。
  // 写入可能暂存在流缓冲区中，此处成功不保证后续刷新成功；
  // 调用方仍须检查 sync()。这里不逐次刷新，避免多次写入时重复刷新。
  return file_.good();
}

bool StdFile::sync() {
  if (!file_.is_open()) {
    return false;
  }
  file_.flush();
  return file_.good();
}

bool StdFile::remove() {
  // 修复类型转换问题
  return std::remove((const char *)filename_.c_str()) == 0;
}

// TODO: Windows下的文件截断实现目前有问题搁置
#ifndef _WIN32
bool StdFile::truncate(size_t size) {
  if (file_.is_open())
    file_.close();
  int ret = ::truncate(filename_.c_str(), size);
  file_.open(filename_, std::ios::in | std::ios::out | std::ios::binary);
  return ret == 0 && file_.is_open();
}
#endif
} // namespace tiny_lsm
