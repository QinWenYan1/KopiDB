# KopiDB 当前计划

最后更新：2026-11-18。

## 公开入口收尾：一条 KopiDB include 使用现有用户接口

目标：用户仅包含 `<lsm/engine.h>` 即可使用LSM公开接口，包括CRUD、批量、事务、遍历／谓词范围、flush／clear和日志级别设置。沿用现有入口、命名空间和API；这是用户新确认的接口易用性目标，不把原先需要两条include的用法追溯为数据正确性缺陷。

**进度：100%（2/2阶段验收）。** 按用户本轮行动请求，已补齐入口的一处头文件依赖、同步两版README，并完成安装后的单头文件验证。未改业务函数实现。

| 阶段 | 具体改动 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 补齐入口依赖 | `include/lsm/engine.h` 顶部加入 `#include "level_iterator.h"` | 单独包含engine.h即可编译并链接现有公开接口；没有新增包含环或接口/布局变化 | 已通过：实际入口已补齐，公开接口探针编译/链接通过 |
| ② 单头文件安装验收与双语示例 | 重新安装当前头文件、验证外部consumer；两版README删除显式level_iterator.h包含 | 独立程序只写一条KopiDB include；CRUD/事务/遍历和范围接口可用；两版README同步，既有使用示例保持 | 已通过：安装后4次consumer运行及两版README示例运行通过 |

百分比按正式验收阶段数计算，不代表工时比例。

## 依赖核查与边界

- engine.h目前只有Level_Iterator前置声明，但begin/end按值返回它；使用遍历需要完整定义。
- transaction.h已提供IsolationLevel及完整TranContext；two_merge_iterator.h已提供谓词范围返回类型；无须重复添加这些依赖。
- level_iterator.h仅前置声明LSMEngine，不包含engine.h；新增包含不形成环。
- 不新增门面/命名空间、PImpl或迭代器重构，不改变类布局、函数签名或磁盘格式。安装包仍安装依赖头文件，用户源码只需一个库入口include。
- 标准库头文件仍由使用者按其代码需要包含，例如iostream；“一条include”指KopiDB入口。

## 实施前草案证据

- 先前仅使用现有engine.h调用begin时，编译器实际报告incomplete return type；证据保存在`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-public-header-check-fzmvucs5`。
- 仅在临时头文件副本加入level_iterator.h，使用一条engine.h包含的公开接口探针已编译并链接成功，覆盖LSM所有公开操作及事务方法。探针不执行数据库操作；尚不能算用户当前版本的安装/运行验收。
- 草案证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-single-header-draft-tbnrueyj`，包含副本、接口探针、编译命令和日志。当时原仓库入口头文件及README尚未修改。

## 前一轮完成

C++独立接入前三阶段已于2026-11-18完成；A17修复、Release安装/外部consumer及双语说明结果仍保留于AUDIT/STATE。旧PLAN快照保存在本轮草案证据目录。

## 完成验收（2026-11-18）

- engine.h增加level_iterator.h及中文说明；两版README示例统一为`#include <lsm/engine.h>`，标准库iostream照常单独包含。未新增门面、改名或改迭代器逻辑/类型布局。
- 当前源码在独立临时副本Release构建并安装成功；原仓库构建配置不变。使用安装头文件与库，单入口公开接口探针、完整consumer、两版README示例共4个编译/链接检查通过。
- consumer在普通阈值0与WiscKey阈值64下各执行write/read两个独立进程，验证CRUD、批量、事务、遍历、谓词范围、flush及重开；两版README示例也分别运行成功，共6/6进程验证通过。公开接口探针覆盖其他维护方法的编译/链接，不执行其数据库操作。
- 运行清除DYLD相关变量，源码/头文件/README/xmake/config执行前后哈希稳定；仅验证macOS arm64，未重跑全库单元测试。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-single-header-final-3oqx_kg4`，包含实际源码副本、安装产物、编译/运行命令和日志、summary.json及哈希。
- 本项已完成，等待用户选择后续首发任务。
