#include "block/block.h"
#include "block/block_cache.h"
#include "logger/logger.h"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace ::tiny_lsm;

class BlockCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 初始化缓存池，容量为3，K值为2
    cache = std::make_unique<BlockCache>(3, 2);
  }

  std::unique_ptr<BlockCache> cache;
};

TEST_F(BlockCacheTest, PutAndGet) {
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();
  auto block3 = std::make_shared<Block>();

  cache->put(1, 1, block1);
  cache->put(1, 2, block2);
  cache->put(1, 3, block3);

  EXPECT_EQ(cache->get(1, 1), block1);
  EXPECT_EQ(cache->get(1, 2), block2);
  EXPECT_EQ(cache->get(1, 3), block3);
}

TEST_F(BlockCacheTest, CacheEviction1) {
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();
  auto block3 = std::make_shared<Block>();
  auto block4 = std::make_shared<Block>();

  cache->put(1, 1, block1);
  cache->put(1, 2, block2);
  cache->put(1, 3, block3);

  // 访问 block1 和 block2
  cache->get(1, 1);
  cache->get(1, 2);

  // 插入 block4，应该驱逐 block3
  cache->put(1, 4, block4);

  EXPECT_EQ(cache->get(1, 1), block1);
  EXPECT_EQ(cache->get(1, 2), block2);
  EXPECT_EQ(cache->get(1, 3), nullptr); // block3 被驱逐
  EXPECT_EQ(cache->get(1, 4), block4);
}

TEST_F(BlockCacheTest, CacheEviction2) {
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();
  auto block3 = std::make_shared<Block>();
  auto block4 = std::make_shared<Block>();

  cache->put(1, 1, block1);
  cache->put(1, 2, block2);
  cache->put(1, 3, block3);

  // 访问 block1 和 block2
  cache->get(1, 1);
  cache->get(1, 2);
  cache->get(1, 3);

  // 插入 block4，应该驱逐 block3
  cache->put(1, 4, block4);

  EXPECT_EQ(cache->get(1, 1), nullptr); // block1 被驱逐
  EXPECT_EQ(cache->get(1, 2), block2);
  EXPECT_EQ(cache->get(1, 3), block3);
  EXPECT_EQ(cache->get(1, 4), block4);
}

TEST_F(BlockCacheTest, HotReaccessPreservesColdEviction) {
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();
  auto block3 = std::make_shared<Block>();
  auto block4 = std::make_shared<Block>();

  cache->put(1, 1, block1);
  cache->put(1, 2, block2);
  cache->put(1, 3, block3);

  // K = 2：block1 和 block2 晋升到热链表，block3 留在冷链表。
  ASSERT_EQ(cache->get(1, 1), block1);
  ASSERT_EQ(cache->get(1, 2), block2);

  // 热链表当前顺序为 block2、block1。
  // 再次访问非头部的热节点，只应调整热链表顺序，不能改变冷链表。
  ASSERT_EQ(cache->get(1, 1), block1);

  // 缓存已满：必须优先淘汰冷节点 block3，保留两个热节点。
  cache->put(1, 4, block4);
  ASSERT_EQ(cache->get(1, 2), block2);
  EXPECT_EQ(cache->get(1, 3), nullptr);
  EXPECT_EQ(cache->get(1, 1), block1);
  EXPECT_EQ(cache->get(1, 4), block4);
}

TEST_F(BlockCacheTest, HotReaccessUpdatesEvictionOrder) {
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();
  auto block3 = std::make_shared<Block>();
  auto block4 = std::make_shared<Block>();

  cache->put(1, 1, block1);
  cache->put(1, 2, block2);
  cache->put(1, 3, block3);

  // 全部晋升到热链表：从最近使用到最久未使用依次为 3、2、1。
  ASSERT_EQ(cache->get(1, 1), block1);
  ASSERT_EQ(cache->get(1, 2), block2);
  ASSERT_EQ(cache->get(1, 3), block3);

  // 再次访问 block1 后，顺序应变为 1、3、2。
  ASSERT_EQ(cache->get(1, 1), block1);

  // 没有冷节点可淘汰：应淘汰热链表尾部的 block2，保留 block1。
  cache->put(1, 4, block4);
  EXPECT_EQ(cache->get(1, 2), nullptr);
  EXPECT_EQ(cache->get(1, 1), block1);
  EXPECT_EQ(cache->get(1, 3), block3);
  EXPECT_EQ(cache->get(1, 4), block4);
}

TEST_F(BlockCacheTest, ColdReaccessBelowKPreservesHotBlocks) {
  // K = 3 才能覆盖“再次访问后仍不足 K 次”的冷链表分支。
  cache = std::make_unique<BlockCache>(3, 3);
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();
  auto block3 = std::make_shared<Block>();
  auto block4 = std::make_shared<Block>();

  // block1 达到三次访问，成为热节点。
  cache->put(1, 1, block1);
  ASSERT_EQ(cache->get(1, 1), block1);
  ASSERT_EQ(cache->get(1, 1), block1);

  cache->put(1, 2, block2);
  cache->put(1, 3, block3);

  // 两个冷节点分别从一次访问变成两次，仍应留在冷链表。
  // 依次访问非头部节点，最终冷链表从新到旧为 block3、block2。
  ASSERT_EQ(cache->get(1, 2), block2);
  ASSERT_EQ(cache->get(1, 3), block3);

  // 应淘汰最久未使用的冷节点 block2，不能误淘汰热节点 block1。
  cache->put(1, 4, block4);
  ASSERT_EQ(cache->get(1, 1), block1);
  EXPECT_EQ(cache->get(1, 2), nullptr);
  EXPECT_EQ(cache->get(1, 3), block3);
  EXPECT_EQ(cache->get(1, 4), block4);
}

TEST_F(BlockCacheTest, HitRate) {
  auto block1 = std::make_shared<Block>();
  auto block2 = std::make_shared<Block>();

  cache->put(1, 1, block1);
  cache->put(1, 2, block2);

  // 访问 block1 和 block2
  cache->get(1, 1);
  cache->get(1, 2);

  // 访问不存在的 block3
  cache->get(1, 3);

  EXPECT_EQ(cache->hit_rate(), 2.0 / 3.0);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
