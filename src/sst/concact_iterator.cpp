#include "sst/concact_iterator.h"
#include "sst/sst_iterator.h"
#include <stdexcept>

namespace tiny_lsm {

// 迭代器刚创建时，就应该指向第一条可见记录，而不一定是第一张 SST
ConcactIterator::ConcactIterator(std::vector<std::shared_ptr<SST>> ssts,
                                 uint64_t tranc_id, bool keep_all_versions)
    : ssts(ssts), 
      cur_iter(nullptr, tranc_id, keep_all_versions), 
      cur_idx(0),
      max_tranc_id_(tranc_id), 
      keep_all_versions_(keep_all_versions) {
  // 从第一张 SST 开始寻找
  // 第一张 SST 可能没有可见记录，不能只尝试 ssts[0]
  for (; cur_idx < this->ssts.size(); ++cur_idx) {
    // SST::begin() 内部调用已经修好的 seek_first()，
    // 会寻找这张 SST 中的第一条可见记录。
  }
}

// Lab 4.3 自增运算符重载
BaseIterator &ConcactIterator::operator++() {
  // 1. 先在当前表内推进一格 (块内/跨块由 SstIterator 自己管)
  ++cur_iter;

  // 2. 当前表读完了 -> 换下一张表
  if (cur_iter.is_end() || !cur_iter.is_valid()) {
    ++cur_idx;
    if (cur_idx < ssts.size()) {
      // 新表从各自起点开始 (begin 内部 seek_first, 自动跳过不可见版本)
      cur_iter = ssts[cur_idx]->begin(max_tranc_id_, keep_all_versions_);
    } else {
      // 全部表读完 -> end 态: 空表迭代器 (m_block_it 为 nullptr)
      cur_iter = SstIterator(nullptr, max_tranc_id_);
    }
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
