#include "lsm/level_iterator.h"
#include "lsm/engine.h"
#include "sst/concact_iterator.h"
#include "sst/sst.h"
#include <memory>
#include <shared_mutex>
#include <string>

// TODO: 需要进行单元测试
namespace tiny_lsm {
Level_Iterator::Level_Iterator(std::shared_ptr<LSMEngine> engine,
                               uint64_t max_tranc_id)
    : engine_(engine), max_tranc_id_(max_tranc_id), rlock_(engine_->ssts_mtx) {
  // 成员变量获取sst读锁

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
