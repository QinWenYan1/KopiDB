#pragma once

#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <sys/types.h>
#include <utility>
#include <cstddef>

namespace tiny_lsm {

enum class IteratorType {
  SkipListIterator,
  MemTableIterator,
  SstIterator,
  HeapIterator,
  TwoMergeIterator,
  ConcactIterator,
  LevelIterator,
};

class BaseIterator {
public:
  using value_type = std::pair<std::string, std::string>;
  using pointer = value_type *;
  using reference = value_type &;

  virtual BaseIterator &operator++() = 0;
  virtual bool operator==(const BaseIterator &other) const = 0;
  virtual bool operator!=(const BaseIterator &other) const = 0;
  virtual value_type operator*() const = 0;
  virtual IteratorType get_type() const = 0;
  virtual uint64_t get_tranc_id() const = 0;
  virtual bool is_end() const = 0;
  virtual bool is_valid() const = 0;
  virtual uint64_t get_cur_tranc_id() const = 0; 
};

class SstIterator;
// *************************** SearchItem ***************************
struct SearchItem {
  std::string key_;
  std::string value_;
  uint64_t tranc_id_;
  // idx 为什么使用 size_t
  // 当 SST ID 比较小时，例如 10、20，存入 int 完全没有问题。
  // 但如果长期生成 SST，ID 超过这个范围，
  // 转换成 int 后就可能变成负数，
  // 导致“ID 越大越优先”的顺序再次出错
  // 来源优先级：同 key、同版本时，数值越大越优先
  size_t idx_;
  int level_; // 来自sst的level

  SearchItem() = default;
  SearchItem(std::string k, std::string v, size_t i, int l, uint64_t tranc_id)
      : key_(std::move(k)), 
        value_(std::move(v)), 
        tranc_id_(tranc_id),
        idx_(i), 
        level_(l) {}
};

bool operator<(const SearchItem &a, const SearchItem &b);
bool operator>(const SearchItem &a, const SearchItem &b);
bool operator==(const SearchItem &a, const SearchItem &b);

// *************************** HeapIterator ***************************
class HeapIterator : public BaseIterator {
  friend class SstIterator;

public:
  HeapIterator(bool skip_delete = true, bool keep_all_versions = false);
  HeapIterator(std::vector<SearchItem> item_vec, uint64_t max_tranc_id,
               bool skip_delete = true, bool keep_all_versions = false);
  pointer operator->() const;
  virtual value_type operator*() const override;
  BaseIterator &operator++() override;
  BaseIterator operator++(int) = delete;
  virtual bool operator==(const BaseIterator &other) const override;
  virtual bool operator!=(const BaseIterator &other) const override;

  virtual IteratorType get_type() const override;
  virtual uint64_t get_tranc_id() const override;
  virtual uint64_t get_cur_tranc_id() const override; 
  virtual bool is_end() const override;
  virtual bool is_valid() const override;

private:
  bool top_value_legal() const;

  // 跳过当前不可见事务的id (如果开启了事务功能)
  void skip_by_tranc_id();

  void update_current() const;

private:
  std::priority_queue<SearchItem, std::vector<SearchItem>,
                      std::greater<SearchItem>>
      items;
  mutable std::shared_ptr<value_type> current; // 用于存储当前元素
  uint64_t max_tranc_id_ = 0;
  bool skip_delete_;
  bool keep_all_versions_ = false;
};
} // namespace tiny_lsm