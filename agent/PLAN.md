# KopiDB 当前计划

## SERIALIZABLE · 单个数据库实例内的悲观并发控制

本轮目标是 C++ 内核的 `SERIALIZABLE` 悲观独占和并发回归。2026-11-23 确认：事务保证覆盖现有上下文的点读、写入和删除，锁的具体实现待定。功能进度 **0/4（0%）**，按验收通过的阶段数计算。业务代码由用户逐函数实现，助手准备测试、执行回归并记录结果。

### 用户应观察到的行为

- 四个隔离级别共用线程约定：同一线程最多一笔活跃的显式事务，活动上下文只供创建线程操作，调用方在创建线程上显式提交或回滚。检查约定后才允许等待、分配事务 ID 或读写事务状态。不同线程仍可共享同一个数据库实例。
- A 请求 `SERIALIZABLE` 时，先等已经进入本实例的事务和普通写入结束。获准后再领取事务 ID。A 从开始到提交或回滚独占写入资格；同时发起的新事务和普通单笔、批量写入要等待。已有事务始终可以继续工作和结束。
- A 通过自己的事务句柄查询，能读到自己的写入和删除。仅作观察的 `db.get()` 不会改变 A 的结果；数据库级查询仍按既有规则读取，不属于 A 的事务。没有独占竞争时，其他三个级别照常并发。
- 并发的两个可串行化事务都按“对方在岗才允许自己下班”写不同的 key，最后至少有一人在岗。

### 实施草案与阶段验收

**先落地线程规则（属于阶段①至③）。** 业务实现从下面的入口顺序开始，独占锁的选型可以另行讨论。

1. 在 [transaction.h](../include/lsm/transaction.h) 的 `TranContext` 中记录只读的创建线程 ID，新增私有 `require_owner_thread() const`；[构造函数和六个公开方法](../src/lsm/transation.cpp) 的 `put/remove/get/commit/abort/get_isolation_level` 共用此检查。检查在访问可变状态与写 WAL 前运行。事务对象本身应禁止复制或移动；仍可复制 `shared_ptr`，跨线程调用由入口拒绝。
2. `TranManager::new_tranc()` 是 `LSM::begin_tran()` 的创建入口。建议用一个 `thread_local std::weak_ptr<TranContext>` 记录本线程经工厂创建的活动事务。同线程再次申请时先报错，再谈等待和分配 ID；成功插入 `activeTrans_` 后才登记到线程槽。按“每线程一笔”使用线程级槽位，独占控制仍按数据库实例独立。
3. `TranManager::add_ready_to_flush_tranc_id()` 成功结算显式事务后，按该上下文的身份清理线程槽。正常提交、显式回滚及冲突回滚都走这里；普通写的结束登记、重复登记和失败的登记不能误清其他事务。保留已结束事务的用户句柄不占用下一笔的名额。
4. 在 [engine.cpp](../src/lsm/engine.cpp) 的 `LSM::put/put_batch/remove/remove_batch` 取资格或分配 ID 之前，复用同一线程槽检查本库活动事务。通过 `tx->put/remove` 的事务内操作继续走已有路径。

验收从公开工厂走。当前 `TranContext` 的构造函数和若干字段公开，白盒用例直接组装上下文；工厂登记本身不能拦截手工构造。若将直接构造也作为对外入口，必须覆盖该路径；否则应将其收口成仅工厂与测试夹具可用。

锁实现仍待选定。候选是一把 `std::shared_mutex`（普通事务和普通写持共享锁，可串行化事务持独占锁），或由 `TranManager` 用短时互斥锁、条件变量和计数控制准入。下文内部凭证是计数路线的草案。两条路线都要落实线程约定。

| 阶段 | 新增或修改的函数 | 验收 | 状态 |
| --- | --- | --- | --- |
| ① 准入与归还 | 在 [transaction.h](../include/lsm/transaction.h) 和 [transation.cpp](../src/lsm/transation.cpp) 设计线程活动事务登记、创建者身份检查和准入。计数路线的候选函数是 `TranManager::acquire_admission(AdmissionMode)` 和 `release_admission(AdmissionMode)`。 | 同一线程再开事务时在等待和取 ID 前报错。两个线程各自持普通事务可并发，独占请求等待它们结束后进入。 | 待开始 |
| ② 显式事务 | 修改 `TranManager::new_tranc()`、`add_ready_to_flush_tranc_id()` 和 `TranContext` 构造函数，建立和解除所有者登记。在 `get/put/remove/commit/abort/get_isolation_level` 的入口统一核对创建线程。 | 跨线程使用或结束事务在访问可变状态前被拒绝，原事务仍可由创建线程结束。正常提交、冲突回滚和显式取消后，同线程均可再开一笔；仍可重试的异常保持活动登记。 | 待开始 |
| ③ 普通写与旧校验交接 | 修改 [LSM](../src/lsm/engine.cpp) 的 `put/put_batch/remove/remove_batch`。同一线程持有本库活动事务时，独立写在取锁、分配 ID、写 WAL 前报错；其他线程按共享或独占规则进入。验证后再删 `TranContext::commit()` 中实验性的单键读集校验。 | 四种普通写均能识别自等待，提示使用事务句柄或先结束事务。其他线程的合法写在独占结束后继续；原有版本和 WAL 语义通过验收。 | 待开始 |
| ④ C++ 内核回归 | 通过 `LSM::begin_tran()` 运行 [LSM 事务测试](../test/test_lsm.cpp) 和[事务 WAL 测试](../test/test_wal.cpp)，验收后更新内核声明及实现中对应的实验性注释。 | 两个值班事务至少保留一人在岗；普通写入与其他隔离级别遵守既定语义。覆盖选定的线程约定、提交与回滚唤醒、异常和重开。 | 待开始 |

**阶段①的三个内部成员。** `AdmissionGuard::AdmissionGuard(...)` 接管一次已取得的资格；`AdmissionGuard::AdmissionGuard(AdmissionGuard&&) noexcept` 移交归还责任，使移出的凭证失效；`AdmissionGuard::~AdmissionGuard() noexcept` 只为有效凭证调用一次 `release_admission()`。凭证持有管理器弱引用，不能形成“管理器 → 活跃上下文 → 管理器”的强引用环。每个 DB 独立维护短时互斥锁和条件变量；取得资格后即可释放互斥锁，等待时也不能持有管理器状态锁或存储层锁。

**阶段②的归还位置。** 工厂用一个局部凭证覆盖构造与登记失败，成功时移入上下文的可选成员。结束登记只取出并归还对应上下文的凭证；重复登记和普通写入的状态登记没有第二份凭证可退。普通写的局部凭证在该次操作退出时归还。登记、WAL 异常及已确认 COMMIT 后应用失败的处理要与现有事务状态一致，不得把未完成操作宣布为成功或擅自改为回滚。

**现有函数的复用。** `LSM::begin_tran()` 继续转发到工厂，`TranManager::getNextTransactionId()` 继续分配 ID。`TranContext::put/remove/get` 增加入口线程检查后沿用现有存取逻辑。保留 `read_map_` 和通用写冲突检查。端到端验收走 `db.begin_tran()`。

**旧测试迁移。** 阶段②将 `TranCommitWALTest.ReadChangeRejectsCommitWithoutBlockingOtherWriter` 改为双线程验证写者等待、被唤醒及 WAL 顺序。阶段③同步修订 `TranCommitTest.SerialReadValidationIncludesMissingAndDeletedKeys` 和 `TranCommitTest.SerialReadValidationIgnoresAbortedChanges`：通过公开入口覆盖缺失、删除和已回滚版本。保留 `TranCommitTest.RepeatableReadDoesNotValidateUnwrittenReadKeys` 的断言。 现有 C++ 用例若靠一个线程保持两笔活动事务或在事务内发起独立写来制造竞争，改用两个线程保留其原有版本和 WAL 断言。另以单线程用例验证误用立即失败。

### 调用方责任与待定事项

1. **调用方显式结束事务。** 创建线程在丢弃活动句柄或退出前显式提交或回滚；`activeTrans_` 强持有上下文，丢弃用户的 `shared_ptr` 不会结束事务。活动句柄的跨线程使用检查必须先于读取或修改可变事务状态，不能从另一线程代为解锁。
2. **如何反馈误用。** 建议统一抛 `std::logic_error`。同线程重复开事务、活动事务内对本库发起独立写、其他线程调用活动句柄都应明确指出操作名与事务 ID，并保留原事务；`get/remove/put` 等事务内操作使用原事务资格。跨线程复制 `shared_ptr` 本身不是可拦截的 API 调用，检查发生在上下文入口。
3. **合法的等待。** 另一线程在独占期间请求普通写，或请求独占时仍有其他线程的活动事务，可以等待。长等告警注明操作、等待时间和可定位的事务 ID；误用在等待前反馈。锁选型时同时核对等待诊断与线程退出前结束事务的约定。

**当前函数**：`TranContext::TranContext()` 与拟新增的 `TranContext::require_owner_thread() const`。业务代码尚未实施。
