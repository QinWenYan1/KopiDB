#include "lsm/level_iterator.h"
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
