#include "block/block.h"
#include "block/block_iterator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace tiny_lsm {
Block::Block(size_t capacity) : capacity(capacity) {}

// CRC32 校验实现 (polynomial 0xEDB88320), 抄自 vlog.cpp
uint32_t Block::crc32_compute(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFF;
}

std::vector<uint8_t> Block::encode(bool with_hash) {
  // TODO: Lab 3.1 编码单个类实例形成一段字节数组
  // ? 格式: [data段] + [offsets数组, 每项uint16_t] + [元素个数 uint16_t]
  // ? 若 with_hash == true, 末尾额外追加 uint32_t 的 CRC 校验值
  // ? CRC 覆盖除自身之外的所有字节
  std::vector<uint8_t> encoded;
  // data 段 + offset 段 + num 段 + crc32段
  // (optional)，先估计一个容量避免反复扩容 [data][offsets：每项
  // 2B][条目数：2B][可选 CRC32：4B]
  encoded.reserve(data.size() + offsets.size() * 2 + 2 + (with_hash ? 4 : 0));

  // 1. data 段，原样拷贝
  encoded.insert(encoded.end(), data.begin(), data.end());

  // 2. offset 段，每个offset 是 uint16_t(2 字节) 所以占两元素，拆成字节逐个
  // push
  //    push_back 只收单字节(uint8_t), 所以要手动把一个 16 位数拆两半
  //    字节序约定: 低位在前 (小端), 与测试数据里 key_len=5 存成 [5, 0] 一致
  //    先 push 低 8 位: & 0xFF 砍掉高位, 只留下最底下那一截
  //    再 push 高 8 位: >> 8 把原来的高 8 位挪到低 8 位的位置, 再 & 0xFF 砍干净
  //    例: off = 20 = 0x0014 -> 低字节 0x14 (20), 高字节 0x00 (0) -> [20, 0]

  for (uint16_t offset : offsets) {
    encoded.push_back(offset & 0xFF);        // 低字节
    encoded.push_back((offset >> 8) & 0xFF); // 高字节
  }

  // 3. num 段：元素个数，同样是 uint16_t 小端
  // uint16_t 将数量转换成 uint16_t，超过 65535 会截断
  // 正常合法块不会达到这个数量，但可以在函数开头加一层防御检查
  if (offsets.size() > UINT16_MAX) {
    throw std::runtime_error("Too many entries");
  }
  uint16_t num = static_cast<uint16_t>(offsets.size());
  encoded.push_back(num & 0xFF);
  encoded.push_back((num >> 8) & 0xFF);

  // 4. 可选 CRC32: 覆盖前面所有字节。然后插入到最后4个字节
  //  encoded 里只有 data + offsets + num——CRC 还没 push 进去。
  //  所以 encoded.size() 天然不含 CRC
  if (with_hash) {
    uint32_t crc = crc32_compute(encoded.data(), encoded.size());
    for (int i = 0; i < 4; ++i)
      encoded.push_back(crc >> (8 * i) & 0xFF);
  }

  return encoded;
}

std::shared_ptr<Block> Block::decode(const std::vector<uint8_t> &encoded,
                                     bool with_hash) {
  // TODO: Lab 3.1 解码字节数组形成类实例
  // ? 从末尾读取元素个数, 若 with_hash 为 true 先校验 CRC
  // ? 然后依次读取 offsets 和 data 段

  // 1. 最小长度检查：至少得装下 num (2B), 带 hash 则再加 4B
  // 至少包含条目数 2B，以及可选的 CRC32 4B
  const size_t crc_size = with_hash ? 4 : 0;
  if (encoded.size() < 2 + crc_size)
    throw std::runtime_error("Block::decode: data too short");

  // 正文：[data][offsets][条目数]，不包含 CRC
  //  传进来的 buffer 是完整成品，CRC 已经在末尾占着 4 字节了。
  //  不减 4 就会把 CRC 自己也算进去
  const size_t body_size = encoded.size() - crc_size;

  // 2. 带 hash 时先校验 CRC32 (覆盖除末 4 字节外的全部)
  if (with_hash) {
    uint32_t actual = 0, expect = crc32_compute(encoded.data(), body_size);
    for (int i = 0; i < 4; ++i) {
      actual |= static_cast<uint32_t>(encoded[body_size + i]) << (8 * i);
    }
    if (expect != actual)
      throw std::runtime_error("Block::decode: CRC32 mismatch");
  }

  // 3. 从尾部往前切三段: [ data ... | offset x num | num(2B) | crc(4B)? ]
  // num 永远在最后 2 字节: 低位在前拼回 uint16_t
  uint16_t num = static_cast<uint16_t>(encoded[body_size - 2] |
                                       (encoded[body_size - 1] << 8));

  // offset 段：num 个条目 * 每个 2 字节，紧贴在 num 段前面
  size_t offset_sec_end = body_size - 2;

  // 在确定 offset_sec_begin 前，要先确认空间足够，再做减法
  // 因为需要避免 size_t 下溢
  // offset_sec_end 是“条目数” num 字段的起始下标，
  // 也等于它前面 data + offsets 的总字节数
  // 因此，需要确认 offset_sec_end 起码是 >= offsets(也就是 2 * num)
  if (2 * num > offset_sec_end) {
    throw std::runtime_error("Invalid offset table");
  }

  size_t offset_sec_begin = offset_sec_end - num * 2;

  // data 段：从头到 offset 段的开头
  // 4. 将 data 段整段拷贝
  auto block = std::make_shared<Block>();
  block->data.assign(encoded.begin(), encoded.begin() + offset_sec_begin);

  // 5. offset 段：每 2 字节小端拼一个 uint16_t
  block->offsets.reserve(num);
  for (size_t i = 0; i < num; ++i) {
    uint16_t off =
        static_cast<uint16_t>(encoded[offset_sec_begin + 2 * i] |
                              (encoded[offset_sec_begin + 2 * i + 1] << 8));
    block->offsets.push_back(off);
  }

  // 6. capacity: 解码产物只读，填当前实际大小（此决定无测试约束）
  block->capacity = block->cur_size();

  return block;
}

std::string Block::get_first_key() {
  if (data.empty() || offsets.empty()) {
    return "";
  }

  // 读取第一个key的长度（前2字节）
  uint16_t key_len;
  memcpy(&key_len, data.data(), sizeof(uint16_t));

  // 读取key
  std::string key(reinterpret_cast<char *>(data.data() + sizeof(uint16_t)),
                  key_len);
  return key;
}

size_t Block::get_offset_at(size_t idx) const {
  if (idx > offsets.size()) {
    throw std::runtime_error("idx out of offsets range");
  }
  return offsets[idx];
}

bool Block::add_entry(const std::string &key, const std::string &value,
                      uint64_t tranc_id, bool force_write) {
  // TODO: Lab 3.1 添加一个键值对到block中
  // ? 每条 entry 格式:
  // [key_len:uint16_t][key][value_len:uint16_t][value][tranc_id:uint64_t] ? 若
  // !force_write 且当前容量不足则返回 false ? 成功添加后记录偏移到 offsets,
  // 返回 true
  return false;
}

// 从指定偏移量获取entry的key
std::string Block::get_key_at(size_t offset) const {
  // TODO: Lab 3.1 从指定偏移量获取entry的key
  // ? 读取 data[offset] 处的 uint16_t key_len, 再取后续 key_len 个字节
  // 1. 读取 key_len: data[offset] 处的 2 字节，小端（低字节在前面）
  uint16_t key_len = static_cast<uint16_t>(
    data[offset] | (data[offset+1] << 8)
  ); 

  // 2. key 内容紧跟 key_len 字段后面，从 offset + 2 开始
  //    一共 key_len 个 字节
  //    data 是 vector<uint8_t>, string 要 const char* 所以是reinterpret
  return std::string(reinterpret_cast<const char*>(data.data() + offset + 2), key_len); 

}

// 从指定偏移量获取entry的value
std::string Block::get_value_at(size_t offset) const {
  // TODO: Lab 3.1 从指定偏移量获取entry的value
  // ? 先跳过 key_len + key, 再读取 uint16_t value_len, 最后取 value
  return "";
}

uint64_t Block::get_tranc_id_at(size_t offset) const {
  // TODO: Lab 3.1 从指定偏移量获取entry的tranc_id
  // ? 先跳过 key 和 value, 读取末尾的 uint64_t tranc_id
  return 0;
}

// 比较指定偏移量处的key与目标key
int Block::compare_key_at(size_t offset, const std::string &target) const {
  std::string key = get_key_at(offset);
  return key.compare(target);
}

// 相同的key连续分布, 且相同的key的事务id从大到小排布
// 这里的逻辑是找到最接近 tranc_id 的键值对的索引位置
int Block::adjust_idx_by_tranc_id(size_t idx, uint64_t tranc_id) {
  // TODO: Lab3.1 不需要在Lab3.1中实现, 只是进行标记
  // ? 后续实现事务后需要更新这里的实现
  // ? tranc_id == 0: 向前找最小索引 (最大事务id) 版本
  // ? tranc_id != 0: 找满足 tranc_id_ <= tranc_id 的最新版本
  return -1;
}

bool Block::is_same_key(size_t idx, const std::string &target_key) const {
  if (idx >= offsets.size()) {
    return false; // 索引超出范围
  }
  return get_key_at(offsets[idx]) == target_key;
}

// 使用二分查找获取value
// 要求在插入数据时有序插入
std::optional<std::string> Block::get_value_binary(const std::string &key,
                                                   uint64_t tranc_id) {
  auto idx = get_idx_binary(key, tranc_id);
  if (!idx.has_value()) {
    return std::nullopt;
  }

  return get_value_at(offsets[*idx]);
}

std::optional<size_t> Block::get_idx_binary(const std::string &key,
                                            uint64_t tranc_id) {
  // TODO: Lab 3.1 使用二分查找获取key对应的索引
  // ? 在 offsets 数组上做二分查找, 利用 compare_key_at 比较
  // ? 找到后调用 adjust_idx_by_tranc_id 进行事务可见性修正
  return std::nullopt;
}

std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::iters_preffix(uint64_t tranc_id, const std::string &preffix) {
  // TODO: Lab 3.3 获取前缀匹配的区间迭代器
  // ? 将前缀匹配转化为单调谓词, 调用 get_monotony_predicate_iters
  // ? 谓词: -key.compare(0, preffix.size(), preffix)
  return std::nullopt;
}

// 返回第一个满足谓词的位置和最后一个满足谓词的位置
// 如果不存在, 返回nullopt
// 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// 返回的区间是闭区间, 开区间需要手动对返回值自增
// predicate返回值:
//   0: 满足谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<
    std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
Block::get_monotony_predicate_iters(
    uint64_t tranc_id, std::function<int(const std::string &)> predicate) {
  // TODO: Lab 3.3 使用二分查找获取满足谓词的区间迭代器
  // ? 第一次二分: 找到 first (满足谓词的最左边索引)
  // ? 第二次二分: 找到 last  (满足谓词的最右边索引)
  // ? 返回 [BlockIterator(first), BlockIterator(last+1)]
  return std::nullopt;
}

Block::Entry Block::get_entry_at(size_t offset) const {
  Entry entry;
  entry.key = get_key_at(offset);
  entry.value = get_value_at(offset);
  entry.tranc_id = get_tranc_id_at(offset);
  return entry;
}

size_t Block::size() const { return offsets.size(); }

size_t Block::cur_size() const {
  return data.size() + offsets.size() * sizeof(uint16_t) + sizeof(uint16_t);
}

bool Block::is_empty() const { return offsets.empty(); }

BlockIterator Block::begin(uint64_t tranc_id) {
  // TODO: Lab 3.2 获取begin迭代器
  // ? 返回指向第 0 个 entry 的迭代器: BlockIterator(shared_from_this(), 0,
  // tranc_id)
  return BlockIterator(nullptr, 0, 0);
}

BlockIterator Block::end() {
  // TODO: Lab 3.2 获取end迭代器
  // ? 返回指向末尾 (offsets.size()) 的迭代器
  return BlockIterator(nullptr, 0, 0);
}
} // namespace tiny_lsm
