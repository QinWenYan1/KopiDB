// 2026-10-25：按用户要求，将依赖延期状态维护的测试移出当前测试集。
// 本文件不参与构建；完成状态维护后，按 README.md 恢复到 test_lsm.cpp。
// 原断言保持不变，移出不代表这些功能已通过验收。

// 目的：RC/RR/SERIALIZABLE 不应读取其他 RU 事务尚未提交的值或墓碑。
// 场景：通过 new_tranc 创建全部事务，先提交 old，再启动 RU 写入和较晚的读事务；
// 在 RU 取消前后都应读到 old。该测试同时捕获缓存脏值和缓存错误“不存在”的问题。
TEST_F(TranReadVisibilityTest, RejectsDirtyValuesAndTombstonesBeforeCaching) {
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ, IsolationLevel::SERIALIZABLE}) {
    for (bool tombstone : {false, true}) {
      SCOPED_TRACE(static_cast<int>(level));
      SCOPED_TRACE(tombstone);
      const auto key = "visibility-" + std::to_string(static_cast<int>(level)) +
                       (tombstone ? "-deleted" : "-updated");
      auto base = manager->new_tranc(IsolationLevel::READ_OP_COMMITTED);
      base->put(key, "old");
      ASSERT_TRUE(base->commit());
      auto writer = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
      if (tombstone) writer->remove(key);
      else writer->put(key, "dirty");
      auto reader = manager->new_tranc(level);
      EXPECT_EQ(reader->get(key), std::make_optional(std::string("old")))
          << "First read must exclude the other transaction's uncommitted record";
      ASSERT_TRUE(writer->abort());
      EXPECT_EQ(reader->get(key), std::make_optional(std::string("old")))
          << "A cached dirty value or false absence must not survive abort";
      ASSERT_TRUE(reader->abort());
    }
  }
}
