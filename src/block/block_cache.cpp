#include "block/block_cache.h"
#include "block/block.h"
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace tiny_lsm {
BlockCache::BlockCache(size_t capacity, size_t k)
    : capacity_(capacity), k_(k) {}

BlockCache::~BlockCache() = default;

// TODO: Lab 4.8 查询一个 Block
std::shared_ptr<Block> BlockCache::get(int sst_id, int block_id) {
  // get 虽然用于读取 Block，但会修改计数和链表位置，
  // 所以也必须加锁，并与 put 使用同一把锁
  std::lock_guard<std::mutex> lock(mutex_);

  // 每次查询都计入总请求数，包括未命中的查询
  // 配合计算缓存命中率
  ++total_requests_;

  const auto key = std::make_pair(sst_id, block_id);
  auto found = cache_map_.find(key);

  // 没缓存是正常情况，不抛异常
  // 返回 nullptr，让 SST::read_block() 继续从磁盘读取
  if (found == cache_map_.end())
    return nullptr;

  // 命中：记录命中次数
  ++hit_requests_;

  // 更新该块的访问次数，并调整它所在的链表和位置。
  // 调用时已经持有 mutex_，辅助函数内部不要重复加锁。
  update_access_count(found->second);

  // 复制 shared_ptr，让调用方与缓存共同持有 Block
  // 不使用 std::move，否则会把缓存节点中的指针移走
  return found->second->cache_block;
}

// TODO: Lab 4.8 插入一个 Block
void BlockCache::put(int sst_id, int block_id,
                     std::shared_ptr<Block> block_ptr) {
  // 哈希索引和链表必须同步更新，整个操作持有同一把锁
  std::lock_guard<std::mutex> lock(mutex_);

  // K 必须至少为 1
  // k_ 是无符号的 size_t，k_ < 0 永远不成立
  if (k_ == 0)
    throw std::runtime_error("BlockCache: k must be at least 1");

  // 空指针属于无效输入，明确通知调用方。
  if (!block_ptr)
    throw std::invalid_argument("BlockCache::put: block must not be null");

  // 容量为 0 表示不缓存；空指针也不占用缓存位置。
  if (capacity_ == 0)
    return;

  const auto key = std::make_pair(sst_id, block_id);
  auto found = cache_map_.find(key);

  // 1. 已存在：
  //      替换块指针，更新访问次数和链表位置
  //      重复回填是可能发生的，例如两个线程都曾查询未命中
  //      此分支不增加节点数量，因此不需要淘汰
  if (found != cache_map_.end()) {
    // 同一个 SST Block 的内容不变，保留已有缓存对象。
    // 本次重复回填仍算一次使用，更新访问次数和链表位置。
    update_access_count(found->second);
    return;
  }

  // 2. 新块：如果缓存已满，先淘汰一个旧节点
  //    cache_map_.size()：当前缓存了多少个 Block
  //    capacity_：最多允许缓存多少个 Block
  if (cache_map_.size() >= capacity_) {
    // 优先淘汰访问次数不足 K 的节点
    // 如果它们不存在，再从达到 K 次的链表中淘汰
    auto &list =
        cache_list_less_k.empty() ? cache_list_greater_k : cache_list_less_k;

    // 两个链表均把最近使用的节点放在头部
    // 因此被选中链表的尾部就是淘汰对象
    const auto &target = list.back();

    // 先利用节点中的 ID 删除哈希索引，再销毁链表节点
    cache_map_.erase(std::make_pair(target.sst_id, target.block_id));
    list.pop_back();
  }

  // 3. 首次插入算一次访问，与参考实现保持一致
  //    K == 1：已经达到阈值，进入 greater_k
  //    K > 1 ：尚未达到阈值，进入 less_k
  auto &target_list = (k_ == 1) ? cache_list_greater_k : cache_list_less_k;
  target_list.push_front(CacheItem{sst_id, block_id, std::move(block_ptr), 1});

  // 4. 保存节点位置，以后可直接通过哈希表定位，无须遍历链表。
  cache_map_.emplace(key, target_list.begin());
}

double BlockCache::hit_rate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_requests_ == 0
             ? 0.0
             : static_cast<double>(hit_requests_) / total_requests_;
}

// TODO: Lab 4.8 更新统计信息
void BlockCache::update_access_count(std::list<CacheItem>::iterator it) {
  // 调用方 get()/put() 已持有 mutex_，这里不能重复加锁
  // it 来自 cache_map_ 中已经找到的条目，指向有效节点

  if (it->access_count < k_)
    // 尚未达到 K 次：节点目前位于冷链表 less_k
    ++it->access_count;

  if (it->access_count == k_)
    // 把 it 指向的那个节点，从原位置 list_less_k 中的 it指向的节点摘下来，
    // 放到链表 list_greater_k 最前面
    cache_list_greater_k.splice(cache_list_greater_k.begin(), cache_list_less_k,
                                it);
  else
    // 已经位于热链表：只需要移动到热链表头部。
    // 次数封顶于 K，因为淘汰策略只关心是否达到 K
    // 无须继续累加，也避免了计数长期增长后溢出
    cache_list_greater_k.splice(cache_list_greater_k.begin(),
                                cache_list_greater_k, it);
}
} // namespace tiny_lsm