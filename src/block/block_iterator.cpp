#include "block/block_iterator.h"
#include "block/block.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

class Block;

namespace tiny_lsm {
BlockIterator::BlockIterator(std::shared_ptr<Block> b, size_t index,
                             uint64_t tranc_id, bool keep_all_versions)
    : block(b), current_index(index), tranc_id_(tranc_id),
      cached_value(std::nullopt), keep_all_versions_(keep_all_versions) {
  skip_by_tranc_id();
}

BlockIterator::BlockIterator(std::shared_ptr<Block> b, const std::string &key,
                             uint64_t tranc_id, bool keep_all_versions)
    : block(b), tranc_id_(tranc_id), cached_value(std::nullopt),
      keep_all_versions_(keep_all_versions) {
  auto key_idx_ops = block->get_idx_binary(key, tranc_id);
  if (key_idx_ops.has_value()) {
    current_index = key_idx_ops.value();
  } else {
    current_index = block->offsets.size();
  }
}

// BlockIterator::BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id)
//     : block(b), current_index(0), tranc_id_(tranc_id),
//       cached_value(std::nullopt) {
//   skip_by_tranc_id();
// }

BlockIterator::pointer BlockIterator::operator->() const {
  // TODO: Lab3.2 -> 重载
  if (!block || current_index >= block->size())
    throw std::out_of_range("BlockIterator::Operator->: Iterator out of range");

  // 与 * 共用缓存值
  update_current();
  return &(*cached_value);
}

BlockIterator &BlockIterator::operator++() {
  // TODO: Lab3.2 ++ 重载
  // ? 在后续的Lab实现事务后，你可能需要对这个函数进行返修
  // 已在末尾/空迭代器：防御性返回
  if (!block || current_index >= block->size())
    return *this;

  // 1. 记下当前 key，用于跨版本去重
  size_t prev_offset = block->get_offset_at(current_index);
  std::string prev_key = block->get_key_at(prev_offset);

  ++current_index;

  // 2. 去重：同 key 的旧版本连续排在后面，全跳过
  // keep_all_versions_ = true 时不跳 (Lab 5 compaction 要看全部版本)
  // keep_all_versions_ 是"要不要保留同 key 的全部版本"的开关，控制 ++ 里去不去重
  if (!keep_all_versions_) {
    while (current_index < block->size() &&
           block->is_same_key(current_index, prev_key)) {
      ++current_index;
    }
  }

  // 3. 跳过不可见版本；内部同时把cahched_value 重置 (位置变了缓存作废)
  //    在 skip_by_tranc_id 里面已经更新了 cached_value
  skip_by_tranc_id();
  return *this;
}

bool BlockIterator::operator!=(const BlockIterator &other) const {
  // TODO: Lab3.2 != 重载
  return !(operator==(other));
}

bool BlockIterator::operator==(const BlockIterator &other) const {
  // TODO: Lab3.2 == 重载
  //1. 双方都为 block: 两个哨兵相等
  if (block == nullptr && other.block == nullptr)
    return true; 

  //2. 一方空，一方非空：不相等
  if (block == nullptr || other.block == nullptr)
    return false; 

  //3. 同一 block 且同一位置才算相等
  return block == other.block && current_index == other.current_index; 

}

// TODO: Lab3.2 * 重载
BlockIterator::value_type BlockIterator::operator*() const {
  // 尾后或迭代器不可解引用
  if (!block || current_index >= block->size())
    throw std::out_of_range("BlockIterator::Operator*: Iterator out of range");

  // 惰性缓存：首次解引用才通过 update_current 解析
  // entry，之后复用（update_current 内部判空）
  update_current();
  return *cached_value;
}

bool BlockIterator::is_end() { return current_index == block->offsets.size(); }

uint64_t BlockIterator::get_cur_tranc_id() const {
  if (!block || current_index >= block->offsets.size()) {
    return 0;
  }
  size_t offset = block->get_offset_at(current_index);
  return block->get_tranc_id_at(offset);
}

void BlockIterator::update_current() const {
  // TODO: Lab3.2 更新当前指针
  // ? 该函数是可选的实现, 你可以采用自己的其他方案实现->, 而不是使用
  // ? cached_value 来缓存当前指针

  // 惰性填充：缓存空且位置合法时才解析 (block 判空是你的防御风格，加上无妨)
  if (!cached_value && block && current_index < block->size()){
    size_t offset = block->get_offset_at(current_index); 
    cached_value = std::make_pair(block->get_key_at(offset), block->get_value_at(offset)); 
  }
}

void BlockIterator::skip_by_tranc_id() {
  // TODO: Lab3.2 * 跳过事务ID
  // ? 只是进行标记以供你在后续Lab实现事务功能后修改
  // ? 现在你不需要考虑这个函数

  if (tranc_id_ == 0){
    // 非事务读：不过滤，但缓存必须失败（调用方刚移动过位置）
    cached_value = std::nullopt; 
    return; 
  }

  // 逐条跳过最新的版本，知道第一个 entry_tranc_id <= tranc_id_ (或者耗尽)
  // 注意：弹单个不弹整组，同key 旧版本可能可见，与 Heapiterator 闸 2 同规则
  while (current_index < block->size()){
    size_t offset  = block->get_offset_at(current_index); 
    if (block->get_tranc_id_at(offset) <= tranc_id_)
      break; //位置合法
    ++ current_index; 
  }

  // 位置变了，缓存作废
  cached_value = std::nullopt; 
}
} // namespace tiny_lsm