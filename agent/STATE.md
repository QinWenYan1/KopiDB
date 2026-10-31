# KopiDB 项目状态

最后核对：2026-10-31。当前 A1～A4 修复计划见 [PLAN.md](PLAN.md)，问题事实、证据及处理状态见 [AUDIT.md](AUDIT.md)。

本文件记录项目现状、已确认的设计、全局约定、验收边界和整体排期。代码与记录不一致时，先检查代码，再更新记录；不能把讨论通过的方案当作已经实现。[AUDIT.md](AUDIT.md) 是全局问题账本；[PLAN.md](PLAN.md) 只记录当前选定工作、执行顺序、TODO 与具体的验收场景。

`agent/` 保存项目协作资料：`STATE.md` 记录状态，`PLAN.md` 记录当前计划，`skills/` 按技能分别建目录。Lab 教学与验收使用 [逐函数完成 Lab](skills/lab-workflow/SKILL.md)；评估疑似 bug 和修改必要性时，使用 [先验证，再修复](skills/verify-before-fix/SKILL.md)。

2026-10-30 文档职责约定：发现问题先登记 AUDIT，选定后进入 PLAN 成为当前工作；AUDIT 保留稳定编号和证据，实施及验收时更新状态，解决后也不删除原记录。STATE 记录项目层面的结果和约定，通过链接引用问题，不重复维护完整问题清单。具体流程见 [audit-to-plan](skills/audit-to-plan/SKILL.md)。

2026-10-30 用户补充：每次进入一个新修复问题，先在聊天中起草 issue，由用户手动发布；同一问题内不逐函数重复起草，也不以发布作为实现的前置条件。章节使用左侧目录编号＋名称：目录 5.1.2「事务管理器」对应文章标题 Lab 5.2；目录 5.2「WAL 和崩溃恢复」尚未开始。后续记录按此定位，历史文章标题不能当作目录编号。

## 当前进度

2026-10-31 用户单独授权助手修复基础文件工具 A1，并要求中文注释与修改说明；其余核心模块仍由用户手写。A1 三个函数已修改并验收，文件写入／刷新错误能够传至刷盘入口，失败保留可查询的内存数据，故障解除后重试成功。相关回归 **197/197 通过**；用户已认可验收并要求进入 A2，当前交付 issue 草稿。整体修复计划仍为 **20%（1/5 阶段）**；A2 未修改业务代码，A3～A4 待开始。执行细节见 [PLAN.md](PLAN.md)，问题状态及测试证据见 [AUDIT.md](AUDIT.md)。

本轮通过数不包含尚未修复的 A2～A4 和依赖后续章节的 11 个 LSM 用例；文件 sync 仍为流 flush，没有据此扩展为 fsync 或崩溃恢复。目录 5.2 的 WAL 与崩溃恢复继续按后续排期推进。

此前审计已完成：原混合隔离用例恢复并新增 3 项回归，4 项均实际失败；额外 File/Bloom、BlockCache、BlockMeta 共 24/24 通过。上述结果是修复前基线，不代表问题已经解决。

原 24 项延期测试现在均已回到 test/test_lsm.cpp，恢复时核对注释与函数体 SHA-256 一致；最后一项也已启用，没有使用 DISABLED_ 隐藏失败。原 test/deferred 的暂存源码、清单及 README 已清理。上一轮 247/247 表示当时的测试范围，不能用它覆盖本轮新增失败。

逐项触发场景、代码位置、复现日志和估时维护在 AUDIT；此处保留项目级验收结果和历史进度。

以下保留上一轮状态维护验收与历史记录。

本轮完成（2026-10-30）：**Lab 5.2 §3.3 事务状态维护 PLAN 已完成，100%（4/4 阶段）**。用户已独立实现五个核心函数，构造函数复测 4/4 通过。随后完成适用延期测试恢复与最终回归：**247/247 通过**。助手未修改业务源码；本轮恢复 19 个延期用例，累计恢复 23/24，仅混合隔离用例继续延期。当前完成范围为状态登记、固定格式读写、正常重启及回滚过滤的相关联调，不代表完整隔离级别、WAL 或 LSM 生产入口已完成。

最终验收日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-final-regression-4rj2s5e5`。五个目标均构建成功；完整 SkipList **31/31**、MemTable **49/49**、Block **35/35**、SST **28/28**，LSM 适用用例 **104/104**。test_lsm 共注册 115 项，11 项依赖后续 Lab 5.5 的初始化／持久化／恢复，沿用既定排期未运行；具体名单和过滤器保存在日志目录。所有测试使用隔离临时目录和独立配置。构造函数单独复测日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-startup-review-y9xoa44t`。

恢复校验：原 24 个延期用例的注释与函数体 SHA-256 全部与清单一致，无重复定义；23 项已进入活动测试，1 项 `TranReadVisibilityTest.RejectsDirtyValuesAndTombstonesBeforeCaching` 留在 test/deferred。本轮新增 `TranCommitTest.RollbackStateSurvivesFlushCompactionAndRestart`，经工厂创建真实事务，验证旧值提交、有效删除、RU 版本刷盘后取消、后续提交推动检查点、真实 L0→L1 整理、正常析构保存和重建组件后的点查／批查／谓词查询。原始 SST 仍保留取消版本，查询依靠恢复的回滚名单排除它，旧值保持可见。

调用链复核：commit 在允许刷盘前登记结束状态；flush 在 SST 构建成功后逐个通知标记 ID；abort 独立登记回滚名单；保存和恢复保留检查点、待处理项及回滚名单。三个刷盘状态 getter 的现有 src 调用位于初始化／WAL 准备，其他当前使用为测试读取，本轮不新增 getter 或改写公开接口。TranManager 析构保存异常已捕获；未测试内存分配失败、断电原子性或 WAL 恢复。LSM 门面的绑定及整体关闭错误处理继续按 Lab 5.5 验收。

2026-10-30 验证：两次 test_lsm 构建均成功。读写相关 10/10 通过，覆盖完整恢复、重复加载、64 位 ID、错误文件、失败保持原状态及保存异常；日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-read-review-klgew016`。现有构造的 4 项启动基线通过，覆盖新库初始化、多次正常关闭重启、坏文件不覆盖、已清理 ready 的回滚 ID 在重启后仍过滤 SST 修改／删除／新增版本；日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-startup-baseline-7eahmsj4`。启动测试显式绑定 Engine 与管理器，并直接构造 SST，不能当作 LSM 生产入口或真实 commit→flush→compaction 全链路已验收；这些继续按原阶段和 Lab 排期处理。

2026-10-29 验证：写入测试覆盖固定字节布局、全部四类状态、缩短重写、实际打开失败和析构兜底，4/4 通过；日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-write-review-l99jwx34`。读取新增用例覆盖完整恢复、重复加载、完整 64 位 ID、非法长度／数量、重复／乱序条目和非法状态码、失败保留原状态，空桩 0/6；日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-read-baseline-3xz62g27`。两次构建均成功，测试均在隔离临时目录运行，未运行其他套件，未执行读取草案。

2026-10-28 验证：对比 transation.cpp 与 4346c5b 的可执行 token 一致，读取上一轮测试结果确认 7/7，不重复执行阶段①。写入空桩基线日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-write-baseline-0y0kc_yg`；4 项检查初始字节布局、全部四类状态、重写缩短和路径被替换为目录时的错误上报／析构兜底。助手未修改业务源码，测试不代表写入草案已经执行。

阶段①验收日志（2026-10-27）：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-stage1-review-3rdydiop`。测试在独立临时目录运行，验证结束登记、并发查询、上下文释放、刷盘乱序和重复、回滚名单保留及真实 commit→flush 衔接；未运行其他套件或注入分配失败。已按用户要求在聊天中重写 add_flushed_tranc_id 的解释性注释，算法保持一致，助手未改业务源码。此前“空桩失败”记录均为历史基线。

本轮复测（2026-10-26）：结束登记构建成功、3/3 通过；回滚名单新增、失败回退顺序和最后移出活跃集合均符合方案，未注入内存分配失败。下一刷盘函数仍为空，新恢复／补充的四项刷盘基线构建成功、0/4 通过，失败对应刷盘集合未更新；不影响已通过的结束登记结论。恢复项哈希一致、无重复定义，未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-registration-review-n6n9ubcr`；`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-flush-baseline-jmknqqde`。

本轮空桩基线（2026-10-26）：`xmake build test_lsm` 成功，在独立临时目录选跑登记、并发查询和上下文释放共 3 项测试，0/3 通过；当前 add_ready_to_flush_tranc_id 仍为空，与失败一致，未改业务源码。未运行其余套件或注入分配失败。两项恢复测试的原始哈希一致，无重复注册；goal-to-plan 的格式校验通过。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-state-registration-baseline-e01mqs7x`。

**Lab 5.2 的本轮事务状态维护任务已完成**。此前已推进 SkipList、MemTable、Block、SST、迭代器和 Engine 的 MVCC 改造；整个数据库的隔离与恢复功能尚未验收完成，延期事项见下文。

此前回滚接入结果：**回滚集合查询接口、存储读取与提交冲突查询的过滤接入已完成**。本轮已补齐两项状态登记、状态文件读写及启动恢复，并通过最终联调；混合隔离仍未完成。

上一轮验收（2026-10-26）：**本轮“回滚过滤代码接入与收尾”已完成，100%（6/6，按用户最终缩小的任务范围）**。Engine::get 两处旧注释和 abort 头部说明已检查正确；相较收尾回归记录对应版本，只有注释/格式变化，无业务逻辑或测试语义变化，本轮未重跑。2026-10-25 回归证据为 SkipList 31/31、MemTable 49/49、Block 35/35、SST 28/28、LSM 适用用例 63/63，合计 **206/206 通过**，已重新核对日志。24 个状态依赖用例与延期清单一致，未接入当前 74 个注册用例；另外 11 个依赖后续 LSM 初始化/持久化/恢复的用例未选跑。本轮完成不代表端到端回滚或完整事务已实现；后续进入下方“Lab 5.2 事务状态维护”，恢复延期测试统一验收。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-rollback-final-regression-37a2kvtw`。

2026-10-22 SST 检查：SST::get 已将回调正确传给按 key 构造的 SstIterator，范围/Bloom 检查、读取上限和有效墓碑处理正确。扩展现有测试到公开 get 入口，完整 test_sst 19/19 通过。随后已接通 Engine 候选 SST 查询及 get。

2026-10-23 最新 Engine 检查：get_batch 在用户修正 if (man) 为 if (!man) 后，管理器前置检查及回调传递正确。新增 2 个批量测试，扩展既有跨来源比较到批量入口；构建成功，相关 48/48 通过。覆盖空/非空输入依赖检查、实际取消新增/修改/删除、活跃表/冻结表/SST、连续取消、历史上限、顺序/重复/缺失、墓碑及同版本来源优先级。上一轮 get 的 46 个回归也包含在本轮中。助手未修改业务代码。

当前 SkipList::get、MemTable::get/get_record/get_batch、Block::get_idx_binary/get_value_binary、BlockIterator/SstIterator 按 key 构造、SstIterator::seek、SST::get 及 Engine::sst_get_ 在显式传入回调时会排除已取消版本，保留有效墓碑，相关测试通过。Engine 的公开 get/get_batch 已从管理器取得并传入回调；commit 的内部候选查询也已接通并通过相关测试。MemTable::get 返回的原节点迭代器不保存回调；get_record/get_batch 在锁内复制结果。BlockIterator 两种构造现已保存回调，块内 ++ 自动沿用筛选；SstIterator 按 key 构造已保存回调，seek 沿用成员并传给当前块，++ 跨块与 seek_first 也已正确传递成员回调，普通构造也已保存回调，公开 SST::begin 已通过；SST 谓词范围入口也已通过；Concact 构造及 ++ 跨 SST 也已通过；Level 构造的三路传递也已通过，Engine::begin 的公开遍历也已通过；Engine 谓词查询也已通过。存储读取的回滚过滤已接通；事务内部读缓存和混合隔离的提交状态过滤仍待处理。

2026-10-23 已确认管理器约定：Engine 公开查询必须有有效的管理器，未绑定和已销毁均报错；不再保留未绑定兼容分支，不需要 owner_before。get 已按此实现并通过缺失/失效测试，回调持有 shared_ptr 到查询结束；查询期间的持有关系经过代码检查，未添加生命周期并发压力测试。get_batch 已沿用此约定并通过测试，即使空输入也先检查依赖。Lab 5.5 的生产入口绑定仍按原计划延期，当前测试显式绑定，不以此降低业务查询的要求。

2026-10-20 用户纠正任务范围：撤回把 LSM 构造函数绑定作为当前下一步的安排，组件初始化整合按 Lab 5.5 推进。绑定是引擎使用管理器的依赖，但现有事务测试已显式完成绑定，可以先验证回滚逻辑；本轮不以修复 LSM 初始化入口为前置任务，也不把测试绑定视为生产入口已完成。

已完成 MemTable::begin：第三个参数接收可选 is_aborted。collect 中先跳过空 key，再用 get_cur_tranc_id 取得真实版本，筛选读取上限与回滚状态；收集所有内存表，保留有效墓碑和来源优先级。查询不删除原节点；测试确认刷盘仍能读取事务标记并报告 ID。后续 getter 已允许读取标记版本，begin 的标记过滤仍保留；is_valid 及 Level/SST 的现有过滤未改动。Engine 范围入口尚未传入回调，因此本函数通过不代表完整范围回滚查询已接通。过滤只在收集阶段执行，返回堆持有数据副本，不随之后的 abort 自动重建。

已完成 MemTable::iters_preffix：沿用 begin 的筛选与来源规则，在每张表的前缀区间收集有效候选；空前缀不暴露事务标记，最终由堆去重并隐藏有效墓碑。iters_monotony_predicate 同样已接通回滚筛选并通过测试；尚未完成整个范围查询验收。

2026-10-23 getter 调整已验证：SkipListIterator::get_cur_tranc_id 只检查 !current，允许读取普通记录、墓碑和实际事务标记的版本，空/end 仍抛 runtime_error。is_valid 保持原样；其他迭代器 getter 未改动。新增 2 个测试，完整 SkipList 31/31 通过。查询仍须从结果中隐藏标记；getter 不承担记录过滤职责。

2026-10-24 谓词约定纠正：作者 MemTable 参考直接把 predicate 传给 SkipList；已核对的 Lab 5.2 与本地接口没有“谓词永远收不到空 key”的要求。MemTable 保留直接调用，谓词应对空字符串有定义且满足单调性；空 key 即使命中，也在收集时跳过。此前助手把回调输入防护作为必须修复的 bug 并写入测试，要求过强，现撤回。测试改为验证含事务标记时的实际范围结果，不再限制谓词是否收到标记。Engine 现有入口包装本轮未改；其作用是额外保护上层谓词输入，不要求各下层重复增加包装。

2026-10-24 进一步核对过滤必要性：SkipList 的谓词函数返回原始命中区间，不必单独排除事务标记；MemTable 的谓词函数最后构造的 HeapIterator 默认 skip_delete=true，现有事务标记的 value 为空，也会被堆过滤。因此，就该函数当前的最终结果而言，收集阶段的 key.empty() 判断并非唯一保障，不应列为缺失即错误的必修项；现有判断可保留，减少无用候选入堆。作者参考也未在该函数收集阶段显式过滤空 key。该结论基于当前标记格式、getter 已允许标记节点、回滚判断回调按 ID 查询且可处理标记，以及堆过滤开启；不能推广为所有 MemTable 入口都可依赖墓碑过滤，例如 begin(..., false) 会关闭该过滤。本轮仅核对代码与参考，未改业务代码，也未运行删除该判断后的测试。

当前排期约束：项目时间紧，优先沿现有结构完成回滚。迭代器职责重构已经讨论，但列为后续事项，本轮只做必要的判断接入与参数传递。

| 位置 | 当前代码状态 |
| --- | --- |
| [TranContext 构造、get、commit](../src/lsm/transation.cpp) | 本轮状态维护与回滚相关调用已接通并通过回归；get 的混合隔离及读缓存修复仍独立延期。 |
| `TranContext::put()` | 已删除旧值查询和备份，更新函数说明，代码检查、构建及对应回归通过。 |
| `TranContext::remove()` | 已独立实现结束状态检查、DELETE 记录、立即/暂存墓碑；新增 4 个测试通过，覆盖重复删除、历史版本、与 put 交替及结束后拒绝删除。 |
| `TranContext::abort()` | 已实现状态检查、回滚登记及上下文清理；点查、批查、范围和 compaction／正常重启后的回滚过滤已通过本轮回归。 |
| `TranManager::getNextTransactionId()`、`new_tranc()` | 已实现 ID 分配、上下文创建和活跃事务登记。 |
| [TranManager 成员](../include/lsm/transaction.h) | 已新增 `aborted_tranc_ids_` 和加锁的 const 查询接口 `is_aborted()`；回滚登记已接入并通过对应测试。 |
| `add_ready_to_flush_tranc_id()` | 已实现结束状态登记、回滚名单维护和成功后移出活跃集合；相关测试通过。 |
| `add_flushed_tranc_id()` | 已实现目标／较小回滚 ID 结算、连续检查点压缩与重复登记保护；阶段①相关 7/7 测试通过。 |
| `write_tranc_id_file()`、`read_tranc_id_file()` | 读写及析构异常捕获已实现，固定布局相关 10/10 测试通过，阶段②完成。 |
| `TranManager` 构造函数 | 用户已独立实现；初始化／恢复 4 项复测及最终回归通过，阶段③完成。 |
| [LSM 构造函数](../src/lsm/engine.cpp) | 已创建 Engine 和 TranManager，互相绑定仍为待办，按用户要求留在 Lab 5.5 初始化整合时处理；当前事务测试自行绑定。 |
| 查询与迭代器 | 存储层回调、Engine 点查/批查/范围与 commit 候选过滤已接通；真实回滚、刷盘、compaction、正常重启链路通过。缓存混合隔离问题未解决。 |

## 已确认的设计

2026-10-28 本轮状态文件实现约定（已按用户要求调整）：取消格式标识，偏移 0/8/16/24 分别为 nextTransactionId、flushed 数量、ready 数量、aborted 数量；整数使用固定 8 字节小端。偏移 32 起依次保存 flushed ID（升序，每项 8 字节）、ready 的 ID 与状态（按 ID 升序，每对 16 字节）、aborted ID（无序，每项 8 字节）。ready 状态编码为提交 0、回滚 1。读取检查当前布局的长度与数据合法性，空文件或非法内容不自动重置 ID；格式未包含运行中的上下文对象。写入先在内存编码，再以截断模式覆盖并检查流刷新结果，当前只验收正常保存／关闭；FileObj::sync 目前是 fstream.flush，并非 fsync，不能将本轮实现描述为已保证断电原子性或完成崩溃恢复。显式保存失败抛异常，TranManager 析构兜底已捕获并记录；LSM 入口整体关闭和恢复流程仍按既定 Lab 排期检查。

跨版本文件识别、兼容和迁移按用户决定暂缓，当前只负责统一格式的读写正确性。取消文件头后，不再要求识别任意旧布局，也不能声称所有旧文件都一定被拒绝；将来实际修改布局且需要保留旧数据时再设计迁移。历史第一版格式与测试证据不作为当前格式的验收要求。

1. **读未提交继续立即写入。** `TranContext::put/remove` 使用本事务 ID 写入存储层；其他隔离级别先暂存，提交时应用修改。
2. **回滚名单由 TranManager 管理。** 采用普通 `std::unordered_set<uint64_t>`，读写均受现有互斥锁保护。当前先接受检查时加锁的成本。
3. **回滚通过排除版本实现。** 事务 5 回滚后，所有 `tranc_id == 5` 的记录都不再作为查询候选，覆盖内存和 SST。原记录可以暂时保留。
4. **继续比较真实版本号。** 保留现有跨 MemTable、SST 选择最大可见版本的逻辑；不改成找到第一个 key 就返回。
5. **先过滤回滚版本，再去重和处理墓碑。** 例如 `k@5 = new` 已回滚、`k@3 = old` 有效，应返回 `old@3`。版本上限为 0 只表示不限制版本大小，仍须排除回滚版本。
6. **Block/SST 记录编码保持现状。** 回滚 ID 随事务状态文件保存。刷盘登记完成不代表旧记录已被物理删除，因此不能顺便清掉回滚名单。
7. **读未提交 put 的冲突检查暂缓。** 当前先完成回滚主线；后续再讨论是否增加这项约束。
8. **迭代器重构不作为本轮前置任务。** 保留现有迭代器组合与推进逻辑，在去重之前接入必要的回滚判断；不要求每个迭代器各新增一套过滤。此前讨论的统一 MVCC 迭代器仍是延期的重构方向。
9. **正式显式事务统一由 begin_tran/new_tranc 创建。** 用户于 2026-10-25 确认；直接构造 TranContext 保留给底层测试，不用于验证正式事务的跨事务可见性。构造函数暂不改访问权限，不要求改写所有底层测试；需要验证混合隔离的测试通过工厂创建并登记事务。


2026-10-24 回调归属审计：当前 include/src 中未发现除已修正的 SstIterator::seek 外，其他普通操作重新配置对象已保存的 is_aborted_。这不表示所有范围入口已经接通。Block 当前不长期保存回调，由调用方提供查询条件；BlockIterator 保存供后续操作复用。用户提出让 Block 也保存，技术上可行，但属于依赖归属调整，不是缺失成员即有 bug。若直接保存当前捕获 shared_ptr<TranManager> 的回调，在管理器已绑定引擎且块仍在缓存中时，会形成 Engine → BlockCache → Block → 回调 → TranManager → Engine 的强引用环；需另外设计弱引用或独立状态对象及初始化路径。固定绑定回滚查询器并不天然导致并发冲突，也不要求改变 Block 磁盘编码。进一步核对：当前 Engine/缓存/Block/SST 不以成员持有返回的 BlockIterator/SstIterator；创建或返回对象并不自动建立长期持有关系。迭代器持有 Block，Block 没有反向持有该迭代器，因此当前回调不会沿这条路径形成闭环。此结论来自成员与调用关系检查，未新增生命周期测试。当前建议维持已有归属，先完成必要的回调传递；未代改业务代码。

不采用“重新写入旧值/旧版本”作为回滚方案：它不能可靠消除已经写入的较新版本。`rollback_map_` 成员及所有业务引用已删除，相关旧值备份说明已清理。

2026-10-20 已核对 Lab 5.3～5.5：Lab 5.5 的“情形3：事务回滚”确实再次使用作者的 rollback_map_，但明确说明还原方式取决于前面 Lab 的实现，并非必须保留该成员。我们继续按事务 ID 排除版本，因此删除旧值备份及其引用；operations、WAL 的提交/回滚记录仍需保留。后续恢复必须接通持久化事务状态与查询过滤，不能仅跳过 WAL 重放就认为已落入 SST 的取消或未完成版本已失效。参考：[Lab 5.5](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.5-Recover.md)。

## 验收边界与待确认事项

- TranContext 按事务创建：每次 begin_tran/new_tranc 返回新上下文，不是按线程缓存的 thread_local 对象，也没有绑定线程的机制。一个线程可以创建多个事务，shared_ptr 也能被传给其他线程。
- 当前 TranContext 没有内部线程同步，同一实例的方法及成员访问必须由调用方串行执行；管理器的锁不保护上下文的 isAborted、temp_map_ 等成员。2026-10-20 核对作者的 transaction.h 与 test_lsm.cpp：上下文无 mutex，事务测试顺序操作句柄；这些代码没有证明“每个线程自动独占一个上下文”。本地 commit/abort 的串行使用注释来自此前助手草案，不是已经核实的 Lab 明文规定。
- 当前保留上述使用限制；2026-10-20 已核对用户加入的类接口说明，明确外部同步与跨线程移交要求。该说明不表示“自动线程隔离”，也不能将同一上下文并发安全列为已验收。若以后要求并发操作同一上下文，再统一同步 put/remove/get/commit/abort 及相关成员访问，而非仅锁 abort。
- 只添加集合或实现 `abort()`，还不能宣称回滚完成；点查、批量查询、范围遍历和提交冲突检查都需要接通。
- `TranContext::get()` 的 `read_map_` 可能绕过存储层查询，需要检查缓存值和缓存的“不存在”结果如何与事务状态配合。
- 混合隔离级别下，读已提交、可重复读对其他事务未提交数据的可见性仍需处理。仅排除 ABORTED 不等于已经实现“只读已提交”。
- 2026-10-25 曾确认混合隔离失败；当前已完成结束登记时移出 activeTrans_，旧的“只增不删”障碍已解除。后续回滚／活跃联合判断仍需在一致的管理器锁保护下进行，不能无锁读取其他上下文的 isCommited/isAborted。未登记的任意 ID 不因此获得“已提交”证明；正式读取依赖已确认的事务入口及普通单次写入约定，恢复一致性仍按后续章节验收。
- 当前 `SERIALIZABLE` 有单键读记录校验，不代表已经实现完整串行化或范围幻读保护。
- 当前提交路径跳过 WAL 持久化；WAL 和崩溃恢复按后续 Lab 推进。
- Block 编码超限沿用已确认的抛异常方案，长期容量设计另行讨论。

## 全局排期与延期事项

- **以 Lab 章节顺序为主线。** 当前推进 Lab 5.2 的事务回滚，不能因发现其他模块未完成，就把后续 Lab 的任务插入当前步骤。每个拟修改函数都应说明它与当前验收目标的直接关系；必要依赖先解释清楚，无关缺陷、优化和重构记入全局待办，不自行扩大任务。
- 后续章节顺序：完成 Lab 5.2 的事务逻辑、状态维护及对应验收，再进入 Lab 5.3 Record、Lab 5.4 WAL、Lab 5.5 恢复与初始化整合。骨架已有函数体不代表已经按我们的流程实现、验收。
- 优先完成当前 Lab 功能。沿用现有迭代器组合、接口拼写与 Block/SST 编码；只对本轮需要的调用链进行局部接入，小型辅助函数仅在直接减少重复代码时引入。
- 每完成一项功能运行对应测试，接通后做受影响范围的回归；没有新改动或失败线索时，不重复全库排查。
- 新发现的问题先判断是否阻碍当前验收；无关问题放入全局待办。有效旧值丢失、取消墓碑继续遮住旧值等问题必须在对应任务中修复。

以下全局排期统一在本文件维护；已进入本轮的任务在 PLAN 展开，延期事项不混入当前计划：

| 事项 | 当前安排 |
| --- | --- |
| 事务状态维护、持久化与正常重启 | 本轮五个核心函数、四阶段验收已完成；2026-10-30 回归 247/247 通过。 |
| 混合隔离与读缓存 | 已进入最新缺陷审计 A2，原测试已恢复并再次复现失败；严格读取策略及 get 修改尚未实现。与之前五个状态维护函数的完成结论区分。 |
| 文件写失败传播、compaction 空尾部、正常换块 | 最新审计 A1/A3/A4 均已用测试复现，位置与修复估时见 AUDIT；未修改业务代码。 |
| 迭代器职责重构、集中 MVCC 筛选 | 当前回滚功能接通后另行评估，详见下方议题一。 |
| 同 key 写入版本单调约束与读取优化 | 与迭代器重构一并讨论，详见下方议题二。 |
| 同一 TranContext 被并发访问的风险 | 当前由调用方保证串行使用，类接口说明已补充；是否支持内部同步留待后续评估，详见下方议题三。 |
| 回滚名单查询性能优化、共享只读快照 | 先采用普通集合加锁方案，性能优化另行评估。 |
| 回滚版本物理清理与回滚 ID 安全回收 | 后续设计；当前名单不能因刷盘登记结束就删除。 |
| LSM 构造函数的组件初始化整合 | 留到 Lab 5.5；当前回滚测试显式绑定 Engine/TranManager。生产入口接通后另做集成验收。 |
| WAL 写入、日志重放和崩溃恢复 | 按后续 Lab 推进；恢复流程需适配事务 ID 回滚方案，覆盖取消/未完成版本已落入 SST 的情况，与下一轮正常关闭、重启验收区分。 |
| Block 长期容量设计 | 保留当前编码超限抛异常方案，长期方案继续讨论。 |


## 已完成：Lab 5.2 事务状态维护

排期：回滚过滤接入 PLAN 已于 2026-10-26 完成。用户随后确认 §3.3 五个核心函数及四阶段实施计划，已于 2026-10-30 全部验收完成；混合隔离修复保持独立待办。下方保留设计依据与验收边界，避免把所有延期事项重新并入本轮。

2026-10-26 函数范围澄清：重新核对 Lab 5.2 §3.3，原文明确列出状态文件写入、读取、结束状态更新、刷盘状态更新和管理器构造，共 5 个函数。本地两项更新分别对应 add_ready_to_flush_tranc_id、add_flushed_tranc_id。此前将三个 getter 也列为必须独立实现，依据不足，改为检查现有 map/set 设计的必要联动，有具体问题才修改。TranContext::get、LSMEngine::get 及拟新增状态查询接口属于此前延期的混合隔离修复，不能算作 §3.3 明列任务；该问题仍未解决，是否与核心五个函数放在同一实施计划中需明确区分，不能随函数清单扩张默认加入，也不能据此将其测试算作通过。

测试结果：此前暂缓的 24 个用例中，23 项状态维护与回滚相关测试已原样恢复并全部通过；混合隔离 1 项仍未解决，留待独立修复时恢复。本轮结论包括恢复后的测试和新增真实重启链路，不将延期用例计为通过。

- 文档接口对应：update_max_finished_tranc_id → add_ready_to_flush_tranc_id；update_max_flushed_tranc_id → add_flushed_tranc_id。职责对应，数据模型已从最大 ID 转为 map/set，不能机械照搬旧实现。
- 独立推导结束登记、刷盘登记及状态查询；Lab 要求实现的函数无论是否已有函数体都要重新推导。保留回滚集合与异常一致性。
- 活跃生命周期：new_tranc 登记，成功结束后同锁移出；登记失败仍保持活跃。后续混合隔离任务再设计一次锁内的回滚/活跃联合判断，不无锁读取上下文状态字段。
- 独立后续任务——读取隔离：Engine 点查询提供严格读取策略，RC/RR/SERIALIZABLE 首次存储读取排除其他活跃事务版本；RU 保持既定语义。本事务修改优先，RR/SERIALIZABLE 缓存合法结果及不存在，不用简单重查掩盖首次脏读。新增状态查询接口随该任务设计，不作为本轮 §3.3 额外必做函数。
- 现存失败证据：TranReadVisibilityTest.RejectsDirtyValuesAndTombstonesBeforeCaching，0/1 通过。全部事务经工厂创建；未提交新值/墓碑造成首次脏读，RR/SERIALIZABLE 取消后仍缓存 dirty/nullopt。最新审计已将该用例恢复到 test/test_lsm.cpp 并重新验证失败。
- 活跃状态与读缓存验收：进行中/提交/回滚/刷盘后的判断一致；合法首次读取可重复，缓存不存在仍稳定；本事务写入优先；取消版本不造成虚假提交冲突；有效变化仍能触发应有拒绝。

### 状态维护的持久化验收依据

- [x] 独立实现 `write_tranc_id_file()`，保存需要恢复的事务状态和回滚 ID（2026-10-29，4/4 通过）。
- [x] 独立实现 `read_tranc_id_file()`，验证文件内容并恢复状态（2026-10-30，读写合计 10/10 通过）。
- [x] 按现有 map/set 结构核查结束登记、刷盘登记、状态查询和 `TranManager` 构造函数，不把已有函数体视为已经验收。
- [x] 正常关闭、重启后继续分配正确的新 ID，并在提供查询前恢复回滚名单。
- [x] 测试回滚数据刷盘、compaction、正常重启后仍不可见，旧值仍可读取。

**验收标准：**

| 场景 | 必须得到的结果 |
| --- | --- |
| 新目录首次启动，以及合法状态文件的写入、读取 | 正确初始化或恢复事务状态和回滚 ID，不把已提交事务误标为回滚。 |
| 回滚的记录已进入 SST，随后正常关闭、重启 | 第一次查询就排除回滚记录；修改/删除回滚后读到旧值，新增回滚后仍不存在。 |
| 回滚 ID 已从待刷盘状态表移除，再保存、重启 | 回滚名单仍包含它；compaction 后结果也保持正确。 |
| 重启前分配过已提交、已回滚或尚未写入记录的 ID | 正常保存、重启后新 ID 大于此前所有已分配 ID，继续分配不重复。 |
| 重复保存和多次正常重启 | 状态结果稳定，回滚版本不会重新出现。 |
| 已存在的状态文件被截断或字段不合法 | 明确报告读取失败，不静默当作新数据库并重置 ID。 |

本阶段只验收当前固定格式的正常保存、正常关闭与重启。跨版本识别和迁移按用户决定延期，不作为本轮验收要求。


## 延期 issue

以下议题均延期，不阻塞当前回滚主线。这里保存 issue 草案与讨论边界，GitHub 编号和链接待用户发布后补充。

### 议题一：明确迭代器职责，集中管理 MVCC 可见性与版本筛选

- 状态：已在聊天中提供 issue 草案，重构尚未启动。
- 位置：[BlockIterator](../src/block/block_iterator.cpp)、[HeapIterator](../src/iterator/iterator.cpp)、[TwoMergeIterator](../src/lsm/two_merge_iterator.cpp)、[Level_Iterator](../src/lsm/level_iterator.cpp)。
- 问题：多个迭代器同时承担推进、版本过滤、同 key 去重或墓碑处理；规则与执行位置分散，新增回滚判断容易遗漏，也难以确认下层是否已经丢掉旧版本。
- 例子：`k@5` 已取消、`k@3` 有效；若下层先去重，上层再过滤 5，就可能漏掉应返回的 3。
- 待讨论方案：明确原始遍历、归并排序、用户查询 MVCC 筛选的职责，评估集中筛选的设计；不是已经决定重写全部迭代器。
- 验收目标：点查、批查、范围查询保持一致；历史版本、墓碑、来源优先级和回滚场景正确；compaction 保留必要版本，并评估性能与内存开销。
- 当前处理：沿用已有结构接通回滚判断，不把重构作为前置条件。

### 议题二：评估写入版本单调约束，以简化读取时的跨来源比较

- 状态：设计评估待办，尚未决定实施写入约束或简化读取；GitHub 链接待补充。
- 核心问题：目前可能先写 `k@5` 并刷入 SST，再把 `k@3` 写入新的 MemTable。较新的来源未必含有较大的版本，因此命中 MemTable 后仍需查询 SST 并比较真实版本号。这是此前多处跨来源比较修补共同应对的问题。
- 优化目标：在实际写入时保证同一个 key 的有效写入版本不倒退；例如已有有效 `k@5` 后，拒绝迟到的 `k@3`。这里是同 key 的约束，不要求不同 key 的 ID 也按顺序写入。
- 预期收益：若这个约束覆盖所有参与查询的数据，且冻结、刷盘、compaction 保持来源先后关系，那么按照活跃表、冻结表从新到旧、L0 从新到旧、L1 及更深层的顺序，遇到第一个可见记录即可决定结果。命中有效墓碑时也可以确定“不存在”，无需继续查找更深来源的较大版本。
- 可能简化的位置：[MemTable 的 frozen_get_/get_/get_batch](../src/memtable/memtable.cpp)、[Engine 的 get/get_batch/sst_get_](../src/lsm/engine.cpp)，以及 [Level_Iterator](../src/lsm/level_iterator.cpp)、[TwoMergeIterator](../src/lsm/two_merge_iterator.cpp) 等普通查询归并中的同 key 来源选择。逐项确认适用条件，不直接删除全版本 compaction 的排序与输出逻辑。
- 当前事实：[TranContext::put/remove](../src/lsm/transation.cpp) 的读未提交分支直接写入，没有写冲突检查；暂存型事务已有 commit 阶段的检查。普通 LSM 的直接、批量写入入口不经过 TranContext::put。
- 检查时机：读未提交需要在实际写入时检查；暂存型事务不能仅在 put 时预检，因为 put 到 commit 之间数据库仍会变化，实际应用修改前的检查必须保留。
- 并发与覆盖要求：检查与实际写入必须正确同步，覆盖所有相关写入入口，包括删除墓碑和批量操作；随后验证冻结、刷盘、compaction 保持用于提前返回的顺序约束。
- 存量数据：新增检查不会自动修正数据库中已经存在的来源与版本倒置；优化读取前需明确旧数据兼容、整理或回退策略。
- 待讨论的写入语义：拒绝低版本写入时如何报告失败、事务如何结束；如何对待已取消版本、其他活跃事务和有效墓碑。
- 与另一个问题的区别：本议题聚焦“首次命中的可见来源能否直接胜出”，不是取消快照可见性筛选。每个来源仍先找出对本次读取可见的候选，之后才应用来源优先的优化。
- 验收方向：证明并测试同 key 有效版本的写入约束；对比优化前后的查询结果与候选来源访问次数，覆盖并发、删除、取消、历史快照、跨层与 compaction 场景，保留必要回归。
- 当前处理：现有代码尚未具备该全局保证，继续保留跨来源比较；本轮先完成回滚，该议题延期。

### 议题三：明确 TranContext 的并发访问边界，评估内部同步保护

- 记录日期：2026-10-20。状态：潜在并发风险，尚未实施内部同步，也未发布 GitHub issue。
- 位置：[TranContext 类定义](../include/lsm/transaction.h)、[put/remove/get/commit/abort](../src/lsm/transation.cpp)。
- 当前事实：每个事务有独立上下文，但没有自动线程绑定；同一个 shared_ptr 可以被传给多个线程。上下文的状态、缓存和暂存容器没有自身互斥保护。
- 触发条件：多个线程未做外部同步，同时操作同一个上下文或直接访问其公开成员；不同线程各自使用不同上下文不属于这一问题。
- 影响：例如 put 插入 temp_map_ 时 abort 同时清空它，会产生数据竞争；commit 与 abort 并发还可能破坏事务结束状态的一致性。TranManager 和存储层的锁不能代替上下文同步。
- 当前处理：用户已补充类接口注释并经核对，明确同一实例必须由调用方串行使用，跨线程移交需正确同步。注释表达使用限制，不代表代码强制执行，也不代表并发风险已消除。
- 后续评估：是否支持同一上下文的并发调用；如果支持，统一保护状态检查、容器访问及结束状态更新，并处理公开成员绕过保护的问题，不能仅给 abort 加锁。检查新增锁与管理器、存储层锁的顺序。
- 验收方向：若实施同步，补充同一上下文 put/abort、get/abort、commit/abort 等并发用例，验证无数据竞争、结束状态唯一、结束后不再接受修改，并检查不同上下文仍可并发；条件允许时使用 ThreadSanitizer。
- 当前排期：作为全局延期事项保存在 STATE，不加入当前回滚 PLAN 的实施步骤。

## 测试状态

- 最新检查（2026-10-24，Block 全量遍历入口）：新增 `RollbackBeginFiltersInitialAndFollowingRecords`，验证首次定位和后续 ++ 筛选取消值/墓碑、保留有效墓碑、读取上限 0/历史上限、回调独立保存、空块、全部取消及默认调用兼容。重新构建并在独立临时目录运行完整 **Block 35/35、SST 19/19 通过**。业务源码由用户修改，既有测试断言未降低，本轮未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-begin-rollback-efcgymdy`。
- 最新检查（2026-10-24，Block 前缀入口）：新增 `RollbackPrefixFiltersVersionsAndBoundaries`、`RollbackPrefixHandlesEmptyPrefixAndNoResults`，验证连续取消值/墓碑、有效墓碑保留、范围两端筛选、读取上限 0/历史上限、空前缀、缺失前缀、空块、全取消及旧接口兼容。重新构建并在独立临时目录运行完整 **Block 34/34、SST 19/19 通过**。未改业务源码、未降低既有断言，本轮未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-prefix-rollback-368dh7bl`。
- 最新检查（2026-10-24，Block 谓词区间修正后）：用户已补齐两个构造调用的 false 并修正左闭右开说明；重新构建 test_block/test_sst，在独立临时目录运行完整 **Block 32/32、SST 19/19 通过**。上一轮新增的 2 个谓词回滚测试通过，验证右边界与 ++ 一致、连续取消、有效墓碑、读取上限 0/历史上限、无匹配及全取消区间。业务代码由用户修改，本轮未改测试断言或运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-predicate-fixed-ju3dbvki`。
- 最新检查（2026-10-24，Block 谓词区间入口）：`xmake build test_block` 失败，错误定位 src/block/block.cpp:566；两处构造将 std::function 作为第④个 bool 参数，缺少 false。新增 `RollbackPredicateFiltersRecordsAndRightBoundary`、`RollbackPredicateHandlesEmptyAndCancelledRanges`，覆盖两端一致筛选、有效墓碑、读取上限、连续取消及空结果等，尚未运行。本轮没有使用旧测试二进制宣称当前通过，未修改业务源码。构建日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-predicate-build-rz0dsng4/build.log`。
- 最新检查（2026-10-24，BlockIterator 按 key 构造）：新增 `RollbackKeyIteratorFiltersThroughoutTraversal` 和 `RollbackKeyIteratorOwnsCallback`，验证普通/全版本模式、读取上限 0 与历史上限、连续取消值和墓碑、有效墓碑保留、缓存、精确查找全部取消时不误返回邻居，以及调用方清空回调后继续 ++。重新构建并在独立临时目录运行完整 **Block 30/30、SST 19/19 通过**。业务源码及头文件由用户修改，其他套件本轮未运行。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-key-callback-review-zjcug46y`。
- 最新检查（2026-10-24，BlockIterator 按下标构造）：用户补齐初始化列表的 is_aborted_(is_aborted) 后重新构建，独立临时目录运行完整 **Block 28/28、SST 19/19 通过**。4 个新增测试覆盖首条定位、++ 后筛选与缓存、复制保存回调、空块/尾后/全部取消；包含连续取消值/墓碑、读取上限 0、全版本模式及有效墓碑保留。补齐前 4 个新增测试全部失败，补齐后全部通过，未降低断言。业务代码由用户修改；本轮未运行其他测试套件。最新日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-index-verified-huwzqk31`；补齐前失败日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-index-callback-review-ueo3rju4/result.log`。
- 最新检查（2026-10-24，BlockIterator 筛选函数）：构建成功，独立临时目录复制配置后运行完整 **Block 24/24、SST 19/19 通过**。新增成员与筛选函数的读取上限、取消状态、单条推进、空块处理和缓存清理通过代码检查；公开构造函数尚未赋值 is_aborted_，本轮测试不能覆盖该函数的非空回调分支，留待构造接通后补集成测试。未改业务代码或测试断言，未运行其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-skip-review-hi7r7063`；构建日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-skip-build-hsvpnl3d`。
- 最新检查（2026-10-24，PLAN 已完成项复核）：五个目标构建成功；各自独立临时目录复制配置后运行，完整 **SkipList 31/31、MemTable 49/49、Block 24/24、SST 19/19**，**LSM 相关选跑 54/54**，共 **177/177 通过**。LSM 沿用上一轮点查/批查、事务创建/读写/提交/回滚等 48 项过滤器，另加入 NewTransaction 和 TransactionEndRegistration 共 6 项，验证阶段①的登记与查询。未运行完整 test_lsm、WAL/恢复或未完成的范围回滚路径；未修改业务代码及测试断言。日志与 `lsm-filter.txt`：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-plan-completed-review-gi26m8_i`；构建日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-plan-check-build-cl3hsb4p`。
- 最新检查（2026-10-24，谓词接口约定纠正）：`xmake build test_memtable` 成功，独立临时目录运行完整 **test_memtable 49/49 通过**。将额外约束回调输入的 `PredicateDoesNotPassMarkersToUserCallback` 替换为 `PredicateRangeResultsExcludeTransactionMarkers`，验证只有标记时无结果，以及含标记的活跃/冻结表查询 [b, d) 返回正确的 key/value/版本。原有全匹配标记过滤、回滚、墓碑等断言保留；业务代码未改，未重跑其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-predicate-contract-o2wecet6/result.log`。以下保留原始测试记录；其中“回调收到空 key”不再被认定为本接口的正确性缺陷。
- 最新检查（2026-10-23，getter 与谓词复测）：两目标构建成功，完整 **test_skiplist 31/31、test_memtable 48/49**。新增 `CurrentTransactionIdIncludesMarkersAndTombstones` 和 `CurrentTransactionIdRejectsEmptyIterators`；验证真实记录、墓碑、多版本标记和空/end 边界。MemTable 唯一失败为 `PredicateDoesNotPassMarkersToUserCallback`，其他原失败项已通过。未运行其他套件。SkipList 日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-skiplist-current-id-k3hf0nan/result.log`；MemTable 日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-predicate-getter-fixed-1smwz9eo/result.log`。
- 最新检查（2026-10-23，谓词查询）：构建成功，选跑 **4 个用例，全部失败**。新增 `PredicateRollbackFiltersBeforeDeduplication`、`PredicateDoesNotPassMarkersToUserCallback`、`PredicateAllKeysSafelySkipsTransactionMarkers`，并选跑原有 `PredicateAllDeletedReturnsNoRange`。确认返回 value 错误、墓碑失效、用户谓词收到空 key、空 key 上调用 get_cur_tranc_id 抛异常。本轮未运行完整 test_memtable 或其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-predicate-review-a_w_3olo/result.log`。
- 最新检查（2026-10-23，MemTable::iters_preffix）：构建成功，完整 **test_memtable 46/46 通过**。新增 `PreffixRollbackRestoresVersionsAcrossSources`、`PreffixRollbackKeepsVersionAndSourcePriority`、`PreffixRollbackHandlesEmptyRangesAndMarkers`；原 `PreffixIter` 增加结果数量与索引边界检查。本轮未运行 test_lsm 或其他套件；谓词查询的新增回滚逻辑尚未实现或验证。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-prefix-rollback-f1tq6065/result.log`。
- 最新检查（2026-10-23，MemTable::begin）：`xmake build test_memtable` 成功，独立临时目录运行完整套件，**43/43 通过**。用户补齐活跃表收集和回滚筛选后，上一轮失败的 `BeginCollectsActiveAndFrozenSources`、`BeginFiltersAbortedBeforeDeduplication`、`BeginRetainsTombstonesForUpperLevelFiltering` 均通过。覆盖取消值/墓碑、读取上限 0 与历史上限、有效墓碑、全部来源收集；`FailedFlushPreservesDataAndTransactionMarkers` 确认查询隐藏标记后刷盘仍报告对应 ID。本轮未运行 test_lsm 或其他套件。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-begin-fixed-i_ic5jod/result.log`。
- 最新检查（2026-10-23，get_batch）：`xmake build test_lsm` 成功，相关 **48/48 通过**。新增 `LSMTest.EngineBatchGetRequiresLiveManager` 与 `EngineGetRollbackTest.BatchKeepsOrderDuplicatesAndHistoricalVersions`，扩展 `ComparesFilteredMemoryAndDiskCandidates` 到批量入口。最初反向条件由代码检查发现，用户自行修正后才运行本轮测试；未运行错误版本，也未修改业务代码。未运行完整 test_lsm 或其他存储测试套件。
- 最新日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-batch-rollback-76p8g7bl/result.log`；同目录 `filter.txt` 保存完整过滤器。
- 前一轮检查（2026-10-23，get）：`xmake build test_lsm` 成功，独立临时目录运行相关测试 **46/46 通过**。新增 `LSMTest.EngineGetRequiresLiveManager` 和 3 个 `EngineGetRollbackTest`，覆盖严格依赖检查、取消值/墓碑、活跃表/冻结表/SST、真实版本与同版本来源优先级、连续回滚、历史读取。复查原有点查/批查基础回归、SST 回滚、刷盘及事务创建/读写/提交/删除/取消测试。旧批查回归没有取消版本，因此不代表批量回滚已完成。未运行完整 test_lsm、其他存储套件或 Lab 5.5 入口。
- 前一轮日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-get-rollback-0eu767bl/result.log`；同目录 `filter.txt` 保存该轮完整 gtest 过滤器。
- 前一轮检查（2026-10-22）：新增 4 个 EngineRollbackSst 测试：过滤后跨 L0/L1/L2 比较及有效墓碑、候选全部取消后继续查找及不清空已有 best、同版本来源优先级、空集合/缺失/回调异常。包含读取上限 0 和历史上限、L1 二分两侧、取消墓碑、取消名单在两次查询间变化和默认空回调行为。`xmake build test_lsm` 成功，独立临时目录复制配置后运行 `LSMTest.EngineRollbackSst*` 及此前 3 个 EngineMvcc 读取回归，**7/7 通过**。未运行完整 test_lsm、真实 abort 查询测试或其他套件，未修改业务代码。
- 前一轮日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-engine-sst-rollback-s_no42vs/result.log`。
- 前一轮 update_best_from_sst 检查（2026-10-22）：实现检查、构建及既有空回调读取路径 **3/3 通过**；当时非空回调尚未接通。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-candidate-review-mfm7c3sa/result.log`。
- 前一轮 SST::get 检查（2026-10-22）：扩展原有 3 个回滚测试及 EndDoesNotReadBlocks，公开入口完整 SST **19/19 通过**，test_lsm 构建通过。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-get-rollback-r4d0s4ki/result.log`。
- 前一轮 SstIterator 按 key 构造检查（2026-10-22）：扩展原有 3 个回滚测试到构造入口，完整 SST **19/19 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-key-ctor-kggoiq63/result.log`。
- 前一轮 seek 修正检查（2026-10-22）：catch (...) 同时重置块编号和块内指针，再重新抛出异常；标准和非标准异常测试及完整 test_sst **19/19 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-seek-fixed-k6df6mic/result.log`。
- 前一轮 seek 检查（2026-10-22）：新增 3 个测试，覆盖重新打开 SST、回滚过滤与读取上限、两种遍历模式、有效墓碑、重复 seek 缓存更新、全部取消/缺失/空 SST 句柄、异常上报与后续再次定位。曾为 18/19，唯一失败断言是异常后的 `it == sst->end()`，现已解决。旧日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-seek-rollback-upp8ca__/result.log`。
- 前一轮 BlockIterator 检查（2026-10-22）：按 key 构造正确转发回调，空指针抛 invalid_argument，未命中定位块尾。扩展已有 5 个回滚用例并新增空指针测试，**Block 24/24、SST 16/16 通过**；未验证 ++ 的回滚过滤。
- 前一轮日志：Block `/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-key-ctor-test_block-b2izzsjo/result.log`；SST `/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-key-ctor-test_sst-pblbxc4g/result.log`。
- 前一轮 get_value_binary 检查（2026-10-22）：用户补上漏传的回调，扩展已有 5 个回滚测试到取值入口后，完整 test_block **23/23 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-value-fixed-xk1s5jsn/result.log`。
- 前一轮补参前，5 个 RollbackPoint 测试因取值接口忽略回调而失败，补参后全部通过。旧日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-value-review-mpuxy6o7/result.log`。
- 前一轮 get_idx_binary 检查（2026-10-22）：新增 5 个回滚测试，完整 test_block **23/23 通过**；当时尚未测试取值入口的回滚过滤。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-index-rollback-imu5tn3s/result.log`。
- 前一轮 Block 检查（2026-10-21）：修正 while 条件后，完整 test_block **18/18 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-adjust-fixed-e8ntbyst/result.log`。
- 已解决的上一轮回归：BlockTest.DecodeTest、EncodeTest、RejectsOffsetOverflowWithoutChangingExistingRecords、ForceWriteWithinEncodingLimitsStillSucceeds、MvccPointReadSelectsNewestVisibleVersion、MvccTombstoneDoesNotFallBackToOlderValue、MvccEncodingPreservesFullWidthTransactionIds 曾因同一个循环条件错误失败，本轮均通过。旧日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-block-adjust-review-gcsp5sbr/result.log`。
- 前一轮 get_batch 检查（本轮 Block 修改之前）：新增 3 个批量测试，覆盖跨表取消值/墓碑、版本上限、顺序和重复 key、逐条查询一致性、空结果和副本稳定性。test_memtable、test_lsm 构建成功，完整 MemTable **41/41 通过**，test_lsm 只构建未运行。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-batch-rollback-ae5vyu8v/result.log`；其中 missing/fail.sst 打开失败来自预期的刷盘失败测试。
- 前一轮 get_record 检查：扩充跨表回滚测试并新增副本与空结果测试，完整 MemTable **38/38 通过**，test_lsm 构建通过。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-record-rollback-vaop21p9/result.log`。
- 前一轮公开 get 检查：新增 7 个回滚点查测试，完整 MemTable **37/37 通过**。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-memtable-get-rollback-ohi4rqe3/result.log`。
- 前一轮 get_ 检查（2026-10-21）：test_lsm 与 test_memtable 均构建成功，MemTable **30/30**、`TranCommitTest.*` **10/10** 通过。本轮没有重跑提交测试，commit 的回滚判断仍待接入。对应日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-internal-memtable-942g3zya/result.log`、`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-get-internal-commit-ompvxjq9/result.log`。
- 上一轮 SkipList 检查（2026-10-20）：用户修正读取上限条件后，构建及完整 test_skiplist **29/29 通过**，包含全部 6 个回滚测试。此前上限条件导致的 3 个失败已解决。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-skiplist-abort-fixed-djbup06n/result.log`。
- 上一轮 LSM 检查（2026-10-20）：remove 的删除路径及结束状态检查正确；搜索 src/include/test 没有 rollback_map_ 引用，`xmake build test_lsm` 成功。独立临时目录中运行当时已实现范围的测试，**36/36 通过**；这是本次 SkipList 修改之前的结果，未修改原有失败用例的断言。
- 上一轮 LSM 过滤器：`--gtest_filter='LSMTest.TranContextConstructor*:LSMTest.TranContextPut*:LSMTest.TranContextGet*:LSMTest.NewTransaction*:LSMTest.TransactionEndRegistration*:TranCommitTest.*:TranRemoveTest.*:TranAbortTest.Cancels*:TranAbortTest.ExpiredManager*:TranAbortTest.ReadUncommittedRegisters*:TranAbortTest.CannotAbort*'`。日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-remove-review-kcekzes_/result.log`。
- [test/test_lsm.cpp](../test/test_lsm.cpp) 已有事务创建、读写、提交、结束登记等测试，以及 `TranAbortTest` 的 7 个回滚用例和 `TranRemoveTest` 的 4 个删除用例。
- 本轮新增删除测试：暂存删除到提交后生效；读未提交立即写墓碑但保留历史版本；put/remove 交替取最后一次操作；实际 commit/abort 后拒绝删除且无副作用。同步扩充暂存取消测试，加入结束后的 remove 检查。
- 2026-10-20：新增 3 个 abort 用例，覆盖空事务取消、管理器失效时保留原状态、读未提交取消不改写物理记录；扩充暂存取消用例，检查暂存删除、清空容器、重复调用及已结束的 put/get/commit。已提交事务取消还检查管理器未误记回滚。
- 前一轮较宽范围验证：在独立临时目录复制配置后运行 `test_lsm --gtest_filter='LSMTest.TranContext*:LSMTest.NewTransaction*:LSMTest.TransactionEndRegistration*:TranCommitTest.*:TranAbortTest.*'`，**35 个测试：32 个通过，3 个失败**。其中两个回滚点查失败已于 2026-10-23 解决并通过；LSM 初始化入口仍留在 Lab 5.5，本轮未重跑该入口用例。
- 失败一：`LSMTest.TranContextTest` 抛出 `TranManager::new_tranc: engine is not initialized`；LSM 构造中的组件绑定尚未实现，按用户要求留到 Lab 5.5 初始化整合，不列为当前回滚阶段的修复项。
- 已解决的失败二、三：`TranAbortTest.ReadUncommittedRestoresPreviousVersionInMemoryAndSst`、`TranAbortTest.KeepsNewerCommitButHidesAbortedVersionFromOlderSnapshot` 于 2026-10-23 接通 Engine::get 后通过，保持原断言。历史验证日志：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-abort-context-review-738zvtgk/result.log`。
- 验证边界：本轮未运行完整 test_lsm；未注入内存分配故障，未启用 ThreadSanitizer。范围、事务缓存/提交的回滚判断以及状态持久化仍待验收。

## 协作方式

- 用户手动修改业务源码及头文件；助手在聊天中给出带中文解释的实现和准确位置。
- 一次推进一个函数，检查完成后，等用户明确说“下一个”再继续。
- 每次展示计划或进度，都先列出全部六个阶段及其已完成、进行中、待开始状态，再标出当前阶段正在处理的函数；必要时补充当前阶段细项。按用户 2026-10-24 要求，同时标注完成百分比：默认按“功能验收通过的阶段数 ÷ 总阶段数”计算，说明口径，不代表工作量或剩余时间；阶段内部分完成暂不折算。旧注释等收尾事项仍明确列出，不能把单个函数通过写成整个阶段完成。
- Lab 指定函数即使已有实现，也要独立推导，并对照作者参考；有差异时说明理由。
- 提出修复前先确认职责、输入输出及实际调用链，检查已有过滤能否保证结果；区分“内部候选包含记录”“回调收到记录”和“最终向用户返回记录”，不能将三者混为一谈。
- 独立分析后再用作者参考与对应 Lab 校正，明确哪些是文档要求、参考实现行为、本地约定或可选增强；参考存在遗漏时用具体代码路径说明，不把参考本身视为正确性证明。
- 必修 bug 应提供当前约定内可到达的触发场景、准确位置及实际错误结果；缺少证据时标为待确认。可选防护和重构不自动加入本轮修复清单，不因假设调用方违反约定而扩大任务。
- 测试先依据接口承诺确定预期，再检查实现；不能用助手自行添加的更强要求制造失败并反过来证明存在 bug。若发现测试预期错误，明确解释并记录原因，再修正测试。
- 助手可以直接补充测试、维护这两份文档，并执行相关检查。测试使用独立临时目录和配置，避免测试清理影响项目数据。
- Git 写操作由用户执行；“提 issue”默认指在聊天中起草内容。

## 全局验收与记录约定

- 每个阶段需满足行为标准并通过相应构建、测试和代码检查，才能认定完成；实现任务和测试任务分别勾选。
- 每轮验证已接通的功能，依赖后续工作的用例保持待验收；不通过跳过失败断言、降低预期或遗漏已知失败来宣称通过。
- 实现任务经检查后勾选，测试任务实际通过后勾选。记录日期、涉及文件/函数、测试命令、用例范围、通过/失败数量和未验证项。
- 未解决或延期的能力不得计入“已完成”，不承诺尚未评估的工时。

## 记录更新规则

实现检查完成、测试结果变化或设计决定改变时更新本文件。更新日期表示最后核对时间，不代表全部功能在该日通过验收。只保留当前有效结论；当前任务的顺序、TODO 和验收标准维护在 [PLAN.md](PLAN.md)。全局协作规则、项目状态和延期事项只在本文件维护。

方案发生变化时改写当前任务的待办，并同步本文件中的设计结论，不保留互相矛盾的执行安排。
