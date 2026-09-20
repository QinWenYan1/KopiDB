#include "lsm/level_iterator.h"
#include "iterator/iterator.h"
#include "lsm/engine.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include <memory>
#include <shared_mutex>
#include <string>

// Lab 4.6 Level_Iterator 初始化
namespace tiny_lsm {
Level_Iterator::Level_Iterator(std::shared_ptr<LSMEngine> engine,
                               uint64_t max_tranc_id)
    : engine_(engine), max_tranc_id_(max_tranc_id), rlock_(engine_->ssts_mtx) {
  // 初始化列表最后一项 rlock_(engine_->ssts_mtx): 这个迭代器从生到死
  // 一直持有引擎的读锁。为什么需要它?
  //   我们的 SST 迭代器是"惰性"的: 不是构造时把数据全读进内存,
  //   而是边迭代边从磁盘文件里读。如果迭代到一半, 后台 compact
  //   把正在读的旧 SST 文件删了, 后续读取就会对着一个已删除的文件
  //   -> 读到垃圾数据或直接崩溃。
  //   读锁就是告诉 flush/compact: "我在读, 这些文件先别动。"
  // 代价也很直白: 迭代器活得越久, flush/compact 被堵得越久。
  //   教学项目接受这个代价, 换取实现简单。

  // ===== 第 1 路来源: memtable (内存表) =====
  // memtable 本身就有序, 直接能给一个 HeapIterator。
  // 它在 iter_vec 里的下标是 0: 下标越靠前代表数据越新,
  // 后面同一个 key 出现在多路来源时, 下标小的赢
  // (memtable 的数据最新, 理应排最前)。
  auto mem_iter = engine_->memtable.begin(max_tranc_id_);
  // 按值拷进堆上对象 (shared_ptr 只能管理堆对象)
  std::shared_ptr<HeapIterator> mem_iter_ptr =
      std::make_shared<HeapIterator>(mem_iter);
  iter_vec.push_back(mem_iter_ptr);

  // ===== 第 2 路来源: L0 整层, 全部条目灌进一个堆 =====
  // 为什么 L0 不能像 L1+ 那样用 ConcactIterator 直接拼接?
  //   拼接的前提是"同层各 SST 的 key 范围互不重叠"。
  //   L0 不满足: 每次 flush 直接产一张新表, 表与表的 key 范围随意重叠。
  // 所以换办法: 把 L0 每张表的所有条目全读出来塞进堆里,
  //   堆每次自动弹出当前最小的 key, 相当于替我们做好了全局排序。
  std::vector<SearchItem> item_vec;
  for (auto &sst_id : engine_->level_sst_ids[0]) {
    auto sst = engine_->ssts[sst_id];
    for (auto iter = sst->begin(max_tranc_id_);
         iter.is_valid() && !iter.is_end(); ++iter) {
      // 事务模式: 真正的版本过滤在 BlockIterator 里已做过，我们不用再手动做

      // SearchItem 第 3 个参数 idx 填 -sst_id, 是个负号小技巧:
      //   堆里同一个 key 撞车时, idx 小的先弹出来。
      //   sst_id 越大文件越新, 加负号后反而越小 -> 新文件的条目先出来。
      //   效果: 同一个 key 在两张 L0 表里都有时, 更新的那张赢。
      item_vec.emplace_back(iter.key(), iter.value(), -sst_id, 0,
                            iter.get_cur_tranc_id());
    }
  }
  std::shared_ptr<HeapIterator> l0_iter_ptr =
      std::make_shared<HeapIterator>(item_vec, max_tranc_id);
  iter_vec.push_back(l0_iter_ptr);

  // ===== 第 3 路来源: L1 及更深层, 每层一个 ConcactIterator =====
  // L1+ 经过 compact 整理: 同层各 SST 的 key 范围有序且不重叠,
  // 所以每层只要一个 ConcactIterator 把各张表首尾相接串起来, 不用堆。
  // 有几层就装几个 —— 这也是教程"Compact 之后才有 Level"的代码体现:
  // compact 之前 level_sst_ids 里只有第 0 层, 这个循环一次都不会执行。
  for (auto &[level, sst_id_list] : engine_->level_sst_ids) {
    if (level == 0)
      continue;

    std::vector<std::shared_ptr<SST>> ssts;
    for (auto sst_id : sst_id_list)
      ssts.push_back(engine_->ssts[sst_id]);
    iter_vec.push_back(std::make_shared<ConcactIterator>(ssts, max_tranc_id_));
  }

  // ===== 收尾: 把迭代器停到第一个"活着的" key 上 =====
  // 刚组装好时, 全局最小 key 已经可以算出来, 但有个坑:
  //   它的最新版本可能是"墓碑" (value 是空串, 表示这个 key 已被删除)。
  //   墓碑不该被查询方看到, 所以循环处理:
  //   算出当前最小 key -> 是墓碑就把这个 key 在所有来源里的副本全部越过
  //   -> 再算下一个, 直到撞上活 key, 或者所有来源都耗尽 (= end 状态)。
  while (!is_end()) {

    // 哪一路来源的头部 key 最小
    auto [min_idx, _] = get_min_key_idx();
    cur_idx_ = min_idx;
    // 把这条 key-value 读进缓存 cached_value
    update_current();

    // 空 value = 墓碑
    if (cached_value->second.empty()) {
      // 所有来源的同 key 副本一起越过
      skip_key(cached_value->first);
      continue;
    }
    // 找到活 key, 构造完成
    break;
  }
}

// Lab 4.6 获取当前 key 最小的迭代器在 iter_vec 中的索引和具体的 key
// 返回: (那一路在 iter_vec 里的下标, 最小 key 本身)
// 这是归并的核心动作: 每一步都从所有来源的头部里挑最小的吐出去
std::pair<size_t, std::string> Level_Iterator::get_min_key_idx() const {
  size_t min_idx = 0;

  // 空串当"还没找到"的哨兵 (正常 key 不会为空)
  std::string min_key;
  for (size_t i = 0; i < iter_vec.size(); ++i) {
    // 这路来源已耗尽, 不参与比较
    if (!iter_vec[i]->is_valid())
      continue;

    auto key = (**iter_vec[i]).first;
    if (min_key.empty() || key < min_key) {
      // 更小的 key 出现了, 更新冠军
      // 注意是严格小于: 同 key 时不换冠军, "先到先留" ->
      // iter_vec 下标小的 (更新的来源) 天然赢

      min_key = key;
      min_idx = i;
    } else if (key == min_key && max_tranc_id_ != 0 &&
               iter_vec[i]->get_tranc_id() >
                   iter_vec[min_idx]->get_tranc_id()) {
      // 同 key 两路都有: 版本号大的 (更新的) 赢
      // (实际上各来源此时 get_tranc_id() 都返回快照 id, 很难触发;
      // 真正的决胜靠上面那句"先到先留")

      min_idx = i;
    }
  }
  return {min_idx, min_key};
}

// Lab 4.6 跳过 key 相同的部分 (跨来源去重)
// 同一个 key 可能 memtable 有一份、L0 有一份、L2 也有一份,
// 我们已经把最新的那份吐出去了, 其余副本必须全部越过,
// 否则同一个 key 会被扫描方看到好几次
void Level_Iterator::skip_key(const std::string &key) {
  for (auto &iter : iter_vec) {

    // 每路来源各自往前走, 直到头部不再是这个 key
    while (iter->is_valid() && (**iter).first == key)
      ++(*iter);
  }
}

// Lab 4.6 把当前位置的 key-value 读进缓存 cached_value
// 为什么要缓存: operator-> 要返回一个指针, 被指的对象必须在函数
// 返回后还活着, 所以需要一个成员变量当"停车位"
void Level_Iterator::update_current() const {
  // cur_idx_ 指向的那一路已经耗尽, 还被要求读值 = 用法错误, 抛异常
  if (!iter_vec[cur_idx_]->is_valid())
    throw std::runtime_error(
        "Level_Iterator::update_current: cannot dereference this iterator");
  // 解引用孩子, 按值拷进停车位
  cached_value = **iter_vec[cur_idx_];
}

// Lab 4.6 ++ 重载
BaseIterator &Level_Iterator::operator++() {
  // 1. 当前 key 已经吐过了: 把它在所有来源里的副本全部越过
  skip_key(cached_value->first);

  // 2. 重新选最小 —— 和构造函数收尾是同一个循环:
  //    选最小 -> 读缓存 -> 是墓碑就整个 key 越过 -> 再选下一个
  while (!is_end()) {
    auto [min_idx, _] = get_min_key_idx();
    cur_idx_ = min_idx;
    update_current();
    // 空 value = 墓碑, 不能给查询方看到
    if (cached_value->second.empty()) {
      skip_key(cached_value->first);
      continue;
    }
    break;
  }
  return *this;
}

// TODO: Lab 4.6 == 重载
bool Level_Iterator::operator==(const BaseIterator &other) const {
  // 类型不同永不相等 (基类引用里可能装着别的迭代器)
  if (other.get_type() != IteratorType::LevelIterator)
    return false; 
  auto &other2 = dynamic_cast<const Level_Iterator &>(other);

  // end 态归一: 双方都耗尽才算相等
  if (!is_valid() || !other2.is_valid()) {
    return !is_valid() && !other2.is_valid(); 
  }

  // 都有效: 比当前位置的 key-value 是否一样
  return cached_value->first == other2.cached_value
}

// Lab 4.6 != 重载
bool Level_Iterator::operator!=(const BaseIterator &other) const {
  return !(operator==(other)); 
}

BaseIterator::value_type Level_Iterator::operator*() const {
  // TODO: Lab 4.6 * 重载
  return {};
}

BaseIterator::pointer Level_Iterator::operator->() const {
  // TODO: Lab 4.6 -> 重载
  return nullptr;
}

IteratorType Level_Iterator::get_type() const {
  return IteratorType::LevelIterator;
}

uint64_t Level_Iterator::get_tranc_id() const { return max_tranc_id_; }

bool Level_Iterator::is_end() const {
  for (auto &iter : iter_vec) {
    if ((*iter).is_valid()) {
      return false;
    }
  }
  return true;
}

bool Level_Iterator::is_valid() const { return !is_end(); }

} // namespace tiny_lsm
