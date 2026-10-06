#include "lsm/two_merge_iterator.h"
#include "iterator/iterator.h"
#include <stdexcept>

namespace tiny_lsm {

TwoMergeIterator::TwoMergeIterator() {}

// A：k@5 = "old"
// B：k@7 = "new"
// 读上限：8
// keep_all_versions：false
// 我们需要注意不能直接skip_it_b，这样可能会跳过最新正确的版本

//构造函数
TwoMergeIterator::TwoMergeIterator(std::shared_ptr<BaseIterator> it_a,
                                   std::shared_ptr<BaseIterator> it_b,
                                   uint64_t max_tranc_id,
                                   bool keep_all_versions)
    : it_a(std::move(it_a)), it_b(std::move(it_b)), max_tranc_id_(max_tranc_id),
      keep_all_versions_(keep_all_versions) {
  // 先跳过不可见的事务
  skip_by_tranc_id();
  // 两路候选都保留下来，比较后再决定输出谁
  // 例如 A=k@5、B=k@7，不能在比较前直接丢掉 B
  // 决定使用哪个迭代器
  choose_a = choose_it_a(); 
}

// Lab 4.4: 实现选择迭代器的逻辑
//          choose_it_a() 要决定“当前应该输出哪条记录”，普通模式也必须比较版本号
//          无关 keep_all_version_
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

  // 同 key，优先输出版本号较大的记录，包括墓碑。
  // 版本号也相等时优先 A，沿用“相同版本 A 来源优先”的约定。
  return it_a->get_cur_tranc_id() >= it_b->get_cur_tranc_id();
}


// Lab 4.4:根据事务可见性进行滤除的辅助函数
void TwoMergeIterator::skip_by_tranc_id() {
  // max_tranc_id_ == 0: 无事务快照的普通读, 不过滤
  // (没有这句, keep_all_versions 模式下 tranc_id>0 的 entry 会被全跳光)
  if (max_tranc_id_ == 0)
    return;

  // 同一 key 的版本按 tranc 降序聚在前头 -> 不可见版本一定堵在队首,
  // while 连续跳, 直到撞上可见版本或到底
  // 需要提前检查是否已经走到末尾
  // 不然调用 get_cur_tranc_id() 后，如果不先判有效，空迭代器就会抛异常
  while (
        it_a && 
        it_a->is_valid() &&
        it_a->get_cur_tranc_id() > max_tranc_id_
      ) {
    ++(*it_a);
  }

  while (
      it_b && 
      it_b->is_valid() &&
      it_b->get_cur_tranc_id() > max_tranc_id_
  ) {
    ++(*it_b);
  }
}

// Lab 4.4:实现 ++ 重载
BaseIterator &TwoMergeIterator::operator++() {
  choose_a = choose_it_a(); 
  const auto &select = choose_a? it_a : it_b; 

  // 已经没有可消费的记录
  if (!select || !select->is_valid())
    return *this; 

  // 1. 只推进当前选中的那一路
  if (keep_all_versions_){
    // 全版本模式：只消费刚输出的一条记录
    // 其他版本留着，下一轮继续比较和输出
    ++(*select); 
  }else{
    // 普通模式：当前 key 的最大可见版本已经输出
    // 必须先复制 key，因为推进迭代器后当前记录会改变
    const std::string &key = (**select).first; 

    // 两路中这个 key 的其余记录都不再输出
    // 例如刚输出 B 的 k@7，A 的 k@5 也要一起跳过，
    // 否则下一次又会输出同一个 key 的旧版本
    while(it_a && it_a->is_valid() && (**it_a).first == key)
      ++(*it_a);
    
    while(it_b && it_b->is_valid() && (**it_b).first == key)
      ++(*it_b);
  }
  // 2. 推进后，重新过滤可见性并选择下一条记录
  skip_by_tranc_id();
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

uint64_t TwoMergeIterator::get_cur_tranc_id() const {
  // 与 operator* 使用同一路，不在 getter 中重新选择来源。
  const auto &selected = choose_a ? it_a : it_b;

  // 先检查指针，再检查它是否指向有效记录。
  // || 会短路，selected 为空时不会调用 is_valid()。
  if (!selected || !selected->is_valid()) {
    throw std::runtime_error(
        "TwoMergeIterator::get_cur_tranc_id: invalid iterator");
  }

  return selected->get_cur_tranc_id();
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