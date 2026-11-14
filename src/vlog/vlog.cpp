#include "vlog/vlog.h"
#include "spdlog/spdlog.h"
#include "utils/files.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <limits>

namespace tiny_lsm {

// Simple CRC32 implementation (polynomial 0xEDB88320)
static uint32_t crc32_compute(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFF;
}

// Lab 6.1 打开或创建 VLog 文件
std::shared_ptr<VLog> VLog::open(const std::string &path) {
  // 1. 父目录由调用方准备；这里只负责这个文件
  auto vlog = std::make_shared<VLog>();
  vlog->path_ = path;

  // 2. FileObj::open 的 create=true 带有“截断文件”的含义
  // 因此只有文件不存在时才使用 true，创建一个空文件
  //
  // 文件已存在时必须使用 false：
  // 例如 SST 记录了 offset=100，重开后该位置的旧数据必须还在
  const bool create = !std::filesystem::exists(path);
  vlog->file_ = FileObj::open(path, create);

  // FileObj::open 失败会抛异常，不会继续返回不可用的 VLog。
  // 此处打开成功后，再记录日志并把对象交给调用方。
  spdlog::info("VLog::open: opened vlog at {}, size={}", path,
               vlog->file_.size());

  return vlog;
}

// Lab 6.1 追加一条 KV 记录到 VLog, 返回记录起始偏移量
// 记录格式: [key_len:uint16][key][val_len:uint32][value][crc32:uint32]
uint64_t VLog::append(const std::string &key, const std::string &value) {

  // 1. 先检查长度，再转换为文件中的固定宽度字段
  // 例如 key 长度为 65536，直接转成 uint16_t 会变成 0
  // 读端就会把后面的 key 字节误当成其他字段
  if (key.size() > std::numeric_limits<uint16_t>::max())
    throw std::length_error("VLog::append: key is too large");

  if (value.size() > std::numeric_limits<uint32_t>::max())
    throw std::length_error("VLog::append: value is too large");

  const uint16_t key_len = static_cast<uint16_t>(key.size());
  const uint32_t value_len = static_cast<uint32_t>(value.size());

  // 2. 先在本次调用自己的缓冲区中编码，不访问共享文件
  // 布局：[key长度:2][key][value长度:4][value][CRC:4]
  // 用 size_t 计算总长度，避免用窄整数求和时溢出
  const size_t record_size = sizeof(key_len) + key.size() + sizeof(value_len) +
                             value.size() + sizeof(uint32_t);

  std::vector<uint8_t> buf(record_size);
  size_t pos = 0;

  // pos 表示缓冲区中下一个待写位置
  // 每放入一个字段，就向后移动该字段占用的字节数
  std::memcpy(buf.data() + pos, &key_len, sizeof(key_len));
  pos += sizeof(key_len);

  std::memcpy(buf.data() + pos, key.data(), key.size());
  pos += key.size();

  std::memcpy(buf.data() + pos, &value_len, sizeof(value_len));
  pos += sizeof(value_len);

  std::memcpy(buf.data() + pos, value.data(), value.size());
  pos += value.size();

  // 3. 此时 [0, pos) 包含长度字段、key 和 value
  // CRC 只覆盖这些内容，不包含最后即将写入的 CRC 自身
  const uint32_t crc = crc32_compute(buf.data(), pos);
  std::memcpy(buf.data() + pos, &crc, sizeof(crc));

  // 4. 从读取文件尾位置到写完整条记录，必须持有同一把锁
  // 否则两个写线程可能都看到文件长 100，返回相同的 offset
  // 编码在锁外完成，可以缩短其他写线程等待文件锁的时间
  std::lock_guard<std::mutex> lock(append_mtx_);
  const uint64_t offset = static_cast<uint64_t>(file_.size());

  // offset = file_.size() (追加前的文件大小即为本次记录的起始位置)
  if (!file_.append(buf))
    throw std::runtime_error("VLog::append: failed to write record");

  spdlog::trace("VLog::append: wrote record at offset={}, key_len={}, "
                "val_len={}",
                offset, key_len, value_len);

  // 返回整条记录的起点，不是 value 的起点。
  // 后续 read_value 会先读取 key_len，再跳过 key 和 value_len。
  return offset;
}

// Lab 6.1 从指定 offset 读取 value
// 记录布局: [key_len:2][key:key_len][val_len:4][value:val_len][crc:4]
std::string VLog::read_value(uint64_t offset, uint32_t value_size) {
    // 1. 读取也会改变底层 fstream 的游标，因此要与 append 共用同一把锁
    // 两个读取同时执行，也可能互相改变读取位置，不能只保护写入
    std::lock_guard<std::mutex> lock(append_mtx_); 

    // 2. 格式嗅探
    // 确认 offset 处至少还有 2 字节，可以读取 key_len
    // 先比较 offset，再用减法检查，避免 offset + 2 溢出
    const uint64_t file_size = file_.size(); 
    if (offset > file_size || file_size - offset < sizeof(uint16_t))
        throw std::out_of_range("VLog::read_value: incomplete key length"); 

    auto key_len_bytes = file_.read_to_slice(offset, sizeof(uint16_t)); 
    uint16_t key_len = 0; 
    std::memcpy(&key_len, key_len_bytes.data(), sizeof(key_len)); 

    // ? 先读 key_len 以跳过 key, 再定位 value 起始位置
    // ? value 起始 = offset + 2 + key_len + 4
    return ""; 
}

uint64_t VLog::tail_offset() const {
  return static_cast<uint64_t>(file_.size());
}

void VLog::sync() { file_.sync(); }

void VLog::del_vlog() { file_.del_file(); }

} // namespace tiny_lsm
