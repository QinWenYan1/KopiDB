#include "iterator/iterator.h"
#include "sst/sst_iterator.h"
#include "block/block_iterator.h"
#include "sst/sst.h"
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>

namespace tiny_lsm {

// predicate返回值:
//   0: 谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<std::pair<SstIterator, SstIterator>> sst_iters_monotony_predicate(
    std::shared_ptr<SST> sst, uint64_t tranc_id,
    std::function<int(const std::string &)> predicate) {
  std::optional<SstIterator> final_begin = std::nullopt;
  std::optional<SstIterator> final_end = std::nullopt;
  for (int block_idx = 0; block_idx < sst->meta_entries.size(); block_idx++) {
    auto block = sst->read_block(block_idx);

    BlockMeta &meta_i = sst->meta_entries[block_idx];
    if (predicate(meta_i.first_key) < 0) {
      break;
    }
    if (predicate(meta_i.last_key) > 0) {
      continue;
    }

    auto result_i = block->get_monotony_predicate_iters(tranc_id, predicate);
    if (result_i.has_value()) {
      auto [i_begin, i_end] = result_i.value();
      if (!final_begin.has_value()) {
        auto tmp_it = SstIterator(sst, tranc_id);
        tmp_it.set_block_idx(block_idx);
        tmp_it.set_block_it(i_begin);
        final_begin = tmp_it;
      }
      auto tmp_it = SstIterator(sst, tranc_id);
      tmp_it.set_block_idx(block_idx);
      tmp_it.set_block_it(i_end);
      if (tmp_it.is_end() && tmp_it.m_block_idx == sst->num_blocks()) {
        tmp_it.set_block_it(nullptr);
      }
      final_end = tmp_it;
    }
  }
  if (!final_begin.has_value() || !final_end.has_value()) {
    return std::nullopt;
  }
  return std::make_pair(final_begin.value(), final_end.value());
}

SstIterator::SstIterator(std::shared_ptr<SST> sst, uint64_t tranc_id,
                         bool keep_all_versions)
    : m_sst(sst), m_block_idx(0), m_block_it(nullptr), max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
  if (m_sst) {
    seek_first();
  }
}

SstIterator::SstIterator(std::shared_ptr<SST> sst, const std::string &key,
                         uint64_t tranc_id, bool keep_all_versions)
    : m_sst(sst), m_block_idx(0), m_block_it(nullptr), max_tranc_id_(tranc_id),
      keep_all_versions_(keep_all_versions) {
  if (m_sst) {
    seek(key);
  }
}

void SstIterator::set_block_idx(size_t idx) { m_block_idx = idx; }
void SstIterator::set_block_it(std::shared_ptr<BlockIterator> it) {
  m_block_it = it;
}

// TODO: Lab 3.6 将迭代器定位到第一个key
void SstIterator::seek_first() {
  // 是否能使用 seek() 来委托？ 不能！
  // seek_first 锁的是"位置 0 + tranc_id"，滑到的是第一个可见 entry——它落点可能已经不在第一个 key 上了。
  // 然而，seek 是必须锁定一个 key 
  // 一个是 key 空间查询，一个是位置空间查询，维度不同，无法委托 

  // 迭代器的状态 = (m_sst, m_block_idx, m_block_it) 三元组
  //seek_first = 钉到第 0 个 block 的第 0 条 entry
  if (!m_sst || m_sst->num_blocks() == 0){
    m_block_it = nullptr; 
    return; 
  }

  m_block_idx = 0; 
  auto block = m_sst->read_block(m_block_idx); 
  //BlockIterator 的下标构造: 定位到 idx = 0，构造内部自动 skip_by_tranc_id
  m_block_it = std::make_shared<BlockIterator>(block, 0, max_tranc_id_, keep_all_versions_);
}

// TODO: Lab 3.6 将迭代器定位到指定key的位置
void SstIterator::seek(const std::string &key) {

  if (!m_sst){
    m_block_it = nullptr; 
    return; 
  }

  try{
    // 1. 两级定位的第一级: 哪个块 (bloom + meta 二分, 返回候选块)
    //    全 SST 都不可能有 -> end 态: (num_blocks, nullptr)
    m_block_idx = m_sst->find_block_idx(key); 
    if (m_block_idx == -1 || m_block_idx >= static_cast<int64_t>(m_sst->num_blocks())){
      m_block_idx = m_sst->num_blocks(); 
      m_block_it = nullptr; 
      return; 
    }

    // 2. 第二级: 进块, 块内二分 (key 构造版 BlockIterator, 找不到会指到块尾)
    //    → 若块中二分找不到目标 key → 返回 nullopt
    //    → 构造器把 current_index 设为 block->offsets.size() (块尾)
    auto block_ptr = m_sst->read_block(m_block_idx);
    m_block_it = std::make_shared<BlockIterator>(block_ptr, key, max_tranc_id_, keep_all_versions_); 
    
    // 3. 终审: 缝隙 key / 版本全不可见 → 块内没找到 → end 态
    if (m_block_it->is_end()){
      m_block_idx = m_sst->num_blocks(); 
      m_block_it = nullptr; 
    }
  }catch (const std::exception &){
    // 参考实现行为: 读盘/解码异常一律按 "没找到" 处理
    m_block_it = nullptr; 
  }

}

std::string SstIterator::key() {
  if (!m_block_it) {
    throw std::runtime_error("Iterator is invalid");
  }
  return (*m_block_it)->first;
}

std::string SstIterator::value() {
  if (!m_block_it) {
    throw std::runtime_error("Iterator is invalid");
  }
  return m_sst->resolve_value((*m_block_it)->second);
}

// TODO: Lab 3.6 实现迭代器自增
BaseIterator &SstIterator::operator++() {
  if (!m_block_idx) // end 态防御：已经到头再 ++ 原地不动了
    return *this; 
  
  // 块内前进：版本去重复和trnac过滤都在BlockIterator::++ 里
  ++(*m_block_it); 
  return *this;

  // 当前块阅读完 -> 跨块
  if(m_block_it->is_end()){

  }

bool SstIterator::operator==(const BaseIterator &other) const {
  // TODO: Lab 3.6 实现迭代器比较
  return false;
}

bool SstIterator::operator!=(const BaseIterator &other) const {
  // TODO: Lab 3.6 实现迭代器比较
  return false;
}

SstIterator::value_type SstIterator::operator*() const {
  // TODO: Lab 3.6 实现迭代器解引用
  return {};
}

IteratorType SstIterator::get_type() const { return IteratorType::SstIterator; }

uint64_t SstIterator::get_tranc_id() const {
  if (keep_all_versions_ && m_block_it) {
    return m_block_it->get_cur_tranc_id();
  }
  return max_tranc_id_;
}
bool SstIterator::is_end() const { return !m_block_it; }

bool SstIterator::is_valid() const {
  return m_block_it && !m_block_it->is_end() &&
         m_block_idx < m_sst->num_blocks();
}
SstIterator::pointer SstIterator::operator->() const {
  update_current();
  return &(*cached_value);
}

void SstIterator::update_current() const {
  if (!cached_value && m_block_it && !m_block_it->is_end()) {
    auto raw = *(*m_block_it);
    raw.second = m_sst->resolve_value(raw.second);
    cached_value = raw;
  }
}

uint64_t SstIterator::get_cur_tranc_id() const {
  if (!m_block_it) {
    return 0;
  }
  return m_block_it->get_cur_tranc_id();
}

std::pair<HeapIterator, HeapIterator>
SstIterator::merge_sst_iterator(std::vector<SstIterator> iter_vec,
                                uint64_t tranc_id, bool keep_all_versions) {
  if (iter_vec.empty()) {
    return std::make_pair(HeapIterator(), HeapIterator());
  }

  HeapIterator it_begin(false, keep_all_versions); // 不跳过删除元素
  for (auto &iter : iter_vec) {
    while (iter.is_valid() && !iter.is_end()) {
      it_begin.items.emplace(
          iter.key(), iter.m_sst->resolve_value(iter.m_block_it->operator*().second),
          -iter.m_sst->get_sst_id(), 0,
          iter.get_cur_tranc_id()); // ! 此处的level暂时没有作用, 都作用于同一层的比较
      ++iter;
    }
  }
  return std::make_pair(it_begin, HeapIterator());
}
} // namespace tiny_lsm