# KopiDB 当前任务：Record 编解码

最后更新：2026-11-04。

章节：目录 **5.2.1「Record 编解码」**，文章标题 **Lab 5.3 WAL 日志编解码**。

**进度：60%（3/5 阶段验收通过）**。按验收阶段数计算，不代表工时比例。五个创建函数及 encode 均已验收，独立 RecordTest 共 25/25 通过；前两阶段的实际编码长度也已验证。当前进入 decode。

当前函数：`Record::decode(const std::vector<uint8_t> &data)`，位于 [src/wal/record.cpp](../src/wal/record.cpp) 当前第 200 行。

## 本轮目标

独立实现 Record 的五个记录创建函数及 encode/decode，使事务操作能够准确转换为字节数组，并从单条或连续多条记录的字节数组中还原事务 ID、操作类型和对应的 key/value。

本轮产物是内存记录与字节数组之间的转换。后续章节和全局延期事项统一见 [STATE.md](STATE.md#全局排期与延期事项)，不混入本轮 TODO。

业务源码及头文件由用户手写；助手逐函数给出带中文注释的完整实现、检查修改、补测试并维护进度。同一阶段内包含多个函数时仍分轮处理。

## 已读资料与当前代码

- [Lab 5.3，lab-doc-2 分支](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.3-Record.md)：正文 2.1 要求五个静态创建函数，2.2 要求 encode/decode；本节仅处理字节数组，文件 I/O 在后续章节。
- [作者 Record 参考，master 分支](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/wal/record.cpp)：2026-11-02 已读取，仅作为方案对照，未运行作者工程。
- 本地 [record.cpp](../src/wal/record.cpp) 的五个创建函数及 encode 均已由用户实现并检查通过，decode 已留为空函数。七个函数均按 Lab 要求重新推导、逐个实现，不能把参考函数体视为本轮已完成。
- [record.h](../include/wal/record.h) 保留现有公开接口和拼写。getter、print、operator==/!= 仅联动复核，不列入必须重写的函数。
- 已核对调用关系：TranContext 的创建、put、remove、commit 会构造 Record 并保存到 operations；当前仅保存在内存。rollbackRecord 是本节要求的记录类型，其后续调用接入按 STATE 排期处理。
- 本轮测试加入 [test/test_wal.cpp](../test/test_wal.cpp) 的独立 `RecordTest` 测试组，目前已有工厂函数 18 项、编码 7 项，共 25 项；只筛选该组运行，不构造 WAL 对象。工厂测试使用 getter 逐字段核对，编码测试直接对照已知字节和完整输出，不依赖尚未实现的 decode。

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
| ④ 解码 | decode | 单条与多条混合流准确还原，顺序不变；空输入正确；截断、错误长度和未知类型明确拒绝 | 进行中：当前 decode |
| ⑤ 整体验收 | Record 测试及受影响事务回归、记录收尾 | 各类记录往返及已知字节对照通过；现有事务记录创建路径不回归；说明实际运行与尚未接通的范围 | 待开始 |

### 阶段①：事务状态记录

- [x] `Record::createRecord()`：当前 record.cpp:17。类型、完整 ID、空 key/value 的 2 项测试通过；阶段③已确认编码为 11B。
- [x] `Record::commitRecord()`：当前 record.cpp:37。类型、完整 ID、空 key/value 的 2 项测试通过；阶段③已确认编码为 11B。
- [x] `Record::rollbackRecord()`：当前 record.cpp:55。类型、完整 ID、空 key/value 的 2 项测试通过；阶段③已确认编码为 11B。
- [x] 助手补充类型、ID 和无负载字段测试，三个函数各 2 项，共 6/6 通过。私有 record_len 未新增 getter，实际编码长度已在阶段③验证。

2026-11-02 检查：`xmake build test_wal` 成功；在隔离临时目录运行 `--gtest_filter=RecordTest.*`，**2/2 通过**。覆盖普通事务开始记录，以及 0、超过 32 位、最高位为 1、uint64_t 最大值的 ID 原样保留。未调用 encode/decode，未运行后续 WAL／恢复用例或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-create-review-9xliflzd`，包含构建日志、测试命令、GoogleTest JSON、测试日志及源码哈希。

2026-11-03 检查：commitRecord 实现正确，新增 2 项测试验证 OP_COMMIT、空 key/value 及完整 64 位 ID。`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，包含前两项 CREATE 测试共 **4/4 通过**。未调用 encode/decode，未运行 WAL／恢复或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-commit-review-curkb6nc`。

2026-11-03 阶段①验收：rollbackRecord 实现正确，新增 2 项测试验证 OP_ROLLBACK、空 key/value 及完整 64 位 ID。`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，三类状态记录共 **6/6 通过**。各函数的 11B 长度计算通过代码检查；未调用 encode/decode，未运行后续 WAL／恢复或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-rollback-review-8w92__ml`。

### 阶段②：数据操作记录

- [x] `Record::putRecord()`：当前 record.cpp:73。6 项新增测试通过，覆盖字段及独立副本、空字符串、内嵌空字节、合法总长边界、总长与字段超限。
- [x] `Record::deleteRecord()`：当前 record.cpp:112。6 项新增测试通过，覆盖字段及 key 独立副本、空 key、内嵌空字节、65522B 合法 key、总长及字段超限。
- [x] 助手覆盖普通字符串、零长度、内嵌空字节、恰好达到总长上限及超限；通过 getter 检查数据完整性。

putRecord 调用链已核对：TranContext::put 在修改 MemTable 或 temp_map_ 前调用该工厂；长度超限若在工厂中抛出，本次操作不会追加到 operations，也不会应用到存储层。沿用现有格式，PUT 总长为 15 + key.size() + value.size()：key 为 1B 时，value 为 65519B 恰好达到 65535B，65520B 则超限。当前实现先检查字段与总长，再复制字符串和转换 uint16_t。

2026-11-03 putRecord 检查：`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，**12/12 通过**，包括新增 6 项 PUT 测试。验证调用方修改原字符串后记录仍稳定、完整 ID、空字符串不改变 PUT 类型、内嵌空字节、总长恰好 65535B、超出 1B、各字段均合法但合计超限，以及单个字段达到 65536B。构建和测试前后 Record 源码、头文件、测试文件哈希一致；未调用 encode/decode 或未完成的 deleteRecord，未运行 WAL／恢复及其他测试组。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-put-review-5c7ro_ep`。

2026-11-03 阶段②验收：deleteRecord 实现正确，用户已补齐 limits、stdexcept。新增 6 项 DELETE 测试，`xmake build test_wal` 成功，原 deleteRecord 缺少返回值的警告已消失；隔离运行 `--gtest_filter=RecordTest.*`，**18/18 通过**。构建和测试前后 Record 源码、头文件、测试文件哈希一致。DELETE 布局为公共头 11B + key 长度 2B + key，不编码 value 长度或内容；已验证 key 为 65522B 时合法，65523B、65535B、65536B 时分别按总长或字段超限拒绝。未调用 encode/decode，未运行 WAL／恢复或其他测试组。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-delete-review-okob8ucq`。

### 阶段③：编码

- [x] `Record::encode()`：当前 record.cpp:147。
- [x] 五类记录用已知字节序列验证字段顺序、操作类型编码和小端整数。
- [x] 验证实际输出长度、字符串原始字节、uint64_t 较高位，以及前两阶段创建的边界记录。

encode 已实现：按小端规则逐字节追加整数，字符串追加长度和原始字节；用 reserve(record_len_) 一次预留容量，再按字段顺序 push_back/insert。CREATE／COMMIT／ROLLBACK 只有公共头；PUT／DELETE 写 key，只有 PUT 写 value。用户保留的 0xffu 与教学中展示的二进制掩码等价，无需为写法差异返工。当前 WAL::log 只有后续编码写入的 TODO，本轮直接通过 Record 测试验收字节输出，不提前实现 WAL 文件写入。

2026-11-04 阶段③验收：新增 7 项编码测试，`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，**25/25 通过**。覆盖三类状态记录的 11B 固定样例、PUT／DELETE 布局、零长度字段、空字节及高位字节、完整 64 位 ID、超过 255 的两字节长度，以及最大 PUT／DELETE 的 65535B 完整输出。没有调用 decode，不依赖编解码互相抵消错误。构建和测试前后 Record 源码、头文件、测试文件哈希一致；未运行 WAL／恢复或其他测试组。当前 decode 为空造成的缺少返回值警告属于下一待实现函数。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-encode-review-i3aue1sh`。

### 阶段④：解码

- [ ] `Record::decode()`：当前 record.cpp:200。
- [ ] 单条、连续多条、混合操作、不同事务 ID 和重复 key 保持原顺序，不在编解码中去重或应用事务操作。
- [ ] 每条记录独立检查头部、记录长度和负载范围；覆盖首条及后续记录截断、零／过小长度、字段长度矛盾、未知操作类型。
- [ ] 只验证输入字节流的完整解码，不用本节测试替代崩溃恢复验收。

decode 草案：用 pos 指向下一待读字节，先以 data.size() 为界读取记录长度，再检查长度至少为 11B 且不超过从 record_start 起的剩余数据，计算 record_end。随后整数和字符串均限制在 record_end 之前；按类型读取负载并要求最终 pos 恰好等于 record_end，再移动记录进结果列表。整数按小端顺序用 uint64_t 拼接，先扩宽再移位；空输入返回空列表，截断、类型未知或长度矛盾抛 runtime_error。字符串按字节长度构造，保留二进制内容。用户需在 record.cpp 显式加入 utility，供 std::move 使用。未来 WAL::recover 调用接入按原排期处理，当前不宣称解码测试已通过。

### 阶段⑤：整体验收

- [ ] 构建 test_wal，在隔离目录运行 `--gtest_filter=RecordTest.*`，保证测试不依赖未实现的 WAL 运行机制。
- [ ] 使用已知字节验证与 encode/decode 往返验证共同检查，避免两个函数犯相同错误却互相抵消。
- [ ] 回归受工厂函数影响的事务构造、put/remove、commit/abort 用例；按实际变化决定是否需要更广回归。
- [ ] 检查本轮注释、函数签名与调用衔接，记录构建／测试结果及限制；未运行的后续用例保留源码。
- [ ] 将本轮完成情况写入 STATE，再收尾 PLAN。发现的问题按 AUDIT 流程分类，不把后续 Lab 的正常未实现功能计为缺陷。
