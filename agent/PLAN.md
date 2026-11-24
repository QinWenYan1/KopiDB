# KopiDB 当前计划

## SERIALIZABLE · 单个数据库实例内的悲观并发控制

本轮目标是 C++ 内核的 `SERIALIZABLE` 悲观独占和并发回归。2026-11-23 确认：事务保证覆盖现有上下文的点读、写入和删除，锁的具体实现待定。功能进度 **1/3（33%）**，按验收通过的阶段数计算。业务代码由用户逐函数实现，助手准备测试、执行回归并记录结果。

### 用户应观察到的行为

- 四个隔离级别共用线程约定：同一线程最多一笔活跃的显式事务，活动上下文只供创建线程操作，调用方在创建线程上显式提交或回滚。检查约定后才允许等待、分配事务 ID 或读写事务状态。不同线程仍可共享同一个数据库实例。
- A 请求 `SERIALIZABLE` 时，先等已经进入本实例的事务和普通写入结束。获准后再领取事务 ID。A 从开始到提交或回滚独占写入资格；同时发起的新事务和普通单笔、批量写入要等待。已有事务始终可以继续工作和结束。
- A 通过自己的事务句柄查询，能读到自己的写入和删除。仅作观察的 `db.get()` 不会改变 A 的结果；数据库级查询仍按既有规则读取，不属于 A 的事务。没有独占竞争时，其他三个级别照常并发。
- 并发的两个可串行化事务都按“对方在岗才允许自己下班”写不同的 key，最后至少有一人在岗。

### 实施草案与阶段验收

**阶段①的实现顺序。** 每轮先接通一个可独立验证的入口。

1. 在 [transaction.h](../include/lsm/transaction.h) 的 `TranContext` 中记录只读的创建线程 ID，新增私有 `require_owner_thread(const char *operation) const`；[构造函数和六个公开方法](../src/lsm/transation.cpp) 的 `put/remove/get/commit/abort/get_isolation_level` 共用此检查。检查在访问可变状态与写 WAL 前运行。事务对象本身应禁止复制或移动；仍可复制 `shared_ptr`，跨线程调用由入口拒绝。
2. `TranManager::new_tranc()` 是 `LSM::begin_tran()` 的创建入口。建议用一个 `thread_local std::weak_ptr<TranContext>` 记录本线程经工厂创建的活动事务。同线程再次申请时先报错，再谈等待和分配 ID；成功插入 `activeTrans_` 后才登记到线程槽。按“每线程一笔”使用线程级槽位，独占控制仍按数据库实例独立。
3. `TranManager::add_ready_to_flush_tranc_id()` 成功结算显式事务后，按该上下文的身份清理线程槽。正常提交、显式回滚及冲突回滚都走这里；普通写的结束登记、重复登记和失败的登记不能误清其他事务。保留已结束事务的用户句柄不占用下一笔的名额。
4. 在 [engine.cpp](../src/lsm/engine.cpp) 的 `LSM::put/put_batch/remove/remove_batch` 取资格或分配 ID 之前，复用同一线程槽检查本库活动事务。通过 `tx->put/remove` 的事务内操作继续走已有路径。

应用代码通过 `LSM::begin_tran()` 取得已登记的事务。带 ID 的构造函数已设为私有，友元 `TranManager` 负责创建和登记，白盒测试通过 `kopidb::LSMTest` 组装指定版本的数据。

阶段②讨论草案：每个 `TranManager` 对应一个 DB。首选带定时获取的读写锁 `std::shared_timed_mutex`：需要长等诊断时用 `try_lock_for` 或 `try_lock_shared_for`，达到诊断阈值记录一次后继续等待。普通 `std::shared_mutex` 支持共享和独占，若要在等待期间按时输出诊断，还需要单独的监测机制。若产品要求独占请求排队后禁止新共享操作插队，则考虑下文的计数和条件变量方案。锁类型、等待阈值与异常收尾待共同确认。

| 阶段 | 工作 | 验收 | 状态 |
| --- | --- | --- | --- |
| ① 落实两条线程规则 | 先记录创建线程并从 `TranContext::get()` 接入所有者检查，再覆盖其余事务 API、工厂登记、结束清理以及本库独立写入口。 | 跨线程调用、同线程重复开启事务均在产生副作用前报错；创建线程可继续使用、结束事务并开启下一笔。两个线程各自使用自己的事务仍可并发。 | 已验收 |
| ② 商讨串行化实现 | 比较 标准读写锁与管理器计数方案，定下取得与归还资格的时机、合法等待的反馈及锁顺序。 | 确认方案、函数清单和并发测试场景。 | 讨论中 |
| ③ 实现并验收串行化 | 接通显式事务和四个普通写入口，为独占增加等待与唤醒。两条路径验收后再移除 `TranContext::commit()` 的旧单键读集校验。 | 值班案例、普通写与混合隔离、提交/回滚唤醒、异常及重开通过 [C++ 事务测试](../test/test_lsm.cpp) 和 [WAL 测试](../test/test_wal.cpp)。 | 待开始 |

**计数路线的候选凭证。** `AdmissionGuard::AdmissionGuard(...)` 接管一次已取得的资格；`AdmissionGuard::AdmissionGuard(AdmissionGuard&&) noexcept` 移交归还责任，使移出的凭证失效；`AdmissionGuard::~AdmissionGuard() noexcept` 只为有效凭证调用一次 `release_admission()`。凭证持有管理器弱引用，不能形成“管理器 → 活跃上下文 → 管理器”的强引用环。每个 DB 独立维护短时互斥锁和条件变量；取得资格后即可释放互斥锁，等待时也不能持有管理器状态锁或存储层锁。

**计数路线的候选归还位置。** 工厂用一个局部凭证覆盖构造与登记失败，成功时移入上下文的可选成员。结束登记只取出并归还对应上下文的凭证；重复登记和普通写入的状态登记没有第二份凭证可退。普通写的局部凭证在该次操作退出时归还。登记、WAL 异常及已确认 COMMIT 后应用失败的处理要与现有事务状态一致，不得把未完成操作宣布为成功或擅自改为回滚。

**现有函数的复用。** `LSM::begin_tran()` 继续转发到工厂，`TranManager::getNextTransactionId()` 继续分配 ID。`TranContext::put/remove/get` 增加入口线程检查后沿用现有存取逻辑。保留 `read_map_` 和通用写冲突检查。端到端验收走 `db.begin_tran()`。

**旧测试迁移。** 阶段①已将同线程重叠事务和跨线程调用迁移为各线程持有各自事务。阶段③接入真正的独占时，需同步调整仍刻意检验实验性行为的调度：包括 `TranCommitWALTest.ReadChangeRejectsCommitWithoutBlockingOtherWriter`、混合隔离读取用例中 RU 与 SERIALIZABLE 重叠的分支、以及旧单键读集校验。对独占分支使用独立线程发起请求，由当前事务先结束，再验收等待者的返回值、WAL 顺序和可见性。保留 RC/RR 既有断言与 `TranCommitTest.RepeatableReadDoesNotValidateUnwrittenReadKeys`。

### 调用方责任与待定事项

1. **调用方显式结束事务。** 创建线程在丢弃活动句柄或退出前显式提交或回滚；`activeTrans_` 强持有上下文，丢弃用户的 `shared_ptr` 不会结束事务。活动句柄的跨线程使用检查必须先于读取或修改可变事务状态，不能从另一线程代为解锁。
2. **如何反馈误用。** 建议统一抛 `std::logic_error`。同线程重复开事务、活动事务内对本库发起独立写、其他线程调用活动句柄都应明确指出操作名与事务 ID，并保留原事务；`get/remove/put` 等事务内操作使用原事务资格。跨线程复制 `shared_ptr` 本身不是可拦截的 API 调用，检查发生在上下文入口。
3. **合法的等待。** 另一线程在独占期间请求普通写，或请求独占时仍有其他线程的活动事务，可以等待。长等告警注明操作、等待时间和可定位的事务 ID；误用在等待前反馈。锁选型时同时核对等待诊断与线程退出前结束事务的约定。

**当前讨论位置**：[TranManager::new_tranc()](../src/lsm/transation.cpp)、[LSM 的四个普通写入口](../src/lsm/engine.cpp) 与 `TranManager::add_ready_to_flush_tranc_id()`。以下是设计建议，尚未实施：
- 显式事务先检查线程规则，取得本库共享或独占资格，再分配 ID、登记上下文。等待资格时须先释放管理器状态锁、SST 锁及内存表锁，已有事务才能完成和唤醒等待者。
- RU、RC、RR 显式事务共享，SERIALIZABLE 显式事务独占。结束决定已正确应用并完成状态登记后，由创建线程释放资格；即使用户仍持有句柄，下一笔也能进入。
- 有效普通写 `put/put_batch/remove/remove_batch` 在参数及误用检查之后取得共享资格，覆盖该次完整操作，批量只取得一次。普通 `db.get()` 延续现有读语义。
- 等待告警和终止事务分别处理。阶段②还要明确可重试异常、WAL 已确认 COMMIT 但数据应用失败、管理器销毁或所有者线程结束时的锁生命周期。遇到部分应用失败须保留提交决定，并向等待者报告需要恢复的状态，不能让它们把部分结果当作成功提交。
- 验收重点是共享并发、独占等待及唤醒、值班写偏差、普通写无旁路和异常收尾。当前跨 DB 的每线程一笔显式事务约定已按阶段①验收，锁的作用范围为单个 DB。

首次基线：`xmake build test_lsm` 成功。独立目录运行 `LSMTest.TranContextGetRejectsOtherThread`，0/1 通过。旧实现允许另一个线程读取暂存值，没有抛出 `std::logic_error`；创建线程后续读取、提交和普通点查均通过。接入检查后用同一个用例验收。

阶段①局部验收：`test_lsm` 构建成功，在独立临时目录运行 `LSMTest.TranContextGet*`，7/7 通过。跨线程读取抛出异常，创建线程仍能读出暂存值并成功提交。下一函数的基线 `LSMTest.TranContextPutRejectsOtherThread` 为 0/1；读未提交和可重复读两种情况下，外线程写入均未抛错，原值都被改为 `foreign`。

阶段①局部验收：已核对 `put/remove/commit` 将线程检查放在函数入口。独立目录运行 LSM 专项 23/23、`TranCommitWALTest.*` 8/8。旧并发刷盘用例原先在主线程创建事务、工作线程提交，触发了新约定；改为两个工作线程各自创建、暂存和提交，保留版本胜负、回滚与最终值的检查。待做的 `LSMTest.TranContextAbortRejectsOtherThread` 单独运行 0/1：外线程取消未抛异常，创建线程随后提交返回 false。

阶段①局部验收：`abort()` 的线程检查早于结束状态、WAL 及回滚登记。独立目录中 LSM 回滚专项 8/8、WAL 回滚专项 6/6 通过。上轮失败的跨线程回滚用例已通过，创建线程可以提交原来的暂存值。下一入口 `LSMTest.TranContextGetIsolationLevelRejectsOtherThread` 的基线为 0/1，唯一失败是外线程调用没有抛出 `std::logic_error`。

阶段①局部验收：`get_isolation_level()` 已在返回级别前检查创建线程。独立目录运行六个事务方法的跨线程拒绝用例和一项 `remove/put` 回归，7/7 通过。同库、跨库重复开启显式事务的两个新用例（`LSMTest.BeginTranRejectsSecond*`）在修改工厂前 0/2 通过：两种情况下第二次申请都成功并占用了 ID。测试还保留已结束句柄再次开事务，并覆盖另一库同 ID 的普通写结束登记。

线程槽首次接入：`xmake build test_lsm` 成功。三个用例在独立临时目录分别运行，同库重复 begin 用例通过，跨库重复 begin 及普通 `DirectPutWritesWalAndRegistersAutomaticFlush` 均因 SIGSEGV 退出。普通写结束时管理器找不到对应活跃上下文，但仍对 `end()` 调用了 `erase`。临时源码副本只将这行移进命中分支，重编并重链后原三项全部通过（3/3）。工作区业务修改待用户手动加入后验收。

阶段①局部验收：在用户工作区构建 `test_lsm` 并复跑之前的三个用例，3/3 通过。工厂先登记上下文再写入线程槽，结束时按对象身份清槽且只删除有效迭代器。工厂用例改为每线程逐笔结束，保留管理器持有关系与四线程合计 256 个唯一 ID 的断言；原有普通写冲突用例由另一线程执行独立写。含上述三项的 LSM 定向回归 16/16 通过。新用例 `LSMTest.DirectPutRejectsActiveTransactionOnCallingThread` 的修改前基线 0/1：同库误用成功写入数据，WAL 从 0 增至 52 字节，消耗了一个 ID。

阶段①局部验收：`LSM::put()` 及管理器的同线程、同库检查已接通，`xmake build test_lsm` 成功。在独立临时目录运行普通写、合法批量写、跨库、跨线程写及事务工厂相关用例，17/17 通过。下一入口的基线 `LSMTest.DirectPutBatchRejectsActiveTransactionOnCallingThread` 为 0/1：有活动事务的线程调用本库非空合法批量写时，已有键被覆盖、新键可见、WAL 从 48 增至 119 字节，并额外消耗一个 ID。

阶段①局部验收：`LSM::put_batch()` 在整批参数校验后、取 ID 前检查一次，原先失败的用例已通过。工作区构建成功，定向回归 22/22 通过。两种删除共用的新用例 `LSMTest.DirectRemoveAndRemoveBatchRejectActiveTransactionOnCallingThread` 在工作区为 0/1：单删和批删均未拒绝，三条旧值不可见，WAL 从 136 增至 227 字节，共消耗两个 ID。只在临时源码副本接入两处检查并重编重链，相同回归加上新用例为 23/23。待用户手动接入后验收工作区。

阶段①局部验收：在工作区提交 `d6066ba` 构建 `test_lsm`，上轮的 23 项定向回归 23/23 通过，包括两种删除被拒后原值、WAL、ID 和原事务的提交检查；六个方法的跨线程拒绝与三项创建测试另跑 9/9 通过。四个独立写入口均已验收。新用例 `LSMTest.TranContextDisallowsValueCopiesButAllowsSharedHandles` 的基线为 0/1：对象的拷贝构造和移动构造仍可用，复制句柄后的读写与结束行为正确。在临时头文件上加入四条删除声明，类型断言编译通过，工作区业务改动由用户手写。

阶段①局部验收：工作区的四条删除声明通过实际检查，`LSMTest.TranContextDisallowsValueCopiesButAllowsSharedHandles` 已通过。含该用例的 38 项公开读写、事务创建与线程归属定向回归全部通过。中英文首页和使用手册已说明复制句柄后的共同可见数据及结束状态，并注明现有发布包的边界。

生产代码仅在 `TranManager::new_tranc()` 创建上下文。测试中的 55 个显式指定 ID 的构造点已归集到 `LSMTest::make_context()`，相应的 42 项白盒用例通过。下一个验收用例 `LSMTest.TranContextRequiresFactoryToCreateTransaction` 在工作区为 0/1，唯一失败是普通代码仍可从 ID、引擎、管理器和隔离级别直接构造上下文。临时副本应用私有构造及工厂两处调整后，以现有 Debug 构建参数重编受影响单元，79/79 项去重用例通过。业务修改仍由用户手动加入。

本轮工作区构建检查：`xmake build test_lsm` 报测试夹具无法调用私有构造，公开读写调用也遇到私有成员访问错误；`xmake build kopidb` 单独确认 `new_tranc()` 仍使用 `std::make_shared<TranContext>`，标准库无权调用私有构造。两项目标均未完成构建，本轮没有运行单元测试。

2026-11-24 阶段①已验收。工作区重新构建四个 C++ 测试目标，约定的工厂、白盒与公开接口 79/79 通过。额外验证发现 21 个旧用例的操作顺序违反新的线程约定（LSM 9、WAL 12）。已调整测试，让重叠的事务各由所属线程使用，或在循环再次开启之前显式结束前一笔。WAL 故障注入继续使用真实的文件轮换错误。最终独立目录回归：LSM 182/182（排除 `Persistence`、`BigPersistence`、`BigPersistence2`、`SmallConfigLargeDataPersistent` 四项），WAL 148/148，Compact 与 WiscKey 事务专项 3/3，共 333/333。79 项包含在上述用例中。`transaction.h` 类前的“不会自动绑定”说明与已验收实现冲突，后续编辑头文件时由用户同步修正文案。
