# KopiDB 当前任务：Record 编解码

最后更新：2026-11-04。

章节：目录 **5.2.1「Record 编解码」**，文章标题 **Lab 5.3 WAL 日志编解码**。

**进度：100%（5/5 阶段验收通过）**。按验收阶段数计算，不代表工时比例。七个函数均由用户实现并完成本轮功能验收：RecordTest **38/38 通过**，相关事务回归 **42/42 通过**，合计 **80/80**。本轮已收尾，下一章按 STATE 中的 Lab 排期推进。

当前函数：无，七个函数已验收。最后核对的 `Record::decode()` 位于 [src/wal/record.cpp](../src/wal/record.cpp) 当前第 202 行，`pos != record_end` 校验已补齐。default 的错误文本仍建议改为 unknown operation type；未知类型已实际被拒绝，此文字修订不阻塞功能验收。

## 本轮目标

独立实现 Record 的五个记录创建函数及 encode/decode，使事务操作能够准确转换为字节数组，并从单条或连续多条记录的字节数组中还原事务 ID、操作类型和对应的 key/value。

本轮产物是内存记录与字节数组之间的转换。后续章节和全局延期事项统一见 [STATE.md](STATE.md#全局排期与延期事项)，不混入本轮 TODO。

业务源码及头文件由用户手写；助手逐函数给出带中文注释的完整实现、检查修改、补测试并维护进度。同一阶段内包含多个函数时仍分轮处理。

## 已读资料与当前代码

- [Lab 5.3，lab-doc-2 分支](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.3-Record.md)：正文 2.1 要求五个静态创建函数，2.2 要求 encode/decode；本节仅处理字节数组，文件 I/O 在后续章节。
- [作者 Record 参考，master 分支](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/wal/record.cpp)：2026-11-02 已读取，仅作为方案对照，未运行作者工程。
- 本地 [record.cpp](../src/wal/record.cpp) 的七个函数均已由用户独立实现并验收。decode 已补齐每条记录末尾的长度一致性检查；read_string 在 while 外定义，通过参数接收 record_end；utility 头文件已补齐。
- [record.h](../include/wal/record.h) 保留现有公开接口和拼写。getter、print、operator==/!= 仅联动复核，不列入必须重写的函数。
- 已核对调用关系：TranContext 的创建、put、remove、commit 会构造 Record 并保存到 operations；当前仅保存在内存。rollbackRecord 是本节要求的记录类型，其后续调用接入按 STATE 排期处理。
- 本轮测试加入 [test/test_wal.cpp](../test/test_wal.cpp) 的独立 `RecordTest` 测试组，目前已有工厂函数 18 项、编码 7 项、解码 13 项，共 38 项；只筛选该组运行，不构造 WAL 对象。工厂测试逐字段核对；编码与解码各有独立的已知字节样例，并补充混合流及最大记录往返验证。

## 共同设计：在首个函数前讲清

沿用 Lab 的字段布局和现有类型编号，保留 uint16_t 长度字段：

| 记录类型 | 字段 | 总编码长度，含 record_len 本身 |
| --- | --- | --- |
| CREATE / COMMIT / ROLLBACK | 总长 2B + 事务 ID 8B + 类型 1B | 11 字节 |
| DELETE | 公共头 + key 长度 2B + key | 13 + key.size() |
| PUT | 公共头 + key 长度 2B + key + value 长度 2B + value | 15 + key.size() + value.size() |

本轮方案：

1. 总长度和 key/value 长度必须能由现有字段表示。先检查字段长度，再用 size_t 计算总长；通过检查后才转换为 uint16_t。总长超限抛 length_error，不能截断后继续编码。
2. 总长上限包括公共头及各长度字段，因此 Record 的限制与 SST 单个 key/value 长度限制不能混为一谈。开始 putRecord 时明确展示边界例子，并检查已有 TranContext 调用的正常行为。
3. 整数明确使用小端编码。作者参考使用 memcpy 写本机字节序；本方案在当前小端平台上保持相同字段字节顺序，避免让编码规则依赖主机。
4. Record 只保存操作内容；零长度字符串和内嵌空字节按长度处理，不在这一层加入公开 API 的空 key 拒绝策略，也不根据空 value 擅自改变操作类型。
5. encode 输出一条完整记录；decode 接受连续拼接的多条记录，按输入顺序返回。空输入返回空列表；非空但截断、长度矛盾或类型未知的输入应明确报错，不越界读取、不静默接受坏记录。读取每个字段前，以本条 record_len 的边界检查剩余空间。

这些规则在逐函数讲解时解释依据；若实际调用约束要求调整，先说明影响再更新计划，不直接扩展成其他模块的重构。

## 阶段与验收

| 阶段 | 依次实现的函数／任务 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 事务状态记录 | createRecord → commitRecord → rollbackRecord | 三种记录的类型、完整 64 位 ID 正确；key/value 为空；长度计算代码检查正确，编码完成后验证为 11B | 已验收 |
| ② 数据操作记录 | putRecord → deleteRecord | 保留对应 key/value；DELETE 不携带 value；合法边界可创建、字段或总长超限明确报错；零长度与二进制内容不被改写 | 已验收 |
| ③ 编码 | encode | 五种记录字节布局正确，长度与实际输出一致；已知字节样例、字段边界和完整 64 位 ID 通过 | 已验收 |
| ④ 解码 | decode | 单条与多条混合流准确还原，顺序不变；空输入正确；截断、错误长度和未知类型明确拒绝 | 已验收：解码 13/13 通过 |
| ⑤ 整体验收 | Record 测试及受影响事务回归、记录收尾 | 各类记录往返及已知字节对照通过；现有事务记录创建路径不回归；说明实际运行与尚未接通的范围 | 已验收：合计 80/80 通过 |

### 阶段①：事务状态记录

- [x] `Record::createRecord()`：当前 record.cpp:18。类型、完整 ID、空 key/value 的 2 项测试通过；阶段③已确认编码为 11B。
- [x] `Record::commitRecord()`：当前 record.cpp:38。类型、完整 ID、空 key/value 的 2 项测试通过；阶段③已确认编码为 11B。
- [x] `Record::rollbackRecord()`：当前 record.cpp:56。类型、完整 ID、空 key/value 的 2 项测试通过；阶段③已确认编码为 11B。
- [x] 助手补充类型、ID 和无负载字段测试，三个函数各 2 项，共 6/6 通过。私有 record_len 未新增 getter，实际编码长度已在阶段③验证。

2026-11-02 检查：`xmake build test_wal` 成功；在隔离临时目录运行 `--gtest_filter=RecordTest.*`，**2/2 通过**。覆盖普通事务开始记录，以及 0、超过 32 位、最高位为 1、uint64_t 最大值的 ID 原样保留。未调用 encode/decode，未运行后续 WAL／恢复用例或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-create-review-9xliflzd`，包含构建日志、测试命令、GoogleTest JSON、测试日志及源码哈希。

2026-11-03 检查：commitRecord 实现正确，新增 2 项测试验证 OP_COMMIT、空 key/value 及完整 64 位 ID。`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，包含前两项 CREATE 测试共 **4/4 通过**。未调用 encode/decode，未运行 WAL／恢复或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-commit-review-curkb6nc`。

2026-11-03 阶段①验收：rollbackRecord 实现正确，新增 2 项测试验证 OP_ROLLBACK、空 key/value 及完整 64 位 ID。`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，三类状态记录共 **6/6 通过**。各函数的 11B 长度计算通过代码检查；未调用 encode/decode，未运行后续 WAL／恢复或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-rollback-review-8w92__ml`。

### 阶段②：数据操作记录

- [x] `Record::putRecord()`：当前 record.cpp:74。6 项新增测试通过，覆盖字段及独立副本、空字符串、内嵌空字节、合法总长边界、总长与字段超限。
- [x] `Record::deleteRecord()`：当前 record.cpp:113。6 项新增测试通过，覆盖字段及 key 独立副本、空 key、内嵌空字节、65522B 合法 key、总长及字段超限。
- [x] 助手覆盖普通字符串、零长度、内嵌空字节、恰好达到总长上限及超限；通过 getter 检查数据完整性。

putRecord 调用链已核对：TranContext::put 在修改 MemTable 或 temp_map_ 前调用该工厂；长度超限若在工厂中抛出，本次操作不会追加到 operations，也不会应用到存储层。沿用现有格式，PUT 总长为 15 + key.size() + value.size()：key 为 1B 时，value 为 65519B 恰好达到 65535B，65520B 则超限。当前实现先检查字段与总长，再复制字符串和转换 uint16_t。

2026-11-03 putRecord 检查：`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，**12/12 通过**，包括新增 6 项 PUT 测试。验证调用方修改原字符串后记录仍稳定、完整 ID、空字符串不改变 PUT 类型、内嵌空字节、总长恰好 65535B、超出 1B、各字段均合法但合计超限，以及单个字段达到 65536B。构建和测试前后 Record 源码、头文件、测试文件哈希一致；未调用 encode/decode 或未完成的 deleteRecord，未运行 WAL／恢复及其他测试组。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-put-review-5c7ro_ep`。

2026-11-03 阶段②验收：deleteRecord 实现正确，用户已补齐 limits、stdexcept。新增 6 项 DELETE 测试，`xmake build test_wal` 成功，原 deleteRecord 缺少返回值的警告已消失；隔离运行 `--gtest_filter=RecordTest.*`，**18/18 通过**。构建和测试前后 Record 源码、头文件、测试文件哈希一致。DELETE 布局为公共头 11B + key 长度 2B + key，不编码 value 长度或内容；已验证 key 为 65522B 时合法，65523B、65535B、65536B 时分别按总长或字段超限拒绝。未调用 encode/decode，未运行 WAL／恢复或其他测试组。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-delete-review-okob8ucq`。

### 阶段③：编码

- [x] `Record::encode()`：当前 record.cpp:148。
- [x] 五类记录用已知字节序列验证字段顺序、操作类型编码和小端整数。
- [x] 验证实际输出长度、字符串原始字节、uint64_t 较高位，以及前两阶段创建的边界记录。

encode 已实现：按小端规则逐字节追加整数，字符串追加长度和原始字节；用 reserve(record_len_) 一次预留容量，再按字段顺序 push_back/insert。CREATE／COMMIT／ROLLBACK 只有公共头；PUT／DELETE 写 key，只有 PUT 写 value。用户保留的 0xffu 与教学中展示的二进制掩码等价，无需为写法差异返工。当前 WAL::log 只有后续编码写入的 TODO，本轮直接通过 Record 测试验收字节输出，不提前实现 WAL 文件写入。

2026-11-04 阶段③验收：新增 7 项编码测试，`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，**25/25 通过**。覆盖三类状态记录的 11B 固定样例、PUT／DELETE 布局、零长度字段、空字节及高位字节、完整 64 位 ID、超过 255 的两字节长度，以及最大 PUT／DELETE 的 65535B 完整输出。没有调用 decode，不依赖编解码互相抵消错误。构建和测试前后 Record 源码、头文件、测试文件哈希一致；未运行 WAL／恢复或其他测试组。当前 decode 为空造成的缺少返回值警告属于下一待实现函数。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-encode-review-i3aue1sh`。

### 阶段④：解码

- [x] `Record::decode()`：当前 record.cpp:202。已补齐 `pos != record_end` 检查并通过复测。
- [x] 单条、连续多条、混合操作、不同事务 ID 和重复 key 保持原顺序，不在编解码中去重或应用事务操作。
- [x] 每条记录独立检查头部、记录长度和负载范围；覆盖首条及后续记录截断、零／过小长度、字段长度矛盾、未知操作类型。
- [x] 只验证输入字节流的完整解码，不用本节测试替代崩溃恢复验收。

decode 方案：用 pos 指向下一待读字节，先以 data.size() 为界读取记录长度，再检查长度至少为 11B 且不超过从 record_start 起的剩余数据，计算 record_end。整数和字符串都以 record_end 为边界；两个读取 lambda 均放在 while 外，边界通过参数传入。按类型读取负载后必须确认 pos 恰好等于 record_end，再移动记录进结果列表。整数按小端顺序用 uint64_t 拼接，先扩宽再移位；空输入返回空列表，截断、类型未知或长度矛盾抛 runtime_error。字符串按字节长度构造，保留二进制内容。未来 WAL::recover 调用接入按原排期处理。

2026-11-04 decode 检查：新增 13 项测试，`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，**37/38 通过**，其中解码 **12/13 通过**。正常五种类型、混合流、重复 key、空／二进制字段、完整 ID、最大记录往返及首条／后续截断、非法长度、未知类型、字段越界均通过。失败项为 `DecodeRejectsUnconsumedBytesInsideRecord`：将第一条声明的长度扩大以包含后一条完整记录，五种类型均未抛出应有异常。

失败原因（已修复）：当时 switch 结束后直接保存记录，没有检查 pos 是否等于 record_end。例如 CREATE 实际只消费 11B，却声明为 22B，后面又有一条 11B 记录，旧实现会接受错误输入。用户现已在 switch 后、push_back 前补上校验；五种记录类型的该场景均复测通过。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-decode-review-ayzyzqi7`。构建与测试前后 Record 源码、头文件、测试文件哈希一致；未修改业务源码，尚未执行阶段⑤的事务回归。

2026-11-04 阶段④复测：`xmake build test_wal` 成功；隔离运行 RecordTest **38/38 通过**，其中解码 **13/13 通过**。修复前的失败记录保留供追溯，不代表当前状态。功能上未知类型已经被拒绝；default 的错误文字仍为长度不匹配，已给用户单行替换建议，不作为新增功能问题。

### 阶段⑤：整体验收

- [x] 构建 test_wal，在隔离目录运行 `--gtest_filter=RecordTest.*`，保证测试不依赖未实现的 WAL 运行机制。
- [x] 使用已知字节验证与 encode/decode 往返验证共同检查，避免两个函数犯相同错误却互相抵消。
- [x] 回归受工厂函数影响的事务构造、put/remove、commit/abort 用例；按实际变化决定是否需要更广回归。
- [x] 检查本轮注释、函数签名与调用衔接，记录构建／测试结果及限制；未运行的后续用例保留源码。
- [x] 将本轮完成情况写入 STATE，并收尾当前 PLAN。下一轮确认目标后再替换本计划。

2026-11-04 最终验收：RecordTest **38/38**、test_lsm 中相关事务用例 **42/42**，共 **80/80 通过**；两个目标均构建成功。事务过滤器：`LSMTest.TranContextConstructor*:LSMTest.NewTransaction*:LSMTest.TranContextPut*:LSMTest.TranContextGet*:TranRemoveTest.*:TranAbortTest.*:TranCommitTest.*`。覆盖事务创建及登记、立即／暂存写入、读取、删除、提交冲突、回滚和相关刷盘衔接。未运行其他测试组、WAL 文件运行机制或崩溃恢复测试。

测试迁移说明：初次事务回归 **41/42**，唯一失败为旧 `FlushFailurePreservesCommittedStateAndMemory`。该用例通过 70000B value 制造刷盘失败，现在会被 Record 在 put 阶段提前拒绝。助手将场景改为同 key 的三个 40000B 版本：单条记录均合法，第三条在 Block 内的偏移超限，从而仍在提交后的刷盘阶段抛异常。原有状态／内存／刷盘登记／不可重复提交断言全部保留，新增旧版本仍可查询的断言；完整 42 项事务用例复测通过。没有修改业务源码或放宽 Record 长度检查。

最终证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-final-review-n7il8x42`。包含 Record、事务修正前／后运行命令与日志、GoogleTest JSON、构建日志、汇总结果和源码哈希。最终构建与运行期间相关源码及测试文件哈希一致；阶段⑤唯一测试改动为上述用例的场景迁移。
