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
                             uint64_t tranc_id, bool keep_all_versions,
                             const std::function<bool(uint64_t)> &is_aborted)
    : block(b), current_index(index), tranc_id_(tranc_id),
      cached_value(std::nullopt), keep_all_versions_(keep_all_versions),
      is_aborted_(is_aborted) {
  // 初始化列表已经把回调复制到 is_aborted_
  // 构造函数返回后，迭代器仍持有自己的函数对象，
  // 后续 operator++() 调用筛选函数时，也能检查回滚状态

  // index 只是开始查找的位置，不保证该记录可以读取
  // 例如 index 指向 K@9，而事务 9 已回滚：
  // 这里应跳过 K@9，继续寻找后面的有效 K@5
  //
  // 如果后面的记录全部不满足条件，就停在块尾
  // tranc_id == 0 时也会执行回滚检查，具体规则由筛选函数负责
  skip_by_tranc_id();
}

BlockIterator::BlockIterator(std::shared_ptr<Block> b, const std::string &key,
                             uint64_t tranc_id, bool keep_all_versions,
                             const std::function<bool(uint64_t)> &is_aborted)
    : block(b), tranc_id_(tranc_id), cached_value(std::nullopt),
      keep_all_versions_(keep_all_versions) {
  // 1. 按 key 定位必须有实际的 Block，先检查再访问
  // 空指针属于调用参数错误；正常的“没有找到 key”在下面处理
  if (!block) {
    throw std::invalid_argument(
        "BlockIterator: block must not be null for key lookup");
  }

  // 2. 将读取上限和回滚判断交给已经验证过的下标查询
  // 例如 k@9 已回滚、k@5 有效，这里直接定位到 k@5
  //
  // 不自行向后 ++ 寻找旧版本：
  // 普通模式的 ++ 会跳过同 key 的旧版本，可能把 k@5 一起跳掉
  auto idx = block->get_idx_binary(key, tranc_id, is_aborted);

  // 3. 命中则使用返回的下标；未命中则定位到块尾
  // value_or(x) 表示：optional 有值就取值，否则使用 x
  // 空块的 size() 为 0，同样能正确形成块尾位置
  current_index = idx.value_or(block->size());

  // 此时没有解引用记录，cached_value 保持为空，
  // 第一次调用 * 或 -> 时再解析 key/value。
}

// BlockIterator::BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id)
//     : block(b), current_index(0), tranc_id_(tranc_id),
//       cached_value(std::nullopt) {
//   skip_by_tranc_id();
// }

// Lab3.2 -> 重载
BlockIterator::pointer BlockIterator::operator->() const {
  if (!block || current_index >= block->size())
    throw std::out_of_range("BlockIterator::Operator->: Iterator out of range");

  // 与 * 共用缓存值
  update_current();
  return &(*cached_value);
}

// Lab3.2 ++ 重载
BlockIterator &BlockIterator::operator++() {
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
  // keep_all_versions_ 是"要不要保留同 key 的全部版本"的开关，控制 ++
  // 里去不去重
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

// Lab3.2 != 重载
bool BlockIterator::operator!=(const BlockIterator &other) const {
  return !(operator==(other));
}

// Lab3.2 == 重载
bool BlockIterator::operator==(const BlockIterator &other) const {
  // 1. 双方都为 block: 两个哨兵相等
  if (block == nullptr && other.block == nullptr)
    return true;

  // 2. 一方空，一方非空：不相等
  if (block == nullptr || other.block == nullptr)
    return false;

  // 3. 同一 block 且同一位置才算相等
  return block == other.block && current_index == other.current_index;
}

// Lab3.2 * 重载
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

// Lab3.2 更新当前指针
void BlockIterator::update_current() const {
  // ? 该函数是可选的实现, 你可以采用自己的其他方案实现->, 而不是使用
  // ? cached_value 来缓存当前指针

  // 惰性填充：缓存空且位置合法时才解析 (block 判空是你的防御风格，加上无妨)
  if (!cached_value && block && current_index < block->size()) {
    size_t offset = block->get_offset_at(current_index);
    cached_value =
        std::make_pair(block->get_key_at(offset), block->get_value_at(offset));
  }
}

// Lab3.2: 跳过不可见事务ID
void BlockIterator::skip_by_tranc_id() {
  // 调用方可能刚刚移动了 current_index
  // 上一次解引用缓存的 key/value 已经不能继续使用
  // 提前清理，也保证下面直接返回时不会留下旧缓存
  cached_value.reset();

  // 空迭代器没有记录可检查
  if (!block)
    return;

  // 上限为 0 只表示“不限制版本号大小”。
  // 只有同时没有回滚回调时，才可以完全跳过筛选
  if (tranc_id_ == 0 && !is_aborted_)
    return;

  // 逐条跳过最新的版本，知道第一个 entry_tranc_id <= tranc_id_ (或者耗尽)
  // 注意：弹单个不弹整组，同key 旧版本可能可见，与 Heapiterator 闸 2 同规则
  while (current_index < block->size()) {
    size_t offset = block->get_offset_at(current_index);
    const auto &version = block->get_tranc_id_at(offset);

    // 两个条件都满足，当前位置才可以交给调用方：
    // 1. 没有读取上限，或者版本没有超过上限
    // 2. 没有回滚回调，或者该版本所属事务没有回滚
    //
    // && 和 || 会短路：
    // 超过上限时不用查回滚名单，空回调也不会被调用
    if ((tranc_id_ == 0 || version <= tranc_id_) &&
        (!is_aborted_ || !is_aborted_(version)))
      break; // 位置合法

    // 每次只跳过一条记录，不能直接跳过整个 key
    // 例如 K@9 已回滚，后面的 K@5 仍可能有效
    ++current_index;
  }

  // 不检查 value：有效墓碑必须继续交给上层归并
  // 这里只移动迭代位置，不删除块中的任何记录
}
} // namespace tiny_lsm