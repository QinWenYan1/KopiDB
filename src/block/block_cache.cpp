#include "block/block_cache.h"
#include "block/block.h"
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace tiny_lsm {
BlockCache::BlockCache(size_t capacity, size_t k)
    : capacity_(capacity), k_(k) {}

BlockCache::~BlockCache() = default;

std::shared_ptr<Block> BlockCache::get(int sst_id, int block_id) {
  // TODO: Lab 4.8 查询一个 Block
  return nullptr;
}

void BlockCache::put(int sst_id, int block_id, std::shared_ptr<Block> block) {
  // TODO: Lab 4.8 插入一个 Block
}

double BlockCache::hit_rate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_requests_ == 0
             ? 0.0
             : static_cast<double>(hit_requests_) / total_requests_;
}

void BlockCache::update_access_count(std::list<CacheItem>::iterator it) {
  ++it->access_count;
  if (it->access_count < k_) {
    // 更新后仍然位于cache_list_less_k
    // 重新置于cache_list_less_k头部
    cache_list_less_k.splice(cache_list_less_k.begin(), cache_list_less_k, it);
  } else if (it->access_count == k_) {
    // 更新后满足k次访问, 升级链表
    // 从 cache_list_less_k 移动到 cache_list_greater_k 头部
    auto item = *it;
    cache_list_less_k.erase(it);
    cache_list_greater_k.push_front(item);
    cache_map_[std::make_pair(item.sst_id, item.block_id)] =
        cache_list_greater_k.begin();
  } else if (it->access_count > k_) {
    // 本来就位于 cache_list_greater_k
    // 移动到 cache_list_greater_k 头部
    cache_list_greater_k.splice(cache_list_greater_k.begin(),
                                cache_list_greater_k, it);
  }
}
} // namespace tiny_lsm