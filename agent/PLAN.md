# KopiDB 当前计划

最后更新：2026-11-15。

## 目标：Lab 7 WiscKey 键值分离

按用户确认的顺序完成 **7.1 VLog → 7.2 SST 支持 → 7.3 Engine 集成**。MemTable 继续保存完整 value；启用 WiscKey 时，将超过阈值或恰好等于引用长度的非空原值分离到 VLog，SST 保存 12 字节位置引用；读取返回原值，合并后仍可正确读取，并验证值分离的实际效果。2026-11-15 用户明确确认执行保留长度方案，不添加类型字节。

用户手写业务源码和头文件，助手逐函数提供中文注释、检查实现、补测试及维护记录。已有函数体也须按 Lab 独立推导；已验收的无关逻辑不整段重写。Redis 移除的独立代写授权已结束。

进度：**25%（1/4 阶段验收）**。阶段① VLog 已完成，用户源码的 open/append/read_value/sync 通过代码检查及 20/20 VLog 测试。阶段②方案已获用户明确确认；当前交付 WiscKey 版 SSTBuilder 构造函数（src/sst/sst.cpp:314）草案，等待用户手动实现。草案语法检查通过，尚未作函数功能验收。

## 阶段与验收标准

| 阶段 | 按顺序处理的函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① Lab 7.1：VLog | `VLog::open` → `append` → `read_value`；复核直接相关辅助函数 | 新建为空、重开不截断；多条记录可按返回偏移读回，二进制内容不变；读取时核对长度和 CRC，损坏／截断明确报错；偏移和编码长度正确，共享文件访问正确同步 | 已验收，20/20 |
| ② Lab 7.2：SST 支持 | 已确认保留引用长度 → WiscKey 版 `SSTBuilder` 构造 → `add` → `build` → `SST::open` → `resolve_value` | 12 字节原值强制分离、读端只识别恰好 12 字节引用；阈值／墓碑／普通模式正确；构建及重开后还原原值，版本号不变 | 当前：构造函数草案待用户加入 |
| ③ Lab 7.3：Engine 集成 | `LSMEngine` 构造 → `flush` → `gen_sst_from_iter`；联动复核清库、配置及引用传递 | 启用／禁用两种配置均可用；真实刷盘、合并、重开及清库后再次写入正确；合并明确区分原始引用与完整值，验证是否重复追加 VLog；状态通知及回滚过滤不退化 | 待开始 |
| ④ 联调与验收 | 完整 WiscKey 测试和受影响存储／事务／恢复回归，更新结果 | 真实启用非零阈值验证 CRUD、批查、遍历、版本过滤、取消事务、合并和正常重开；默认关闭模式通过回归；报告实际测试范围和未验证边界 | 待开始 |

百分比按验收通过阶段数计算，不代表工时比例；阶段中一个函数完成，不算整阶段完成。

## ① VLog：当前逐函数 TODO

文件：`include/vlog/vlog.h`、`src/vlog/vlog.cpp`。

- [x] `VLog::open(const std::string &path)`：复用 FileObj 创建／非截断打开；失败传播异常。用户实现已检查，创建空文件、保留二进制内容后重开、父目录不存在时抛异常共 3/3 通过。函数上方残留的 Lab 6.1 注释应改为 Lab 7.1，属于非阻塞文字修正。
- [x] `VLog::append(const std::string &key, const std::string &value)`：用户已修正 value_len 类型和记录总长度；固定字节布局／CRC、65536 字节二进制值、重开续写偏移、超长 key 拒绝共 4/4 通过。锁范围及追加失败处理静态检查通过；并发和 I/O 失败路径尚未实测。`<limits>` 已直接包含。
- [x] `VLog::read_value(uint64_t offset, uint32_t value_size)`：用户已实现整记录范围检查、引用长度核对和 CRC 校验；正常二进制／空值／重开、各字段损坏、截断、极端偏移均通过。保留原 lock_guard，未要求切换 unique_lock。
- [x] `tail_offset()`、`del_vlog()` 的直接调用复核：tail_offset 当前仅由测试顺序调用，不要求为假设并发加锁；del_vlog 唯一生产调用在排他的 clear，且目录清理已先移除文件，保持当前行为，不新增文件不存在错误。
- [x] `VLog::sync()` 辅助收尾：用户已实现加锁和刷新失败抛 runtime_error；正常刷新、POSIX 子进程真实刷新失败传播两项测试通过。供后续 7.2 SSTBuilder::build 的 VLog 刷新衔接使用，调用接入留到阶段②。它是本地整合需要，不是 Lab 7.1 原文额外列出的核心函数。
- [x] VLog 核心与辅助测试：20/20 通过，包括双写双读、正常刷新及真实刷新失败。原有 3 项用例已迁移到独占目录，名称、断言与工作量保持，避免使用固定 /tmp 路径。

阶段①收尾范围：FileObj::sync 当前为 fstream::flush，不将它解释为 fsync；CRC 不提供纠错或断电持久性保证。剩余非阻塞文字整理：三个函数上方 Lab 6.1 应改为 Lab 7.1；sync 前“这里忽略了结果”已经过时，应改为当前会传播失败的描述。这些文字不影响上述功能验收，助手未代改业务源码。

2026-11-14 方案调整依据：Lab／作者参考跳过读取 CRC；用户明确要求加入校验。写入校验码本身不能发现后来发生的内容变化，SST 的校验只覆盖其存储内容，不能代替 VLog 校验；不采用文档中“写入 CRC 就保证完整性”的解释。完整记录本来就保存 CRC，因此本轮只调整读取函数及对应测试。

## ② SST：已确认规则与逐函数 TODO

引用保持 `[offset:uint64][value_size:uint32]`，共 12 字节，不增加类型标记。规则只用于 WiscKey 模式：空值保持墓碑；原始非空值长度为 12 或超过阈值时分离，其余内联；读端只有长度恰好为 12 才解析引用。普通模式原样保存和读取。阈值 0 禁用，不新增阈值必须大于 12 的限制。

- [ ] 四参 `SSTBuilder` 构造函数（当前）：用户最新选择回到委托两参构造的版本。阈值 0 是否应报错正在解释；现有草案仍约定 0 保持普通模式、非零且缺 VLog 报错。当前用户代码已委托，但尾部保留了重复 Bloom／clear，并遗漏 vlog_、wisckey_threshold_、storage_mode_ 的赋值，导致非零阈值也不会启用 WiscKey；已给出准确局部替换位置，尚未验收。检查阈值 0、非零但缺 VLog、小阈值及普通构造，不把构造风格作为验收条件。
- [ ] `SSTBuilder::add`：读写共用引用长度常量；仅改分离选择及必要编码，保留已有版本分组／换块／Bloom／事务范围逻辑。传给 VLog::append 的仍是原值，引用保持 12 字节。
- [ ] `SSTBuilder::build`：核对已有 footer 和对象字段传递；写入引用 SST 前刷新 VLog，刷新失败向上传递，保留已有空表和构建失败行为。
- [ ] `SST::open`：核对模式识别、元数据及 VLog 依赖恢复；普通模式与 WiscKey 模式均能正常重开。
- [ ] `SST::resolve_value`：普通模式／空值直接返回；WiscKey 模式将 `< 12` 改为基于共同常量的 `!= 引用长度`，仅精确长度的引用进入 VLog 读取。
- [ ] 阶段验收：11/12/13 字节、阈值及相邻长度、小阈值、关闭模式、墓碑、二进制内容、真实版本及构建／重开；启用模式缺失 VLog 的约定错误明确报告。测试由助手随实现补充；此前未验收的 WiscKey 文件格式不自动成为兼容任务。

当前精确修改点：`src/sst/sst.cpp:314` 四参构造函数；当前第 331～340 行重复初始化应替换为 vlog_ 移动赋值、wisckey_threshold_ 保存及 storage_mode_=1。`include/sst/sst.h:153` 构造说明同步最终约定，无需修改签名或成员。用户已选择委托版，后续按实际选择检查，不要求采用直接初始化版；完成后再进入 add。

## ②③ 仍需核对的衔接

这些属于本章实现设计，尚不当作已完成模块的独立 bug，也不要求当前提前修改：

1. **混合存储规则已确定，等待实现。** 以上述保留引用长度规则为准；不继续实施类型字节草案。构造、写入和读取必须对模式解释一致，不能声明 WiscKey 模式后又将 12 字节原值静默内联。
2. **合并拿到的到底是什么。** 本地 `SstIterator::update_current` 会调用 `resolve_value`，`gen_sst_from_iter` 消费的可能是完整值。教程中“迭代器透明还原 value”与“合并直接搬 12 字节引用”的描述不能同时直接套用。7.2／7.3 实施前推导引用传递方式，列清必要的声明、调用方和拟新增接口后再逐函数落地；不预先要求重构全部迭代器。
3. **已有辅助路径。** 复核 `SstIterator::update_current/operator*/operator->` 的值暴露、`SST::is_wisckey`、`LSMEngine::clear`、`TomlConfig::loadFromFile/saveToFile/getWisckeyValueThreshold` 和配置项。已有正确行为保留；SST 刷盘前的 VLog 刷新顺序也在此确认。
4. **测试确实启用功能。** 当前测试的部分 Persistence／Compaction 场景没有设置非零阈值；测试名不能证明经过 VLog。测试配置使用项目已有的大写 `WISCKEY_VALUE_THRESHOLD`，并检查实际 VLog／SST 状态。现有固定 /tmp 路径需要改为测试独占临时目录，单纯切换工作目录不够。

已确认的取舍：保留引用长度方案不为每条非空 SST 值增加标签，但会将原本可内联的 12 字节值写入 VLog。用户选择它的依据是正确性能够保证且现有编码改动更小，不是已证明总体性能更好。整体读写成本依赖数据长度分布；不得据此假设 12 字节值很少。本规则不自动解决 compaction 对 VLog 的重复读写，后者仍按上方第 2 项处理。

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

2026-11-15：用户要求基于现有实现局部追加 CRC。修订草案只替换函数尾部，保留原 lock_guard 和头部计算；临时构建再次 **14/14 通过**，业务源码哈希未变。沿用上述证据目录，新增 `vlog-crc-minimal.cpp`、`minimal-commands.json`、`minimal-results.json` 及日志；仍等待用户手动加入后复核。

2026-11-15 用户 CRC 实现验收：构建成功，运行 `--gtest_filter=VLog*`，**18/18 通过**（open 3、append 4、read 8、原有 VLog 3）。新增并发用例为两写线程各追加 48 条，两读线程各读取种子 128 次，join 后核对全部记录、偏移连续无重叠及总尾位置；未引入并发 tail/sync/clear 要求。原有 3 项测试仅迁移隔离路径及清理方式，断言不变。首次构建因本机 libc++ 不支持测试使用的 jthread/stop_token 失败，已由助手改为 std::thread 并补异常时 join，重建和测试通过，不是业务失败。业务源码哈希稳定，无 test_lsm 干预。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-vlog-read-review-020wppt1`，含首次／最终构建日志、运行命令、GTest JSON、源码哈希。未运行尚未接通的 SST/Engine WiscKey 测试。

2026-11-15 阶段①最终验收：用户 sync 加锁与失败传播检查通过，新增正常刷新后独立文件流可读、子进程 RLIMIT_FSIZE=0 注入真实刷新失败共 2 项。失败场景确认 append 已成功返回，再由 sync 抛出自身的 runtime_error；资源限制与信号处理只作用于测试子进程，退出前清理其独占目录。构建成功，`--gtest_filter=VLog*` **20/20 通过**，零失败／禁用。源码哈希稳定，未干预 test_lsm，未运行阶段②③尚未完成的测试。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-vlog-stage1-final-js3muu44`，包含命令、构建／测试日志、GTest JSON、业务源码前后哈希。阶段①完成，整体 25%。

2026-11-15 阶段②启动：用户明确说“确定计划并开始执行”后正式选择保留引用长度。四参构造草案已核对成员默认值和 Engine 三处调用（均先判断阈值非零及 VLog 有效），无需改接口／成员；独立临时文件语法检查通过。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-constructor-draft-m38si_ra`。未修改业务源码，未运行构造功能测试，未将骨架已有函数体计为本轮完成。

2026-11-15 构造草案修订：用户指出默认初始化后再赋值的额外过程，要求重新改为直接初始化。现已改为成员初始化列表直接构造，包括 BloomFilter；阈值 0／有效 VLog／非零启用的行为与上一版一致。独立复核和语法检查通过，证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-direct-constructor-mumvat_l`。此草案替代之前的委托版本；助手仅维护文档与临时草案，未覆盖用户正在修改的 src/sst/sst.cpp，功能测试待用户加入后执行。
