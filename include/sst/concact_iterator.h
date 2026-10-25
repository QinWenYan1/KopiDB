#pragma once

#include "sst.h"
#include "sst_iterator.h"
#include <memory>
#include <vector>
#include <functional>

namespace tiny_lsm {
class ConcactIterator : public BaseIterator {
private:
  SstIterator cur_iter;
  size_t cur_idx; // 不是真实的sst_id, 而是在需要连接的sst数组中的索引
  std::vector<std::shared_ptr<SST>> ssts;
  uint64_t max_tranc_id_;
  bool keep_all_versions_ = false;
  // 当前 SST 的迭代器只负责自己的表。
  // ConcactIterator 切换到下一张 SST 时，还需要提供同样的回滚规则，
  // 所以这里保存一份，供构造和后续 ++ 复用。
  std::function<bool(uint64_t)> is_aborted_;

public:
  ConcactIterator(std::vector<std::shared_ptr<SST>> ssts, uint64_t tranc_id,
                  bool keep_all_versions = false,
                const std::function<bool(uint64_t)> &is_aborted = {});

  std::string key();
  std::string value();

  virtual BaseIterator &operator++() override;
  virtual bool operator==(const BaseIterator &other) const override;
  virtual bool operator!=(const BaseIterator &other) const override;
  virtual value_type operator*() const override;
  virtual IteratorType get_type() const override;
  virtual uint64_t get_tranc_id() const override;
  virtual uint64_t get_cur_tranc_id() const override;
  virtual bool is_end() const override;
  virtual bool is_valid() const override;

  pointer operator->() const;
};
} // namespace tiny_lsm
