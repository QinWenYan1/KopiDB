# KopiDB 当前计划

最后更新：2026-11-14。

## 目标：Lab 7 WiscKey 键值分离

按用户确认的顺序完成 **7.1 VLog → 7.2 SST 支持 → 7.3 Engine 集成**。MemTable 继续保存完整 value；刷盘时将超过阈值的 value 存入 VLog，SST 保存位置引用；读取返回原值，合并后仍可正确读取，并验证值分离的实际效果。

用户手写业务源码和头文件，助手逐函数提供中文注释、检查实现、补测试及维护记录。已有函数体也须按 Lab 独立推导；已验收的无关逻辑不整段重写。Redis 移除的独立代写授权已结束。

进度：**0%（0/4 阶段验收）**。`VLog::open` 和 `append` 已通过代码检查及 7 项专项测试；VLog 阶段尚未完成。用户已写入原版 read_value，并明确要求加入 CRC 校验。当前交付带 CRC 的完整替换草案，临时构建通过 14/14 专项测试，等待用户加入业务源码后复核，不把草案结果计为源码验收。

## 阶段与验收标准

| 阶段 | 按顺序处理的函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① Lab 7.1：VLog | `VLog::open` → `append` → `read_value`；复核直接相关辅助函数 | 新建为空、重开不截断；多条记录可按返回偏移读回，二进制内容不变；读取时核对长度和 CRC，损坏／截断明确报错；偏移和编码长度正确，共享文件访问正确同步 | 进行中，open/append 已通过，当前 read_value |
| ② Lab 7.2：SST 支持 | 确定记录类型表达 → WiscKey 版 `SSTBuilder` 构造 → `add` → `build` → `SST::open` → `resolve_value` | 同表的小 value、VLog 引用与墓碑不会混淆；阈值边界正确；构建／重开后点查与遍历还原原值，版本号不变；普通内联 SST 仍可读 | 待开始 |
| ③ Lab 7.3：Engine 集成 | `LSMEngine` 构造 → `flush` → `gen_sst_from_iter`；联动复核清库、配置及引用传递 | 启用／禁用两种配置均可用；真实刷盘、合并、重开及清库后再次写入正确；合并明确区分原始引用与完整值，验证是否重复追加 VLog；状态通知及回滚过滤不退化 | 待开始 |
| ④ 联调与验收 | 完整 WiscKey 测试和受影响存储／事务／恢复回归，更新结果 | 真实启用非零阈值验证 CRUD、批查、遍历、版本过滤、取消事务、合并和正常重开；默认关闭模式通过回归；报告实际测试范围和未验证边界 | 待开始 |

百分比按验收通过阶段数计算，不代表工时比例；阶段中一个函数完成，不算整阶段完成。

## ① VLog：当前逐函数 TODO

文件：`include/vlog/vlog.h`、`src/vlog/vlog.cpp`。

- [x] `VLog::open(const std::string &path)`：复用 FileObj 创建／非截断打开；失败传播异常。用户实现已检查，创建空文件、保留二进制内容后重开、父目录不存在时抛异常共 3/3 通过。函数上方残留的 Lab 6.1 注释应改为 Lab 7.1，属于非阻塞文字修正。
- [x] `VLog::append(const std::string &key, const std::string &value)`：用户已修正 value_len 类型和记录总长度；固定字节布局／CRC、65536 字节二进制值、重开续写偏移、超长 key 拒绝共 4/4 通过。锁范围及追加失败处理静态检查通过；并发和 I/O 失败路径尚未实测。`<limits>` 已直接包含。
- [ ] `VLog::read_value(uint64_t offset, uint32_t value_size)`：读取含 CRC 的完整记录，核对记录里的 value_len 与 SST 引用长度一致；CRC 覆盖 key_len/key/value_len/value，匹配后才返回 value。按用户确认的新要求替换此前只检查边界的读取方案。
- [ ] 直接联动复核 `tail_offset()`、`sync()`、`del_vlog()`：底层 StdFile 共用 fstream，协调同一对象的定位、读取和追加；检查 sync 返回值的处理。它们不是默认全部重写的 Lab 桩；确需修改时先说明调用依据。
- [ ] 测试随函数推进：open 独立测试已补充并通过；追加／重开续写／二进制／长度／并发测试随后补齐。原有 3 项 VLog 联合测试依赖 append/read_value，本轮未运行，不因 open 通过就宣称 VLog 已完成。

当前修改范围：`src/vlog/vlog.cpp` 中 `VLog::read_value`；不需要新增成员、改声明、文件格式或调用方。读取 key_len 后用它和 value_size 计算整条记录含 CRC 的长度，边界检查通过才读取；使用与 append 相同的 mutex，读取结束后 unique_lock 提前解锁，长度核对和 CRC 计算只使用本地缓冲区。长度或 CRC 不匹配抛 runtime_error，越界／截断抛 out_of_range。CRC 用于检测损坏，不提供纠错或断电持久性保证。辅助函数的共享 fstream 访问随后复核，Lab 7.1 注释标注仍需补齐。

2026-11-14 方案调整依据：Lab／作者参考跳过读取 CRC；用户明确要求加入校验。写入校验码本身不能发现后来发生的内容变化，SST 的校验只覆盖其存储内容，不能代替 VLog 校验；不采用文档中“写入 CRC 就保证完整性”的解释。完整记录本来就保存 CRC，因此本轮只调整读取函数及对应测试。

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

2026-11-14 append 修正后复测：新增 65536 字节二进制 value 场景，检查完整 4 字节长度与全部 payload；构建成功，`VLogOpenTest.*:VLogAppendTest.*` **7/7 通过**（open 3、append 4）。运行期间业务源码哈希一致，测试全部使用独占临时目录，未干预用户 test_lsm。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-vlog-append-recheck-ricj1pqk`。read_value 草案另存临时文件，仅通过语法检查，功能测试待用户实现后进行；助手未改业务源码。

2026-11-14 CRC 专项：新增 `VLogReadTest` 7 项，覆盖独立固定样本、65536 字节二进制值／空值／不同偏移与重开、key/value/CRC 与长度字段损坏、引用长度不符、截断和极端偏移。当前仓库未加 CRC 的 read_value **3/7 通过，4 项失败**；将聊天草案保存到临时文件，独立编译该对象替代链接中的 libvlog 后，open/append/read 专项 **14/14 通过**。只运行这些独占目录测试，未运行旧固定 /tmp 用例或用户 test_lsm；源码前后哈希一致，助手未改业务源码。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-vlog-crc-draft-6e_ieqce`，包含基线／草案 JSON、完整编译链接及运行命令、日志与业务源码哈希。草案验证不代表用户尚未加入的代码已验收。
