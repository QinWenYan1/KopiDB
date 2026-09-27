#include "block/block_cache.h"
#include "block/block.h"
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

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
    found->second->cache_block = std::move(block_ptr); 
    update_access_count(found->second); 
    return; 
  }


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