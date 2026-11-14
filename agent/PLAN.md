# KopiDB 当前计划

最后更新：2026-11-14。

## 目标：Lab 7 WiscKey 键值分离

按用户确认的顺序完成 **7.1 VLog → 7.2 SST 支持 → 7.3 Engine 集成**。MemTable 继续保存完整 value；刷盘时将超过阈值的 value 存入 VLog，SST 保存位置引用；读取返回原值，合并后仍可正确读取，并验证值分离的实际效果。

用户手写业务源码和头文件，助手逐函数提供中文注释、检查实现、补测试及维护记录。已有函数体也须按 Lab 独立推导；已验收的无关逻辑不整段重写。Redis 移除的独立代写授权已结束。

进度：**0%（0/4 阶段验收）**。`VLog::open` 已通过代码检查及 3 项专项测试；VLog 阶段尚未完成。当前函数：`VLog::append`，用户已实现，检查发现两处编码笔误；新增专项测试 1/3 通过，修正后复测再进入 read_value。

## 阶段与验收标准

| 阶段 | 按顺序处理的函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① Lab 7.1：VLog | `VLog::open` → `append` → `read_value`；复核直接相关辅助函数 | 新建为空、重开不截断；多条记录可按返回偏移读回，二进制内容不变；偏移和编码长度正确，超限及读写失败明确报告；共享文件访问正确同步 | 进行中，open 已通过，当前 append |
| ② Lab 7.2：SST 支持 | 确定记录类型表达 → WiscKey 版 `SSTBuilder` 构造 → `add` → `build` → `SST::open` → `resolve_value` | 同表的小 value、VLog 引用与墓碑不会混淆；阈值边界正确；构建／重开后点查与遍历还原原值，版本号不变；普通内联 SST 仍可读 | 待开始 |
| ③ Lab 7.3：Engine 集成 | `LSMEngine` 构造 → `flush` → `gen_sst_from_iter`；联动复核清库、配置及引用传递 | 启用／禁用两种配置均可用；真实刷盘、合并、重开及清库后再次写入正确；合并明确区分原始引用与完整值，验证是否重复追加 VLog；状态通知及回滚过滤不退化 | 待开始 |
| ④ 联调与验收 | 完整 WiscKey 测试和受影响存储／事务／恢复回归，更新结果 | 真实启用非零阈值验证 CRUD、批查、遍历、版本过滤、取消事务、合并和正常重开；默认关闭模式通过回归；报告实际测试范围和未验证边界 | 待开始 |

百分比按验收通过阶段数计算，不代表工时比例；阶段中一个函数完成，不算整阶段完成。

## ① VLog：当前逐函数 TODO

文件：`include/vlog/vlog.h`、`src/vlog/vlog.cpp`。

- [x] `VLog::open(const std::string &path)`：复用 FileObj 创建／非截断打开；失败传播异常。用户实现已检查，创建空文件、保留二进制内容后重开、父目录不存在时抛异常共 3/3 通过。函数上方残留的 Lab 6.1 注释应改为 Lab 7.1，属于非阻塞文字修正。
- [ ] `VLog::append(const std::string &key, const std::string &value)`：用户实现的锁范围、长度检查及追加失败处理符合方案；当前 value_len 误声明为 uint16_t，应为 uint32_t，记录总长度误用 sizeof(value.size())，应为 value.size()。补直接包含 <limits>。这两处修正前不进入 read_value。
- [ ] `VLog::read_value(uint64_t offset, uint32_t value_size)`：按记录头定位 value，返回指定长度的字节；说明长度／文件边界和 CRC 的实际读取策略，不把“写入了 CRC”解释为“读取已验证完整性”。
- [ ] 直接联动复核 `tail_offset()`、`sync()`、`del_vlog()`：底层 StdFile 共用 fstream，协调同一对象的定位、读取和追加；检查 sync 返回值的处理。它们不是默认全部重写的 Lab 桩；确需修改时先说明调用依据。
- [ ] 测试随函数推进：open 独立测试已补充并通过；追加／重开续写／二进制／长度／并发测试随后补齐。原有 3 项 VLog 联合测试依赖 append/read_value，本轮未运行，不因 open 通过就宣称 VLog 已完成。

当前修改范围：`src/vlog/vlog.cpp` 中 `VLog::append`，增加 `<limits>`；不需要新增成员、改声明或调用方。草案沿用作者 VLog 的 memcpy 字段编码，先检查长度，再锁外编码和计算 CRC；锁内取得文件尾偏移、追加并检查返回值。后续 read_value／辅助函数统一复核共享 fstream 的锁，不提前宣称所有并发访问已接通。

## ②③ 开始编码前需要确定的衔接

这些属于本章实现设计，尚不当作已完成模块的独立 bug，也不要求当前提前修改：

1. **混合存储的辨别方式。** 本地 `SSTBuilder::add` 只分离超阈值的 value，而 `resolve_value` 把 WiscKey 表内长度不小于 12 的非空值都当引用。实现 7.2 前，先确定内联／引用的明确区分与读写对称性，覆盖长度恰好 12 及超过 12 的合法小 value；不能照搬参考的长度判断。
2. **合并拿到的到底是什么。** 本地 `SstIterator::update_current` 会调用 `resolve_value`，`gen_sst_from_iter` 消费的可能是完整值。教程中“迭代器透明还原 value”与“合并直接搬 12 字节引用”的描述不能同时直接套用。7.2／7.3 实施前推导引用传递方式，列清必要的声明、调用方和拟新增接口后再逐函数落地；不预先要求重构全部迭代器。
3. **已有辅助路径。** 复核 `SstIterator::update_current/operator*/operator->` 的值暴露、`SST::is_wisckey`、`LSMEngine::clear`、`TomlConfig::loadFromFile/saveToFile/getWisckeyValueThreshold` 和配置项。已有正确行为保留；SST 刷盘前的 VLog 刷新顺序也在此确认。
4. **测试确实启用功能。** 当前测试的部分 Persistence／Compaction 场景没有设置非零阈值；测试名不能证明经过 VLog。测试配置使用项目已有的大写 `WISCKEY_VALUE_THRESHOLD`，并检查实际 VLog／SST 状态。现有固定 /tmp 路径需要改为测试独占临时目录，单纯切换工作目录不够。

## 资料与范围

已阅读 Lab 7 概述及 7.1～7.3 全文，核对本地直接调用链，并读取作者 master 的 VLog／SST 参考；本地 reference-impl 的 VLog 仍是桩，不能冒充完整参考。

- [Lab 7 概述](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab7/lab7-WiscKey.md)
- [Lab 7.1 VLog](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab7/lab7.1-VLog.md)
- [Lab 7.2 SST](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab7/lab7.2-SSTBuilder-WiscKey.md)
- [Lab 7.3 Engine](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab7/lab7.3-Engine-WiscKey.md)
- [作者 VLog 参考](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/vlog/vlog.cpp)
- [作者 SST 参考](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/sst/sst.cpp)

本轮不加入 VLog GC；它属于教程明确未要求的后续扩展。公开 API、事务可见性及既有编码上限沿用现状；测试使用现有写接口可接受的数据规模。普通刷新仍按当前文件层能力验收，不将本章通过等同于断电持久性。全局延期问题继续留在 STATE/AUDIT。

## 当前验收记录

2026-11-14：`xmake build -j 2 test_wisckey` 成功；独立临时目录内仅运行 `--gtest_filter=VLogOpenTest.*`，**3/3 通过，零失败／禁用**。新增测试位于 `test/test_wisckey.cpp`，使用原子创建的独占临时目录，不依赖 append/read_value，不改原有用例。失败场景中的打开错误日志是预期异常，测试通过。

证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-vlog-open-review-n6kzzsdd`，包含构建／测试日志、GTest JSON、命令和业务源码前后哈希（保持一致）。append 草案仅在临时文件通过语法检查，尚未做功能验收；助手未改业务源码。没有停止、重启或运行用户的 test_lsm，也没有访问其测试数据。

2026-11-14 append 初次检查：构建成功，独占临时目录内运行新增 `VLogAppendTest.*`，**1/3 通过、2 项失败**。超长 key 拒绝且不改文件通过；固定记录布局／CRC、重开续写偏移失败。k/abc 应为 14 字节，实际为 17 字节；value_len 字段实际占 2 字节，与设计的 4 字节不符。仅测试 value 长度不超过 8 的安全输入，没有执行当前实现会越界的大值写入。原有依赖 read_value 的测试未运行，未干预用户 test_lsm。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-vlog-append-review-4__jgw16`；构建／测试日志、JSON 和业务源码哈希已保存，测试期间代码稳定，助手未改业务源码。
