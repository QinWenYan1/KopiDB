# KopiDB 当前计划

最后更新：2026-11-17。

## A6：结束尝试失败后拒绝继续追加写操作

目标：真实 commit/abort 尝试已经在 operations 末尾留下 COMMIT/ROLLBACK、但尚未完成时，`TranContext::put/remove` 必须在修改上下文或发送WAL之前以logic_error拒绝新写入。正常事务行为保持，逐函数由用户手写。问题事实见 [AUDIT A6](AUDIT.md#a6)。

**进度：0%（0/3阶段验收）。** A7已完成，当前函数是 `TranContext::put()`。

## 全部阶段

| 阶段 | 具体函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① put 前置检查 | `src/lsm/transation.cpp:115` 的 `TranContext::put` | 已开始但未完成结束的事务拒绝put，operations/temp_map_不变；正常put及参数检查保持 | 进行中：测试已复现，提供草案待用户修改 |
| ② remove 前置检查 | 同文件 `:170` 的 `TranContext::remove` | 同样在追加DELETE之前拒绝，原记录和暂存表保持；正常删除保持 | 待开始 |
| ③ 整体验收 | 结束失败的两个定向用例，正常事务写入/提交/取消及相关WAL回归 | 定向失败消失，不影响正常事务结束、读未提交写入及既有日志顺序保护 | 待开始 |

百分比按通过验收阶段数计算，不代表工时比例。

## 方案与边界

- 在已提交/取消标志检查之后、参数处理和追加记录之前，检查非空operations的末条类型是否COMMIT/ROLLBACK；命中则抛logic_error。
- 复用现有结束记录，无新成员、无磁盘格式变化。合法生产路径只在末尾追加结束记录，两个写入口拦住后续追加；无需每次写遍历整个历史记录数组。
- commit/abort已有拒绝再次结束的逻辑，本轮只联动复核，不重写。
- 已复现的是结束失败后上下文仍被修改；WAL已故障的RU场景中，新增记录没有成功进入磁盘。健康WAL前的分配失败路径仍为推演，不混入实测结论。
- 同一TranContext仍按既定约定串行使用，不新增上下文互斥锁，不加入A8或其他首发任务。

## 修复前证据

- 扩充test/test_wal.cpp现有两项：`TranCommitWALTest.WalFailurePreservesDataAndRejectsRepeatedCommit`、`TranAbortWALTest.WalFailurePreservesActiveContextAndRejectsRepeatedEnd`。
- 使用实际WAL轮换失败保留结束记录；不通过手工篡改operations制造状态。
- test_wal构建成功；定向2/2失败。暂存型事务后续put/remove均未抛异常，修改operations/temp_map_；RU后续写操作先修改operations，再抛WAL失败runtime_error，原WAL仍保持。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a6-baseline-pbymsj9i`，含命令、日志、JSON、源码哈希及A7完成计划快照。尚无修复后结果。

## 上一项收尾

A7已在约定范围完成，52/52相关回归通过；结果与证据归入AUDIT A7和STATE。其非阻塞旧注释 `transation.cpp:869–872` 已提示用户删除，并将原874行改为第6步临时文件写入说明；业务注释由用户整理。
