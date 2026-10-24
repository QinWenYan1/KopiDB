#pragma once

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <functional>

namespace tiny_lsm {
class Block;

class BlockIterator {
public:
  // 标准迭代器类型定义
  using iterator_category = std::forward_iterator_tag;
  using value_type = std::pair<std::string, std::string>;
  using difference_type = std::ptrdiff_t;
  using pointer = const value_type *;
  using reference = const value_type &;

  // 构造函数
  BlockIterator(std::shared_ptr<Block> b, size_t index, uint64_t tranc_id,
                bool keep_all_versions = false);
  
  // 在指定 Block 中定位 key 的最大有效版本，未命中指向块尾
  // b 必须非空；回调只用于本次定位，不保存在迭代器中
  BlockIterator(std::shared_ptr<Block> b, const std::string &key,
                uint64_t tranc_id, bool keep_all_versions = false, 
              const std::function<bool(uint64_t)> &is_aborted = {});
  // BlockIterator(std::shared_ptr<Block> b, uint64_t tranc_id);
  BlockIterator()
      : block(nullptr), current_index(0), tranc_id_(0) {} // end iterator

  // 迭代器操作
  pointer operator->() const;
  BlockIterator &operator++();
  BlockIterator operator++(int) = delete;
  bool operator==(const BlockIterator &other) const;
  bool operator!=(const BlockIterator &other) const;
  value_type operator*() const;
  bool is_end();
  uint64_t get_cur_tranc_id() const;

private:
  void update_current() const;
  // 跳过当前不可见事务的id (如果开启了事务功能)
  void skip_by_tranc_id();

private:
  std::shared_ptr<Block> block;                   // 指向所属的 Block
  size_t current_index;                           // 当前位置的索引
  uint64_t tranc_id_;                             // 当前事务 id
  mutable std::optional<value_type> cached_value; // 缓存当前值
  bool keep_all_versions_ = false;
  // 保存回滚判断，后续 ++ 也需要使用。
  // 默认空回调表示没有提供回滚筛选。
  // 保存函数对象本身，避免引用构造函数中已经结束生命周期的参数对象。
  std::function<bool(uint64_t)> is_aborted_;
};
} // namespace tiny_lsm
