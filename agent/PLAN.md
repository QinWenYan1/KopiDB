# KopiDB 当前计划

最后更新：2026-11-17。

## A6：结束尝试失败后拒绝继续追加写操作

目标：真实 commit/abort 尝试已经在 operations 末尾留下 COMMIT/ROLLBACK、但尚未完成时，`TranContext::put/remove` 必须在修改上下文或发送WAL之前以logic_error拒绝新写入。正常事务行为保持，逐函数由用户手写。问题事实见 [AUDIT A6](AUDIT.md#a6)。

**进度：33%（1/3阶段验收）。** A7已完成，put阶段已验收，当前函数是 `TranContext::remove()`。

## 全部阶段

| 阶段 | 具体函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① put 前置检查 | `src/lsm/transation.cpp:115` 的 `TranContext::put` | 已开始但未完成结束的事务拒绝put，operations/temp_map_不变；正常put及参数检查保持 | 已通过：两层检查保留，正常及异常结束后的put验证通过 |
| ② remove 前置检查 | 同文件 `:184` 的 `TranContext::remove` | 同样在追加DELETE之前拒绝，原记录和暂存表保持；正常删除保持 | 当前阶段：提供草案待用户修改 |
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

A7已在约定范围完成，52/52相关回归通过；结果与证据归入AUDIT A7和STATE。用户已将第6步标题调整为先写同目录临时文件，注释整理不另起业务修改。

## put实现检查（2026-11-17）

- 用户新增末条结束记录检查后，A6两个故障用例中的put拒绝及无副作用断言通过；remove尚未修改，两个用例仍因remove断言失败。
- 但用户将原isCommited/isAborted检查删除；正常abort会清空operations，回滚后的put因此越过新检查。必须先恢复原检查，再推进remove。
- 构建两个目标通过；定向8项为3通过、5失败：LSM的6项中3通过/3失败（结束状态保护回归），WAL的2项均因remove尚未接入失败。未代改业务，进度保持0/3。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a6-put-review-2x8o76md`，含命令、日志、JSON及执行前后源码/测试哈希（稳定）。

## put阶段验收（2026-11-17）

- 用户已补回isCommited/isAborted检查，同时保留末条结束记录检查；代码检查通过。
- 两个目标构建成功；原8项定向复测，test_lsm 6/6通过；test_wal 2项仍失败，但所有失败断言都来自尚未修改的remove，put拒绝及无副作用断言均通过。整体A6尚未完成，不把两个组合用例记为通过。
- 阶段①完成，进度1/3；下一步在remove原结束标志检查后（当前transation.cpp:194、空key注释前）追加末条记录判断，保留原检查。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a6-put-fixed-tpscs_xe`。测试执行前后源码/测试哈希稳定，未修改业务代码。
