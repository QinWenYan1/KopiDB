# KopiDB 当前计划

最后更新：2026-11-14。

## 上一轮已完成：Lab 5.5 崩溃恢复

目录 **5.2.3「崩溃恢复」**已完成约定范围的功能验收，进度 **100%（6/6 阶段）**。旧实施流水已从当前计划清理，完成摘要与边界保存在 [STATE](STATE.md#已完成lab-55-崩溃恢复)，问题及性能证据保存在 [AUDIT](AUDIT.md)。

| 阶段 | 状态 |
| --- | --- |
| ① 收集 WAL 日志 | 已验收 |
| ② 管理器恢复检查入口 | 已验收 |
| ③ 初始化新 WAL | 已验收 |
| ④ 接通启动恢复 | 已验收 |
| ⑤ 公开接口、clear 与关闭衔接 | 已验收 |
| ⑥ 最终回归与记录收尾 | 已验收 |

最终受影响的完整目标：LSM 171/171、Compact 5/5、WAL 144/144，共320项；此前未受影响的存储目标177/177，源码哈希核对后复用结果，当前适用测试合计 **497项全部完成**。定向10项属于重复子集，不另计总数。未缩减数据、禁用用例或削弱断言，当前无助手启动的后台测试。

A11 三步均完成：反向通知代码检查 → 定向验证 → 耗时比较与大规模回归。两个十万条用例由 **189.896s→5.288s**、**191.736s→5.257s**；此前未完成的 BigPersistence、BigPersistence2、TombstoneDefaultConfig 均已通过。该性能数据仅描述实测用例，不推广为所有场景的加速保证。

证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a11-review-cowmepgu`，汇总见 `lab55-closure.json`，性能对比见 `persistence-comparison.json`。全局未选事项和非阻塞说明清理统一留在 STATE/AUDIT，不继续占用当前实施清单。

## 下一步：确认 Lab 6.1 的目标与边界

作者目录已核对：下一站是 **Lab 6 Redis 兼容 → Lab 6.1 简单字符串**，WiscKey 属于 Lab 7。

已阅读 [Lab 6 总览](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab6/lab6-Redis.md) 和 [Lab 6.1 简单字符串](https://github.com/Vanilla-Beauty/tiny-lsm/blob/lab-doc-2/src/lab6/lab6.1-String.md)，并定位作者相关参考入口。当前只完成阅读准备，尚未开始该章节的业务实现；先确认目标与接口约定，再拆实施阶段与验收标准。

文档指定的七个函数位于 `src/redis_wrapper/redis_wrapper.cpp`，顺序为：

1. `RedisWrapper::redis_set`
2. `RedisWrapper::redis_expire`
3. `RedisWrapper::redis_ttl`
4. `RedisWrapper::redis_get`
5. `RedisWrapper::redis_incr`
6. `RedisWrapper::redis_decr`
7. `RedisWrapper::redis_del`

正式实施前核对：RESP 返回与现有转发层的契约、过期信息保存及查询方式、INCR/DECR 等组合操作的同步边界，以及 Redis 空字符串与底层空 key/value 保留约定如何衔接。根据已有代码和 Lab 推导方案，不因参考函数已有函数体就跳过独立实现，也不自动重写其他辅助入口。

沿用既定协作：业务代码由用户手写，助手给带中文注释的实现及精确位置；测试由助手补充。一次处理一个函数，用户说“检查并下一个”后，检查通过再继续。
