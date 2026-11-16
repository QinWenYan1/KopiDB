# KopiDB 当前计划

最后更新：2026-11-17。

## 目标：A15 合并时复用已有 VLog 引用

问题与证据见 [AUDIT A15](AUDIT.md#a15)。上一轮 Lab 7 已完成约定范围的验收，结果归入 [STATE](STATE.md)。本轮只优化合并的数据传递路径：目标 SST 仍启用 WiscKey 时，已有 VLog 引用直接写入新 SST，不为搬运引用读取或重新追加原值；普通查询仍返回完整 value。

优化前基线已采集；阶段①②已验收。阶段②通过创建入口、读取模式、类型标志、快照与回滚等专项及普通SST回归，共52/52通过。阶段③merge和Concact相关修改已检查；类型转发专项与相关回归8/8通过，下一函数为TwoMergeIterator::is_vlog_ref。进度：**33.3%（2/6 阶段验收）**。用户手写业务源码与头文件，助手逐函数提供带中文注释的实现、检查、补测试和维护记录；一次只推进一个业务函数。

## 已核对的设计与边界

1. **分开表达读取模式与版本保留。** 在 `include/iterator/iterator.h` 增加 `ValueReadMode { Query, Compaction }`。Query 返回原值，Compaction 返回 SST 实际存储的字节；默认 Query，不能借用 keep_all_versions 控制值解引用。模式只在创建迭代器时设置，seek/seek_first/++ 继续沿用成员。
2. **类型标志只存在内存中。** BaseIterator 新增有默认实现的 `is_vlog_ref()`，默认 false；SearchItem 增加默认 false 的标志，相关派生迭代器转发当前记录的类型。保持 pair<string,string> 的现有返回类型和磁盘编码。
3. **标志说明当前输出值，而非底层原来如何保存。** 仅 Compaction 模式＋来源 SST 为 WiscKey＋存储值长度为 VLOG_REF_SIZE 时为 true。Query 输出、普通 SST 的12B原值、墓碑、空/end 均为 false。比较规则仍只依赖 key、真实版本和来源优先级，类型标志不参与排序／相等比较。
4. **整条合并输入保持 Compaction。** TwoMerge 的选路会通过子迭代器 operator* 取得 key，不能只在 gen_sst_from_iter 最后一步换成 raw getter。SstIterator 的 value/*/-> 保持一致，Concact 换 SST 时继续传递模式。
5. **两个写入口共用原有组块逻辑。** `SSTBuilder::add` 继续接收原值；新增 `add_vlog_ref` 接收已有引用，经共用的私有 `add_stored_entry` 完成 Bloom、首尾 key、版本范围、组块与换块。引用入口检查目标模式和12B长度，不把引用本身再次 append 到 VLog，也不为了验证引用而读取原值。
6. **引用归属沿用现有 DB 结构。** 复用的是同一数据库当前共享的 `vlog.data` 中的引用，不扩展为任意跨数据库复制。目标仍为正阈值时，既有引用保持引用；新遇到的内联原值按当前阈值决定是否分离。目标阈值0时，两个合并入口使用 Query 模式，生成保存原值的普通 SST。
7. **共享引用长度常量。** 将 `src/sst/sst.cpp:26` 的 VLOG_REF_SIZE 移到已需修改的 `include/sst/sst.h`，供写入与类型判断共用；该头文件需要包含 iterator/iterator.h，以使用 ValueReadMode 的默认枚举值。

## 全部阶段及验收标准

| 阶段 | 具体函数（阶段内仍逐个处理） | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 内存类型信息 | BaseIterator::is_vlog_ref（新增默认实现）→ SearchItem 构造函数 → HeapIterator::is_vlog_ref（新增） | 旧调用默认非引用；堆推进后类型与当前条目对应；空堆安全返回false；原排序不变 | 已验收，11/11通过 |
| ② SST 存储值读取 | SstIterator 顺序构造 → update_current → value → is_vlog_ref（新增）→ SST::begin；共享VLOG_REF_SIZE | 默认查询返回原值；Compaction返回原始字节且不读VLog；普通12B原值不误判；跨块/seek/结束状态保持规则 | 已验收，52/52通过 |
| ③ 合并中传递类型 | SstIterator::merge_sst_iterator → ConcactIterator构造 → operator++ → is_vlog_ref（新增）→ TwoMergeIterator::is_vlog_ref（新增） | L0堆、跨SST拼接与双路归并均保留字节及类型；比较与推进不触发解引用；版本和来源优先级不变 | 进行中，Concact已检查，待TwoMerge类型getter |
| ④ Builder 复用引用 | add_stored_entry（新增私有）→ add → add_vlog_ref（新增） | 引用直写保持字节与VLog尾位置不变；普通12B原值仍正确分离；原组块、Bloom、版本范围和墓碑规则通过回归 | 待开始 |
| ⑤ Engine 接通 | full_l0_l1_compact → full_common_compact → gen_sst_from_iter | 启用模式的两级合并复用引用，换输出表后仍正确；普通/WiscKey混合可合并；禁用后读出原值并生成普通SST | 待开始 |
| ⑥ 最终回归与效果比较 | 组件、公开API、事务／恢复回归及原合并基线复测 | 无隐式VLog读取证据成立，已有引用不新增VLog数据；正常值/版本/墓碑/回滚/重开正确；报告VLog新增字节和同条件耗时差异，不泛化吞吐提升 | 待开始 |

百分比按通过验收阶段数计算，不代表工时比例。阶段①～④可以分别编译和组件验证；阶段⑤才启用真实 Engine 优化路径，不能提前把局部组件通过写成整条链路完成。

## 函数清单与准确起点

以下行号对应已核对、远程可访问的基线提交 `b774ee6f4b74a0f960440eed20077e319ed617c6`；用户修改后重新定位。函数声明和定义合计为一处函数，新增成员／枚举／常量另外列出，不算函数。

1. `include/iterator/iterator.h`：新增 BaseIterator::is_vlog_ref 默认实现；修改 SearchItem 五参构造，尾部增加默认非引用的参数与成员；声明 HeapIterator::is_vlog_ref；增加 ValueReadMode。
2. `src/iterator/iterator.cpp`：实现 HeapIterator::is_vlog_ref，读取当前堆顶类型，空堆返回false。
3. `include/sst/sst_iterator.h`、`src/sst/sst_iterator.cpp`：顺序构造（:98）增加末尾默认读取模式并保存；update_current（:357）、value（:243）沿用模式；新增is_vlog_ref；merge_sst_iterator（:373）消费迭代器当前模式输出并把标志传入SearchItem，不再自行无条件resolve。
4. `include/sst/sst.h`、`src/sst/sst.cpp`：SST::begin（:269）新增末尾默认模式并传给构造；共享VLOG_REF_SIZE；SSTBuilder::add（:350）提取add_stored_entry共用写块逻辑，并新增add_vlog_ref入口及声明。
5. `include/sst/concact_iterator.h`、`src/sst/concact_iterator.cpp`：构造（:8）保存并转发读取模式；operator++（:36）换表和空哨兵构造沿用模式；新增is_vlog_ref，转发当前有效SstIterator。
6. `include/lsm/two_merge_iterator.h`、`src/lsm/two_merge_iterator.cpp`：新增is_vlog_ref，按choose_a转发当前选中子流，不重新比较或修改当前位置。
7. `src/lsm/engine.cpp`：full_l0_l1_compact（:824）、full_common_compact（:871）按目标模式创建Compaction/Query输入；gen_sst_from_iter（:902）在推进前读取当前类型，分别调用add_vlog_ref或add，保留真实版本、同key不拆表及非空收尾逻辑。

合计 **11个业务文件、19处函数级新增／修改**。多数转发是小改，不表示19个函数全部重写。SstIterator按key构造保留成员默认Query；现有key、seek、seek_first、++及TwoMerge选路/推进算法仅联动核查，不预先列为必改。SkipList、MemTable、Level、Block和WAL没有本轮必要的业务改动。

## 定向验收设计

- **数据路径不读VLog：** 合法源WiscKey SST以SST::open(..., nullptr)打开，保留其模式和引用；输出builder仍持有原合法VLog。Compaction的构造、比较、value/*/->、跨块、跨SST、Heap与TwoMerge全链路应成功；同源Query访问引用应报缺少VLog，作为负对照。输出SST绑定VLog后读回原值，证明没有为通过测试丢失数据。无需新增生产计数器。
- **引用不重写：** 比较合并前后原始12B引用字节以及VLog尾位置；读取原值相同本身不能证明没有重复append。保留输入内联值且不新增待分离原值的可控场景。
- **长度与来源：** 普通SST的12B原值、WiscKey的12B引用、小值、大值、墓碑混合，覆盖跨块／跨SST／多张输出表；同key版本降序和同版本来源优先级保持。
- **配置变化：** 独立进程/配置准备来源，验证正阈值升降与阈值0；已有引用在启用模式下继续复用，普通原值按新阈值处理，禁用模式还原完整原值。不能把整体VLog尾不增要求套到新分离原值的场景。
- **端到端：** L0→L1、L1→L2、反复合并、点查／批查／遍历／谓词、快照、回滚、正常重开及已有WAL恢复回归。延续原12B编码规则，不更改用户接口。
- **效果基线：** 现有WiscKeyCompactionDeathTest场景为880→1760→2640B；触发记录是11B内联值，不额外分离。优化后同一场景预期保持880B；这是空间和重复写入对比，不是吞吐量提升倍数。基线证据见AUDIT A15。

## 优化前后量化对比

阶段与业务修改范围不变；在阶段⑥补齐量化验收。**修改业务代码前先建立并保存计时基线**，这是助手的测试准备工作，不新增业务阶段；优化前专项已完成，结果如下；优化后的对比留在阶段⑥。

1. **已有空间基线：** 同一可控样本初始VLog为880B，两轮合并后2640B，合并额外追加1760B；优化目标是在无新分离值的同一场景中保持880B、额外追加0B。它是文件新增字节数，不是物理磁盘I/O统计。
2. **计时负载：** 初始采用10000个key、每个原值4KiB（约39.1MiB），阈值64，预建四张L0 SST；11B内联触发记录避免混入新原值的分离。每轮都用相同未合并输入的独立目录，不能连续测已经合并过的数据。
3. **计时边界：** 用steady_clock记录触发合并的flush调用耗时。输入生成／复制、数据库打开、触发记录写入、读回校验均在计时外。该数值包含合并和少量固定刷盘开销，报告时不叫作纯合并函数耗时；无需公开私有compact函数。
4. **公平对照：** 同一机器、相同编译模式、配置、数据和运行方式；预热1轮、正式测5轮，报告中位数和最小／最大值。若过短难以比较，再把两边负载统一扩大。此方案不声称测量冷缓存性能，不清理系统缓存或影响其他进程。
5. **保留证据：** 保存优化前二进制、源码版本／哈希、配置、输入说明、运行命令、每轮耗时与VLog增量；优化后复用同一测量程序和负载。旧小样本整项GTest耗时含配置、写入、重开等步骤，不拿它替代本项耗时基线。
6. **分别证明读与写：** VLog不增长不能单独证明没有读取原值；无VLog依赖的Compaction路径测试仍须通过。耗时改善按实测值计算，不预先承诺倍数；当前已知的是重复追加可以消除，不能推广为整个DB的读写放大降低100%。

### 优化前实测基线（2026-11-16）

新增测试 `WiscKeyCompactionBenchmarkDeathTest.MeasuresReferenceCompactionCost` 位于 `test/test_wisckey.cpp`，构建成功、定向运行1/1通过。Debug，四张L0，10000条唯一key×4096B值，阈值64；底层Engine绑定管理器提供查询状态，不启动WAL，测量不包含事务/WAL开销。

| 正式轮次 | flush耗时(ms) | VLog新增字节 |
| --- | ---: | ---: |
| 1 | 3186.886583 | 41200000 |
| 2 | 3160.391500 | 41200000 |
| 3 | 3181.784583 | 41200000 |
| 4 | 3167.836667 | 41200000 |
| 5 | 3180.269917 | 41200000 |

中位数 **3180.269917ms（约3.180s）**，范围3160.391500～3186.886583ms；预热轮3163.859125ms不计入。每轮VLog从41200000B增长到82400000B，额外追加约39.3MiB。所有轮次在计时外核对全部10000条原值、真实版本、触发记录和合并层级。整个GTest约30.745s，不能拿它代替flush耗时。

证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-baseline-ike5lafd`，保存旧测试二进制、测试源码、配置、构建／运行命令、GTest JSON、逐轮日志与统计、业务源码前后哈希；源码稳定。基线业务版本为b774ee6f4b74a0f960440eed20077e319ed617c6，新增基准程序已单独归档。暂未采集优化后结果，不预承诺加速倍数。

## 当前执行位置

- [x] BaseIterator::is_vlog_ref：用户实现为const虚函数，默认false且不读取当前元素；代码核对及 `xmake build -j 2 test_lsm` 通过，全部相关派生类无须新增override即可编译。本轮仅检查／编译，未新增镜像默认返回值的测试，也未重跑业务套件。
- [x] SearchItem构造：用户已添加默认false成员、尾部默认false参数及对应初始化；默认构造和已有五参调用兼容。Base注释已改为当前记录类型的准确说明。构建成功，现有IteratorMvccTest.*回归9/9通过，来源比较和版本／墓碑行为保持。
- [x] HeapIterator::is_vlog_ref：用户已实现const override，先检查堆非空再读取items.top().is_vlog_ref_。新增排序／来源优先级／推进、版本过滤／墓碑过滤两项组合测试，通过BaseIterator引用检查当前标志，并覆盖默认构造、普通12B原值、引用、空堆及耗尽后++。构建与IteratorMvccTest.*共11/11通过，阶段①完成。

检查证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-base-getter-review-5g73f_b9`，包含构建日志、源码前后哈希与summary；业务源码在检查期间稳定，助手未代改。已核对现有生产与测试的五参创建方式，新增末尾默认参数兼容现有调用。issue已在聊天交付、未发布；继续用户手写、助手检查及补测试的方式。

注释澄清：is_vlog_ref()报告当前输出记录的value类型，++后对应新记录；SearchItem::is_vlog_ref_保存每条记录的类型。WiscKey SST的原始存储值仍可按12B长度识别引用，约定不变。已在聊天重新给出职责明确的Base注释及SearchItem草案；此段为此前澄清记录；最新SearchItem实现现已检查，正进入Heap getter实现。本轮为注释／草案讲解，未改业务源码或重跑测试。

SearchItem检查证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-searchitem-review-8ewlo027`，包含构建／运行命令、日志、GTest JSON及源码前后哈希。9项既有回归通过，源码在检查期间稳定，助手未代改业务。引用类型转发的新增组合测试留到Heap getter实现后运行，不把当前9项通过当作整条引用链路已接通。

阶段①验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-heap-review-4b0txle4`，含构建／运行命令、日志、GTest JSON、源码前后哈希。用户业务实现未由助手改写；助手仅新增2项组合测试，检查期间源码稳定。本轮未重跑性能基准或其他业务套件。

阶段②当前草案：在include/iterator/iterator.h的IteratorType后增加ValueReadMode{Query,Compaction}；在include/sst/sst_iterator.h的is_aborted_之后增加默认Query的value_read_mode_成员；顺序构造声明末尾增加默认Query参数，cpp构造在初始化列表保存该参数后继续seek_first。按key构造沿用成员默认值，不改接口。已只读核对seek_first仅定位Block，不会解引用VLog；本轮仅配置保存，实际Compaction读取等待update_current/value后续实现，不能提前记为读取优化通过。

阶段②顺序构造验收（2026-11-16）：用户采用 `ValueReadMode::Query / Compaction` 命名，枚举、默认成员、顺序构造声明及定义一致，参数在初始化列表保存；按key构造沿用默认Query。构建test_sst/test_wisckey成功，test_sst全量29/29和SSTResolveValueTest四项4/4，共33/33通过。此处验收参数保存与默认查询兼容，尚未实现Compaction按模式取值。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-sst-ctor-review-nfjk4owe`，源码／测试在检查期间稳定，助手未代改业务。

update_current已完成（2026-11-17）：用户仅在Query时resolve，成功后移动到cached_value；Compaction保留原始存储字节。新增SSTIteratorReadModeTest两项验证普通/WiscKey两模式的*/->、12B原文/引用、内联值、墓碑、全宽版本，及没有VLog依赖时跨块、seek、seek_first与重复读取的缓存更新；Query读取同一引用抛错作负对照。现有缓存失效逻辑可以沿用。

缓存验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-sst-cache-review-54mo13rt`。test_sst与test_wisckey构建成功，最终test_sst29/29、SSTResolveValueTest4/4、SSTIteratorReadModeTest2/2，共35/35通过。检查中用户提交了源码格式调整，因此按新源码重建复测；前一次29项结果另存test_sst-before-format，不重复计数。最终运行期间业务与测试哈希稳定；助手补2项测试，未代改业务。未调用尚未接通的Compaction.value()或Sst类型getter，不将此项验收扩大为整个合并链路已优化。

value已完成（2026-11-17）：用户先用is_valid拒绝无效位置，再复用update_current并返回cached_value->second的副本。已扩展现有模式测试，使value先填缓存再由*/->验证，随后重复value确认未移空缓存；增加耗尽/未命中时报错及Query无VLog负对照。SSTIteratorReadModeTest与SSTResolveValueTest共6/6通过，三取值入口现已统一。operator*中无条件resolve的旧两行注释仍待文字同步，不影响本项功能验收。

value验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-sst-value-review-ksh2v9p1`，含构建／测试命令、日志、GTest JSON、源码前后哈希，运行期间源码稳定。助手扩展已有2项模式测试，未代改业务；本轮只运行相关6项，没有重复全量或性能基准。

SstIterator::is_vlog_ref已完成（2026-11-17）：用户实现Compaction＋有效位置＋WiscKey来源的前置判断，再填缓存检查引用长度。VLOG_REF_SIZE已移到sst.h的tiny_lsm命名空间inline constexpr，cpp旧定义删除。扩展现有模式测试，经BaseIterator接口在读取value前后核对类型；覆盖普通12B原文、Query原值、墓碑、空/end、无VLog依赖的引用、跨块及seek后位置变化。共享常量涉及的SST构建／读取组件共22/22通过。

类型getter验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-sst-ref-review-73y0mgdy`，包含构建／运行命令、日志、GTest JSON、源码前后哈希。助手只扩展现有测试，未代改业务；本轮没有运行性能基准或全库回归。类型检查在Compaction中不会解引用；阶段②仍待创建入口接通，不把当前组件结果当作Engine已优化。

SST::begin已完成（2026-11-17）：用户已直接include iterator/iterator.h，声明末尾默认Query，cpp把模式和既有读取条件一并传给新SstIterator。现有两个模式测试改为通过begin创建；新增BeginPreservesModeAndMvccConditions覆盖快照上限、回滚回调、保留全部版本、墓碑、跨块、不可见结果为空及Query/Compaction都与原end哨兵正确比较。

阶段②验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-sst-stage2-review-tu0sghvj`。test_sst和test_wisckey构建成功；普通SST29/29＋WiscKey SST组件23/23，共52/52通过（模式专项现在3项）。业务与测试在检查期间哈希稳定，助手未代改业务；未重跑基准或Engine全链路，不把SST层完成当作整个优化完成。

merge_sst_iterator已完成（2026-11-17）：用户已改为读取*iter及当前类型，移动临时key/value并保存标志，保留来源ID、真实版本、墓碑与keep_all_versions，移动返回堆。新增SSTMergeReadModeTest两项验证Query原值、Compaction原始字节和标志、普通/WiscKey混合、同版本来源优先、全版本/去重、墓碑、空输入及没有VLog依赖时仍可合并。输入沿用既定读取模式，未增加模式参数；旧tranc_id未使用的行为保持。

merge验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-merge-review-zco_zyoo`。test_lsm/test_wisckey构建成功；WiscKey合并／模式／原值读取9/9，迭代器及LSM遍历相关14/14，共23/23通过。助手新增2项测试，未代改业务；源码和测试稳定。没有重跑计时或完整Engine优化验收。

ConcactIterator构造已完成（2026-11-17）：最新用户实现正确保存模式，循环给候选SST的begin传入同一参数，结束状态构造补齐回滚回调和读取模式，并移动参数SST列表到成员。初始空cur_iter沿用默认Query是占位状态，选表时会被正确模式的新对象替换，不构成功能错误。新增两项构造测试覆盖前表超快照／回滚、首张可见表12B引用、无VLog的Compaction值、默认Query、空输入／全部不可见及有效墓碑；不提前验收跨SST++或Concact类型getter。

Concact构造证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-concat-ctor-review-pfg5ssqf`。检查中用户及时修正了末尾参数顺序并改用移动列表，实际构建成功；按最终源码完成test_lsm旧Concact回归5/5及test_wisckey构造／SST模式／合并7/7，共12/12通过。保留初次读取与最终检查的哈希以说明期间用户修改，最终运行源码和测试稳定。助手只补测试，未代改业务。

ConcactIterator::operator++已完成（2026-11-17）：用户在创建下一张SST迭代器和空结束迭代器时均转发is_aborted_与value_read_mode_。新增IncrementKeepsModeAcrossMixedAndInvisibleSsts：七张不重叠SST包含跨块、多版本、连续超快照/回滚表、普通12B原文、有效墓碑和末尾不可见表；Query/Compaction×是否保留全部版本均正确，Compaction源不绑定VLog以证明跨表不解引用，耗尽和空输入再次++安全。未提前断言尚未实现的Concact类型getter。

Concact跨表验收证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-concat-increment-review-srg723iv`。test_lsm/test_wisckey构建成功，默认Concact回归5/5及模式／合并／拼接专项8/8，共13/13通过；业务和测试哈希稳定，助手仅补1项组合测试，未代改业务。未重复性能或全库回归。

ConcactIterator::is_vlog_ref已完成（2026-11-17）：用户已添加const override并直接转发cur_iter的类型判断。扩展现有3项ConcactReadModeTest，经BaseIterator接口在读取value前后核对标志，覆盖Query原值、Compaction引用、普通SST的12B原文、墓碑、无VLog源、跨块/跨SST、快照/回滚过滤、空/end及耗尽后++。未新增重复状态或额外判空。

Concact类型转发证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a15-concat-ref-review-xwgepekj`。test_lsm/test_wisckey构建成功，旧Concact回归5/5＋扩展类型断言的专项3/3，共8/8通过。运行期间源码／测试哈希稳定，助手未代改业务；未重复性能或全库回归。

当前函数为TwoMergeIterator::is_vlog_ref（2026-11-17用户要求下一步）：include/lsm/two_merge_iterator.h:34的版本getter声明后新增const override；src/lsm/two_merge_iterator.cpp:175的get_cur_tranc_id结束后新增定义。沿用choose_a选中子流，先判selected指针和有效位置，无当前记录返回false，否则转发selected->is_vlog_ref()；不重新选路、不增加读取模式或重复缓存类型。草案在聊天交付，待用户手写，阶段③尚未完成。本轮只读核查，未构建／运行测试、未改业务源码。

阶段③收尾测试准备：用户加入TwoMerge getter后，组合验证真实SST生成的Heap＋Concact及Concact＋Concact，两路交替、同key版本/同版本A优先、墓碑、普通12B原值、跨SST和空/end。WiscKey源不绑定VLog，验证从构造选路到推进取值不隐式解引用。空状态使用默认TwoMerge或指向空迭代器的有效shared_ptr，不擅自增加带参构造接受nullptr子流的要求。
