# KopiDB 当前计划

最后更新：2026-11-17。

## A7：保存事务状态失败时保住上一份完整文件

目标：`TranManager::write_tranc_id_file()` 写入／刷新临时文件或替换失败时，上一次成功保存的正式 `tranc_id` 文件保持完整；排除故障后可以重试保存或重新启动。沿用既有状态编码、互斥锁、错误报告及当前 macOS/Linux 文件替换语义。问题证据见 [AUDIT A7](AUDIT.md#a7)。

**进度：0%（0/2 阶段验收）。** 业务源码及头文件由用户手写；助手提供中文注释草案、补充测试、运行验收。本轮只有一个业务函数需要修改。

## 全部阶段

| 阶段 | 具体工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 保存函数修复 | 修改 `src/lsm/transation.cpp` 的 `TranManager::write_tranc_id_file()` 第6步；按需显式包含 `<system_error>` | 先完整写入同目录临时文件并检查 write/sync，再 rename 替换；失败不删除正式文件；成功后成员句柄重新打开正式路径。已有字段编码、较短文件替换和重复保存行为保持 | 进行中：草案与故障测试准备，等待用户实现 |
| ② 故障与重启验收 | `test/test_wal.cpp` A7定向测试、`test/test_lsm.cpp` 既有事务状态测试；相关启动／clear回归 | 部分写入失败后正式文件字节不变、旧WAL保留且没有新WAL；解除故障后重启成功并恢复值和ID。替换失败明确上报，同一管理器可重试保存并读到新文件 | 待开始 |

百分比按验收阶段数计算，不代表工时比例。

## 实现范围与调用关系

- **必改函数**：`TranManager::write_tranc_id_file()`（当前 `transation.cpp:800`，文件保存部分 `:868–880`）。保留第1～5步的内存编码，仅调整文件发布顺序。
- **仅复核**：管理器构造／`read_tranc_id_file()`／析构；LSM构造、析构、clear 的状态保存调用；FileObj 的 open/write/sync/close 接口。当前无需重写它们。
- 固定临时名 `tranc_id.tmp` 受当前管理器互斥锁保护；沿用单实例管理一个数据目录的约定。
- macOS/Linux 上 rename 可以替换仍被旧句柄打开的目标；替换前保留旧句柄，替换成功后关闭并重新打开，避免读取旧文件内容。
- 临时文件打开成功后才将其纳入失败清理；清理使用 error_code，不覆盖原始异常。临时写入／替换失败保住旧文件；替换后重开句柄若失败，仍报告异常，此时磁盘已经是完整的新文件，不宣称任何异常都回到旧版本。
- 本轮不改变文件格式、不新增 fsync 或断电保障、不扩展 A8 SST 发布修复。全局延期与其他首发候选见 STATE。

## 本轮测试准备与结果

- 历史部分写入失败证据沿用 AUDIT：完整40B状态文件被截断为16B。
- 本轮已构建 test_wal/test_lsm；修复前仅运行3个A7定向用例，1通过、2失败，符合当前缺陷。目录占用后的重试通过；部分写入破坏旧字节，故障解除后的LSM重启实际抛出 invalid file length。
- 已强化 `LSMStartupWALTest.StateSaveFailurePreservesWalAndStopsInitialization`，新增 `TranManagerRecoveryTest.StateSaveFailurePreservesBytesAndAllowsRetry` 和 `StateReplaceFailureAllowsSameManagerRetry`。用户修改后运行这些用例及既有事务状态/启动/clear回归，尚无修复后结果。
- 修复前证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a7-baseline-t187tia_`，含旧PLAN快照、命令、日志及results.json；测试在独立工作目录运行。
- 前一轮 A15 已完成，验收和性能结果保留于 AUDIT A15 与 STATE；不再作为本轮执行计划。
