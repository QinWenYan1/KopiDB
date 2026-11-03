# KopiDB 当前任务：Record 编解码

最后更新：2026-11-03。

章节：目录 **5.2.1「Record 编解码」**，文章标题 **Lab 5.3 WAL 日志编解码**。

**进度：0%（0/5 阶段验收通过）**。按验收阶段数计算，不代表工时比例。createRecord、commitRecord 已通过代码检查，独立 RecordTest 共 4/4 通过；阶段①尚有 rollbackRecord 待实现。

当前函数：`Record::rollbackRecord(uint64_t tranc_id)`，位于 [src/wal/record.cpp](../src/wal/record.cpp) 当前第 55 行。

## 本轮目标

独立实现 Record 的五个记录创建函数及 encode/decode，使事务操作能够准确转换为字节数组，并从单条或连续多条记录的字节数组中还原事务 ID、操作类型和对应的 key/value。

本轮产物是内存记录与字节数组之间的转换。后续章节和全局延期事项统一见 [STATE.md](STATE.md#全局排期与延期事项)，不混入本轮 TODO。

业务源码及头文件由用户手写；助手逐函数给出带中文注释的完整实现、检查修改、补测试并维护进度。同一阶段内包含多个函数时仍分轮处理。

## 已读资料与当前代码

- [Lab 5.3，lab-doc-2 分支](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab5/lab5.3-Record.md)：正文 2.1 要求五个静态创建函数，2.2 要求 encode/decode；本节仅处理字节数组，文件 I/O 在后续章节。
- [作者 Record 参考，master 分支](https://github.com/Vanilla-Beauty/tiny-lsm/blob/master/src/wal/record.cpp)：2026-11-02 已读取，仅作为方案对照，未运行作者工程。
- 本地 [record.cpp](../src/wal/record.cpp) 的 createRecord、commitRecord 已由用户实现并检查通过，其余三个创建函数仍为桩，encode/decode 仍有参考函数体。七个函数均按 Lab 要求重新推导、逐个实现，不能把参考函数体视为本轮已完成。
- [record.h](../include/wal/record.h) 保留现有公开接口和拼写。getter、print、operator==/!= 仅联动复核，不列入必须重写的函数。
- 已核对调用关系：TranContext 的创建、put、remove、commit 会构造 Record 并保存到 operations；当前仅保存在内存。rollbackRecord 是本节要求的记录类型，其后续调用接入按 STATE 排期处理。
- 本轮测试加入 [test/test_wal.cpp](../test/test_wal.cpp) 的独立 `RecordTest` 测试组，目前已有 createRecord、commitRecord 各 2 项测试，只筛选该组运行，不构造 WAL 对象。新增测试使用 getter 逐字段核对，避免仅依赖现有 operator== 得出结论。

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
| ① 事务状态记录 | createRecord → commitRecord → rollbackRecord | 三种记录的类型、完整 64 位 ID 正确；key/value 为空；长度计算代码检查正确，编码完成后验证为 11B | 进行中：createRecord、commitRecord 已检查，当前 rollbackRecord |
| ② 数据操作记录 | putRecord → deleteRecord | 保留对应 key/value；DELETE 不携带 value；合法边界可创建、字段或总长超限明确报错；零长度与二进制内容不被改写 | 待开始 |
| ③ 编码 | encode | 五种记录字节布局正确，长度与实际输出一致；已知字节样例、字段边界和完整 64 位 ID 通过 | 待开始 |
| ④ 解码 | decode | 单条与多条混合流准确还原，顺序不变；空输入正确；截断、错误长度和未知类型明确拒绝 | 待开始 |
| ⑤ 整体验收 | Record 测试及受影响事务回归、记录收尾 | 各类记录往返及已知字节对照通过；现有事务记录创建路径不回归；说明实际运行与尚未接通的范围 | 待开始 |

### 阶段①：事务状态记录

- [x] `Record::createRecord()`：当前 record.cpp:16。类型、完整 ID、空 key/value 的 2 项测试通过；11B 长度计算代码检查正确，实际编码长度留待阶段③。
- [x] `Record::commitRecord()`：当前 record.cpp:36。类型、完整 ID、空 key/value 的 2 项测试通过；11B 长度计算代码检查正确，实际编码长度留待阶段③。
- [ ] `Record::rollbackRecord()`：当前 record.cpp:55。
- [ ] 助手补充类型、ID 和无负载字段测试，按函数逐项验收。私有 record_len 不新增 getter，只做计算检查，阶段③补充实际编码长度验证。

2026-11-02 检查：`xmake build test_wal` 成功；在隔离临时目录运行 `--gtest_filter=RecordTest.*`，**2/2 通过**。覆盖普通事务开始记录，以及 0、超过 32 位、最高位为 1、uint64_t 最大值的 ID 原样保留。未调用 encode/decode，未运行后续 WAL／恢复用例或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-create-review-9xliflzd`，包含构建日志、测试命令、GoogleTest JSON、测试日志及源码哈希。

2026-11-03 检查：commitRecord 实现正确，新增 2 项测试验证 OP_COMMIT、空 key/value 及完整 64 位 ID。`xmake build test_wal` 成功；隔离运行 `--gtest_filter=RecordTest.*`，包含前两项 CREATE 测试共 **4/4 通过**。未调用 encode/decode，未运行 WAL／恢复或其他测试组。构建和测试前后 Record 源码、头文件、测试文件哈希一致。

证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-record-commit-review-curkb6nc`。

### 阶段②：数据操作记录

- [ ] `Record::putRecord()`：当前 record.cpp:59。
- [ ] `Record::deleteRecord()`：当前 record.cpp:64。
- [ ] 助手覆盖普通字符串、零长度、内嵌空字节、恰好达到总长上限及超限；通过 getter 检查数据完整性。

### 阶段③：编码

- [ ] `Record::encode()`：当前 record.cpp:69。
- [ ] 五类记录用已知字节序列验证字段顺序、操作类型编码和小端整数。
- [ ] 验证实际输出长度、字符串原始字节、uint64_t 较高位，以及前两阶段创建的边界记录。

### 阶段④：解码

- [ ] `Record::decode()`：当前 record.cpp:111。
- [ ] 单条、连续多条、混合操作、不同事务 ID 和重复 key 保持原顺序，不在编解码中去重或应用事务操作。
- [ ] 每条记录独立检查头部、记录长度和负载范围；覆盖首条及后续记录截断、零／过小长度、字段长度矛盾、未知操作类型。
- [ ] 只验证输入字节流的完整解码，不用本节测试替代崩溃恢复验收。

### 阶段⑤：整体验收

- [ ] 构建 test_wal，在隔离目录运行 `--gtest_filter=RecordTest.*`，保证测试不依赖未实现的 WAL 运行机制。
- [ ] 使用已知字节验证与 encode/decode 往返验证共同检查，避免两个函数犯相同错误却互相抵消。
- [ ] 回归受工厂函数影响的事务构造、put/remove、commit/abort 用例；按实际变化决定是否需要更广回归。
- [ ] 检查本轮注释、函数签名与调用衔接，记录构建／测试结果及限制；未运行的后续用例保留源码。
- [ ] 将本轮完成情况写入 STATE，再收尾 PLAN。发现的问题按 AUDIT 流程分类，不把后续 Lab 的正常未实现功能计为缺陷。
