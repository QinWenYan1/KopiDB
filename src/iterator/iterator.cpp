#include "iterator/iterator.h"
#include <memory>
#include <tuple>
#include <vector>

namespace tiny_lsm {

// *************************** SearchItem ***************************
bool operator<(const SearchItem &a, const SearchItem &b) {
  // TODO: Lab2.2 实现比较规则
  // 1. 实现方法为 key 升序比较
  if (a.key_ != b.key_) {
    return a.key_ < b.key_;
  }

  // 2. 同key的情况下比较 tranc_id事务版本，方式为降序
  if (a.tranc_id_ != b.tranc_id_) {
    return a.tranc_id_ > b.tranc_id_;
  }

  // 3. 同 key 同版本: 表新者赢
  return a.idx_ > b.idx_;
}

bool operator>(const SearchItem &a, const SearchItem &b) {
  // TODO: Lab2.2 实现比较规则
  // 直接逆关系，不用重写逻辑
  return operator<(b, a);
}

bool operator==(const SearchItem &a, const SearchItem &b) {
  // TODO: Lab2.2 实现比较规则
  return a.key_ == b.key_ && a.tranc_id_ == b.tranc_id_ && a.idx_ == b.idx_;
}

// *************************** HeapIterator ***************************
HeapIterator::HeapIterator(bool skip_delete, bool keep_all_versions)
    : skip_delete_(skip_delete), keep_all_versions_(keep_all_versions) {
  // 默认构造函数
  // TODO: Lab2.2 实现 HeapIterator 构造函数
  // 空堆，什么都不用写入，成员初始化列表是唯一要做的事情
}
HeapIterator::HeapIterator(std::vector<SearchItem> item_vec,
                           uint64_t max_tranc_id, bool skip_delete,
                           bool keep_all_versions)
    : max_tranc_id_(max_tranc_id), skip_delete_(skip_delete),
      keep_all_versions_(keep_all_versions) {
  // TODO: Lab2.2 实现 HeapIterator 构造函数
  // 1. 全部灌入到最小堆，堆中自动按照SearchItem 的 operator< 排列好
  for (auto &item : item_vec) {
    items.push(std::move(item));
  }
  // 2. 初始滤除(对应教程 Hint 1):
  //    堆顶 = 最小 key 的最新版本; 它若是墓碑(空 value),
  //    说明该 key 已被删除, 整组(墓碑 + 同 key 旧版本)一起弹出
  while (!items.empty() && skip_delete_ && items.top().value_.empty()) {
    std::string key = items.top().key_;
    items.pop();
    while (!items.empty() && items.top().key_ == key)
      // 墓碑下面还压着同 key 的旧版本，一并清除
      items.pop();
  }

  // 3. 缓存当前元素 (依赖 update_current)
  update_current();
}

HeapIterator::pointer HeapIterator::operator->() const {
  // TODO: Lab2.2 实现 -> 重载
  return current.get();
}

HeapIterator::value_type HeapIterator::operator*() const {
  // TODO: Lab2.2 实现 * 重载
  // 解引用缓存; value_type = pair<string, string>
  return *current; 
}

BaseIterator &HeapIterator::operator++() {
  // TODO: Lab2.2 实现 ++ 重载
  if (items.empty()) //节点已经耗尽，防御性返回
    return *this;

  // 1. 当前 key 消费完毕：弹出堆顶 + 连同 key 旧版本（去重）
  std::string key = items.top().key_;
  items.pop();
  while (!items.empty() && items.top().key_ == key)
    // 下面还压着同 key 的旧版本，一并跳过
    items.pop();
  
  // 2. 新堆顶如果是墓碑，整组弹掉; 循环直到堆顶合法或堆空
  while (!items.empty() && skip_delete_ && items.top().value_.empty()) {
    key = items.top().key_;
    items.pop();
    while (!items.empty() && items.top().key_ == key)
      // 墓碑下面还压着同 key 的旧版本，一并清除
      items.pop();
  }
  
  // 3. 刷新缓存
  update_current(); 
  return *this; 

}

bool HeapIterator::operator==(const BaseIterator &other) const {
  // TODO: Lab2.2 实现 == 重载
  // 1. 检查类型相等不相等, 不相等返回false
  if (other.get_type() != IteratorType::HeapIterator) return false; 

  // 2. 如果有一方耗尽，双方耗尽才能相等
  if (items.empty() || other.is_end()) return items.empty() && other.is_end(); 

  // 3. 两边都是有效的：比当前key-value对
  return *(*this) == *other; 
}

bool HeapIterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab2.2 实现 != 重载
  // 使用 operator == 语法糖，逆关系而已
  return !operator==(other); 
}

bool HeapIterator::top_value_legal() const {
  // TODO: Lab2.2 判断顶部元素是否合法
  // ? 被删除的值是不合法
  // ? 不允许访问的事务创建或更改的键值对不合法(暂时忽略)
  // 堆都空了，当然不合法了
  if (items.empty()) return false; 
  // 对外暴露 skip_delete_ = true， 墓碑不合法
  if (skip_delete_ && items.top().value_.empty()) return false; 

  // TODO: Lab5 在这里加事务可见性判断
  // (条目 tranc_id_ 对 max_tranc_id_ 不可见 -> 不合法)
  return true; 
}

void HeapIterator::skip_by_tranc_id() {
  // Lab2.2 仅作标记, 函数体留空
  // TODO: Lab5 实现: 若堆顶条目的事务对 max_tranc_id_ 不可见, 弹出整组同 key 项, 循环直到堆顶可见或堆空
}

bool HeapIterator::is_end() const { return items.empty(); }
bool HeapIterator::is_valid() const { return !items.empty(); }

void HeapIterator::update_current() const {
  // current 缓存了当前键值对的值, 你实现 -> 重载时可能需要
  // TODO: Lab2.2 更新当前缓存值
  if (items.empty())
    current.reset(); 
  else current = std::make_shared<value_type>(items.top().key_, items.top().value_); 
}

IteratorType HeapIterator::get_type() const {
  return IteratorType::HeapIterator;
}

uint64_t HeapIterator::get_tranc_id() const {
  if (keep_all_versions_ && !items.empty()) {
    return items.top().tranc_id_;
  }
  return max_tranc_id_;
}
} // namespace tiny_lsm