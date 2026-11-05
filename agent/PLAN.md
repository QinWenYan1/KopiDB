# KopiDB 当前任务：WAL 运行机制

最后更新：2026-11-06。

章节：目录 **5.2.2「WAL 运行机制」**，文章标题 **Lab 5.4 WAL 运行机制**。

**进度：33.3%（2/6 阶段验收通过）**。按验收阶段数计算，不代表工时比例。阶段①、②已验收；阶段③的 cleaner 编译错误已修复，新增等待／退出测试后，当前 WAL 29/29 通过。旧日志筛选与删除尚未验收，阶段③未完成。上一阶段的 Record 38/38 保留为历史回归结果，本轮未重跑该组。

当前函数：[src/wal/wal.cpp](../src/wal/wal.cpp) 当前第 288 行的 `WAL::cleanWALFile()`，完整草案在聊天中提供，由用户手写。cleaner 当前在第 239 行，wait_for 已正确接收 should_stop 函数对象，代码检查及等待／关闭测试通过。

## 本轮目标

让上一节完成的 Record 真正进入 WAL 文件：支持缓冲、强制写入、文件轮换、按安全检查点清理旧日志，并让事务写入、提交和回滚按正确顺序使用 WAL。

完成后应能通过真实文件验证：记录内容完整、正常关闭不遗失待写日志、并发写入不破坏记录、写入失败不能报告成功；事务结束状态与日志中的提交／回滚决定一致。

本轮采用现有 FileObj 的文件流刷新能力。“WAL 写入成功”指追加和 sync 检查成功，不据此承诺断电后数据必然保留；该能力边界及后续章节排期见 [STATE.md](STATE.md#全局排期与延期事项)。

业务源码及头文件由用户手写；助手逐函数讲解、提供带中文注释的实现、补测试和检查。每次只推进一个函数，展示全部阶段及进度。

## 已核对资料与调用链

- [Lab 5.4 全文](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.4-WAL.md)：正文 3.1 是 WAL 组件，3.2 是 TranContext 日志接入，二者都属于本轮。
- [作者 WAL 参考](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/wal/wal.cpp)与[事务参考](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/lsm/transation.cpp)：2026-11-04 已读取；只作对照，未运行作者工程。
- [本地 WAL](../src/wal/wal.cpp)的构造、析构、log、flush、cleaner 尚未实现；cleanWALFile、reset_file 已有参考函数体，需要按本轮方案推导，不能算作已验收。
- [本地事务](../src/lsm/transation.cpp)：put/remove 先追加 operations；RU 随即调用 Engine，其他级别暂存。commit 尚未调用 write_to_wal；abort 尚未记录 ROLLBACK。write_to_wal 虽已有函数体，仍按 Lab 要求独立实现。
- [Engine](../src/lsm/engine.cpp)的 put/remove 会按总内存阈值调用 flush。因此 RU 数据可能在事务结束前进入 SST，不能不加说明地照搬“所有记录只在 commit 时写 WAL”。
- [FileObj](../src/utils/files.cpp)的 append/sync 返回 bool，调用方必须检查；[StdFile::open](../src/utils/std_file.cpp)的 create=true 会截断已有文件。sync 当前使用 fstream::flush。
- 已核对下一章的职责划分：WAL::recover、TranManager::check_recover、init_new_wal 的恢复后初始化和 LSM 构造入口属于目录 5.2.3，详见 STATE 排期。

## 函数范围

### 独立实现

1. WAL::WAL(...)
2. WAL::~WAL()
3. WAL::log(...)
4. WAL::flush()
5. WAL::cleaner()
6. TranManager::write_to_wal(...)

前五个在 [src/wal/wal.cpp](../src/wal/wal.cpp)，第六个在 [src/lsm/transation.cpp](../src/lsm/transation.cpp)。按 Lab 的“组件 → 事务接入”顺序推进。

### 实现上述功能所需的联动

- WAL::reset_file()：实现轮换，避免覆盖已有文件。
- WAL::cleanWALFile()：明确活动文件、检查点和完整记录的删除规则。
- TranContext::put()/remove()/commit()/abort()：接入预写日志，保留已经验证的 MVCC、冲突检查和回滚名单方案。
- 头文件只补必要的私有状态。建议增加清理线程的唤醒条件变量、WAL 写入故障状态，以及 TranContext 已写日志的位置。
- 建议新增一个私有辅助函数 `TranContext::persist_pending_wal_()`，只发送 operations 中尚未写入的部分；名称与签名为草案。它直接消除 put/remove/commit/abort 的重复处理，单独一轮讲解。

范围澄清：reset_file 并非 Lab 单独明列的必做函数，文件大小限制与轮换是可选方案。我们已经选择轮换且 log 会调用它，因此需要配套适配；不是看到骨架有辅助函数就要求全部重写。cleanWALFile 同样按当前清理方案检查必要改动，合理的参考代码可以沿用。

### 仅联动复核

WAL::set_checkpoint_tranc_id、TranContext 构造函数/get、已有事务状态登记、FileObj 的打开/追加/同步及测试初始化。复核不等于全部重写；若发现当前功能确实需要联动，说明具体原因后更新清单。

## 开始写函数前共同确定的方案

这些是本轮建议，逐函数实施沿用同一套规则，避免写完后反复补依赖：

1. **文件与参数。** 沿用 wal.<序号> 和连续 Record 字节流。buffer_size 按记录条数计，file_size_limit 按字节计，clean_interval 按秒计；建议缓冲阈值 0 表示直接写、文件阈值 0 表示不轮换、清理间隔 0 表示关闭周期清理。构造不清除已有日志；目录已有 WAL 时选择新序号，重放由下一章负责。
2. **写入与轮换。** 同一把 WAL 锁保护缓冲和文件状态；log 低于阈值可暂存，force_flush 和 flush 必须处理全部待写记录。写成功后才清除已写缓冲。文件阈值不是单条记录的编码上限：完整写一批允许稍超阈值，下一次有数据写入时再轮换，避免日志已经写成功却因随后建新文件失败而报告本次事务失败。
3. **写失败。** 检查 append 与 sync。发生可能留下部分写入的 I/O 错误时，报告错误并使该 WAL 停止接受后续写入，避免析构或重试把同一批记录再次追加到不确定的文件尾部。未写成功的内容不能被当作成功处理；本轮不增加自动修复文件尾部的接口。
4. **后台线程与关闭。** 清理线程等待可被析构唤醒；停止后 join，再处理最后的缓冲和关闭文件。析构不向外抛异常，写入/清理失败有明确日志。显式 flush 承担向调用方报告失败的职责。线程启动和成员初始化顺序在构造阶段一起确定。
5. **旧日志清理。** 活动文件始终保留；历史文件只有完整解码成功、所有记录 ID 都被安全检查点覆盖时才能删除。检查点是连续安全前缀，不是最大的零散已刷盘 ID。无法确认安全的文件保留并报告；避免清理与轮换、检查点更新互相竞争。事务状态文件与运行时检查点的完整接通按 STATE 排期，不直接把 get_max_flushed_tranc_id 当作删除依据。
6. **事务写日志的时机。** RC/RR/当前 SERIALIZABLE 仍暂存修改，commit 检查通过后统一写入操作及 COMMIT，再应用内存修改和登记完成。RU 的 put/remove 会立即写引擎，因此先把新增操作写入 WAL，再修改引擎；首次写入连同 CREATE 一起发送，commit 再写 COMMIT。
7. **回滚与失败。** 已经发送日志的事务取消前写 ROLLBACK；从未发送操作的暂存事务可以直接丢弃并登记回滚。沿用 ID 回滚名单，不恢复 rollback_map_ 或回写旧值。WAL 失败不登记提交成功；成功写出 COMMIT 后的内存处理错误，不能再被当作“没有提交记录，可以随意回滚”处理。具体状态转换在 commit 实现轮次明确。
8. **记录不重复发送。** 保留 operations 现有内容，用私有位置记录已写前缀，成功后才推进；失败时不继续应用对应的 RU 写入。get 不产生数据修改日志，其读取和隔离逻辑无需因 WAL 接入重写。

与作者参考相比，保留缓冲、强制写入、序号文件及检查点清理的主结构。RU 提前写日志是适配本地“写入可触发 SST 刷盘”的必要选择；代价是 RU 写操作增加日志 I/O。其他级别保持提交时批量写，不额外引入异步写线程或新的日志格式。

## 阶段与验收标准

| 阶段 | 依次处理的函数／任务 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 生命周期 | WAL 构造 → 析构 | 能创建并关闭 WAL；已有日志不被截断；初始化失败明确报错；线程和文件释放路径正确。含实际清理循环的关闭验证在阶段③补齐 | 已验收基础生命周期：7/7 |
| ② 缓冲与写入 | log → flush → reset_file | 未达阈值可缓冲；达阈值、强制刷盘、正常关闭后记录可完整解码；重复空 flush 不重复写；轮换不丢记录、不覆盖文件；写失败明确上报 | 已验收：WAL 27/27，连同 Record 共 65/65 |
| ③ 旧日志清理 | cleaner → cleanWALFile；复核 checkpoint setter | 只删除安全检查点覆盖的历史文件；活动文件及未完成范围保留；空目录／单文件正常；清理与写入并发不破坏文件；关闭能唤醒等待中的线程 | 进行中：cleaner 等待／退出通过；当前 cleanWALFile |
| ④ 管理器接入 | TranManager::write_to_wal | 实际写入并刷新，返回真实结果；WAL 未初始化或写入失败不能伪装成功；新目录测试可显式接通组件 | 待开始 |
| ⑤ 事务接入 | 私有日志辅助函数草案 → put → remove → commit → abort；复核构造/get | RU 先记录后写数据，暂存事务检查通过后写 COMMIT；冲突不写成功提交记录；取消结果与 ROLLBACK／回滚名单一致；重复结束不重复写；原有隔离与删除语义保持 | 待开始 |
| ⑥ 整体验收 | WAL 文件测试、事务联调、受影响回归、文档收尾 | 实际文件内容、线程退出、写入失败、提交／回滚顺序均有验证；Record 与原有事务测试无回归；明确已测范围和后续恢复依赖 | 待开始 |

同一行有多个函数，也分多轮给出实现。顺序沿 Lab 3.1 → 3.2；reset_file、cleanWALFile 和私有日志辅助函数在其直接使用阶段补齐。前面函数依赖后面尚未实现的功能时只标记代码检查完成，不把依赖场景提前算作通过。

## 验收用例安排

- **生命周期：** 新目录、已有日志保留、无法创建文件、空对象关闭；清理线程实现后验证可唤醒退出。带剩余缓冲的正常关闭在阶段②验收。
- **缓冲与格式：** 阈值前后、force_flush、显式 flush、空输入、混合记录、完整 64 位 ID；读取真实文件并用已验收的 Record::decode 核对内容与顺序。
- **轮换与并发：** 小文件阈值、单条记录大于轮换阈值、多个文件、多个线程各写独立批次；检查无丢失／重复，同一批内顺序不变，不要求不同线程有固定先后。
- **错误路径：** 可复现的打开／写入失败不报告成功，失败后不悄悄重写；对清理失败和析构路径分别说明实际测试与代码检查范围，不以假设的磁盘故障冒充已验证。
- **清理：** 检查点覆盖／未覆盖、乱序事务 ID、活动文件、空文件、损坏历史文件保留；测试中显式提供可信检查点，不使用最大的零散已刷盘 ID。
- **事务：** RU 的 PUT/DELETE 先写日志，CREATE/操作不重复发送；暂存事务成功提交、写冲突、显式取消、重复 commit/abort、日志失败；RU 已写数据的取消仍由回滚名单过滤。
- **原有测试迁移：** 需要事务日志的测试夹具在全新临时目录初始化 WAL，保留已有语义断言。不能为维持“没有 WAL 也能提交”的旧夹具假设，让生产 write_to_wal 无条件成功。
- **测试边界：** 继续运行 RecordTest；增加独立 WAL 运行测试和事务联调用例。旧 test_wal 中只有不依赖恢复的用例进入本轮；recover 和公开 LSM 启动相关用例源码保留，具体排期见 STATE。现有 MockWAL 的同名非虚函数不会替换基类实际调用，新增测试不能依赖这种假隔离。
- **运行方式：** 构建 test_wal/test_lsm，按阶段筛选适用测试，在独立临时目录执行；最终报告实际通过数量、命令、证据目录与未验证项。未改业务逻辑的规划轮次不运行测试。

## 当前 TODO

- [x] ① 基础生命周期（7/7；实际刷盘、等待唤醒分别在阶段②、③验收）。
- [x] ② 缓冲与写入（包含轮换和文件失败路径；WAL 27/27）。
- [ ] ③ 旧日志清理。
- [ ] ④ 管理器接入。
- [ ] ⑤ 事务接入。
- [ ] ⑥ 整体验收。

2026-11-04 构造函数草案：按成员声明顺序初始化配置与停止标记；空目录参数归一为当前目录，创建所需目录；扫描普通文件中 wal.<纯数字序号>，取最大序号加一，无旧日志时使用 wal.0；使用 uint64_t 序号，无法再递增时明确报错；打开新文件后最后启动清理线程，clean_interval=0 不启动线程。沿用 FileObj 的异常上报，不吞掉初始化失败。

历史阶段依赖：构造初次验收时析构仍为空桩，std::thread 即使已执行完函数，只要尚未 join/detach 就仍然 joinable，销毁时会 terminate。因此构造检查先用 clean_interval=0 验证目录、文件与失败路径，随后才验收创建线程后的析构。当前析构已实现，真实等待唤醒仍在阶段③补齐。

2026-11-05 构造检查：目录创建、纯数字日志名筛选、uint64_t 序号与溢出检查、旧日志保留、最后启动线程均符合方案。初始化列表仍沿用原骨架的文字顺序，当前成员初始化互不依赖，不影响结果；建议按头文件声明顺序整理，不作为阻塞问题。

助手在 test/test_wal.cpp 新增 5 项 WALTest.Constructor*：嵌套目录创建；数字最大序号（含超过 int 范围）与旧文件／无关文件保留；普通文件被当成目录；新 WAL 文件打开失败；序号达到 uint64_t 上限。xmake build test_wal 成功；隔离执行 **5/5 通过**，全部使用 clean_interval=0。测试直接检查文件存在、大小、原始字节及错误类型；未使用 MockWAL 替换后台线程，未运行后台清理、恢复或其他测试组。相关源码和测试在构建／运行期间哈希一致，助手未修改业务源码。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-constructor-review-97qgp25k`，含构建日志、执行命令、GoogleTest JSON、运行日志、汇总和源码哈希。空目录参数归一逻辑本轮仅代码检查；实际后台线程关闭依赖下一函数。

析构实现顺序：在 mutex_ 下设置停止标记，解锁后 notify_one，随后 join 清理线程，再调用自行加锁的 flush，最后 close。只有一条清理线程等待，用户采用 notify_one，当前设计下与 notify_all 都正确。flush 异常被记录，关闭流程继续；不持有 mutex_ 等待线程，以免线程无法取锁退出。cleaner_cv_ 为私有成员，后续 cleaner 使用同一 mutex_ 配合谓词等待，避免遗漏停止通知。构造时关闭后台清理或线程已经结束但尚未 join 的情况，都由 joinable 检查正确处理。

2026-11-05 析构检查：用户已正确实现停止标记、notify_one、join、flush 异常捕获和关闭文件；条件变量位置及头文件依赖正确。新增 2 项测试验证创建真实 std::thread 后正常离开作用域，以及调用方抛出 logic_error 时的栈展开。xmake build test_wal 成功，隔离运行 WALTest.Constructor*:WALTest.Destructor*，**7/7 通过**。cleaner 仍为空桩，所以只证明线程对象正确回收，不能证明实际周期等待能提前唤醒。flush 仍为桩，刷盘失败 catch 路径本轮仅代码检查。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-destructor-review-f3gww5ce`，包含构建日志、测试命令、GoogleTest JSON、运行日志、汇总和前后源码哈希。构建／测试期间源码和测试未变；助手只新增测试及维护文档，未修改业务源码。

log 草案：同一锁下检查 write_failed_、追加 Record、判断空缓冲和条数阈值；先在内存中合并编码，再执行文件操作。若原文件已达到非零轮换阈值，先 reset_file，然后检查 append 和 sync 的结果，全部成功后才 clear 缓冲。I/O 阶段发生异常则标记故障并原样抛出，后续调用拒绝继续写，防止在部分写入后整批重放。沿用现有 FileObj 时，size 的失败值也需拒绝，不能被当作超大文件而触发轮换。

2026-11-05 log 检查：用户已补齐 write_failed_、编码、append/sync 检查、异常标记及成功后清空缓冲。current_size 使用 const auto，实际推导为 size_t，按当前 FileObj 接口检查其错误值正确。用户此前讨论的 long long 接收方案未保留；本轮不修改 FileObj 的返回类型或错误接口。

新增 7 项测试：按记录条数达到阈值；旧缓冲与新批次强制合并（五类记录、二进制 value）；空输入强制写出且不重复；零缓冲阈值逐批追加；8 个线程各写完整批次；关闭文件后追加失败；启用大小检查时关闭文件造成 size 失败。两项错误场景检查保留缓冲，并在测试重新打开文件后仍拒绝新记录，验证故障状态不会悄悄清除。测试专用派生类只观察受保护成员及关闭／重新打开文件，不替换非虚函数，也不新增业务接口。

xmake build test_wal 成功；隔离运行构造 5、析构 2、新增 log 7，共 **14/14 通过**。实际文件使用 Record::decode 验证完整内容与顺序，所有写入场景未触发文件轮换。未运行原 LogAndFlush、恢复或其他测试组；未注入短写、磁盘满或单独的 sync 失败，sync 返回值处理为代码检查。flush 尚未接通，所以当前结果不能证明显式 flush 或析构能写出剩余缓冲。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-log-review-ydt1n4rj`。包含构建日志、精确过滤器、GoogleTest JSON、测试日志、汇总及源码哈希；构建／运行期间相关源码和测试未变，助手未修改业务源码。

flush 实现：直接调用 log({}, true)，空 records 不新增记录，true 强制处理全部已有缓冲。锁、编码、文件写入与错误传播复用 log；用户已删除原有 lock_guard，没有重复获取同一把非递归 mutex。显式调用时异常自然向上传递，析构处已有捕获。

2026-11-05 flush 检查：新增 5 项测试，覆盖显式 flush 写出不足阈值的记录与空调用不重复、文件失败异常传播、无清理线程／有清理线程时析构自动写出缓冲、析构捕获真实写入错误，以及调用方异常展开时仍保留原异常。xmake build test_wal 成功；隔离运行构造／析构／log／flush 共 **19/19 通过**。析构中的失败通过关闭测试文件制造，保存确实未成功，测试只确认错误处理和资源退出；未注入磁盘短写或单独的 sync 失败。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-flush-review-aicjsgnl`。过滤器为 WALTest.Constructor*:WALTest.Destructor*:WALTest.Flush*:WALTest.Log*-WALTest.LogAndFlush；原 LogAndFlush 及恢复组仍未运行。包含构建日志、命令、GoogleTest JSON、汇总和源码哈希，构建／运行期间源码和测试未变；助手只修改测试与文档。

reset_file 草案：仅由持有 mutex_ 的 log 调用，本函数不重复加锁。解析当前文件名中的 uint64_t 序号，检查递增上限；构造同目录下一序号的路径，发现目标已存在时明确拒绝覆盖。先以局部变量调用 FileObj::create_and_write 创建空文件，成功后用 FileObj 的 noexcept 移动赋值接管句柄，再用 string::swap 更新路径；创建失败时旧句柄与旧路径都不变。旧日志文件保留给 cleaner，当前缓冲仍由 log 负责写入及清空。

2026-11-05 reset_file 检查：用户实现符合当前轮换方案，utility 已补齐，不重复加锁，创建文件前保持成员不变，成功后用 noexcept 移动赋值和 swap 切换。新增 7 项测试：精确阈值下连续三个文件及空 flush 不轮换；单条超出轮换阈值仍完整；超过 int 范围的序号；已有目标拒绝覆盖；实际创建失败保留旧句柄／路径／缓冲；uint64_t 序号耗尽；并发写入与轮换串行完成。创建失败通过重命名测试目录制造 ENOENT，旧文件打开句柄仍可读取，未通过改写业务代码模拟失败。

阶段②验收：xmake build test_wal 成功，隔离运行 **WAL 27/27、Record 38/38，合计 65/65 通过**。WAL 包括此前 19 项、新增轮换 7 项及原 LogAndFlush；未运行恢复、LSM 事务集成或其他目标。构建／运行期间源码及测试哈希一致，助手未修改业务源码。单独的 sync 失败和磁盘短写仍未注入，不将当前错误路径测试解释为完整故障注入或断电持久性验证。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-rotation-review-o_a8yop6`。过滤器为 RecordTest.*:WALTest.Constructor*:WALTest.Destructor*:WALTest.Flush*:WALTest.Log*:WALTest.Rotation*；包含构建日志、执行命令、GoogleTest JSON、汇总与源码哈希。

cleaner 草案：使用 unique_lock 与 cleaner_cv_.wait_for，等待时释放 mutex_；停止谓词放在循环外定义，读取 stop_cleaner_。谓词为 true 则退出，超时才执行清理；调用自行加锁的 cleanWALFile 前解锁，结束后重新取锁进入下一次等待。清理异常记录后继续下一周期，避免异常逃出线程入口终止进程。析构原有的同锁停止标记、notify_one 和 join 与此配合。

2026-11-06 cleaner 初次检查：已填写代码，等待／解锁／异常处理／重新加锁的结构与方案一致，但 wait_for(lock, interval, should_stop()) 提前调用 lambda，第三参数实际为 bool。xmake build test_wal 失败，编译器报告 called object type 'bool' is not a function or function pointer，定位 wal.cpp:263。应改成 wait_for(lock, interval, should_stop)，交给条件变量按需调用谓词。本轮没有运行测试或执行旧二进制，助手未修改业务源码或测试。

历史失败证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-cleaner-review-w8muy_vb`，含构建日志、返回码及前后源码哈希，构建期间源码和测试未变。该编译错误已在下面的复测中修复。

2026-11-06 cleaner 复测：should_stop 参数已修正，解锁后调用 cleanWALFile、异常捕获和重新加锁正确。新增 2 项测试：60 秒清理间隔下前台 log 能完成，以及析构能够提前结束长等待；各预留 100ms 调度机会，用 5 秒上限避免依赖毫秒级调度精度。构建成功，隔离运行 **WAL 29/29 通过**，包括此前 27 项回归。测试未等待清理超时，不据此认定旧文件删除逻辑正确；清理异常分支仍为代码检查。cleaner 前的 TODO 标签可移除，不影响功能判断。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-wal-cleaner-wait-review-is2ady3p`。过滤器为 WALTest.Constructor*:WALTest.Destructor*:WALTest.Flush*:WALTest.Log*:WALTest.Rotation*:WALTest.Cleaner*；包含构建／测试日志、命令、JSON 和源码哈希。执行期间源码与测试未变，助手只补测试和文档，未改业务源码。

cleanWALFile 草案：整轮持有 mutex_，确保活动文件、检查点和删除决策保持一致；逐个检查普通文件及纯数字 WAL 后缀，只处理序号小于当前活动文件的历史日志。复用 Record::decode 验证整份内容；任一记录 ID 大于 checkpoint_tranc_id_ 就保留整个文件，全部满足才关闭读取句柄并删除。空历史日志可删，损坏或读取失败的文件保留并报告，单个文件失败不阻碍检查其余文件。文件大小错误值沿用现有接口检查。

取舍：该简单同步方案在清理期间会让 log 等待，等待周期本身仍会释放锁。打开与读取使用 FileObj；删除使用 std::filesystem::remove，因为当前 FileObj::del_file 丢弃删除结果，无法上报实际文件系统错误。本轮不扩展 FileObj 接口。该辅助函数是当前清理方案的配套实现，不新增独立 Lab 任务。

本轮只给出 cleanWALFile 草案，尚未执行。下一轮验收检查点边界、活动／未来序号保护、损坏文件保留、实际周期清理及并发衔接，完成阶段③后再进入管理器接入。
