# KopiDB 当前计划

## SERIALIZABLE · 单个数据库实例内的悲观并发控制

本轮目标是 C++ 内核的 `SERIALIZABLE` 悲观独占和并发回归。2026-11-23 确认：锁的实现仍在讨论，事务范围读取的接口也待确认。功能进度 **0/5（0%）**，按验收通过的阶段数计算。业务代码由用户逐函数实现，助手准备测试、执行回归并记录结果。

### 用户应观察到的行为

- A 请求 `SERIALIZABLE` 时，先等已经进入本实例的事务和普通写入结束。获准后再领取事务 ID。A 从开始到提交或回滚独占写入资格；同时发起的新事务和普通单笔、批量写入要等待。已有事务始终可以继续工作和结束。
- A 通过自己的事务句柄查询，能读到自己的写入和删除。仅作观察的 `db.get()` 不会改变 A 的结果；数据库级查询仍按既有规则读取，不属于 A 的事务。没有独占竞争时，其他三个级别照常并发。
- 并发的两个可串行化事务都按“对方在岗才允许自己下班”写不同的 key，最后至少有一人在岗。如果提供事务内的范围读取，A 两次扫描同一范围，期间另一线程试图插入或删除，A 应看到相同的键和值，除非 A 自己修改了数据。

### 实施草案与阶段验收

两条候选路线是事务持有 `std::shared_mutex` 的共享或独占锁，以及 `TranManager` 用短时互斥锁、条件变量和计数控制准入。下列凭证、函数名和跨线程归还用例是第二条路线的设计草案。选择第一条路线时，需要先明确同线程重复获取和跨线程解锁的约束，再调整函数清单。

| 阶段 | 新增或修改的函数 | 验收 | 状态 |
| --- | --- | --- | --- |
| ① 准入与归还 | 在 [transaction.h](../include/lsm/transaction.h) 声明、[transation.cpp](../src/lsm/transation.cpp) 实现 `TranManager::acquire_admission(AdmissionMode)` 和 `TranManager::release_admission(AdmissionMode) noexcept`。内部枚举 `AdmissionMode::{Shared, Exclusive}` 区分普通操作与独占事务。拟新增的内部凭证 `AdmissionGuard` 只有三个需要实现的成员，见下。 | 两个共享参与者能一起进入，独占等已有参与者退出。独占结束后等待者继续。资格可跨线程归还，两个 DB 独立。 | 待开始 |
| ② 显式事务 | 修改 `TranManager::new_tranc()` 和 `TranManager::add_ready_to_flush_tranc_id()`。前者先取得资格，再调用 `getNextTransactionId()` 并登记上下文；后者在事务结束状态登记成功后，归还该活跃上下文持有的资格。核对 `TranContext::commit()` 的成功、冲突、异常分支和 `TranContext::abort()`；它们已经调用同一个登记函数。 | 已活跃的 RU 可以提交或回滚；等待的独占事务随后读到正确版本。已结束句柄即使保留也不占资格。重复结束、创建失败、结束失败均按事务实际状态处理。同步迁移旧 WAL 用例的调度。 | 待开始 |
| ③ 普通写与旧校验交接 | 修改 [LSM](../src/lsm/engine.cpp) 的 `put()`、`put_batch()`、`remove()`、`remove_batch()`。每个入口在分配 ID 前取得一个局部共享凭证，一批数据共用一次资格。先验证这四条路径和阶段②，随后修改 `TranContext::commit()`，移除仅 SERIALIZABLE 执行的单键读集校验，再回归。 | 独占期间四个写入口均等待；先开始的普通批量写完整结束后才允许独占进入。去掉实验性分支后，值班例子仍通过，RR 原有写冲突测试仍通过。 | 待开始 |
| ④ 事务范围读取（接口待定） | 候选方案为新增 C++ `TranContext::scan(const std::string &lower, const std::string &upper)`，返回按 key 排序的 `std::vector<std::pair<std::string, std::string>>`。复用 `LSMEngine::lsm_iters_monotony_predicate()` 收集区间，再合入 `temp_map_` 的新增、覆盖和删除。 | 左闭右开区间、空结果及 MemTable/SST 混合数据正确。另一事务试图新增或删除时，两次扫描稳定；自己的修改即时可见。 | 待开始 |
| ⑤ C++ 内核回归 | 通过 `LSM::begin_tran()` 运行 [LSM 事务测试](../test/test_lsm.cpp) 和[事务 WAL 测试](../test/test_wal.cpp)，验收后更新内核声明及实现中对应的实验性注释。 | 两个值班事务至少保留一人在岗；若提供事务范围读取，验证不出现幻读；普通写入与其他隔离级别遵守既定语义。覆盖跨线程归还、提交与回滚唤醒、异常和重开。 | 待开始 |

**阶段①的三个内部成员。** `AdmissionGuard::AdmissionGuard(...)` 接管一次已取得的资格；`AdmissionGuard::AdmissionGuard(AdmissionGuard&&) noexcept` 移交归还责任，使移出的凭证失效；`AdmissionGuard::~AdmissionGuard() noexcept` 只为有效凭证调用一次 `release_admission()`。凭证持有管理器弱引用，不能形成“管理器 → 活跃上下文 → 管理器”的强引用环。每个 DB 独立维护短时互斥锁和条件变量；取得资格后即可释放互斥锁，等待时也不能持有管理器状态锁或存储层锁。

**阶段②的归还位置。** 工厂用一个局部凭证覆盖构造与登记失败，成功时移入上下文的可选成员。结束登记只取出并归还对应上下文的凭证；重复登记和普通写入的状态登记没有第二份凭证可退。普通写的局部凭证在该次操作退出时归还。登记、WAL 异常及已确认 COMMIT 后应用失败的处理要与现有事务状态一致，不得把未完成操作宣布为成功或擅自改为回滚。

**直接复用的现有函数。** `LSM::begin_tran()` 继续转发到工厂；`TranManager::getNextTransactionId()` 继续分配 ID；`TranContext::TranContext()` 沿用原签名，由工厂转交凭证；`TranContext::put()/remove()/get()` 继续处理自身暂存数据。首版保留 `read_map_` 以及暂存写事务共用的写冲突检查。直接构造上下文、调用裸 Engine 的内部夹具与公开 `db.begin_tran()` 的独占验收分开。

**旧测试迁移。** 阶段②将 `TranCommitWALTest.ReadChangeRejectsCommitWithoutBlockingOtherWriter` 改为双线程验证写者等待、被唤醒及 WAL 顺序。阶段③同步修订 `TranCommitTest.SerialReadValidationIncludesMissingAndDeletedKeys` 和 `TranCommitTest.SerialReadValidationIgnoresAbortedChanges`：通过公开入口覆盖缺失、删除和已回滚版本。保留 `TranCommitTest.RepeatableReadDoesNotValidateUnwrittenReadKeys` 的断言。

### 调用方责任与待定事项

1. **调用方显式结束事务。** 调用者应在丢弃活动句柄前调用 `commit()` 或 `abort()`。遗漏结束属于调用方误用；当前 `activeTrans_` 强持有上下文，其他操作可能因此一直等待。自动回滚不作为本轮前置任务。同一线程若在独占事务里同步调用独立的 `db.put()`，它后面的 `tx->commit()` 就无法执行；事务内的写入用 `tx->put()`。
2. **范围读取入口。** 现有 `LSM::begin/end` 和 `LSM::lsm_iters_monotony_predicate()` 都属于数据库。`TranContext::scan(lower, upper)` 是让调用者能在同一事务内查询一段 key 的提议；是否提供这一入口、名称和适用级别均待确认。

**当前讨论**：阶段①的锁实现。若采用计数路线，第一个候选函数为 `TranManager::acquire_admission(AdmissionMode)`；若采用 `std::shared_mutex`，先确认持锁与解锁的线程约定。
