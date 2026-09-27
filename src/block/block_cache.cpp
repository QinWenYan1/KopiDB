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
  
  return nullptr;
}

// TODO: Lab 4.8 插入一个 Block
void BlockCache::put(int sst_id, int block_id, std::shared_ptr<Block> block_ptr) {
  // K 必须至少为 1
  if(k_ < 0)
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
  if (found != cache_map_.end()){
    // 同一个 SST Block 的内容不变，保留已有缓存对象。
    // 本次重复回填仍算一次使用，更新访问次数和链表位置。
    update_access_count(found->second); 
    return; 
  }

  // 2. 新块：如果缓存已满，先淘汰一个旧节点
  //    cache_map_.size()：当前缓存了多少个 Block
  //    capacity_：最多允许缓存多少个 Block
  if (cache_map_.size() >= capacity_){
    // 优先淘汰访问次数不足 K 的节点
    // 如果它们不存在，再从达到 K 次的链表中淘汰
    auto &list = cache_list_less_k.empty()
                          ? cache_list_greater_k 
                          : cache_list_less_k;

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
  auto &target_list = (k_ == 1)
                              ? cache_list_greater_k
                              : cache_list_less_k; 
  target_list.push_front(
    CacheItem{sst_id, block_id, std::move(block_ptr), 1}
  );

  // 4. 保存节点位置，以后可直接通过哈希表定位，无须遍历链表。
  cache_map_.emplace(key, target_list.begin()); 
}

double BlockCache::hit_rate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_requests_ == 0
             ? 0.0
             : static_cast<double>(hit_requests_) / total_requests_;
}

void BlockCache::update_access_count(std::list<CacheItem>::iterator it) {
  // TODO: Lab 4.8 更新统计信息
}
} // namespace tiny_lsm