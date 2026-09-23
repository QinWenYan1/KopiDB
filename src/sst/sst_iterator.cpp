#include "sst/sst_iterator.h"
#include "block/block_iterator.h"
#include "sst/sst.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tiny_lsm {

// Lab 3.7 实现谓词查询功能
// predicate返回值:
//   0: 谓词
//   >0: 不满足谓词, 需要向右移动
//   <0: 不满足谓词, 需要向左移动
std::optional<std::pair<SstIterator, SstIterator>> sst_iters_monotony_predicate(
    std::shared_ptr<SST> sst, uint64_t tranc_id,
    std::function<int(const std::string &)> predicate) {
  // 块级别枝剪 -> 块中精找
  //    命中区是连续的 (谓词单调), 所以块与命中区只有三种关系:
  //    整块在左 (跳过) / 相交 (进块二分) / 整块在右 (后面的块更右, 收工)
  std::optional<SstIterator> final_begin = std::nullopt;
  std::optional<SstIterator> final_end = std::nullopt;

  for (size_t block_idx = 0; block_idx < sst->meta_entries.size();
       ++block_idx) {
    const auto &meta_i = sst->meta_entries[block_idx];

    // 1. 块级剪枝: 只看 meta 的首尾 key, 不读块 (省 IO 的核心)
    if (predicate(meta_i.first_key) < 0)
      // 块首都过了命中区右界 → 后面块更右 → 整体结束
      break;
    if (predicate(meta_i.last_key) > 0)
      // 块尾还在命中区左界之前 → 整块不相交, 跳过
      continue;

    // 2. 落到范围中 → 读块精找 (块内两次二分, 返回 [first, last+1) 迭代器对)
    auto block = sst->read_block(block_idx);
    auto result_i = block->get_monotony_predicate_iters(tranc_id, predicate);
    if (!result_i.has_value())
      // 范围命中但可见性过滤后无命中 (tranc 太旧)
      continue;
    auto [i_begin, i_end] = result_i.value();

    // 3. 组装 SST 级迭代器: 把"块内迭代器"升级成"SST 级迭代器"
    //    begin 只在第一个命中块定一次; end 每个命中块都刷新
    //    (循环结束自然留下最右命中块的 end)
    if (!final_begin.has_value()) {
      auto tmp_it = SstIterator(sst, tranc_id);
      tmp_it.set_block_idx(block_idx);
      tmp_it.set_block_it(i_begin);
      final_begin = tmp_it;
    }

    auto tmp_it = SstIterator(sst, tranc_id);
    tmp_it.set_block_idx(block_idx);
    tmp_it.set_block_it(i_end);

    // 4. 命中区顶到 SST 末尾: i_end 已是末块块尾 → 归一化成全局 end 态
    //    参考实现这里的条件写错了 (is_end() 在 set_block_it 后恒 false,
    //    永远不触发); 这里按意图修正, 否则边界场景扫到末尾会死循环
    if (block_idx + 1 == sst->num_blocks() && i_end->is_end()) {
      tmp_it.set_block_idx(sst->num_blocks());
      tmp_it.set_block_it(nullptr);
    }
    final_end = tmp_it;
  }

  // 5. 一个命中块都没有 → 无区间
  if (!final_begin.has_value() || !final_end.has_value())
    return std::nullopt;
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

// Lab 3.6 将迭代器定位到第一个key
void SstIterator::seek_first() {
  // 是否能使用 seek() 来委托？ 不能！
  // seek_first 锁的是"位置 0 + tranc_id"，滑到的是第一个可见
  // entry——它落点可能已经不在第一个 key 上了。 然而，seek 是必须锁定一个 key
  // 一个是 key 空间查询，一个是位置空间查询，维度不同，无法委托

  // 1. seek_first 也可能在迭代过程中再次调用，
  //    因此先清除旧位置和旧的 key-value 缓存。
  cached_value.reset(); 
  m_block_it = nullptr; 
  m_block_idx = 0;

  // 没有关联 SST，保持 end 状态。
  // 必须先判空，再通过 m_sst 访问成员
  if (!m_sst) 
    return;

  // 与 m_block_idx 的 int64_t 类型保持一致。
  const auto block_count = static_cast<int64_t>(m_sst->num_blocks()); 

  // 2. seek_first = 钉到第 1 个 可见 block 的可见的第 1 条 entry
  //    找到第一个可见的块的可见开头，一个 block 有可能都不可见，那就要读下一个快
  for (; m_block_idx < block_count; ++m_block_idx){
    auto block = m_sst->read_block(m_block_idx); 

    // 从当前块的第一条记录开始
    // BlockIterator 构造时会自动跳过不可见版本
    // BlockIterator 的下标构造: 定位到 idx，构造内部自动 skip_by_tranc_id
    m_block_it = std::make_shared<BlockIterator>(block, 0, max_tranc_id_, keep_all_versions_); 

    // 过滤后仍有记录，就找到了整张 SST 的第一条可见记录
    if (!m_block_it->is_end()) return; 

    // 当前块没有可见记录，继续检查下一个块。
  }

  // 3. 所有块都检查完了，仍没有可见记录。
  //    此时 m_block_idx == block_count，清空指针表示 SST 耗尽。
  m_block_it = nullptr;
}

// Lab 3.6 将迭代器定位到指定key的位置
void SstIterator::seek(const std::string &key) {

  if (!m_sst) {
    m_block_it = nullptr;
    return;
  }

  try {
    // 1. 两级定位的第一级: 哪个块 (bloom + meta 二分, 返回候选块)
    //    全 SST 都不可能有 -> end 态: (num_blocks, nullptr)
    m_block_idx = m_sst->find_block_idx(key);
    if (m_block_idx == -1 ||
        m_block_idx >= static_cast<int64_t>(m_sst->num_blocks())) {
      m_block_idx = m_sst->num_blocks();
      m_block_it = nullptr;
      return;
    }

    // 2. 第二级: 进块, 块内二分 (key 构造版 BlockIterator, 找不到会指到块尾)
    //    → 若块中二分找不到目标 key → 返回 nullopt
    //    → 构造器把 current_index 设为 block->offsets.size() (块尾)
    auto block_ptr = m_sst->read_block(m_block_idx);
    m_block_it = std::make_shared<BlockIterator>(block_ptr, key, max_tranc_id_,
                                                 keep_all_versions_);

    // 3. 终审: 缝隙 key / 版本全不可见 → 块内没找到 → end 态
    if (m_block_it->is_end()) {
      m_block_idx = m_sst->num_blocks();
      m_block_it = nullptr;
    }
  } catch (const std::exception &) {
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

// Lab 3.6 实现迭代器自增
BaseIterator &SstIterator::operator++() {
  if (!m_block_it) // end 态防御：已经到头再 ++ 原地不动了
    return *this;

  // 块内前进：版本去重复和tranc过滤都在BlockIterator::++ 里
  ++(*m_block_it);

  // 当前块阅读完 -> 跨块
  if (m_block_it->is_end()) {
    ++m_block_idx;
    // 边界检查，是否到了本 SST 的最后一个 block
    if (m_block_idx < static_cast<int64_t>(m_sst->num_blocks())) {
      auto next_block = m_sst->read_block(m_block_idx);
      // 新块从头开始 (下标构造, 自动 skip_by_tranc_id)
      // 复用同一个 shared_ptr，把新迭代器放入到原对象内部
      (*m_block_it) =
          BlockIterator(next_block, 0, max_tranc_id_, keep_all_versions_);
    } else {
      // 已经到了边界，全部读完了 -> end 状态（约定为直接置空）
      m_block_it = nullptr;
    }
  }

  // 位置动了，缓存作废
  cached_value = std::nullopt;
  return *this;
}

// Lab 3.6 实现迭代器比较
bool SstIterator::operator==(const BaseIterator &other) const {

  // 1. 类型不同永不相等 (基类引用可能装着 MemIterator/HeapIterator...)
  if (other.get_type() != IteratorType::SstIterator)
    return false;

  // 2. get_type 已保证类型, dynamic_cast 引用版必然成功 (失败会抛 bad_cast)
  auto other2 = dynamic_cast<const SstIterator &>(other);

  // 3. 不同 SST 或不同块，直接不等
  if (m_sst != other2.m_sst || m_block_idx != other2.m_block_idx)
    return false;

  // 4. 双空 = 两个 end 哨兵, 相等; 一空一非空, 不等
  if (!m_block_it && !other2.m_block_it)
    return true;
  if (!m_block_it || !other2.m_block_it)
    return false;

  // 5. 同 SST 同块, 比块内位置 (委托 BlockIterator::operator==)
  return *m_block_it == *other2.m_block_it;
}

// Lab 3.6 实现迭代器比较
//  由 operator==委托
bool SstIterator::operator!=(const BaseIterator &other) const {
  return !operator==(other);
}

// Lab 3.6 实现迭代器解引用
SstIterator::value_type SstIterator::operator*() const {

  if (!is_valid())
    throw std::runtime_error(
        "SstIterator::operator*: cannot dereference this iterator");

  // WiscKey 模式: value 是 12 字节提货单 [offset:8][size:4]
  // resolve_value 拿单子去 vlog 取真值; 普通模式原样返回 (零成本直通)
  update_current();
  return *cached_value;
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
  if (!is_valid())
    throw std::runtime_error(
        "SstIterator::operator->: cannot dereference this iterator");

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
          iter.key(),
          iter.m_sst->resolve_value(iter.m_block_it->operator*().second),
          -iter.m_sst->get_sst_id(), 0,
          iter.get_cur_tranc_id()); // ! 此处的level暂时没有作用,
                                    // 都作用于同一层的比较
      ++iter;
    }
  }
  return std::make_pair(it_begin, HeapIterator());
}
} // namespace tiny_lsm
