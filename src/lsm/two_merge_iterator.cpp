#include "lsm/two_merge_iterator.h"
#include "iterator/iterator.h"
#include <stdexcept>

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

// Lab 4.4:实现选择迭代器的逻辑
bool TwoMergeIterator::choose_it_a() {
  // 一路耗尽, 无条件选另一路
  if (it_a->is_end())
    return false;
  if (it_b->is_end())
    return true;
  auto key_a = (**it_a).first;
  auto key_b = (**it_b).first;

  // key 不同选小者: 归并的基本法
  if (key_a != key_b)
    return key_a < key_b;

  // key 相同: keep_all_versions 模式选 tranc 大者 (版本降序,
  // compaction 重建 block 依赖这个序)
  if (keep_all_versions_)
    return it_a->get_tranc_id() > it_b->get_tranc_id();

  // 普通模式: 同 key 选 a (a 是更新的一路, 旧版本让 skip_it_b 沉掉)
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

// Lab 4.4:根据事务可见性进行滤除的辅助函数
void TwoMergeIterator::skip_by_tranc_id() {
  // max_tranc_id_ == 0: 无事务快照的普通读, 不过滤
  // (没有这句, keep_all_versions 模式下 tranc_id>0 的 entry 会被全跳光)
  if (max_tranc_id_ == 0)
    return;

  // 同一 key 的版本按 tranc 降序聚在前头 -> 不可见版本一定堵在队首,
  // while 连续跳, 直到撞上可见版本或到底
  while (it_a->get_tranc_id() > max_tranc_id_) {
    ++(*it_a);
  }

  while (it_b->get_tranc_id() > max_tranc_id_) {
    ++(*it_b);
  }
}

// Lab 4.4:实现 ++ 重载
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

// Lab 4.4:实现 == 重载
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

// Lab 4.4:实现 != 重载
bool TwoMergeIterator::operator!=(const BaseIterator &other) const {
  return !operator==(other);
}

// Lab 4.4:实现 * 重载
BaseIterator::value_type TwoMergeIterator::operator*() const {
  if (!is_valid())
    throw std::runtime_error(
        "TwoMergeIterator::operator*: cannot dereference this iterator");
  if (choose_a)
    return **it_a;
  else
    return **it_b;
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

// Lab 4.4:实现 -> 重载
TwoMergeIterator::pointer TwoMergeIterator::operator->() const {
  // current 缓存提供稳定地址 (同 SstIterator 的 cached_value 动机)
  // 为什么 operator* 不使用缓存而 operator-> 使用呢？
  //    1. operator* 返回 value_type（按值）
  //    2. operator-> 返回 pointer：被指的 pair 必须在函数返回后还活着
  //    3. operator* 若走 current，得先 update_current() → make_shared
  //    一次堆分配 → 再 *current 拷出来
  //       没有必要

  if (!is_valid())
    throw std::runtime_error(
        "TwoMergeIterator::operator->: cannot dereference this iterator");
  update_current();
  return current.get();
}

void TwoMergeIterator::update_current() const {  if (choose_a) {
    current = std::make_shared<value_type>(**it_a);
  } else {
    current = std::make_shared<value_type>(**it_b);
  }
}
} // namespace tiny_lsm