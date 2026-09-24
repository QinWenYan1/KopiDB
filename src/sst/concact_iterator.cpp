#include "sst/concact_iterator.h"
#include "sst/sst_iterator.h"
#include <stdexcept>

namespace tiny_lsm {

// 迭代器刚创建时，就应该指向第一条可见记录，而不一定是第一张 SST
ConcactIterator::ConcactIterator(std::vector<std::shared_ptr<SST>> ssts,
                                 uint64_t tranc_id, bool keep_all_versions)
    : cur_iter(nullptr, tranc_id, keep_all_versions), cur_idx(0), ssts(ssts),
      max_tranc_id_(tranc_id), keep_all_versions_(keep_all_versions) {
  // 从第一张 SST 开始寻找
  // 第一张 SST 可能没有可见记录，不能只尝试 ssts[0]
  for (; cur_idx < this->ssts.size(); ++cur_idx) {
    // SST::begin() 内部调用已经修好的 seek_first()，
    // 会寻找这张 SST 中的第一条可见记录。
    cur_iter = this->ssts[cur_idx]->begin(max_tranc_id_, keep_all_versions_);

    // 找到可见记录，构造完成。
    // 可见墓碑也属于有效记录，继续保留给上层处理。
    if (cur_iter.is_valid())
      return;

    // 整张 SST 都没有可见记录，继续尝试下一张。
  }

  // 输入为空，或者全部 SST 都没有可见记录。
  // cur_idx == ssts.size()，统一使用空 SST 迭代器表示结束。
  cur_iter = SstIterator(nullptr, max_tranc_id_, keep_all_versions_);
}

// Lab 4.3 自增运算符重载
BaseIterator &ConcactIterator::operator++() {
  // 已经结束时保持原状，避免继续增加 cur_idx。
  if (is_end())
    return *this;

  // 1. 先在当前表内推进一格 (块内/跨块由 SstIterator 自己管)
  //    SST 内部的跨块和可见性过滤由 SstIterator 负责
  ++cur_iter;

  // 2. 当前表读完了 -> 换下一张表
  //    当前 SST 耗尽后，连续寻找后面的可见 SST
  //    使用 while，因为新进入的 SST 也可能完全不可见
  while (!cur_iter.is_valid()) {
    ++cur_idx;

    // 所有 SST 都检查完了，统一进入结束状态
    // 必须先检查边界，再访问 ssts[cur_idx]
    // 超出边界，直接返回 end() iterator
    if (cur_idx >= ssts.size()) {
      cur_iter = SstIterator(nullptr, max_tranc_id_, keep_all_versions_);
      return *this;
    }

    // 新 SST 从第一条可见记录开始。
    // 这里不能再额外 ++，否则会跳过它的第一条可见记录。
    cur_iter = ssts[cur_idx]->begin(max_tranc_id_, keep_all_versions_);

    // 新 SST 仍不可见：继续循环。
    // 新 SST 有可见记录：退出循环，停在该记录上。
  }

  return *this;
}

// Lab 4.3 比较运算符重载
bool ConcactIterator::operator==(const BaseIterator &other) const {
  // 1. 类型不同永不相等 (基类引用可能装着 MemIterator/HeapIterator...)
  if (other.get_type() != IteratorType::ConcactIterator)
    return false;

  // 2. get_type 已保证类型, dynamic_cast 引用版必然成功 (失败会抛 bad_cast)
  auto other2 = dynamic_cast<const ConcactIterator &>(other);

  // 3. 只比当前位置 (cur_iter 位置语义), 不比 ssts 数组/cur_idx
  //    两个 end 态: cur_iter 都是空表迭代器 -> SstIterator::== 双空相等
  //    一句话：== 回答的是"指向同一条 entry 吗"，而"指向哪"的全部信息都在
  //    cur_iter
  return other2.cur_iter == cur_iter;
}

// Lab 4.3 比较运算符重载
bool ConcactIterator::operator!=(const BaseIterator &other) const {
  // 直接委托 operator==
  return !(operator==(other));
}

// Lab 4.3 解引用运算符重载
ConcactIterator::value_type ConcactIterator::operator*() const {
  if (!is_valid())
    throw std::runtime_error(
        "ConcactIterator::operator*: cannot dereference this iterator");
  return *cur_iter;
}

// Lab 4.3 ->运算符重载
ConcactIterator::pointer ConcactIterator::operator->() const {
  if (!is_valid())
    throw std::runtime_error(
        "ConcactIterator::operator->: cannot dereference this iterator");
  return cur_iter.operator->();
}

IteratorType ConcactIterator::get_type() const {
  return IteratorType::ConcactIterator;
}

uint64_t ConcactIterator::get_tranc_id() const {
  if (keep_all_versions_) {
    return cur_iter.get_tranc_id();
  }
  return max_tranc_id_;
}

uint64_t ConcactIterator::get_cur_tranc_id() const {
  return cur_iter.get_cur_tranc_id();
}

bool ConcactIterator::is_end() const {
  return cur_iter.is_end() || !cur_iter.is_valid();
}

bool ConcactIterator::is_valid() const {
  return !cur_iter.is_end() && cur_iter.is_valid();
}

std::string ConcactIterator::key() { return cur_iter.key(); }

std::string ConcactIterator::value() { return cur_iter.value(); }
} // namespace tiny_lsm
