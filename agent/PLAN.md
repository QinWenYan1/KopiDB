# KopiDB 当前计划

最后更新：2026-11-17。

## A8：SST 写入失败时不发布残缺正式文件

目标：`SSTBuilder::build()` 写入／刷新失败时，不留下会被启动扫描加载的残缺正式SST；完整WAL保留时，解除故障后可以重启恢复数据并正常刷盘。问题与证据见 [AUDIT A8](AUDIT.md#a8)。

**进度：0%（0/2阶段验收）。** A6已完成，当前唯一业务修改函数是 `SSTBuilder::build()`；用户手写业务，助手补测试与验收。

## 全部阶段

| 阶段 | 具体工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① SST 文件发布 | `src/sst/sst.cpp:504` 的 `SSTBuilder::build` 第6步文件写入部分；补filesystem/system_error头文件 | 先同步VLog，再完整写同目录隐藏临时文件、检查刷新、rename正式文件，返回对象打开正式路径；失败不发布残缺SST | 进行中：已复现，提供草案待用户修改 |
| ② 故障与正常路径验收 | `test/test_wal.cpp` 的公开flush故障/重启用例，SST读写/删除、必要WiscKey回归 | 部分写入失败不留正式SST，WAL保持；解除故障可重开读回全部数据、刷盘及再次重开；隐藏临时文件不干扰加载；成功SST可读且删除正确路径 | 待开始 |

百分比按验收阶段数计算，不代表工时比例。

## 实现范围

- 只修改 `SSTBuilder::build` 的文件发布步骤，不改编码、Block、MemTable成功后移除顺序或Engine扫描接口。
- 保留现有先sync VLog再发布SST的顺序。
- 临时名称使用同目录的 `.` + 正式文件名 + `.tmp`，例如 `.sst_1.0.tmp`。不能只在正式文件名后加 `.tmp`：Engine当前检查sst_前缀，stoull接受数字前缀，会误加载这种文件。
- 临时文件完整write/sync并关闭后rename；失败只清理本次成功打开的临时文件，清理错误不遮蔽原异常。
- 发布后通过 `FileObj::open(path, false)` 打开正式路径再交给SST；不能直接搬运内部仍记录临时路径的句柄，后续del_sst需要正确路径。
- 仅复核 FileObj/StdFile、SST::del_sst、MemTable::flush_last 和 Engine 构造；无需重写这些函数。
- 不扩展为所有文件的发布框架、不改文件格式、不加入fsync/断电完整保障或旧损坏SST自动修复。

## 修复前证据

- 新增 `LSMStartupWALTest.FailedSstFlushPreservesWalAndAllowsRestart`：公开put_batch成功写入100个不同key/单字节value并确认完整WAL；子进程限制512B文件写入，再公开flush，捕获失败后_Exit避免析构重试。
- 修复前定向1/1失败：留下正式 `sst_00000000000000000000000000000001.0`，长度512B；父进程解除限制后重启实际抛出 `Read beyond file size`。原WAL内容保持。
- 该触发来自当前公开写入路径，不是手工构造坏SST。单字节value在阈值1下也不会分离，避免VLog先失败掩盖本项。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a8-baseline-o4pso34f`，含命令、日志、JSON及源码哈希。业务尚未修改，没有修复后验收结果。

## 上一项完成

A6已完成，100%（3/3阶段）；事务及WAL相关73/73通过，证据归AUDIT A6与STATE，完成计划快照保存在本轮证据目录。A7也已完成；其他首发候选尚未自动加入本轮。
