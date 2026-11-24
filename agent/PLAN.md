# KopiDB 当前计划

## C++ 可串行化事务 · 2026-11-25 实施计划

**目标**　一个 `TranManager` 管一个 DB。可串行化事务先等本库已有的显式事务和普通写结束，再独占到自己提交或回滚登记完成。其间用户能通过 `tx->get/put/remove` 读取已有值、自己的写入和删除。其他三个隔离级别与普通写可以并发。不同 DB 各用自己的锁。[已验收的线程约定](STATE.md#当前进度)

**接口范围**　C++ 显式事务覆盖现有的单键读写。普通 `db.get/get_batch` 和数据库级遍历沿用各自的可见性约定。读未提交在 `put/remove` 时写入，其余三级暂存到共同的 `commit()`。取得库级锁后才分配版本号，等待顺序由标准读写锁调度。[现有事务接口](../include/lsm/transaction.h#L58) · [暂存与立即写入](../src/lsm/transation.cpp#L161)

**当前步骤**　1.1 · `TranManager::lock_for_direct_write()`。本轮给出声明和函数体，由用户写入，助手逐函数检查、补测试并更新清单。

1.1 草案已在独立源码副本用项目编译参数通过语法检查。工作区实现和行为测试待验收。

| 阶段 | 任务与改动 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 库级锁 | `lock_for_direct_write()` 返回标准共享锁，`wait_for_transaction_lock()` 处理共享或独占请求的等待。 | 同库共享持有者可并发，共享与独占、独占与独占互斥。一个库的等待不影响另一个库。长等只告警一次，放锁后请求可继续。 | 进行中 |
| ② 显式事务 | `new_tranc()` 在分配 ID 前取锁，`add_ready_to_flush_tranc_id()` 登记结束后由创建线程放锁。 | 经 `begin_tran()` 验证三个较低级别可并发，可串行化等它们全部结束。同线程误用在等待前报错。保留已结束句柄或在别处销毁它，均不妨碍下一笔事务。 | 待开始 |
| ③ 普通写 | 给 `LSM` 的 `put`、`put_batch`、`remove`、`remove_batch` 各加一次共享锁。 | 同库独占期间四个入口等待，此时未分配 ID、写 WAL 或改动数据。反过来，先进入的整批写完后，独占事务才能开始。空批次仍可直接返回。 | 待开始 |
| ④ 故障收尾 | 四级共用现有提交、回滚和重开恢复。用 `recovery_required_` 向等待者和新请求报告严重故障。 | 普通冲突可正常回滚并放行。结束结果不确定或提交只应用了一部分时，等待中和随后申请的事务、普通写收到重开提示。重开后按 WAL 恢复。 | 待开始 |
| ⑤ 切换与回归 | 移除 `commit()` 的实验性读集校验，用并发用例验证行为并同步 C++ 文档。 | 两笔可串行化事务做值班交接后至少一人仍在岗。点查、删除、混合级别、普通写、错误和重开回归通过，文档准确描述源码与已发布版本各自的能力。 | 待开始 |

共 21 项。旧版把取锁、等待和故障检查合算为一项。新版分别计普通写取锁入口、正常等待、故障后的等待检查，增加两个验收点。其余计数仍按函数承担的具体改动，最后一项是整体回归与文档。前置线程规则、头文件准备及旧草案的独立验证不计入本轮。

<!-- plan-progress: 可串行化实现 -->

### ① 库级锁

- [ ] **1.1** 新增 `TranManager::lock_for_direct_write(const char *operation)`。给管理器加 `transaction_mutex_`，每个实例创建一把 `std::shared_timed_mutex`。函数复用 `require_direct_write_allowed()` 检查同线程误用，返回持有这把锁的 `std::shared_lock<std::shared_timed_mutex>`。普通写的调用栈由 `LSM` 保证管理器存活。[管理器声明](../include/lsm/transaction.h#L105) · [现有线程检查](../src/lsm/transation.cpp#L1263)
- [ ] **1.2** 新增私有 `TranManager::wait_for_transaction_lock(Lock &lock, const char *operation)`。调用方传入延迟加锁的标准共享锁或独占锁，1.1 改用此函数等待。无竞争时直接取锁。等待达到 1 秒只告警一次，记录操作名、时长和当时能取得的活跃事务 ID，之后继续等。查看 ID 时短暂尝试取得管理器锁，记录日志时释放它。[活跃事务表](../include/lsm/transaction.h#L186)

已有的 `IsolationLevel` 和标准库锁对象替代旧草案中的 `AdmissionMode`、`AdmissionLock` 和 `AdmissionState`。`transaction_mutex_` 由 `shared_ptr` 持有，方便显式事务的创建线程在管理器先销毁时仍能安全解锁。`mutex_` 继续保护事务登记表。等待库级锁前，调用方应已放开管理器、SST 和 MemTable 的锁。[管理器短锁](../include/lsm/transaction.h#L166) · [既有生命周期用例](../test/test_lsm.cpp#L2520)

### ② 显式事务

- [ ] **2.1** 新增 `ThreadTransactionSlot::release() noexcept`。扩充现有线程槽，在同一处保存事务弱引用、mutex 的保活指针和实际持有的标准锁。`std::variant` 表示空、共享、独占三种情况。释放时由创建线程先解锁，再清理引用。[当前线程槽](../include/lsm/transaction.h#L189)
- [ ] **2.2** 修改 `TranManager::require_direct_write_allowed()`，从槽内读取原来的事务弱引用。切换线程槽声明时，同步适配 `new_tranc()` 及 `add_ready_to_flush_tranc_id()` 现有的弱引用访问，使中间版本可构建。同线程对同库的独立写在等锁前报错，对另一个 DB 的普通写按目标库的锁执行。[误用检查](../src/lsm/transation.cpp#L1263)
- [ ] **2.3** 修改 `TranManager::add_ready_to_flush_tranc_id()`。提交、主动回滚和冲突回滚共用此处。登记成功后，仅在结束的上下文与本线程槽中的对象相同时释放。普通写和启动恢复也会调用此函数，其登记须保留其他事务持有的锁。[现有登记与身份比较](../src/lsm/transation.cpp#L1136)
- [ ] **2.4** 修改 `TranManager::new_tranc()`。检查线程占位后，`SERIALIZABLE` 选择独占锁，其余三级选择共享锁，两者都经过 1.2 取锁。接着短暂持有 `mutex_` 分配 ID、创建并登记上下文，登记成功才把局部锁移入线程槽。创建失败由局部标准锁解锁。按对象识别已经失效的旧库绑定，在创建线程清理它。同一事务的点读写沿用已取得的锁。[事务工厂](../src/lsm/transation.cpp#L1281) · [C++ 公开入口](../src/lsm/engine.cpp#L1519)

在 2.4 接通独占的同一轮调整已有并发用例。先确认另一线程已发起申请，让持锁者结束，再等申请返回。使用明确的线程信号和有界等待，保留原有数据、WAL 和事务状态的有效断言。[现有同步等待的用例](../test/test_wal.cpp#L2070)

### ③ 普通写

以下四个入口分别把原有的单独线程检查换成 1.1 的取锁调用，返回的标准锁作为局部变量保存。在参数检查后、分配 ID 前取锁，覆盖 WAL、数据应用、结束登记及最后的完成标记，直到整次调用退出。事务内的 `engine->put/remove` 和启动重放继续走底层路径。

- [ ] **3.1** `LSM::put()` 保护整次单键写入。[单键写入](../src/lsm/engine.cpp#L1204)
- [ ] **3.2** `LSM::put_batch()` 整批共享一个锁，保留参数检查和输入顺序。[批量写入](../src/lsm/engine.cpp#L1271)
- [ ] **3.3** `LSM::remove()` 保护墓碑的写入和完成登记。[单键删除](../src/lsm/engine.cpp#L1337)
- [ ] **3.4** `LSM::remove_batch()` 整批共享一个锁，包括最后一个完成标记。[批量删除](../src/lsm/engine.cpp#L1393)

### ④ 故障收尾

四个级别继续共用 `commit()`、`abort()`。正常写冲突登记回滚并返回 `false`。严重错误仍向原调用方抛出，此外由同一个 `TranManager` 通知尚在等待及之后申请的显式事务和普通写。其他已经通过入口的共享事务可能仍在执行，标志的检查点只设在取库级锁的路径。让调用方协调停用旧实例、重开同一目录，由现有 WAL 流程恢复。[冲突回滚](../src/lsm/transation.cpp#L483) · [启动重放](../src/lsm/engine.cpp#L1075)

- [ ] **4.1** 新增 `TranManager::mark_recovery_required(const TranContext *ending = nullptr) noexcept`。单个内存标志 `recovery_required_` 初始为 `false`，故障时先置 `true`。显式结束失败可传入当前上下文，仅释放身份匹配的线程槽。普通写由局部锁自行释放。保留原异常和持久化的事务决定。
- [ ] **4.2** 修改 `TranManager::wait_for_transaction_lock()`，在等待前、分段等待期间和成功取锁后检查标志，拒绝时抛出提示重开的 `std::runtime_error`。覆盖四级事务和四个普通写入口，故障库之外的 DB 可继续使用。
- [ ] **4.3** 新增 `ThreadTransactionSlot::~ThreadTransactionSlot() noexcept`。创建线程带着尚未结束的持锁事务退出时，先将仍存活的管理器标为需要重开，再由该线程释放锁。管理器已经销毁时用槽内保活的 mutex 完成解锁。测试同时覆盖其他线程销毁已结束句柄的情况。
- [ ] **4.4** 修改 `TranContext::commit()` 的失败出口。结束日志结果不确定、已确认 COMMIT 后应用或登记未完成，先置标志再放锁。普通冲突走原分支。已经完整应用并登记后，冻结或刷盘出错保留提交决定，报告维护异常。保留 `commit(true)` 的恢复测试语义。[WAL 确认](../src/lsm/transation.cpp#L500) · [完成登记](../src/lsm/transation.cpp#L539)
- [ ] **4.5** 修改 `TranContext::abort()` 的失败出口。仍可继续的事务保持持锁。回滚日志结果不确定，或已写出回滚决定但结束登记失败时，置标志并由创建线程释放锁，保留真实结果供重开。读未提交继续按需要写 ROLLBACK 并登记回滚 ID。[回滚及登记](../src/lsm/transation.cpp#L639)
- [ ] **4.6** `LSM::put()` 区分构造记录失败、WAL 结果不确定、数据应用和结束登记失败。[写入路径](../src/lsm/engine.cpp#L1241)
- [ ] **4.7** `LSM::put_batch()` 覆盖整组 COMMIT 已写出但只应用了部分 key 的情况。[批量写入路径](../src/lsm/engine.cpp#L1318)
- [ ] **4.8** `LSM::remove()` 按墓碑的应用和登记进度处理异常。[删除路径](../src/lsm/engine.cpp#L1369)
- [ ] **4.9** `LSM::remove_batch()` 覆盖部分墓碑已应用的情况。[批量删除路径](../src/lsm/engine.cpp#L1437)

普通写准备记录时已知尚未发送 WAL 的错误沿用原有取消逻辑。进入实际 WAL 写入后结果不确定、或已提交数据未完整应用与登记时，在局部锁析构前标记故障。故障验证分别覆盖等待者、故障后的新请求、另一库及实际重开。WAL 的提交判定、文件格式和重放沿用现有实现。[已确认提交后的维护异常用例](../test/test_lsm.cpp#L4063) · [重开恢复用例](../test/test_lsm.cpp#L4169)

### ⑤ 切换与回归

- [ ] **5.1** 修改 `TranContext::commit()`，在前四阶段接通后移除 `SERIALIZABLE` 专用的实验性读集比较。保留暂存型事务共用的写版本冲突检查、可重复读的读缓存和读取自身修改的行为。[共用写集检查](../src/lsm/transation.cpp#L430) · [实验读集检查](../src/lsm/transation.cpp#L453)
- [ ] **5.2** 完成 C++ 回归和使用文档。两个工作线程分别读取对方的值班状态再决定自己是否下班，任一先后顺序下至少一人仍在岗。同时覆盖读自己的写、删除与不存在的 key、混合隔离、普通写双向等待、合法回滚与故障重开。核对源码实验性注释，以及中英文首页和使用手册的版本说明。[LSM 用例](../test/test_lsm.cpp) · [WAL 用例](../test/test_wal.cpp) · [中文首页](../README.md) · [英文首页](../README.en.md) · [使用手册](../docs/usage.md#concurrency-and-transactions)

<!-- /plan-progress -->

回归在独立临时目录运行新增用例、原 `test_lsm` 功能集、完整 `test_wal` 及 Compact/WiscKey 的事务专项。沿用既有口径，`LSMTest.Persistence`、`BigPersistence`、`BigPersistence2`、`SmallConfigLargeDataPersistent` 四项长测另行记录实际运行情况。发布与 SDK 排期见[项目状态](STATE.md#当前进度)。
