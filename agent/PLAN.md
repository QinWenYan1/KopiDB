# KopiDB 当前计划

最后更新：2026-11-14。

## 目标：Lab 7 WiscKey 键值分离

按用户确认的顺序完成 **7.1 VLog → 7.2 SST 支持 → 7.3 Engine 集成**。MemTable 继续保存完整 value；刷盘时将超过阈值的 value 存入 VLog，SST 保存位置引用；读取返回原值，合并后仍可正确读取，并验证值分离的实际效果。

用户手写业务源码和头文件，助手逐函数提供中文注释、检查实现、补测试及维护记录。已有函数体也须按 Lab 独立推导；已验收的无关逻辑不整段重写。Redis 移除的独立代写授权已结束。

进度：**0%（0/4 阶段验收）**。资料阅读和计划准备不计功能完成。当前函数：`VLog::open`，草案已在聊天中交付，等待用户实现。

## 阶段与验收标准

| 阶段 | 按顺序处理的函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① Lab 7.1：VLog | `VLog::open` → `append` → `read_value`；复核直接相关辅助函数 | 新建为空、重开不截断；多条记录可按返回偏移读回，二进制内容不变；偏移和编码长度正确，超限及读写失败明确报告；共享文件访问正确同步 | 进行中，当前 open |
| ② Lab 7.2：SST 支持 | 确定记录类型表达 → WiscKey 版 `SSTBuilder` 构造 → `add` → `build` → `SST::open` → `resolve_value` | 同表的小 value、VLog 引用与墓碑不会混淆；阈值边界正确；构建／重开后点查与遍历还原原值，版本号不变；普通内联 SST 仍可读 | 待开始 |
| ③ Lab 7.3：Engine 集成 | `LSMEngine` 构造 → `flush` → `gen_sst_from_iter`；联动复核清库、配置及引用传递 | 启用／禁用两种配置均可用；真实刷盘、合并、重开及清库后再次写入正确；合并明确区分原始引用与完整值，验证是否重复追加 VLog；状态通知及回滚过滤不退化 | 待开始 |
| ④ 联调与验收 | 完整 WiscKey 测试和受影响存储／事务／恢复回归，更新结果 | 真实启用非零阈值验证 CRUD、批查、遍历、版本过滤、取消事务、合并和正常重开；默认关闭模式通过回归；报告实际测试范围和未验证边界 | 待开始 |

百分比按验收通过阶段数计算，不代表工时比例；阶段中一个函数完成，不算整阶段完成。

## ① VLog：当前逐函数 TODO

文件：`include/vlog/vlog.h`、`src/vlog/vlog.cpp`。

- [ ] `VLog::open(const std::string &path)`：保留现有接口，复用 FileObj 创建／非截断打开；失败传播异常，不返回不可用对象。父目录由调用者准备，Engine 已有该步骤。
- [ ] `VLog::append(const std::string &key, const std::string &value)`：按 `[key_len:u16][key][value_len:u32][value][crc:u32]` 编码，检查长度可表示；CRC 覆盖此前全部字段；取得尾偏移并追加整条记录，返回记录起点。
- [ ] `VLog::read_value(uint64_t offset, uint32_t value_size)`：按记录头定位 value，返回指定长度的字节；说明长度／文件边界和 CRC 的实际读取策略，不把“写入了 CRC”解释为“读取已验证完整性”。
- [ ] 直接联动复核 `tail_offset()`、`sync()`、`del_vlog()`：底层 StdFile 共用 fstream，协调同一对象的定位、读取和追加；检查 sync 返回值的处理。它们不是默认全部重写的 Lab 桩；确需修改时先说明调用依据。
- [ ] 补充 open 独立测试，再随 append/read_value 完成追加／重开／二进制／长度／并发相关测试。当前 3 项 VLog 原测试依赖 append/read_value，不能在 open 完成后就宣称 VLog 已通过。

当前首函数的修改范围仅 `src/vlog/vlog.cpp` 中 `VLog::open`；不需要新增成员、改声明或调用方。骨架上的“Lab 6.1”是章节标注错误，本次修改函数时同步改为 Lab 7.1。

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

当前仅完成阅读、设计范围核对和首函数草案；未修改业务代码，未运行 WiscKey 测试。
