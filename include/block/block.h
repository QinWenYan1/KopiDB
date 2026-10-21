#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/***
Refer to https://skyzh.github.io/mini-lsm/week1-03-block.html for memory layout

-----------------------------------------------------------------------------
|             Data Section           |      Offset Section |     Extra      |
-----------------------------------------------------------------------------
|Entry#1|Entry#2|...|Entry#N|Offset#1|Offset#2|...|Offset#N|num_of_elements |
-----------------------------------------------------------------------------

---------------------------------------------------------------------
|                           Entry #1 |                          ... |
--------------------------------------------------------------|-----|
|key_len (2B)|key(keylen)|val_len(2B)|val(vallen)|tranc_id(8B)| ... |
---------------------------------------------------------------------

*/

namespace tiny_lsm {
class BlockIterator;

class Block : public std::enable_shared_from_this<Block> {
  friend BlockIterator;

private:
  std::vector<uint8_t> data;
  std::vector<uint16_t> offsets;
  size_t capacity;

  struct Entry {
    std::string key;
    std::string value;
    uint64_t tranc_id;
  };
  Entry get_entry_at(size_t offset) const;
  std::string get_key_at(size_t offset) const;
  std::string get_value_at(size_t offset) const;
  uint64_t get_tranc_id_at(size_t offset) const;
  int compare_key_at(size_t offset, const std::string &target) const;

  // 从 idx 所属的同 key 版本组中，选择符合读取上限且未回滚的最大版本。
  // idx 可以指向组内任意记录；成功返回记录下标，无候选返回 -1。
  // 不传 is_aborted 时保持原有查询行为。
  int adjust_idx_by_tranc_id(
      size_t idx, uint64_t tranc_id,
      const std::function<bool(uint64_t)> &is_aborted = {});

  bool is_same_key(size_t idx, const std::string &target_key) const;

  // CRC32 校验实现 (polynomial 0xEDB88320), 抄自 vlog.cpp
  static uint32_t crc32_compute(const uint8_t *data, size_t len);

public:
  Block() = default;
  Block(size_t capacity);
  // ! 这里的编码函数不包括 hash
  std::vector<uint8_t> encode(bool with_hash = true);
  // ! 这里的解码函数可指定切片是否包括 hash
  static std::shared_ptr<Block> decode(const std::vector<uint8_t> &encoded,
                                       bool with_hash = true);
  std::string get_first_key();
  size_t get_offset_at(size_t idx) const;
  bool add_entry(const std::string &key, const std::string &value,
                 uint64_t tranc_id, bool force_write);

  // 返回符合读取上限且未回滚的最大版本的 value
  // 未命中返回 nullopt；有效墓碑返回有值的 optional，内部字符串为空
  std::optional<std::string> get_value_binary(const std::string &key,
                                              uint64_t tranc_id,
                                              const std::function<bool(uint64_t)> &is_aborted = {});

  size_t size() const;
  size_t cur_size() const;
  bool is_empty() const;

  // 二分查找 key，返回符合读取上限且未回滚的最大版本的记录下标。
  // 未命中返回 nullopt；有效墓碑也返回其下标。
  std::optional<size_t>
  get_idx_binary(const std::string &key, uint64_t tranc_id,
                 const std::function<bool(uint64_t)> &is_aborted = {});

  // 按照谓词返回迭代器, 左闭右开
  std::optional<
      std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
  get_monotony_predicate_iters(uint64_t tranc_id,
                               std::function<int(const std::string &)> func);

  BlockIterator begin(uint64_t tranc_id = 0);

  std::optional<
      std::pair<std::shared_ptr<BlockIterator>, std::shared_ptr<BlockIterator>>>
  iters_preffix(uint64_t tranc_id, const std::string &preffix);

  BlockIterator end();
};
} // namespace tiny_lsm
