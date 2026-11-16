# KopiDB 当前计划

最后更新：2026-11-16。

## 目标：Lab 7 WiscKey 键值分离

按用户确认的顺序完成 **7.1 VLog → 7.2 SST 支持 → 7.3 Engine 集成**。MemTable 继续保存完整 value；启用 WiscKey 时，将超过阈值或恰好等于引用长度的非空原值分离到 VLog，SST 保存 12 字节位置引用；读取返回原值，合并后仍可正确读取，并验证值分离的实际效果。2026-11-15 用户明确确认执行保留长度方案，不添加类型字节。

用户手写业务源码和头文件，助手逐函数提供中文注释、检查实现、补测试及维护记录。已有函数体也须按 Lab 独立推导；已验收的无关逻辑不整段重写。Redis 移除的独立代写授权已结束。

进度：**50%（2/4 阶段验收）**。阶段①②已验收。阶段③配置保存已检查，配置3/3、Engine初始化2项及真实开启分离的公开刷盘1项，共6/6通过。gen_sst_from_iter 正常合并专项2/2通过，下一函数为 LSMEngine::clear 的联动验收；引用复用优化紧接本 PLAN 完成后开展，排期与评估见 STATE/AUDIT。

## 阶段与验收标准

| 阶段 | 按顺序处理的函数／工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① Lab 7.1：VLog | `VLog::open` → `append` → `read_value`；复核直接相关辅助函数 | 新建为空、重开不截断；多条记录可按返回偏移读回，二进制内容不变；读取时核对长度和 CRC，损坏／截断明确报错；偏移和编码长度正确，共享文件访问正确同步 | 已验收，20/20 |
| ② Lab 7.2：SST 支持 | 已确认保留引用长度 → WiscKey 版 `SSTBuilder` 构造 → `add` → `build` → `SST::open` → `resolve_value` | 12 字节原值强制分离、读端只识别恰好 12 字节引用；阈值／墓碑／普通模式正确；构建及重开后还原原值，版本号不变 | 已验收，本轮组件及普通回归49/49 |
| ③ Lab 7.3：Engine 集成 | 配置保存联动 → 核查 `LSMEngine` 构造 → `flush` → `gen_sst_from_iter`；复核清库与引用传递 | 启用／禁用两种配置均可用；真实刷盘、合并、重开及清库后再次写入正确；合并读取完整值后按当前阈值重新编码，验证结果并记录 VLog 写入量；状态通知及回滚过滤不退化 | 配置/初始化/flush/合并通过，待清库验收 |
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

- [x] 四参 `SSTBuilder` 构造函数：用户采用委托版，已去除重复 Bloom／clear 并保存三个 WiscKey 成员。阈值 0 保持普通模式、非零但缺 VLog 报错、1/12/13/64 阈值与两种 Bloom、普通构造均通过 4/4 专项测试。不把构造风格作为验收条件；未用尚未完成的混合长度读取判断构造正确性。
- [x] `SSTBuilder::add`：用户已引入 VLOG_REF_SIZE，并实现超过阈值或等于引用长度时分离；引用仍为 offset(8B)+value_size(4B)。4/4 新测试验证原始 Block 编码、引用读回、阈值与关闭模式、墓碑和同 key 多版本分组；构造 4 项回归同时通过。读取解引用规则尚未配套，不将写入测试算作混合读取已完成。
- [x] `SSTBuilder::build`：用户已在创建 SST 文件前刷新 VLog 并复用 footer 常量；正常双模式字段／依赖、空表拒绝、真实刷新失败且未创建 SST 共 3 项测试通过。无需联动修改 MemTable 或 VLog。
- [x] `SST::open`：按用户确认的首版范围，独立对照 build 核对现有实现，24B／26B 正常文件的字段、metadata、Bloom、首尾键、版本范围、缓存及 VLog 绑定均已具备。两模式×有无 Bloom 的正常重开和独立 VLog 依赖持有已实测通过，无需为了形式重写逻辑。损坏检测延期安排见 STATE，不计为已解决；13～阈值内联值的完整读取仍待下一项。
- [x] `SST::resolve_value`：用户已改为 `!= VLOG_REF_SIZE`，普通模式／墓碑保持原样，只有 12B 引用进入 VLog；新的四项读取测试和原组件测试均通过，缺 VLog 仍在实际解引用时明确报错。
- [x] 阶段验收：覆盖 11/12/13、63/64/65 等二进制长度、阈值 1/12/13/64 与关闭模式、墓碑、全宽版本、快照／回滚过滤、跨块与范围遍历、构建与独立重开。新 WiscKey SST 组件 20 项和原 test_sst 29 项均通过；没有纳入延期的 SST 文件损坏强化要求。

## ③ Engine：当前步骤与必要联动

- [x] `TomlConfig::saveToFile`：用户已补保存 lsm.wisckey.WISCKEY_VALUE_THRESHOLD，配置3/3通过，包含非默认1536读取及重新生成配置后字段保留。仅补已有字段，不增加接口；现有配置一直存在时原来也不受遗漏影响。
- [x] `LSMEngine::LSMEngine`：现有无条件打开VLog并传给SST::open的实现通过2项测试。验证新库创建空VLog、当前阈值0时加载预建普通/WiscKey SST，原构建对象全部销毁后由新Engine恢复依赖，核对层级、下一ID、全宽版本和12B二进制原值/65B大值/短值。此项直接验证构造职责，不冒充公开DB跨配置完整切换测试。
- [x] `LSMEngine::flush`：现有接线通过真实非零配置的公开LSM集成测试。独立子进程加载阈值64，写入11/12/13/64/65B二进制值及删除，显式flush_all后VLog非空、SST模式1，get/get_batch/遍历及正常重开均返回正确原值和版本。未触发compaction，不将此项记作合并已验收。VLog刷新由build完成，不在Engine重复刷新。
- [x] `LSMEngine::gen_sst_from_iter`：现有首次与换表后的 builder 均正确携带阈值／VLog，真实版本和同 key 分组保留；2/2 新测试验证真实开启分离的 L0→L1 与 L1→L2、多张输出 SST、12B 原值／内联／大值、墓碑、历史快照、回滚过滤及正常重开。无需重写业务代码。当前仍解引用并重新追加：两次实际合并观察 VLog 为 880→1760→2640B；测试只记录变化，不要求以后必须重复写入。
- [ ] `LSMEngine::clear` 联动验收：沿用已有独占维护及 VLog 重建流程，检查清库后再次写入／刷盘／重开；不自动列为源码重写任务。

当前配置、构造与 flush 已验证。2026-11-16 用户确认先完成原计划，本轮验收现有 gen_sst_from_iter 路径，已有正确业务逻辑不为形式重写；整阶段仍未完成。

## ②③ 仍需核对的衔接

这些属于本章实现设计，尚不当作已完成模块的独立 bug，也不要求当前提前修改：

1. **混合存储规则已实现并完成组件验收。** 以上述保留引用长度规则为准；不继续实施类型字节草案。构造、写入和读取必须对模式解释一致，不能声明 WiscKey 模式后又将 12 字节原值静默内联。
2. **合并拿到的到底是什么。** 本地 `SstIterator::update_current` 会调用 `resolve_value`，`gen_sst_from_iter` 消费的可能是完整值。教程中“迭代器透明还原 value”与“合并直接搬 12 字节引用”的描述不能同时直接套用。本轮按实际完整值路径验收，不宣称已经复用引用，不在当前阶段改造迭代器接口。
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

2026-11-15 构造最终验收：以用户最终选择的委托版为准，代码检查与构建通过；`SSTBuilderWiscKeyTest.*` **4/4 通过**，覆盖 0 阈值传空／有效 VLog 时普通格式及 12 字节原文重开、非零空依赖报错、1/12/13/64 阈值与两种 Bloom 的依赖和阈值传递、调用方释放 VLog 指针后读回、两参普通构造回归。均用独占目录；正阈值只测试 <12 字节内联值和严格超阈值的分离值，不将尚未完成的保留长度规则算作通过。业务源码哈希稳定，未运行 test_lsm 或完整 WiscKey 集成测试。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-constructor-review-5a4zz45a`。同目录的 add 草案通过独立语法检查，尚未写入业务源码或功能验收。

2026-11-16 add 验收：构建成功，`SSTBuilderAddTest.*:SSTBuilderWiscKeyTest.*` **8/8 通过**（新增 add 4＋构造回归 4）。新增用例直接读取原始 Block：阈值 64 覆盖 0/11/12/13/63/64/65；小阈值 1/12/13；普通及阈值 0 保持 12 字节／65 字节原文；跨块、同 key 五个版本及墓碑、超过 32 位的事务号和快照选择均正确。引用独立解析后通过 VLog 读回，测试显式刷新，不借用未完成的 resolve_value/build 刷新衔接。独占临时目录运行，业务源码哈希稳定，未运行 test_lsm。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-add-review-hhj0lwdp`，含命令、构建／测试日志、JSON、哈希及 build 草案语法检查记录。build 的刷新先后和错误传播待用户实现后验收。

2026-11-16 恢复首版主线验收：用户已明确将新发现的损坏检测问题延期，本轮仅验证原计划 build/open 正常行为及既有刷新失败传播。新增 3 项 SSTBuilderBuild 测试，运行 `--gtest_filter=SSTBuilder*` **11/11 通过**（构造 4、add 4、build 3）。双模式×有无 Bloom 检查 footer、首尾键、块数、完整事务号、模式和普通读取，独立重开 SST/VLog 并释放调用方指针后仍可读回；空 builder 拒绝；子进程 add 成功后用文件大小限制使 build 的 VLog::sync 失败，确认异常前缀及 SST 尚未创建。源码哈希稳定，未增加字段损坏测试，未运行 test_lsm。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-build-open-review-ewz356f7`。open 现有正常路径通过复核，无需逻辑改写；其过强的校验注释为非阻塞文字整理，不据此扩大完整性承诺。resolve_value 的单行草案仅通过语法检查，混合长度读取待用户实现后验收。

2026-11-16 阶段②最终验收：用户 resolve_value 精确引用长度判断检查通过。新增4项 SSTResolveValueTest：混合长度各读取入口与重开，小／零阈值和普通构造，跨块快照／回滚／墓碑／全版本，缺 VLog 的原有延迟报错。5项旧SST组件用例仅迁移独占目录，名称及原断言保留，PutRemove原1→2插入次序保留；3项Engine用例逐字未改、留阶段③。构建 test_wisckey/test_sst 成功，隔离目录实际运行 SST*:WiscKeyPutGet.*:WiscKeyPutRemove.* **20/20** 与 test_sst **29/29**，合计 **49/49**，零失败／禁用。没有重跑VLog20项或test_lsm；没有新增延期的SST损坏用例。测试前后业务源码与测试哈希稳定；构建后只澄清新增测试的一句注释，执行逻辑不变。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-lab72-final-iay2qss5`，含构建日志、两套件JSON/日志、summary.json及哈希。阶段②完成，整体50%。同目录配置保存草案仅语法检查通过；test_config新增保存阈值断言尚未运行，等待用户完成下一函数。

2026-11-16 阶段③入口验收：用户配置保存修改检查通过。构建 test_config/test_wisckey 成功；独立目录运行配置3/3、新Engine初始化2项＋真实WiscKey公开刷盘1项3/3，合计6/6。新测试直接加入test/test_wisckey.cpp，公开刷盘在threadsafe子进程首次加载阈值64，正常析构后清理目录，不增加setter。构造关闭模式测试使用合法预建SST并销毁原依赖，阈值0由新Engine加载；公开刷盘测试同一非零配置下关闭重开，不混同为跨配置公共API测试。业务源码哈希稳定，未运行test_lsm或尚未决定范围的合并优化。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-wisckey-entry-khowakuj`，包含构建日志、两套件命令/JSON/日志、summary与源码哈希。

2026-11-16 合并专项验收：`xmake build -j 2 test_wisckey` 成功，隔离运行 `WiscKeyCompactionDeathTest.*` **2/2 通过**，零失败／禁用。两项均通过公开 LSM 事务及 flush_all 触发真实合并，非零阈值64；39个初始key、历史修改／删除／RU取消和少量触发记录，检查多张WiscKey输出表、点查／批查／快照遍历及正常关闭重开。触发记录为11B内联值，本身不新增VLog原值；L0合并观察880→1760B，深层场景随后1760→2640B，中间无合并刷盘保持1760B。测试没有把重复增长写为功能断言，便于后续优化对比。业务源码哈希稳定，没有运行或干预test_lsm。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wisckey-compaction-review-x2qi3o3f`，包含构建、命令、日志、GTest JSON、源文件前后哈希和summary。下一步是clear联动验收，阶段③尚未完成。
