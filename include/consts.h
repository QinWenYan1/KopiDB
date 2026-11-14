#pragma once

// 下述配置参数已经迁移至 config.toml, 本头文件已被标记为 deprecated

namespace tiny_lsm {

#define LSM_TOL_MEM_SIZE_LIMIT (64 * 1024 * 1024) // 内存表的大小限制, 64MB
#define LSM_PER_MEM_SIZE_LIMIT (4 * 1024 * 1024)  // 内存表的大小限制, 4MB
#define LSM_BLOCK_SIZE (32 * 1024)                // BLOCK的大小, 32KB

#define LSM_SST_LEVEL_RATIO 4 // 不同层级的sst的大小比例

// 测试时使用的小批量数据, 测试时可以注释上面的定义
// #define LSM_TOL_MEM_SIZE_LIMIT (4  * 1024) // 内存表的大小限制, 4kb
// #define LSM_PER_MEM_SIZE_LIMIT (1 * 1024) // 内存表的大小限制, 1KB
// #define LSM_BLOCK_SIZE (256)               // BLOCK的大小, 1KB

#define LSMmm_BLOCK_CACHE_CAPACITY 1024 // 缓存池的块缓存容量
#define LSMmm_BLOCK_CACHE_K 8           // 缓存池的LRU-K的K值

// Bloom Filter
#define BLOOM_FILTER_EXPECTED_SIZE 65536
#define BLOOM_FILTER_EXPECTED_ERROR_RATE 0.1
} // namespace tiny_lsm