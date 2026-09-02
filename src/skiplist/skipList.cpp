#include "skiplist/skiplist.h"
#include "spdlog/fmt/bundled/format.h"
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tiny_lsm {

// ************************ SkipListIterator ************************
BaseIterator &SkipListIterator::operator++() {
  // 实现SkipListIterator的++操作符
  // ? current 是当前节点指针, forward_[0] 是最底层链表的下一个节点

  if (current) {
    // 沿着第0层前进一步
    current = current->forward_[0];
  }
  return *this;
}

bool SkipListIterator::operator==(const BaseIterator &other) const {
  // 实现SkipListIterator的==操作符
  // ? 需要先通过 get_type() 判断类型再做 dynamic_cast

  // 类型都不同，谈不上相等
  // 参数是基类引用 BaseIterator&，运行时可能是任何子类
  if (get_type() != other.get_type())
    return false;
  const auto &other_iter = dynamic_cast<const SkipListIterator &>(other);
  return current == other_iter.current;
}

bool SkipListIterator::operator!=(const BaseIterator &other) const {
  // 实现SkipListIterator的!=操作符
  return !(operator==(other));
}

SkipListIterator::value_type SkipListIterator::operator*() const {
  // 实现SkipListIterator的*操作符
  // ? 若 current 为空需抛出异常
  // ? 若 current 为空需抛出异常
  if (!current) {
    throw std::runtime_error(
        "dereferencing invalid SkipListIterator: Node empty");
  }
  return {current->key_, current->value_};
}

IteratorType SkipListIterator::get_type() const {
  // 实现SkipListIterator的get_type
  // ? 主要是为了熟悉基类的定义和继承关系, 返回 IteratorType::SkipListIterator
  return IteratorType::SkipListIterator;
}

bool SkipListIterator::is_valid() const {
  return current && !current->key_.empty();
}
bool SkipListIterator::is_end() const { return current == nullptr; }

std::string SkipListIterator::get_key() const { return current->key_; }
std::string SkipListIterator::get_value() const { return current->value_; }
uint64_t SkipListIterator::get_tranc_id() const { return current->tranc_id_; }

// ************************ SkipList ************************
// 构造函数
SkipList::SkipList(int max_lvl) : max_level(max_lvl), current_level(1) {
  head = std::make_shared<SkipListNode>("", "", max_level, 0);
  dis_01 = std::uniform_int_distribution<>(0, 1);
  dis_level = std::uniform_int_distribution<>(0, (1 << max_lvl) - 1);
  gen = std::mt19937(std::random_device()());
}

int SkipList::random_level() {

  // ? 通过"抛硬币"的方式随机生成层数：
  // ? - 每次有50%的概率增加一层
  // ? - 确保层数分布为：第1层100%，第2层50%，第3层25%，以此类推
  // ? - 层数范围限制在[1, max_level]之间，避免浪费内存
  // TODO: Lab1.1 任务：插入时随机为这一次操作确定其最高连接的链表层数
  int level = 1;
  // 每一次 50% 概率加一层，最多到 max_level
  while (dis_01(gen) && level < max_level) {
    ++level;
  }
  return level;
}

// 插入或更新键值对
void SkipList::put(const std::string &key, const std::string &value,
                   uint64_t tranc_id) {
  spdlog::trace("SkipList--put({}, {}, {})", key, value, tranc_id);
  // TODO: Lab1.1 任务：实现插入或更新键值对
  // ? Hint: 你需要保证不同`Level`的步长从底层到高层逐渐增加
  // ? 你可能需要使用到`random_level`函数以确定层数, 其注释中为你提供一种思路
  // ? tranc_id 为事务id, 直接将其传递到 SkipListNode 的构造函数中即可
  // ? 若key存在且tranc_id相同, 仅更新value; 否则插入新节点
  // ? 注意维护 size_bytes

  // 1. 先创建 node 等待插入（必须在堆上，由 shared_ptr 管理生命周期）
  auto new_node_ptr =
      std::make_shared<SkipListNode>(key, value, random_level(), tranc_id);
  int node_lvl =
      static_cast<int>(new_node_ptr->forward_.size()); // 新节点的身高

  // 2. 找： 自顶向下，记录每层"最后一个小于新节点"的前驱
  //        1. update[lvl]数组表示 第 lvl 层上，最后一个小于目标 key
  //        的节点（也就是前驱）
  //        2. 为什么要按 max_level 开？因为新节点最高可能到 max_level
  //        3. 层，每层都要有一个前驱来接它
  std::vector<std::shared_ptr<SkipListNode>> update(max_level, nullptr);
  // 从当前实际使用的最高层开始（层号从 0 数，所以减 1）
  // 一层一层降到第 0 层（最底层的完整链表
  auto current = head;
  for (int lvl = current_level - 1; lvl >= 0; --lvl) {
    // "下一个还比你小？那我就再往前站一步。"
    // 直到下一个节点 ≥ 目标（或没有下一个了），停
    while (current->forward_[lvl] &&
           *(current->forward_[lvl]) < *new_node_ptr) {
      current = current->forward_[lvl];
    }
    // current 正好是这层"最后一个小于目标的节点
    update[lvl] = current;
  }

  // 3. 判：第 0 层候选节点 key 与 tranc_id 都相等 → 仅更新 value
  auto candidate = current->forward_[0];
  if (candidate && candidate->key_ == key && candidate->tranc_id_ == tranc_id) {
    // 更新新老 value 的差值即可
    size_bytes = size_bytes - candidate->value_.size() + value.size();
    candidate->value_ = value;
    return; // 更新完直接返回，别掉进插入分支
  }

  // 4. 插：新节点比现在的表还高 → 高出的层前驱就是 head，同时拔高 current_level
  for (int lvl = current_level; lvl < node_lvl; ++lvl) {
    update[lvl] = head;
  }
  // 更新现在的表高度
  if (node_lvl > current_level)
    current_level = node_lvl;

  // 逐层挂链：forward_ 与 backward_ 双向都要接
  for (int lvl = 0; lvl < node_lvl; ++lvl) {
    // ① 新节点 指向 前驱动的后继节点
    new_node_ptr->forward_[lvl] = update[lvl]->forward_[lvl];
    // ② 前驱改指新节点
    update[lvl]->forward_[lvl] = new_node_ptr;
    // ③ 新节点回指前驱
    new_node_ptr->set_backward(lvl, update[lvl]);
    if (new_node_ptr->forward_[lvl]) {
      // ④ 后继节点回指新节点
      new_node_ptr->forward_[lvl]->set_backward(lvl, new_node_ptr);
    }
  }
  // 5. 记账：key + value + tranc_id（口径见 MemorySizeTracking 测试）
  size_bytes += key.size() + value.size() + sizeof(uint64_t);
}

// 查找键值对
SkipListIterator SkipList::get(const std::string &key, uint64_t tranc_id) {
  spdlog::trace("SkipList--get({}) called", key);

  // 实现查找键值对
  // ? 从最高层开始向下查找, 最终在底层确认 key 是否存在
  // ? 若 tranc_id == 0, 直接比较 key 返回; 否则需满足事务可见性 (tranc_id_ <=
  // tranc_id)
  // 完成查找后还需要额外实现SkipListIterator中的TODO部分(Lab1.2)

  // 1. 找：下楼梯，只比 key（落在"该 key 的最新版本"跟前）
  auto current = head;
  for (int lvl = current_level - 1; lvl >= 0; --lvl) {
    while (current->forward_[lvl] && current->forward_[lvl]->key_ < key) {
      current = current->forward_[lvl];
    }
  }

  // 2. 候选 = 第 0 层下一个节点 = 该 key 的最新版本（同 key 按 tranc_id
  // 降序，最新排最前）
  auto candidate = current->forward_[0];

  // 3. 版本可见性：沿同 key 的版本链向右走（tranc_id 降序 = 越走越老），
  //    tranc_id == 0 表示非事务读，直接要最新版本；
  //    否则第一个满足 tranc_id_ <= tranc_id 的就是最新可见版本
  while (candidate && candidate->key_ == key) {
    if (tranc_id == 0 || candidate->tranc_id_ <= tranc_id) {
      return SkipListIterator(candidate);
    }
    candidate = candidate->forward_[0];
  }

  // 4. key 不存在，或所有版本对改事务都不可见
  return SkipListIterator{};
}

// 删除键值对
// 这里的 remove 是跳表本身真实的 remove,  lsm 应该使用 put 空值表示删除,
// 这里只是为了实现完整的 SkipList 不会真正被上层调用
void SkipList::remove(const std::string &key) {
  // 实现删除键值对
  // ? 从最高层开始查找目标节点并更新各层指针
  // ? 注意同时维护 backward_ 指针和 size_bytes
  // 1. 找：同款下楼梯，但只按 key 比较（删除针对 key 本身，不挑版本）
  std::vector<std::shared_ptr<SkipListNode>> update(max_level, nullptr);
  auto current = head;
  for (int lvl = current_level - 1; lvl >= 0; --lvl) {
    while (current->forward_[lvl] && current->forward_[lvl]->key_ < key) {
      current = current->forward_[lvl];
    }
    update[lvl] = current;
  }

  // 2. 判：候选节点 key 相等才算找到：key 不存在，无事发生
  //       如果有相等的，那么一定在 current 节点 右边节点
  auto target = current->forward_[0];
  if (!target || target->key_ != key) {
    return; // 没有，那么直接返回
  }

  // 3. 摘链：逐层让前驱"跳过" target，同时修好后继的 backward_
  int target_lvl = static_cast<int>(target->forward_.size());
  for (int lvl = 0; lvl < target_lvl; ++lvl) {
    // ① 前驱节点跳过 target 节点
    update[lvl]->forward_[lvl] = target->forward_[lvl];
    // ② 后继节点回指前驱节点
    if (target->forward_[lvl]) {
      target->forward_[lvl]->set_backward(lvl, update[lvl]);
    }
  }

  // 4. 记账扣减（口径与 put 对称）
  size_bytes -= target->key_.size() + target->value_.size() + sizeof(uint64_t);
}

// 刷盘时可以直接遍历最底层链表
std::vector<std::tuple<std::string, std::string, uint64_t>> SkipList::flush() {
  // std::shared_lock<std::shared_mutex> slock(rw_mutex);
  spdlog::debug("SkipList--flush(): Starting to flush skiplist data");

  std::vector<std::tuple<std::string, std::string, uint64_t>> data;
  auto node = head->forward_[0];
  while (node) {
    data.emplace_back(node->key_, node->value_, node->tranc_id_);
    node = node->forward_[0];
  }

  spdlog::debug("SkipList--flush(): Flushed {} entries", data.size());

  return data;
}

size_t SkipList::get_size() {
  // std::shared_lock<std::shared_mutex> slock(rw_mutex);
  return size_bytes;
}

// 清空跳表，释放内存
void SkipList::clear() {
  // std::unique_lock<std::shared_mutex> lock(rw_mutex);
  head = std::make_shared<SkipListNode>("", "", max_level, 0);
  size_bytes = 0;
}

SkipListIterator SkipList::begin() {
  // return SkipListIterator(head->forward[0], rw_mutex);
  return SkipListIterator(head->forward_[0]);
}

SkipListIterator SkipList::end() {
  return SkipListIterator(); // 使用空构造函数
}

// 找到前缀的起始位置
// 返回第一个前缀匹配或者大于前缀的迭代器
SkipListIterator SkipList::begin_preffix(const std::string &preffix) {
  // Lab1.3 任务：实现前缀查询的起始位置
  // ? 从最高层开始查找, 找到第一个 key >= preffix 的节点
  // 1. 下楼梯：和 get 同款，每层走到"下一个节点 key >= preffix"之前停下
  //    循环不变式：current 始终是当前层最后一个小于 preffix 的节点
  //              也就是说，如果没有大于 preffix 的节点也不会是 nullptr
  auto current = head;
  for (int lvl = current_level - 1; lvl >= 0; --lvl) {
    while (current->forward_[lvl] && current->forward_[lvl]->key_ < preffix) {
      current = current->forward_[lvl];
    }
  }

  // 2. 第 0 层的下一个节点 = 第一个 key >= preffix 的节点
  //    没有则是空迭代器，天然 == end()
  return SkipListIterator(current->forward_[0]);
}

// 找到前缀的终结位置
SkipListIterator SkipList::end_preffix(const std::string &prefix) {
  // TODO: Lab1.3 任务：实现前缀查询的终结位置
  // ? 找到第一个 key 不以 prefix 开头的节点作为终结位置
  // 1. 巧劲：构造 prefix 的"后继字符串"（末字符 +1）
  //    性质：所有以 prefix 开头的 key 都 < 后继；
  // end_preffix("a") → 后继 "b" → 第一个 ≥"b" 是 banana ✓
  // end_preffix("cherry") → 后继 "cherz"，cherry2 < "cherz" 被跳过 → end()

  std::string successor = prefix;

  // char 是一个字节，最大 255(0xff)，如果再+1的话会回绕为0x00
  // 解决办法：从后往前找第一个 != 0xff 的字节，+1并截断其后的 0xff 字节
  //         全是 0xff 则后继不存在，直接返回 end()
  while (!successor.empty() && successor.back() == '\xff') {
    successor.pop_back();
  }

  // 空前缀匹配一切，终结即 end()
  if (successor.empty())
    return end();

  // "a"->"b", "cherry"->"cherz"
  successor.back() += 1;
  return begin_preffix(successor);
}

// ? 这里单调谓词的含义是, 整个数据库只会有一段连续区间满足此谓词
// ? 例如之前特化的前缀查询，以及后续可能的范围查询，都可以转化为谓词查询
// ? 返回第一个满足谓词的位置和最后一个满足谓词的迭代器
// ? 如果不存在, 返回 nullopt
// ? 谓词作用于key, 且保证满足谓词的结果只在一段连续的区间内, 例如前缀匹配的谓词
// ? predicate返回值:
// ?   0: 满足谓词
// ?   >0: 不满足谓词, 需要向右移动
// ?   <0: 不满足谓词, 需要向左移动
// ! Skiplist 中的谓词查询不会进行事务id的判断, 需要上层自己进行判断
std::optional<std::pair<SkipListIterator, SkipListIterator>>
SkipList::iters_monotony_predicate(
    std::function<int(const std::string &)> predicate) {
  // TODO: Lab1.3 任务：实现谓词查询
  // ? 分两步: 1. 利用多层跳表快速找到谓词满足区间内的一个节点
  // ?         2. 分别向前/向后扩展, 利用 backward_ 和 forward_ 确定区间边界
  // ? 注意: 向前查找时需要利用 backward_ 指针从当前节点的最高层开始回溯
  // 1. 多层下降定位区间内任意一个节点 node1：
  //    谓词即指南针 >0 往右走 / ==0 命中 / <0 下楼细化
  std::shared_ptr<SkipListNode> node1 = nullptr;
  auto current = head;
  for (int lvl = current_level - 1; lvl >= 0 && !node1; --lvl) {
    while (current->forward_[lvl]) {
      int outcome = predicate(current->forward_[lvl]->key_);
      if (outcome > 0)
        current = current->forward_[lvl]; // 区间在右侧，前进
      else if (outcome == 0)
        node1 = current->forward_[lvl]; // 进入到区间
      else
        break; // 右侧越界了，下楼
    }
  }

  if (!node1) // 没有满足谓词条件的key在表中
    return std::nullopt;

  // 3. 沿0层向左边扩张到区间左端
  // （backward_ 是 weak_ptr，要 lock()；不能越过 head）
  auto node0 = node1;
  while (auto prev = node0->backward_[0].lock()) {
    if (prev == head || predicate(prev->key_) != 0)
      break;
    node0 = prev;
  }

  return std::nullopt;
}

// ? 打印跳表, 你可以在出错时调用此函数进行调试
void SkipList::print_skiplist() {
  for (int level = 0; level < current_level; level++) {
    std::cout << "Level " << level << ": ";
    auto current = head->forward_[level];
    while (current) {
      std::cout << current->key_;
      current = current->forward_[level];
      if (current) {
        std::cout << " -> ";
      }
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;
}
} // namespace tiny_lsm
