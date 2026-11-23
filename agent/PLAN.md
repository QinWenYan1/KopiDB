# KopiDB 当前计划

## SERIALIZABLE · 单个数据库实例内的悲观并发控制

本轮目标是 C++ 内核的 `SERIALIZABLE` 悲观独占和并发回归。2026-11-23 确认：事务保证覆盖现有上下文的点读、写入和删除，锁的具体实现待定。功能进度 **0/3（0%）**，按验收通过的阶段数计算。业务代码由用户逐函数实现，助手准备测试、执行回归并记录结果。

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

验收从公开工厂走。当前 `TranContext` 的构造函数和若干字段公开，白盒用例直接组装上下文；工厂登记本身不能拦截手工构造。若将直接构造也作为对外入口，必须覆盖该路径；否则应将其收口成仅工厂与测试夹具可用。

锁实现仍待选定。候选是一把 `std::shared_mutex`（普通事务和普通写持共享锁，可串行化事务持独占锁），或由 `TranManager` 用短时互斥锁、条件变量和计数控制准入。下文内部凭证是计数路线的草案。两条路线都要落实线程约定。

| 阶段 | 工作 | 验收 | 状态 |
| --- | --- | --- | --- |
| ① 落实两条线程规则 | 先记录创建线程并从 `TranContext::get()` 接入所有者检查，再覆盖其余事务 API、工厂登记、结束清理以及本库独立写入口。 | 跨线程调用、同线程重复开启事务均在产生副作用前报错；创建线程可继续使用、结束事务并开启下一笔。两个线程各自使用自己的事务仍可并发。 | 进行中 |
| ② 商讨串行化实现 | 比较 `std::shared_mutex` 与管理器计数方案，定下取得与归还资格的时机、合法等待的反馈及锁顺序。 | 确认方案、函数清单和并发测试场景。 | 待开始 |
| ③ 实现并验收串行化 | 接通显式事务和四个普通写入口，为独占增加等待与唤醒。两条路径验收后再移除 `TranContext::commit()` 的旧单键读集校验。 | 值班案例、普通写与混合隔离、提交/回滚唤醒、异常及重开通过 [C++ 事务测试](../test/test_lsm.cpp) 和 [WAL 测试](../test/test_wal.cpp)。 | 待开始 |

**计数路线的候选凭证。** `AdmissionGuard::AdmissionGuard(...)` 接管一次已取得的资格；`AdmissionGuard::AdmissionGuard(AdmissionGuard&&) noexcept` 移交归还责任，使移出的凭证失效；`AdmissionGuard::~AdmissionGuard() noexcept` 只为有效凭证调用一次 `release_admission()`。凭证持有管理器弱引用，不能形成“管理器 → 活跃上下文 → 管理器”的强引用环。每个 DB 独立维护短时互斥锁和条件变量；取得资格后即可释放互斥锁，等待时也不能持有管理器状态锁或存储层锁。

**计数路线的候选归还位置。** 工厂用一个局部凭证覆盖构造与登记失败，成功时移入上下文的可选成员。结束登记只取出并归还对应上下文的凭证；重复登记和普通写入的状态登记没有第二份凭证可退。普通写的局部凭证在该次操作退出时归还。登记、WAL 异常及已确认 COMMIT 后应用失败的处理要与现有事务状态一致，不得把未完成操作宣布为成功或擅自改为回滚。

**现有函数的复用。** `LSM::begin_tran()` 继续转发到工厂，`TranManager::getNextTransactionId()` 继续分配 ID。`TranContext::put/remove/get` 增加入口线程检查后沿用现有存取逻辑。保留 `read_map_` 和通用写冲突检查。端到端验收走 `db.begin_tran()`。

**旧测试迁移。** 阶段③将 `TranCommitWALTest.ReadChangeRejectsCommitWithoutBlockingOtherWriter` 改为双线程验证写者等待、被唤醒及 WAL 顺序。阶段③同步修订 `TranCommitTest.SerialReadValidationIncludesMissingAndDeletedKeys` 和 `TranCommitTest.SerialReadValidationIgnoresAbortedChanges`：通过公开入口覆盖缺失、删除和已回滚版本。保留 `TranCommitTest.RepeatableReadDoesNotValidateUnwrittenReadKeys` 的断言。 现有 C++ 用例若靠一个线程保持两笔活动事务或在事务内发起独立写来制造竞争，改用两个线程保留其原有版本和 WAL 断言。另以单线程用例验证误用立即失败。

### 调用方责任与待定事项

1. **调用方显式结束事务。** 创建线程在丢弃活动句柄或退出前显式提交或回滚；`activeTrans_` 强持有上下文，丢弃用户的 `shared_ptr` 不会结束事务。活动句柄的跨线程使用检查必须先于读取或修改可变事务状态，不能从另一线程代为解锁。
2. **如何反馈误用。** 建议统一抛 `std::logic_error`。同线程重复开事务、活动事务内对本库发起独立写、其他线程调用活动句柄都应明确指出操作名与事务 ID，并保留原事务；`get/remove/put` 等事务内操作使用原事务资格。跨线程复制 `shared_ptr` 本身不是可拦截的 API 调用，检查发生在上下文入口。
3. **合法的等待。** 另一线程在独占期间请求普通写，或请求独占时仍有其他线程的活动事务，可以等待。长等告警注明操作、等待时间和可定位的事务 ID；误用在等待前反馈。锁选型时同时核对等待诊断与线程退出前结束事务的约定。

**当前函数**：`LSM::put_batch()`。空批次直接返回；整批 key/value 校验完成后、创建 WAL 记录和分配 ID 前，对整批调用一次 `require_direct_write_allowed(__func__)`。

首次基线：`xmake build test_lsm` 成功。独立目录运行 `LSMTest.TranContextGetRejectsOtherThread`，0/1 通过。旧实现允许另一个线程读取暂存值，没有抛出 `std::logic_error`；创建线程后续读取、提交和普通点查均通过。接入检查后用同一个用例验收。

阶段①局部验收：`test_lsm` 构建成功，在独立临时目录运行 `LSMTest.TranContextGet*`，7/7 通过。跨线程读取抛出异常，创建线程仍能读出暂存值并成功提交。下一函数的基线 `LSMTest.TranContextPutRejectsOtherThread` 为 0/1；读未提交和可重复读两种情况下，外线程写入均未抛错，原值都被改为 `foreign`。

阶段①局部验收：已核对 `put/remove/commit` 将线程检查放在函数入口。独立目录运行 LSM 专项 23/23、`TranCommitWALTest.*` 8/8。旧并发刷盘用例原先在主线程创建事务、工作线程提交，触发了新约定；改为两个工作线程各自创建、暂存和提交，保留版本胜负、回滚与最终值的检查。待做的 `LSMTest.TranContextAbortRejectsOtherThread` 单独运行 0/1：外线程取消未抛异常，创建线程随后提交返回 false。

阶段①局部验收：`abort()` 的线程检查早于结束状态、WAL 及回滚登记。独立目录中 LSM 回滚专项 8/8、WAL 回滚专项 6/6 通过。上轮失败的跨线程回滚用例已通过，创建线程可以提交原来的暂存值。下一入口 `LSMTest.TranContextGetIsolationLevelRejectsOtherThread` 的基线为 0/1，唯一失败是外线程调用没有抛出 `std::logic_error`。

阶段①局部验收：`get_isolation_level()` 已在返回级别前检查创建线程。独立目录运行六个事务方法的跨线程拒绝用例和一项 `remove/put` 回归，7/7 通过。同库、跨库重复开启显式事务的两个新用例（`LSMTest.BeginTranRejectsSecond*`）在修改工厂前 0/2 通过：两种情况下第二次申请都成功并占用了 ID。测试还保留已结束句柄再次开事务，并覆盖另一库同 ID 的普通写结束登记。

线程槽首次接入：`xmake build test_lsm` 成功。三个用例在独立临时目录分别运行，同库重复 begin 用例通过，跨库重复 begin 及普通 `DirectPutWritesWalAndRegistersAutomaticFlush` 均因 SIGSEGV 退出。普通写结束时管理器找不到对应活跃上下文，但仍对 `end()` 调用了 `erase`。临时源码副本只将这行移进命中分支，重编并重链后原三项全部通过（3/3）。工作区业务修改待用户手动加入后验收。

阶段①局部验收：在用户工作区构建 `test_lsm` 并复跑之前的三个用例，3/3 通过。工厂先登记上下文再写入线程槽，结束时按对象身份清槽且只删除有效迭代器。工厂用例改为每线程逐笔结束，保留管理器持有关系与四线程合计 256 个唯一 ID 的断言；原有普通写冲突用例由另一线程执行独立写。含上述三项的 LSM 定向回归 16/16 通过。新用例 `LSMTest.DirectPutRejectsActiveTransactionOnCallingThread` 的修改前基线 0/1：同库误用成功写入数据，WAL 从 0 增至 52 字节，消耗了一个 ID。

阶段①局部验收：`LSM::put()` 及管理器的同线程、同库检查已接通，`xmake build test_lsm` 成功。在独立临时目录运行普通写、合法批量写、跨库、跨线程写及事务工厂相关用例，17/17 通过。下一入口的基线 `LSMTest.DirectPutBatchRejectsActiveTransactionOnCallingThread` 为 0/1：有活动事务的线程调用本库非空合法批量写时，已有键被覆盖、新键可见、WAL 从 48 增至 119 字节，并额外消耗一个 ID。
