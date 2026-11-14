# KopiDB 当前计划

最后更新：2026-11-14。

## 已完成：移除 Redis 组件

目标：专注提供库接口的嵌入式 LSM 数据库，移除 Redis 包装、协议服务、专属配置和测试。用户明确授权助手独立完成本轮业务代码、测试与文档修改，用户参与最终验收；该授权仅限本轮，后续 Lab 继续沿用原有逐函数教学方式。

进度：**100%（5/5 阶段验收）**。等待用户最终审阅。

| 阶段 | 结果与验收 | 状态 |
| --- | --- | --- |
| ① 删除专属组件 | 移除 redis_wrapper、server、test_redis 和 RESP 笔记；LSM/MVCC/WAL/SST/SDK 源码保留，有效源码无悬空引用 | 已验收 |
| ② 清理构建依赖 | 删除三个专属目标与 Asio 依赖，清理共享库／示例依赖；全部现有目标重新构建成功，共享库无 Redis 符号 | 已验收 |
| ③ 清理配置 | 清除成员、getter、默认／加载／保存段、宏与 TOML 节；新增非默认配置加载和保存测试均通过，配置套件 3/3 | 已验收 |
| ④ 整理文档与产物 | README 保留原资料并明确嵌入式定位；学习索引和路线同步；旧专属产物及过时 Release 产物清理，63 项编译数据库无移除组件 | 已验收 |
| ⑤ 回归验收 | 本轮实跑 497/497；C++ example/debug 与 Python 扩展 CRUD、事务、重启冒烟通过；源码哈希稳定，独立复核通过 | 已验收 |

## 验收证据

- 构建：`xmake build -r -a -j 6`，全部现有目标成功，保持 Debug 模式。WiscKey 目标仅构建，不把其尚未实现的行为计为通过。
- 实跑：Config 3、SkipList 31、MemTable 51、Block 37、BlockMeta 6、Utils 15、SST 29、BlockCache 7、LSM 169、Compact 5、WAL 144，合计 **497 项，零失败、禁用或跳过**。
- 明确排除并复用先前结果：`LSMTest.BigPersistence`、`LSMTest.BigPersistence2`。验证核心引擎实现未变、两个函数体未变、原始测试文件只差两处后补注释、非 Redis 配置值保持一致；未重复运行约 30 分钟的大规模测试。原有 497 项加本轮新增配置 2 项，当前适用集合为 **499 项**，不声称这 499 项全由本轮重跑。
- C++ 示例直接运行通过。Python 扩展在指定 `PYTHONPATH` 和 `DYLD_LIBRARY_PATH=build/lib` 后通过 CRUD、提交／取消、flush 与重开验证；只设置 PYTHONPATH 时遇到既有 macOS 动态库定位问题，详细边界记录在 STATE/AUDIT，本轮不宣称 Python 安装包已可直接发布。
- 原测试断言与数据规模未改，核心 LSM／事务／WAL／迭代器业务逻辑未修改。测试运行期间代码哈希一致。没有 Git 写操作，当前无助手启动的后台测试。

原始证据目录：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-remove-redis-7tjk4sfe`。包含构建命令／日志、各套件命令与 GTest JSON、冒烟结果、源码哈希、生成物清理清单、既有测试复用依据、`closure.json` 汇总。首次构建受沙箱依赖文件锁限制，使用本机依赖缓存权限后成功；首次 Python 导入失败日志与指定库路径后的成功日志均保留。

## 下一步：Lab 7 WiscKey

按用户确认的路线跳过 Lab 6。下一轮依次推进 **7.1 VLog → 7.2 SST 支持 → 7.3 Engine 集成**，开始前阅读对应 Lab，列明具体函数和验收标准，再逐函数实现。当前未提前实现 Lab 7。

Lab 5.5 的既有验收、性能对照和全局限制仍保留在 STATE/AUDIT；后续项目完善和打包单独规划，不作为本轮删除的附带重构。
