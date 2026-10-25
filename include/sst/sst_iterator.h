#pragma once

#include "block/block_iterator.h"
#include "iterator/iterator.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace tiny_lsm {

class SstIterator;
class SST;

std::optional<std::pair<SstIterator, SstIterator>>
sst_iters_monotony_predicate(std::shared_ptr<SST> sst, uint64_t tranc_id,
                             std::function<int(const std::string &)> predicate,
                            const std::function<bool(uint64_t)> &is_aborted = {}
                          );

class SstIterator : public BaseIterator {
  friend std::optional<std::pair<SstIterator, SstIterator>>
  sst_iters_monotony_predicate(
      std::shared_ptr<SST> sst, uint64_t tranc_id,
      std::function<int(const std::string &)> predicate, 
    const std::function<bool(uint64_t)> &is_aborted);

  friend SST;

private:
  std::shared_ptr<SST> m_sst;
  int64_t m_block_idx;
  uint64_t max_tranc_id_;
  std::shared_ptr<BlockIterator> m_block_it;
  mutable std::optional<value_type> cached_value; // 缓存当前值
  bool keep_all_versions_ = false;
  // SST 会创建和替换块迭代器，因此自己也需要保存回滚判断，
  // 以便进入其他块时，将同一判断继续传给新的 BlockIterator
  // 按值保存函数对象，默认空回调表示没有提供回滚筛选
  std::function<bool(uint64_t)> is_aborted_;

  void update_current() const;
  void set_block_idx(size_t idx);
  void set_block_it(std::shared_ptr<BlockIterator> it);

public:
  // 保存读取条件，并定位到第一条可见记录
  // 默认空回调保持原有调用方式兼容
  SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id,
              bool keep_all_versions = false,
            const std::function<bool(uint64_t)> &is_aborted = {});

  // 创建迭代器，并定位 key 的最大有效版本
  // SST 为空或没有有效版本时进入 end 状态
  // 构造时保存回滚判断，后续定位沿用该回调
  SstIterator(std::shared_ptr<SST> sst, const std::string &key,
              uint64_t tranc_id, bool keep_all_versions = false,
              const std::function<bool(uint64_t)> &is_aborted = {});

  // 创建迭代器, 并移动到第指定前缀的首端或者尾端
  static std::optional<std::pair<SstIterator, SstIterator>>
  iters_monotony_predicate(std::shared_ptr<SST> sst, uint64_t tranc_id,
                           std::function<bool(const std::string &)> predicate);

  void seek_first();

  // 精确查找 key；没有有效版本时进入 end 状态
  // 沿用构造时保存的回调，并传给当前块迭代器
  void seek(const std::string &key);
  std::string key();
  std::string value();

  virtual BaseIterator &operator++() override;
  virtual bool operator==(const BaseIterator &other) const override;
  virtual bool operator!=(const BaseIterator &other) const override;
  virtual value_type operator*() const override;
  virtual IteratorType get_type() const override;
  virtual uint64_t get_tranc_id() const override;
  virtual bool is_end() const override;
  virtual bool is_valid() const override;

  pointer operator->() const;
  uint64_t get_cur_tranc_id() const override;

  static std::pair<HeapIterator, HeapIterator>
  merge_sst_iterator(std::vector<SstIterator> iter_vec, uint64_t tranc_id,
                     bool keep_all_versions = false);
};
} // namespace tiny_lsm