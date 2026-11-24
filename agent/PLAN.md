# KopiDB 当前计划

## C++ 可串行化事务的实现路线

库级悲观锁方案已选定。以下五阶段是供审阅的代码修改顺序。**实施进度 `[░░░░░░░░░░]` 0%（0/5）**。每个阶段验收通过计 20%，五项全部通过为 100%。已验收的线程规则作为起点，历史结果见 [STATE](STATE.md#当前进度)。

**完成后的行为**　A 在本库成功取得 `SERIALIZABLE` 事务时，先前进入的显式事务和普通写入均已结束其逻辑操作。A 通过 `tx->get/put/remove` 读取和修改数据，包括读取自己的暂存写入和删除。直到 A 提交或回滚完成，其他线程在本库新开事务或调用普通写都要等待，即使操作的是另一个 key。RU、RC、RR 和普通写之间可以共享入口。普通 `db.get/get_batch` 及数据库遍历继续按其各自的读取约定使用。

| 阶段 | 主要代码 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 建立每个 DB 的读写锁 | 新增 `TranManager::acquire_admission()` 和 `mark_recovery_required()` | 同库两个共享持有者可以同时进入，独占与共享、独占与独占互斥。A 库的锁不阻塞 B 库。长等请求告警一次，持锁方退出后仍能进入。 | 待开始 |
| ② 接通显式事务的开始和结束 | `TranManager::new_tranc()`、`add_ready_to_flush_tranc_id()`、`require_direct_write_allowed()` | 两个线程经 `db.begin_tran()` 双向验证共享与独占的等待关系。提交、主动回滚、冲突回滚都能放行等待者。保留已结束句柄也能开下一笔。 | 待开始 |
| ③ 接通四个普通写入口 | `LSM::put()`、`put_batch()`、`remove()`、`remove_batch()` | 独占期间四个入口都等待，期间没有新 ID、WAL 或数据副作用。反过来，一批已进入的写必须完整结束，独占事务才能读到它的结果。 | 待开始 |
| ④ 验证异常时的放锁时机 | `TranContext::commit()`、`abort()`、四个普通写入口接入 `mark_recovery_required()` | 可继续的事务仍持锁。结束结果不确定或只应用一部分时，等待者得到需重开的错误。已经应用并登记的提交遇到后续刷盘失败，保留其提交结果。 | 待开始 |
| ⑤ 替换实验分支并完成验收 | `TranContext::commit()` 的单键读集检查、C++ 并发与 WAL 测试、双语说明 | 两个可串行化事务执行值班案例后至少一人仍在岗。三种较低隔离级别、四个普通写、线程归属、故障重开回归通过，文档与实际 C++ 能力一致。 | 待开始 |

### ① 锁归谁，怎样等待

- 在 `TranManager` 增加一份本库独有的锁状态（`std::shared_timed_mutex` 和内存中的需恢复标志）。拟新增内部方法 `[[nodiscard]] AdmissionLock acquire_admission(AdmissionMode mode, const char *operation)`。返回值持有共享锁或独占锁，由标准锁对象管理释放。另一内部方法 `TranManager::mark_recovery_required() noexcept` 只设置本库标志，供后续故障路径使用。等待库级锁之前须释放已有的管理器 `mutex_`、SST 锁和 MemTable 锁。[事务声明](../include/lsm/transaction.h#L102)
- 等待用单调时钟计时。建议首版用内部 **1 秒**作为长等告警阈值，记录操作、已等时长、能定位的活跃事务 ID。每次请求只告警一次，并继续等待。发生需重开的故障时，等待循环及成功拿锁后都检查本库状态。调度采用标准读写锁提供的顺序。[C++ 读写锁及所有权](https://eel.is/c++draft/thread.sharedtimedmutex.class)

### ② 先接释放点，再让事务持锁

- 扩充已有的 `thread_transaction_`。建议由一个私有 `ThreadTransactionSlot` 保存事务的弱引用和持锁对象。持锁对象让本库的锁状态存活，普通写的持锁对象只放在调用栈上。事务的创建线程负责解锁，用户可以在之后另行销毁已经结束的句柄。[已有线程槽](../include/lsm/transaction.h#L186)
- 先接 `TranManager::add_ready_to_flush_tranc_id()` 的成功登记分支。匹配本线程**同一个上下文**时，调用拟新增的私有 `ThreadTransactionSlot::release() noexcept` 释放并清槽。成功提交、主动回滚、写冲突回滚共用这里。普通写和启动恢复也调用登记函数，但它们不能清掉另一笔事务的锁。`require_direct_write_allowed()` 同步改为从槽内取弱句柄。[结束登记](../src/lsm/transation.cpp#L1136) · [同线程误用检查](../src/lsm/transation.cpp#L1263)
- 再改 `TranManager::new_tranc()`。顺序为检查本线程占位 → 取本库锁 → 在管理器短锁内分配 ID 并登记 → 将锁从局部移入线程槽。构造或登记失败由局部对象释放。`SERIALIZABLE` 取独占，其他三级取共享。`LSM::begin_tran()` 已转发工厂，事务内 `get/put/remove` 直接使用原有存取路径。[工厂](../src/lsm/transation.cpp#L1281) · [公开入口](../src/lsm/engine.cpp#L1519)
- 调用方在创建线程显式结束事务。如果线程带着活跃事务退出，拟在 `ThreadTransactionSlot::~ThreadTransactionSlot()` 中先将仍存活的本库标为需恢复，再由原线程释放锁，交给 WAL 恢复决定事务结果。管理器先销毁时，线程槽仍持有锁状态，原线程下次使用槽时清理失效绑定。测试分别覆盖句柄跨线程析构和线程退出。[C++ 线程局部对象的析构时机](https://eel.is/c++draft/basic.start.term)
- 并发测试从本阶段起使用独立拥有者线程、明确的启动信号和有界等待。先让持锁事务结束，再等待发起申请的线程返回。原来同步等待另一事务先提交的实验用例，要同步迁移调度，保留数据和 WAL 断言。[现有等待顺序的实例](../test/test_wal.cpp#L2070)

### ③ 普通写也要经过同一把锁

在四个入口已有的 `require_direct_write_allowed()` 之后、取 ID 之前，各加一次共享锁。一批只取一次锁，覆盖写 WAL、应用整批数据、登记完成和写入完成标记。保留四个入口已有的参数检查和空批次行为。`LSMEngine::put/remove` 同时用于事务内部或恢复，放在公开 `LSM` 层可以避免重复取锁。[put](../src/lsm/engine.cpp#L1204) · [put_batch](../src/lsm/engine.cpp#L1271) · [remove](../src/lsm/engine.cpp#L1337) · [remove_batch](../src/lsm/engine.cpp#L1393)

### ④ 按事务实际走到哪一步处理异常

`commit()` 当前先确认 WAL 并设 `isCommited = true`，随后才应用 `temp_map_`、写完成标记和登记。**允许下一笔事务看到提交结果的时机是数据已完整应用且登记成功。** [提交决定](../src/lsm/transation.cpp#L500) · [完成登记](../src/lsm/transation.cpp#L537) · [回滚](../src/lsm/transation.cpp#L639)

| 发生情况 | 处理和验证 |
| --- | --- |
| 数据或参数错误后事务仍可继续，或一次普通写在发送 WAL 前失败 | 显式事务继续持锁，可在原线程完成。普通写离开函数即释放。用例验证下一次合法操作有正确结果。 |
| 结束日志写出结果不确定，或 WAL 已确认结束但数据或完成登记尚未完成 | 保留原异常及 WAL 决定。`mark_recovery_required() noexcept` 先设置该库标志，再由原线程释放锁。等待中和以后申请的事务及普通写收到 `std::runtime_error`，提示重新打开该库。原线程同时清理自己的占位，管理器保留真实的事务状态供恢复。真实 WAL 故障和重开验证覆盖显式结束及普通写。 |
| 已完整应用并登记，之后冻结或刷 SST 抛异常 | 保存已登记结果，向调用方传递维护错误。恢复及后续等待者按已完成的事务处理。`commit(true)` 的故意跳过应用沿用专用恢复测试约定。 |

### ⑤ 切换保证并验收公开 C++ 接口

- 前四阶段通过后，移除 `TranContext::commit()` 专属 `SERIALIZABLE` 的单键读集比较。共享的写版本冲突检查、可重复读的 `read_map_`、读回自身暂存值继续使用。[旧读集分支](../src/lsm/transation.cpp#L453) · [共用写冲突检查](../src/lsm/transation.cpp#L430)
- 通过 `db.begin_tran()` 验收两个线程的值班场景。两人均在岗，各自先读对方，只在对方仍在岗时才让自己下班。允许任一线程先进入，后进入者必须看见前一笔的结果。再验证点查不存在、读自己的写和删除、独占与普通写两种先后顺序、其他隔离级别共享、跨库独立、误用在等待前报错及 WAL 重开。[LSM 测试](../test/test_lsm.cpp) · [WAL 测试](../test/test_wal.cpp)
- 最终回归在独立临时目录运行新增用例、原 LSM 功能集、完整 `test_wal` 及原 Compact/WiscKey 事务专项。沿用上次 LSM 功能集边界，四项长测 `Persistence`、`BigPersistence`、`BigPersistence2`、`SmallConfigLargeDataPersistent` 单独标明。验收按本轮实际运行结果计数。
- 核对 `transaction.h`、`engine.h`、`transation.cpp`、`engine.cpp` 及测试中的实验性标识。普通点读、迭代器和维护操作沿用各自的接口约定。C++ 源码验收后同步首页和使用手册，并区分源码能力与已经发布的 SDK 版本。[中文首页](../README.md) · [英文首页](../README.en.md) · [使用手册](../docs/usage.md#concurrency-and-transactions)

**当前第一项**　在 `transaction.h` 定义库级锁状态和获取接口，再在 `transation.cpp` 实现 `TranManager::acquire_admission()`。路线经用户审阅后逐函数开始，用户手写业务代码，助手维护测试并报告每阶段验收结果。
