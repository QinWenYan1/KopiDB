# 事务状态维护阶段恢复的测试

2026-10-25，用户明确要求：结束登记、刷盘登记、活跃状态及混合隔离留到下一任务，依赖它们的测试先从当前测试集移出，完成实现后统一测试。

最初从 `test/test_lsm.cpp` 移出 **24 个用例**。2026-10-26 阶段①开始后，其中 **2 个**已原样移回 `test/test_lsm.cpp`：`TransactionEndRegistrationTracksOnlyAbortedIds`、`TransactionEndRegistrationSupportsConcurrentQueries`；恢复时核对注释和函数体的 SHA-256 与清单一致。

剩余 **22 个用例**保存在 [transaction_state_tests.inc](transaction_state_tests.inc)，该文件仍未接入 `test_lsm`。原位置、名称、内容校验值及已恢复项位置见 [transaction_state_tests.json](transaction_state_tests.json)。恢复编译不代表验收通过，实际结果见 STATE。

这些用例覆盖：

- 结束/回滚登记、并发查询、刷盘登记及登记清理。
- 真实 abort 后的点查、批查、范围查询、compaction 和提交冲突检查。
- 混合隔离下的首次读取及读缓存。

移出前选跑了 29 个相关用例：以上 24 个失败，另外 5 个通过。通过的用例保留在当前测试集中。**延期不是测试通过，也不是功能已经修复。** 业务实现未在本次移动中修改；完整排期见 [STATE.md](../../agent/STATE.md)。

## 恢复方式

相关 fixture 和辅助函数保留在 `test_lsm.cpp`。后续按当前任务范围逐项原样移回用例，并从 `.inc` 移除对应定义、更新清单的 `restored_to` 字段，避免重复定义。混合隔离用例仍单独延期。

只有剩余用例所依赖的功能全部接通后，才可以在该文件的 `main` 函数之前整体加入：

```cpp
#include "deferred/transaction_state_tests.inc"
```

然后重新构建 `test_lsm`，在独立临时目录运行这些用例及相关回归。也可以把用例移回原文件；两种方式只选一种，避免重复定义。恢复时保持原断言，报告实际通过/失败结果。
