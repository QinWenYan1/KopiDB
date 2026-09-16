#include "lsm/two_merge_iterator.h"
#include "iterator/iterator.h"

namespace tiny_lsm {

TwoMergeIterator::TwoMergeIterator() {}

TwoMergeIterator::TwoMergeIterator(std::shared_ptr<BaseIterator> it_a,
                                   std::shared_ptr<BaseIterator> it_b,
                                   uint64_t max_tranc_id,
                                   bool keep_all_versions)
    : it_a(std::move(it_a)), it_b(std::move(it_b)), max_tranc_id_(max_tranc_id),
      keep_all_versions_(keep_all_versions) {
  // 先跳过不可见的事务
  skip_by_tranc_id();
  skip_it_b();              // 跳过与 it_a 重复的 key
  choose_a = choose_it_a(); // 决定使用哪个迭代器
}

bool TwoMergeIterator::choose_it_a() {
  if (it_a->is_end()) {
    return false;
  }
  if (it_b->is_end()) {
    return true;
  }
  auto key_a = (**it_a).first;
  auto key_b = (**it_b).first;
  if (key_a != key_b) {
    return key_a < key_b; // 比较 key
  }
  // Same key: in keep_all_versions mode emit the larger tranc_id first so
  // that block entries remain in descending-tranc_id order, which is what
  // Block::adjust_idx_by_tranc_id expects.
  if (keep_all_versions_) {
    return it_a->get_tranc_id() > it_b->get_tranc_id();
  }
  // Normal query mode: prefer it_a (newer, from memtable / higher level).
  return true;
}

void TwoMergeIterator::skip_it_b() {
  if (keep_all_versions_) {
    return;
  }
  if (!it_a->is_end() && !it_b->is_end() && (**it_a).first == (**it_b).first) {
    ++(*it_b);
  }
}

void TwoMergeIterator::skip_by_tranc_id() {
  if (max_tranc_id_ == 0) {
    return;
  }
  while (it_a->get_tranc_id() > max_tranc_id_) {
    ++(*it_a);
  }
  while (it_b->get_tranc_id() > max_tranc_id_) {
    ++(*it_b);
  }
}

// TODO: Lab 4.4: 实现 ++ 重载
BaseIterator &TwoMergeIterator::operator++() {
  // 1. 只推进当前选中的那一路
  if (choose_a)
    ++(*it_a);
  else
    ++(*it_b);

  // 2. 推进后重做"构造三件套": tranc 过滤 -> 同 key 去重 -> 重新抉择
  skip_by_tranc_id();
  skip_it_b();              // 跳过与 it_a 重复的 key
  choose_a = choose_it_a(); // 重新决定使用哪个迭代器
  return *this;
}

// TODO: Lab 4.4: 实现 == 重载
bool TwoMergeIterator::operator==(const BaseIterator &other) const {
  if (other.get_type() != IteratorType::TwoMergeIterator)
    return false;

  auto other2 = dynamic_cast<const TwoMergeIterator &>(other);
  // end 态归一: 双 end 相等, 单 end 不等 (it != end() 循环靠这个收尾)
  if (is_end() && other2.is_end())
    return true;

  if (is_end() || other.is_end())
    return false;

  // 身份语义: 孩子是用 shared_ptr 借来的, 指针相同 = 同一路数据流
  return it_a == other2.it_a && it_b == other2.it_b &&
         choose_a == other2.choose_a;
}

bool TwoMergeIterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab 4.4: 实现 != 重载
  return false;
}

BaseIterator::value_type TwoMergeIterator::operator*() const {
  // TODO: Lab 4.4: 实现 * 重载
  return {};
}

IteratorType TwoMergeIterator::get_type() const {
  return IteratorType::TwoMergeIterator;
}

uint64_t TwoMergeIterator::get_tranc_id() const {
  if (keep_all_versions_) {
    if (choose_a && it_a && !it_a->is_end()) {
      return it_a->get_tranc_id();
    }
    if (!choose_a && it_b && !it_b->is_end()) {
      return it_b->get_tranc_id();
    }
  }
  return max_tranc_id_;
}

bool TwoMergeIterator::is_end() const {
  if (it_a == nullptr && it_b == nullptr) {
    return true;
  }
  if (it_a == nullptr) {
    return it_b->is_end();
  }
  if (it_b == nullptr) {
    return it_a->is_end();
  }
  return it_a->is_end() && it_b->is_end();
}

bool TwoMergeIterator::is_valid() const {
  if (it_a == nullptr && it_b == nullptr) {
    return false;
  }
  if (it_a == nullptr) {
    return it_b->is_valid();
  }
  if (it_b == nullptr) {
    return it_a->is_valid();
  }
  return it_a->is_valid() || it_b->is_valid();
}

TwoMergeIterator::pointer TwoMergeIterator::operator->() const {
  // TODO: Lab 4.4: 实现 -> 重载
  return nullptr;
}

void TwoMergeIterator::update_current() const {
  if (choose_a) {
    current = std::make_shared<value_type>(**it_a);
  } else {
    current = std::make_shared<value_type>(**it_b);
  }
}
} // namespace tiny_lsm