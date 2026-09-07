#include "block/block_iterator.h"
#include "block/block.h"
#include <cstdint>
#include <memory>
#include <stdexcept>

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
  return nullptr;
}

BlockIterator &BlockIterator::operator++() {
  // TODO: Lab3.2 ++ 重载
  // ? 在后续的Lab实现事务后，你可能需要对这个函数进行返修
  return *this;
  
}

bool BlockIterator::operator!=(const BlockIterator &other) const {
  // TODO: Lab3.2 != 重载
  return true;
}

bool BlockIterator::operator==(const BlockIterator &other) const {
  // TODO: Lab3.2 == 重载
  return true;
}

BlockIterator::value_type BlockIterator::operator*() const {
  // TODO: Lab3.2 * 重载
  return {};
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
  if (!cached_value && current_index < block->offsets.size()) {
    size_t offset = block->get_offset_at(current_index);
    cached_value =
        std::make_pair(block->get_key_at(offset), block->get_value_at(offset));
  }
}

void BlockIterator::skip_by_tranc_id() {
  if (tranc_id_ == 0) {
    // 没有开启事务功能
    cached_value = std::nullopt;
    return;
  }

  while (current_index < block->offsets.size()) {
    size_t offset = block->get_offset_at(current_index);
    auto tranc_id = block->get_tranc_id_at(offset);
    if (tranc_id <= tranc_id_) {
      // 位置合法
      break;
    }
    // 否则跳过不可见事务的键值对
    ++current_index;
  }
  cached_value = std::nullopt;
}
} // namespace tiny_lsm