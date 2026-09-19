#include "lsm/level_iterator.h"
#include "iterator/iterator.h"
#include "lsm/engine.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include <memory>
#include <shared_mutex>
#include <string>

// TODO: Lab 4.6 Level_Iterator 初始化
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
  std::shared_ptr<HeapIterator> mem_iter_ptr = std::make_shared<HeapIterator>(mem_iter);
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
        iter.is_valid() && !iter.is_end(); ++iter){
      // 事务模式: 真正的版本过滤在 BlockIterator 里已做过，我们不用再手动做
    
      // SearchItem 第 3 个参数 idx 填 -sst_id, 是个负号小技巧:
      //   堆里同一个 key 撞车时, idx 小的先弹出来。
      //   sst_id 越大文件越新, 加负号后反而越小 -> 新文件的条目先出来。
      //   效果: 同一个 key 在两张 L0 表里都有时, 更新的那张赢。
      item_vec.emplace_back(iter.key(), iter.value(), -sst_id, 0, iter.get_cur_tranc_id()); 
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
    if (level == 0) continue; 

    std::vector<std::shared_ptr<SST>> ssts; 
    for (auto sst_id : sst_id_list)
      ssts.push_back(engine_->ssts[sst_id]); 
    iter_vec.push_back(std::make_shared<ConcactIterator>(ssts, max_tranc_id_)); 
  }


}

std::pair<size_t, std::string> Level_Iterator::get_min_key_idx() const {
  // TODO: Lab 4.6 获取当前 key 最小的迭代器在 iter_vec 中的索引和具体的 key
  return {};
}

void Level_Iterator::skip_key(const std::string &key) {
  // TODO: Lab 4.6 跳过 key 相同的部分(即被当前激活的迭代器覆盖的写入记录)
}

void Level_Iterator::update_current() const {
  // TODO: Lab 4.6 更新当前值 cached_value
  // ? 实现 -> 时你也许会用到 cached_value
}

BaseIterator &Level_Iterator::operator++() {
  // TODO: Lab 4.6 ++ 重载
  return *this;
}

bool Level_Iterator::operator==(const BaseIterator &other) const {
  // TODO: Lab 4.6 == 重载
  return false;
}

bool Level_Iterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab 4.6 != 重载
  return false;
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
