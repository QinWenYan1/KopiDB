// 2026-10-25：按用户要求，将依赖延期状态维护的测试移出当前测试集。
// 本文件不参与构建；完成状态维护后，按 README.md 恢复到 test_lsm.cpp。
// 原断言保持不变，移出不代表这些功能已通过验收。

// 目的：begin 返回后，惰性遍历的回调仍持有管理器；销毁迭代器后不形成引用环。
// 场景：L1 有两张 SST，遍历 a 后才进入 b；调用方清空管理器指针，仍须跳过 b@9。
TEST_F(LSMTest, EngineBeginKeepsManagerForLazySstReads) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = bind_test_manager(engine);
  manager->add_ready_to_flush_tranc_id(9, TransactionState::ABORTED);
  add_sst_for_point_query(engine, 1, 1, {{"a", "a3", 3}});
  add_sst_for_point_query(engine, 1, 2, {{"b", "", 9}, {"b", "b5", 5}});
  std::weak_ptr<TranManager> weak = manager;
  {
    auto it = engine->begin(0);
    manager.reset();
    EXPECT_FALSE(weak.expired());
    expect_mvcc_records(it, {{"a", "a3", 3}, {"b", "b5", 5}});
  }
  EXPECT_TRUE(weak.expired());
}

// 目的：公开 get 必须有有效管理器，不能把丢失的回滚名单当作空名单。
// 场景：未绑定时拒绝查询；绑定后过滤 SST 中的取消版本；管理器销毁后再次拒绝。
//       空库和缺失 key 也不能绕过依赖检查，正常绑定不要求先调用 begin_tran。
TEST_F(LSMTest, EngineGetRequiresLiveManager) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  EXPECT_THROW(engine->get("missing", 0), std::runtime_error);
  engine->put("disk", "old", 3);
  engine->put("disk", "cancelled", 5);
  engine->flush();
  engine->put("memory", "current", 7);
  for (const auto &key : {"disk", "memory", "missing"}) {
    EXPECT_THROW(engine->get(key, 0), std::runtime_error);
  }

  auto manager = bind_test_manager(engine);
  manager->add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  EXPECT_EQ(engine->get("disk", 0), std::make_optional(
                std::make_pair(std::string("old"), uint64_t{3})));
  EXPECT_EQ(engine->get("memory", 0), std::make_optional(
                std::make_pair(std::string("current"), uint64_t{7})));
  EXPECT_FALSE(engine->get("missing", 0));

  manager.reset();
  ASSERT_TRUE(engine->tran_manager.expired());
  for (const auto &key : {"disk", "memory", "missing"}) {
    EXPECT_THROW(engine->get(key, 0), std::runtime_error);
  }
}

// 目的：公开谓词查询需要有效管理器，并能区分空库、有效删除和全部取消的范围。
// 场景：k@9 已取消、k@6 为有效墓碑、k@3 为旧值；上限 0 无结果，上限 5 可见旧值。
TEST_F(LSMTest, EnginePredicateRequiresLiveManagerAndHandlesEmptyRanges) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto all = [](const std::string &) { return 0; };
  EXPECT_THROW((void)engine->lsm_iters_monotony_predicate(0, all), std::runtime_error);
  auto manager = bind_test_manager(engine);
  EXPECT_FALSE(engine->lsm_iters_monotony_predicate(0, all));
  engine->memtable.put("k", "old", 3);
  engine->memtable.put("k", "", 6);
  engine->memtable.put("k", "cancelled", 9);
  engine->memtable.put("x", "cancelled", 9);
  manager->add_ready_to_flush_tranc_id(9, TransactionState::ABORTED);
  EXPECT_FALSE(engine->lsm_iters_monotony_predicate(0, all));
  auto historical = engine->lsm_iters_monotony_predicate(5, all);
  ASSERT_TRUE(historical);
  expect_mvcc_records(historical->first, {{"k", "old", 3}});
  EXPECT_FALSE(engine->lsm_iters_monotony_predicate(
      0, [](const std::string &key) { return -key.compare("x"); }));
  EXPECT_FALSE(engine->lsm_iters_monotony_predicate(
      0, [](const std::string &key) { return -key.compare("missing"); }));
  manager.reset();
  EXPECT_THROW((void)engine->lsm_iters_monotony_predicate(0, all), std::runtime_error);
}

// 目的：前缀范围跨 MemTable/L0/L1 过滤回滚版本，并保留有效墓碑参与归并。
// 场景：p/a 的内存新值取消，p/b 的 L0 墓碑取消，p/c 的 L1 墓碑取消；
// p/k 的有效 L0 墓碑必须遮住 L1 旧值，取消的 p/z 之后的 q/d 不能越界进入结果。
TEST_F(LSMTest, EnginePredicateRollbackMergesBoundedRange) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = bind_test_manager(engine);
  manager->add_ready_to_flush_tranc_id(9, TransactionState::ABORTED);
  add_sst_for_point_query(engine, 0, 1, {{"p/b", "b5", 5}, {"p/k", "k5", 5}});
  add_sst_for_point_query(engine, 0, 2, {{"p/b", "", 9}, {"p/k", "", 7}});
  add_sst_for_point_query(engine, 1, 3,
      {{"p/c", "", 9}, {"p/c", "c4", 4}, {"p/k", "k3", 3},
       {"p/z", "cancelled", 9}, {"q/d", "outside", 4}});
  engine->memtable.put("", "", 9);
  engine->memtable.put("a-before", "outside", 3);
  engine->memtable.put("p/a", "a3", 3);
  engine->memtable.put("p/a", "cancelled", 9);
  engine->memtable.put("z-after", "outside", 3);
  for (uint64_t read_id : {0, 6, 10}) {
    SCOPED_TRACE(read_id);
    auto range = engine->lsm_iters_monotony_predicate(
        read_id, [](const std::string &key) { return -key.compare(0, 2, "p/"); });
    ASSERT_TRUE(range);
    std::vector<MvccRecord> expected = {
        {"p/a", "a3", 3}, {"p/b", "b5", 5}, {"p/c", "c4", 4}};
    if (read_id == 6) expected.emplace_back("p/k", "k5", 5);
    expect_mvcc_records(range->first, expected);
    EXPECT_TRUE(range->first == range->second);
    for (const auto &[key, value, version] : expected) {
      EXPECT_EQ(engine->get(key, read_id), std::make_optional(std::make_pair(value, version)));
    }
  }
}




// 目的：真实 abort 后，新增/修改/删除均不可见，有效的旧墓碑仍应阻止旧值复活。
// 场景：同一批记录先在活跃表查询，再冻结、刷入 SST；每一步都核对值和真实版本。
TEST_F(EngineGetRollbackTest, ValuesAndTombstonesStayFilteredAcrossFreezeAndFlush) {
  engine->put("updated", "old", 3);
  engine->put("restored", "keep", 3);
  engine->put("deleted", "ancient", 1);
  engine->remove("deleted", 3);
  TranContext context(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  context.put("updated", "first");
  context.put("updated", "last");
  context.remove("restored");
  context.put("added", "new");
  context.put("deleted", "cancelled-resurrection");
  ASSERT_TRUE(context.abort());

  for (int phase = 0; phase < 3; ++phase) {
    SCOPED_TRACE(phase); // 0：活跃表；1：冻结表；2：SST。
    if (phase == 1) {
      engine->memtable.frozen_cur_table();
      ASSERT_EQ(engine->memtable.get_cur_size(), 0u);
      ASSERT_GT(engine->memtable.get_frozen_size(), 0u);
    } else if (phase == 2) {
      engine->flush();
      ASSERT_EQ(engine->memtable.get_total_size(), 0u);
      ASSERT_FALSE(engine->ssts.empty());
    }
    for (uint64_t read_id : {uint64_t{0}, uint64_t{3}, uint64_t{5}}) {
      SCOPED_TRACE(read_id);
      EXPECT_EQ(engine->get("updated", read_id), std::make_optional(
                    std::make_pair(std::string("old"), uint64_t{3})));
      EXPECT_EQ(engine->get("restored", read_id), std::make_optional(
                    std::make_pair(std::string("keep"), uint64_t{3})));
      EXPECT_FALSE(engine->get("deleted", read_id));
      EXPECT_FALSE(engine->get("added", read_id));
      EXPECT_FALSE(engine->get("missing", read_id));
      // 同一状态从公开遍历入口读取，也应隐藏取消记录及有效墓碑。
      // 迭代器在本轮结束时销毁，下一阶段冻结/刷盘前已经释放 SST 读锁。
      auto it = engine->begin(read_id);
      expect_mvcc_records(it, {{"restored", "keep", 3}, {"updated", "old", 3}});
    }
    EXPECT_FALSE(engine->get("updated", 2));
    EXPECT_EQ(engine->get("deleted", 2), std::make_optional(
                  std::make_pair(std::string("ancient"), uint64_t{1})));
    // 上面的 Level 迭代器已销毁、读锁已释放，再独立验证谓词查询。
    for (uint64_t read_id : {uint64_t{0}, uint64_t{3}, uint64_t{5}}) {
      auto range = engine->lsm_iters_monotony_predicate(
          read_id, [](const std::string &) { return 0; });
      ASSERT_TRUE(range);
      expect_mvcc_records(range->first,
          {{"restored", "keep", 3}, {"updated", "old", 3}});
    }
  }
}

// 目的：回滚过滤不改变 compaction 的全版本保留，整理后仍能找到有效旧值。
// 场景：事务 5 的更新、删除、新增已刷盘后 abort，再通过正常 flush 触发 L0→L1。
// 核对点查、前缀查询及物理版本，避免仅查询正确却把历史记录悄悄丢弃。
TEST_F(EngineGetRollbackTest, RangeRollbackSurvivesActualCompaction) {
  engine->put("p/k", "old", 3);
  engine->put("p/revive", "keep", 3);
  engine->put("p/tomb", "ancient", 1);
  engine->remove("p/tomb", 3);
  engine->flush();
  TranContext context(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  context.put("p/k", "cancelled");
  context.remove("p/revive");
  context.put("p/new", "cancelled");
  context.put("p/tomb", "cancelled-resurrection");
  engine->flush();
  ASSERT_TRUE(context.abort());

  auto check_range = [&] {
    for (uint64_t read_id : {0, 3, 5}) {
      auto range = engine->lsm_iters_monotony_predicate(
          read_id, [](const std::string &key) { return -key.compare(0, 2, "p/"); });
      ASSERT_TRUE(range);
      expect_mvcc_records(range->first, {{"p/k", "old", 3}, {"p/revive", "keep", 3}});
      EXPECT_FALSE(engine->get("p/new", read_id));
      EXPECT_FALSE(engine->get("p/tomb", read_id));
      EXPECT_EQ(engine->get("p/k", read_id), std::make_optional(
          std::make_pair(std::string("old"), uint64_t{3})));
    }
  };
  check_range();
  const int ratio = TomlConfig::getInstance().getLsmSstLevelRatio();
  ASSERT_GE(ratio, 2);
  for (int i = 0; i < ratio; ++i) {
    engine->put("zz-filler-" + std::to_string(i), "v", 100 + i);
    engine->flush();
  }
  ASSERT_FALSE(engine->level_sst_ids[1].empty());
  check_range();

  std::vector<uint64_t> versions;
  for (const auto &[id, sst] : engine->ssts) {
    for (auto it = sst->begin(0, true); it.is_valid(); ++it) {
      if (it->first == "p/k") versions.push_back(it.get_cur_tranc_id());
    }
  }
  std::sort(versions.begin(), versions.end());
  EXPECT_EQ(versions, (std::vector<uint64_t>{3, 5}));
}

// 目的：内存与磁盘都过滤后，再按真实版本比较；同版本仍优先内存来源。
// 场景：取消 L0 的 11 和内存的 9，L1 的 7 胜过内存的 3；另检查两个 5
//       中内存的普通值/墓碑分别胜出，防止过滤接入改变原有来源优先级。
TEST_F(EngineGetRollbackTest, ComparesFilteredMemoryAndDiskCandidates) {
  add_sst_for_point_query(engine, 1, 10,
      {{"deleted", "", 7}, {"equal_deleted", "disk5", 5},
       {"equal_kept", "", 5}, {"k", "disk7", 7},
       {"restored", "disk7", 7}});
  TranContext disk_cancel(11, engine, manager,
                          IsolationLevel::READ_UNOP_COMMITTED);
  for (const auto &key : {"deleted", "equal_deleted", "equal_kept", "k",
                          "restored"}) {
    disk_cancel.put(key, "cancelled-disk");
  }
  engine->flush();
  engine->put("k", "memory3", 3);
  engine->put("deleted", "memory3", 3);
  engine->remove("restored", 3);
  engine->put("equal_kept", "memory5", 5);
  engine->remove("equal_deleted", 5);
  TranContext memory_cancel(9, engine, manager,
                            IsolationLevel::READ_UNOP_COMMITTED);
  for (const auto &key : {"deleted", "equal_deleted", "equal_kept", "k"}) {
    memory_cancel.put(key, "cancelled-memory");
  }
  memory_cancel.remove("restored");
  ASSERT_TRUE(disk_cancel.abort());
  ASSERT_TRUE(memory_cancel.abort());

  for (uint64_t read_id : {uint64_t{0}, uint64_t{7}, uint64_t{11}}) {
    SCOPED_TRACE(read_id);
    for (const auto &key : {"k", "restored"}) {
      EXPECT_EQ(engine->get(key, read_id), std::make_optional(
                    std::make_pair(std::string("disk7"), uint64_t{7})));
    }
    EXPECT_FALSE(engine->get("deleted", read_id));
    EXPECT_FALSE(engine->get("equal_deleted", read_id));
    EXPECT_EQ(engine->get("equal_kept", read_id), std::make_optional(
                  std::make_pair(std::string("memory5"), uint64_t{5})));
    // 批量入口也要遵守相同的跨来源版本和同版本来源优先级。
    const std::vector<std::string> keys = {
        "equal_kept", "deleted", "k", "equal_deleted", "restored", "k"};
    const auto results = engine->get_batch(keys, read_id);
    ASSERT_EQ(results.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
      EXPECT_EQ(results[i].first, keys[i]);
      EXPECT_EQ(results[i].second, engine->get(keys[i], read_id));
    }
    // 公开 begin 也要同时过滤内存和 L0 的取消版本，再与 L1 比较。
    auto it = engine->begin(read_id);
    expect_mvcc_records(it, {{"equal_kept", "memory5", 5},
                             {"k", "disk7", 7}, {"restored", "disk7", 7}});
  }
  EXPECT_EQ(engine->get("k", 6), std::make_optional(
                std::make_pair(std::string("memory3"), uint64_t{3})));
  EXPECT_FALSE(engine->get("k", 2));
}

// 目的：每次独立查询都重新检查回滚名单，连续取消后能继续向旧版本查找。
// 场景：SST 中 k@3、冻结表中 k@5、活跃表中 k@7；依次取消 7 和 5，
//       最新结果应按 7→5→3 变化，历史查询也必须跳过已经取消的版本。
TEST_F(EngineGetRollbackTest, RechecksCancellationOnEachQuery) {
  engine->put("k", "base", 3);
  engine->flush();
  TranContext first(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  first.put("k", "first");
  engine->memtable.frozen_cur_table();
  TranContext second(7, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  second.put("k", "second");
  EXPECT_EQ(engine->get("k", 0), std::make_optional(
                std::make_pair(std::string("second"), uint64_t{7})));

  ASSERT_TRUE(second.abort());
  EXPECT_EQ(engine->get("k", 0), std::make_optional(
                std::make_pair(std::string("first"), uint64_t{5})));
  ASSERT_TRUE(first.abort());
  for (uint64_t read_id : {uint64_t{0}, uint64_t{5}, uint64_t{7}}) {
    EXPECT_EQ(engine->get("k", read_id), std::make_optional(
                  std::make_pair(std::string("base"), uint64_t{3})));
  }
  EXPECT_FALSE(engine->get("k", 2));
  // 取消只影响可见性，未传回调的底层查询仍能看到物理版本 7。
  EXPECT_EQ(engine->memtable.get_record("k", 0), std::make_optional(
                std::make_pair(std::string("second"), uint64_t{7})));
}

// 目的：批量查询过滤取消值/墓碑后返回有效旧版本，并保持顺序、重复 key 和缺失项。
// 场景：有效旧版本位于 SST，内存中事务 5 和 7 连续被取消；随后冻结、刷盘，
//       各阶段的批量结果都应符合明确预期，并与逐条 get 一致。
TEST_F(EngineGetRollbackTest, BatchKeepsOrderDuplicatesAndHistoricalVersions) {
  engine->put("k", "old", 3);
  engine->put("restored", "keep", 3);
  engine->put("deleted", "ancient", 1);
  engine->remove("deleted", 3);
  engine->flush();

  TranContext first(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  first.put("k", "cancelled5");
  first.remove("restored");
  first.put("deleted", "cancelled-resurrection");
  first.put("added", "cancelled-new");
  ASSERT_TRUE(first.abort());
  TranContext second(7, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  second.put("k", "cancelled7");
  ASSERT_TRUE(second.abort());

  const std::vector<std::string> keys = {
      "restored", "k", "added", "deleted", "missing", "k"};
  for (int phase = 0; phase < 3; ++phase) {
    SCOPED_TRACE(phase); // 0：取消记录在活跃表；1：冻结表；2：SST。
    if (phase == 1) {
      engine->memtable.frozen_cur_table();
      ASSERT_EQ(engine->memtable.get_cur_size(), 0u);
      ASSERT_GT(engine->memtable.get_frozen_size(), 0u);
    } else if (phase == 2) {
      engine->flush();
      ASSERT_EQ(engine->memtable.get_total_size(), 0u);
    }
    for (uint64_t read_id : {uint64_t{0}, uint64_t{2}, uint64_t{5}, uint64_t{7}}) {
      SCOPED_TRACE(read_id);
      auto results = engine->get_batch(keys, read_id);
      ASSERT_EQ(results.size(), keys.size());
      for (size_t i = 0; i < keys.size(); ++i) {
        SCOPED_TRACE(i);
        std::optional<std::pair<std::string, uint64_t>> expected;
        if (read_id == 2) {
          if (keys[i] == "deleted") {
            expected = std::make_pair(std::string("ancient"), uint64_t{1});
          }
        } else if (keys[i] == "k") {
          expected = std::make_pair(std::string("old"), uint64_t{3});
        } else if (keys[i] == "restored") {
          expected = std::make_pair(std::string("keep"), uint64_t{3});
        }
        EXPECT_EQ(results[i].first, keys[i]);
        EXPECT_EQ(results[i].second, expected);
        EXPECT_EQ(results[i].second, engine->get(keys[i], read_id));
      }
    }
  }
}

// 目的：暂存型事务主动取消后，不把修改写入数据库，也不能再提交。
// 场景：修改和删除的原值保持不变，新增 key 不出现；同时检查取消登记、
//       暂存清理、重复取消以及结束后的 put/remove/get/commit。
TEST_F(TranAbortTest, CancelsStagedWritesWithoutChangingDatabase) {
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto existing = "existing-" + suffix;
    const auto added = "added-" + suffix;
    const auto removed = "removed-" + suffix;
    engine->put(existing, "old", 3);
    engine->put(removed, "keep", 3);
    TranContext context(10 + static_cast<int>(level), engine, manager, level);
    EXPECT_EQ(context.get(existing), std::make_optional(std::string("old")));
    context.put(existing, "pending");
    context.put(added, "pending");
    context.remove(removed);
    ASSERT_TRUE(context.abort());
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(context.isCommited);
    EXPECT_TRUE(manager->is_aborted(context.tranc_id_));
    EXPECT_TRUE(context.temp_map_.empty());
    EXPECT_TRUE(context.operations.empty());
    EXPECT_EQ(engine->get(existing, 0),
              std::make_optional(std::make_pair(std::string("old"), uint64_t{3})));
    EXPECT_EQ(engine->get(removed, 0),
              std::make_optional(std::make_pair(std::string("keep"), uint64_t{3})));
    EXPECT_FALSE(engine->get(added, 0));
    EXPECT_FALSE(context.abort());
    EXPECT_THROW(context.put(added, "too-late"), std::logic_error);
    EXPECT_THROW(context.remove(existing), std::logic_error);
    EXPECT_THROW(context.get(existing), std::logic_error);
    EXPECT_FALSE(context.commit());
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(context.isCommited);
    EXPECT_TRUE(context.temp_map_.empty());
    EXPECT_TRUE(context.operations.empty());
  }
}

// 目的：没有执行过写入的事务也能正常取消，不能要求必须先产生数据记录。
// 场景：四种隔离级别分别创建空事务，首次取消成功，再次取消返回 false。
TEST_F(TranAbortTest, CancelsEmptyTransactionsOnlyOnce) {
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    TranContext context(10 + static_cast<int>(level), engine, manager, level);
    ASSERT_TRUE(context.abort());
    EXPECT_TRUE(manager->is_aborted(context.tranc_id_));
    EXPECT_TRUE(context.isAborted);
    EXPECT_FALSE(context.isCommited);
    EXPECT_TRUE(context.temp_map_.empty());
    EXPECT_TRUE(context.operations.empty());
    EXPECT_FALSE(context.abort());
    EXPECT_FALSE(context.commit());
  }
}

// 目的：读未提交的取消只登记 ID，不通过改写原节点或补写墓碑来模拟撤销。
// 场景：修改、新增、删除后保留原节点的迭代器，取消后检查物理记录和内存大小不变。
//       这里检查原节点，不用用户查询结果判断回滚；查询过滤由后续阶段验收。
TEST_F(TranAbortTest, ReadUncommittedRegistersWithoutRewritingRecords) {
  engine->put("existing", "old", 3);
  engine->put("removed", "keep", 3);
  TranContext context(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  context.put("existing", "first");
  context.put("existing", "last");
  context.put("added", "new");
  context.remove("removed");

  auto updated = engine->memtable.get("existing", 0);
  auto original = engine->memtable.get("existing", 3);
  auto added = engine->memtable.get("added", 0);
  auto removed = engine->memtable.get("removed", 0);
  ASSERT_TRUE(updated.is_valid());
  ASSERT_TRUE(original.is_valid());
  ASSERT_TRUE(added.is_valid());
  ASSERT_TRUE(removed.is_valid());
  const auto bytes = engine->memtable.get_total_size();

  ASSERT_TRUE(context.abort());
  EXPECT_TRUE(context.isAborted);
  EXPECT_FALSE(context.isCommited);
  EXPECT_TRUE(manager->is_aborted(5));
  EXPECT_FALSE(manager->is_aborted(3));
  EXPECT_TRUE(context.operations.empty());
  EXPECT_EQ(engine->memtable.get_total_size(), bytes);
  EXPECT_EQ(updated.get_value(), "last");
  EXPECT_EQ(updated.get_cur_tranc_id(), 5u);
  EXPECT_EQ(original.get_value(), "old");
  EXPECT_EQ(original.get_cur_tranc_id(), 3u);
  EXPECT_EQ(added.get_value(), "new");
  EXPECT_EQ(added.get_cur_tranc_id(), 5u);
  EXPECT_EQ(removed.get_value(), "");
  EXPECT_EQ(removed.get_cur_tranc_id(), 5u);
}

// 目的：撤销读未提交事务的更新或删除后，应重新读到原记录及其真实版本号。
// 场景：k@3=old，事务写入 k@10=new 或墓碑，再 abort；分别覆盖内存与 SST。
//       只重写 k@3 无法遮住 k@10；写一个 k@10 的墓碑也会错误遮住原值。
TEST_F(TranAbortTest, ReadUncommittedRestoresPreviousVersionInMemoryAndSst) {
  uint64_t id = 10;
  for (bool flushed : {false, true}) {
    for (bool deleted : {false, true}) {
      SCOPED_TRACE(flushed);
      SCOPED_TRACE(deleted);
      const auto key = "k-" + std::to_string(id);
      engine->put(key, "old", 3);
      TranContext context(id, engine, manager,
                          IsolationLevel::READ_UNOP_COMMITTED);
      if (deleted) {
        context.remove(key);
      } else {
        context.put(key, "first");
        context.put(key, "last");
      }
      if (flushed)
        engine->flush();

      ASSERT_TRUE(context.abort());
      const auto original =
          std::make_optional(std::make_pair(std::string("old"), uint64_t{3}));
      EXPECT_EQ(engine->get(key, 0), original);
      EXPECT_EQ(engine->get(key, id), original);
      EXPECT_EQ(engine->get(key, 3), original);
      ++id;
    }
  }
}

// 目的：取消事务 5 不能改掉事务 6 的提交结果，也不能让旧快照继续看到已取消的 5。
// 场景：k@3=old -> 未提交 k@5=dirty -> 已提交 k@6=newest -> abort(5)。
//       最新查询应得到 6，读上限 5 应退回 3；只检查最新值会漏掉后一种错误。
TEST_F(TranAbortTest, KeepsNewerCommitButHidesAbortedVersionFromOlderSnapshot) {
  engine->put("k", "old", 3);
  TranContext older(5, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
  older.put("k", "dirty");
  engine->flush();
  TranContext newer(6, engine, manager, IsolationLevel::READ_OP_COMMITTED);
  newer.put("k", "newest");
  ASSERT_TRUE(newer.commit());

  ASSERT_TRUE(older.abort());
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("newest"), uint64_t{6})));
  EXPECT_EQ(engine->get("k", 5),
            std::make_optional(std::make_pair(std::string("old"), uint64_t{3})));
}

// 目的：正常提交应用最后一次暂存值、墓碑和事务标记，重复提交没有副作用。
// 场景：分别走四种隔离级别；RU 已提前写入，其余级别在 commit 前不可见。
TEST_F(TranCommitTest, AppliesWritesAndEndsOnlyOnce) {
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto key = "written-" + suffix;
    const auto removed = "deleted-" + suffix;
    const uint64_t id = 10 + static_cast<int>(level);
    SCOPED_TRACE(suffix);
    engine->put(removed, "old", 3);
    TranContext context(id, engine, manager, level);
    context.put(key, "first");
    context.put(key, "last");
    context.put(removed, "");
    if (level != IsolationLevel::READ_UNOP_COMMITTED) {
      EXPECT_FALSE(engine->get(key, 0));
      ASSERT_TRUE(engine->get(removed, 0));
    }
    ASSERT_TRUE(context.commit());
    EXPECT_TRUE(context.isCommited);
    EXPECT_FALSE(context.isAborted);
    EXPECT_EQ(engine->get(key, 0),
              std::make_optional(std::make_pair(std::string("last"), id)));
    EXPECT_FALSE(engine->get(removed, 0));
    EXPECT_EQ(engine->memtable.get_record(removed, 0),
              std::make_optional(std::make_pair(std::string(""), id)));
    // 事务标记不会作为普通点查询结果返回；用刷盘通知验证它确实存在。
    engine->flush();
    EXPECT_TRUE(manager->get_flushed_tranc_ids().contains(id));
    ASSERT_EQ(context.operations.back().getOperationType(), OperationType::OP_COMMIT);
    const auto operation_count = context.operations.size();
    EXPECT_FALSE(context.commit());
    EXPECT_EQ(context.operations.size(), operation_count);
    EXPECT_THROW(context.put("after-end", "value"), std::logic_error);
    EXPECT_THROW(context.get(key), std::logic_error);
  }
}

// 目的：已取消的较新值或墓碑不能阻止旧事务提交；两路内部查询均应过滤。
// 场景：有效 k@3，旧事务暂存写 k，随后较新 RU 事务写值/墓碑并取消；
// 取消版本分别在活跃表、冻结表和 SST，覆盖三个暂存型隔离级别。
TEST_F(TranCommitTest, IgnoresAbortedNewerVersionsAcrossStorage) {
  uint64_t id = 10;
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ, IsolationLevel::SERIALIZABLE}) {
    for (int storage = 0; storage < 3; ++storage) {
      for (bool tombstone : {false, true}) {
        SCOPED_TRACE(static_cast<int>(level));
        SCOPED_TRACE(storage);
        SCOPED_TRACE(tombstone);
        const auto key = "commit-cancelled-" + std::to_string(id);
        engine->put(key, "old", 3);
        TranContext older(id, engine, manager, level);
        older.put(key, "accepted");
        TranContext newer(id + 1, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
        if (tombstone) newer.remove(key);
        else newer.put(key, "cancelled");
        if (storage == 1) engine->memtable.frozen_cur_table();
        if (storage == 2) {
          while (engine->memtable.get_total_size() != 0) engine->flush();
        }
        ASSERT_TRUE(newer.abort());
        ASSERT_TRUE(older.commit());
        EXPECT_TRUE(older.isCommited);
        EXPECT_FALSE(manager->is_aborted(id));
        EXPECT_EQ(engine->get(key, 0),
                  std::make_optional(std::make_pair(std::string("accepted"), id)));
        id += 2;
      }
    }
  }
}

// 目的：跳过取消版本后，仍须发现下面的有效新版本，不能误放行或部分应用写入。
// 场景：旧事务 id 暂存 k 和另一 key；id+1 已提交值/墓碑，id+2 的遮盖版本被取消。
TEST_F(TranCommitTest, StillRejectsValidConflictBelowAbortedVersion) {
  uint64_t id = 10;
  for (bool disk : {false, true}) {
    for (bool tombstone : {false, true}) {
      SCOPED_TRACE(disk);
      SCOPED_TRACE(tombstone);
      const auto key = "conflict-" + std::to_string(id);
      const auto other = "unwritten-" + std::to_string(id);
      TranContext older(id, engine, manager, IsolationLevel::REPEATABLE_READ);
      older.put(key, "must-not-apply");
      older.put(other, "must-not-apply");
      TranContext committed(id + 1, engine, manager, IsolationLevel::READ_OP_COMMITTED);
      if (tombstone) committed.remove(key);
      else committed.put(key, "committed");
      ASSERT_TRUE(committed.commit());
      TranContext cancelled(id + 2, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
      cancelled.put(key, "cancelled");
      if (disk) engine->flush();
      ASSERT_TRUE(cancelled.abort());
      EXPECT_FALSE(older.commit());
      EXPECT_TRUE(manager->is_aborted(id));
      EXPECT_FALSE(engine->get(other, 0));
      if (tombstone) EXPECT_FALSE(engine->get(key, 0));
      else EXPECT_EQ(engine->get(key, 0), std::make_optional(
          std::make_pair(std::string("committed"), id + 1)));
      id += 3;
    }
  }
}

// 目的：SERIALIZABLE 的读集校验也使用过滤后的版本，取消的更新不造成虚假冲突。
// 场景：先缓存有效旧值和不存在；后来 RU 更新/删除旧值并新增缺失 key，再刷盘取消。
TEST_F(TranCommitTest, SerialReadValidationIgnoresAbortedChanges) {
  uint64_t id = 10;
  for (bool tombstone : {false, true}) {
    const auto key = "read-" + std::to_string(id);
    const auto missing = "missing-" + std::to_string(id);
    const auto written = "written-" + std::to_string(id);
    engine->put(key, "old", 3);
    TranContext reader(id, engine, manager, IsolationLevel::SERIALIZABLE);
    ASSERT_EQ(reader.get(key), std::make_optional(std::string("old")));
    ASSERT_FALSE(reader.get(missing));
    TranContext cancelled(id + 1, engine, manager, IsolationLevel::READ_UNOP_COMMITTED);
    if (tombstone) cancelled.remove(key);
    else cancelled.put(key, "cancelled");
    cancelled.put(missing, "cancelled");
    engine->flush();
    ASSERT_TRUE(cancelled.abort());
    reader.put(written, "accepted");
    ASSERT_TRUE(reader.commit());
    EXPECT_EQ(engine->get(written, 0),
              std::make_optional(std::make_pair(std::string("accepted"), id)));
    id += 2;
  }
}

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


// 目的：0 容量阈值触发刷盘后可以正常退出，避免对空 MemTable 无限循环。
TEST_F(TranCommitTest, ZeroLimitsStopAfterMemoryBecomesEmpty) {
  config->modify_lsm_per_mem_size_limit(0);
  config->modify_lsm_tol_mem_size_limit(0);
  TranContext context(5, engine, manager, IsolationLevel::REPEATABLE_READ);
  context.put("k", "v");
  ASSERT_TRUE(context.commit());
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  EXPECT_TRUE(manager->get_flushed_tranc_ids().contains(5));
  EXPECT_EQ(engine->get("k", 0),
            std::make_optional(std::make_pair(std::string("v"), uint64_t{5})));
}
