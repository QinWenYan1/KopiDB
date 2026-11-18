# KopiDB 当前计划

最后更新：2026-11-18。

## C++ SDK：下载后接入用户自己的项目

目标：交付 macOS arm64 的预编译 C++ SDK。用户下载解压，在自己的 Xmake 或 CMake 项目中添加一次依赖，再通过 `<lsm/engine.h>` 使用数据库，无需重新编译数据库或使用我们的示例工程。

已确认包结构：头文件与库分开；`KopiDBConfig.cmake` 与包内的 `xmake.lua` 同放包根目录。配置根据自身位置定位文件，不写死开发机器路径。我们继续使用 Xmake 构建引擎。

用户新增要求：统一 xmake 构建与交付名称为 kopidb。当前先处理这一项；C++ 命名空间及 Python 导入名属于接口变更，暂保留现有名称。`lsm` 在源码路径、类名、测试名中表示数据结构，不作全文替换。

**进度：20%（1/5 阶段验收）。** 之前单头文件入口已完成，本表记录新一轮 SDK 交付。百分比按已验收阶段数计算，不代表工时比例。

| 阶段 | 具体工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 统一构建与交付名称 | xmake 项目名、核心/共享库/Python 构建目标、库 basename、头文件安装目录；同步 SDK 库引用与两版 README | 新名称构建/安装成功；独立程序使用新路径编译、链接、运行；内部目标依赖完整 | 已通过：Release 构建/安装、独立 consumer 4/4 运行通过 |
| ② SDK 接入配置 | 新增 `packaging/cpp/KopiDBConfig.cmake` 和 `packaging/cpp/xmake.lua`，打包时放根目录 | CMake 与 Xmake 用户只指定 SDK 位置并声明依赖，就获得头文件、库、C++20 和必要运行路径；不重新构建引擎 | 待开始 |
| ③ 可重复执行的打包脚本 | 新增 `scripts/package_cpp.sh`，整理 Release 库、匹配头文件、接入配置、版本/平台说明与许可文件 | 产出带版本和平台名的压缩包；不依赖旧构建产物，不改变用户原工作区构建模式 | 待开始 |
| ④ 解压包验收 | 将真实压缩包解压到新的目录，以用户自己的 C++ 项目接入 | CMake/Xmake 构建及直接运行通过；无仓库/包缓存头文件依赖或 DYLD 环境变量补丁；CRUD、事务、遍历、普通/WiscKey 重开通过 | 待开始 |
| ⑤ 双语文档与交付说明 | 更新 README.md/README.zh-CN.md 的下载与依赖接入步骤，保留源码构建说明 | 文档与实际压缩包一致，下载附件和支持平台清楚；发布附件准备好，GitHub 发布由用户执行 | 待开始 |

## 阶段①命名映射

| 原名称 | 新名称 |
| --- | --- |
| Xmake project `tiny-lsm` | `kopidb` |
| 内部静态核心目标 `lsm` | `kopidb` |
| 共享库目标 `lsm_shared` | `kopidb_shared` |
| Python 构建目标 `lsm_pybind` | `kopidb_pybind` |
| 对外共享库 `liblsm_shared.so` | `libkopidb.so` |
| 安装头文件 `include/tiny-lsm/` | `include/kopidb/` |

- 静态核心目标沿用模块依赖关系，不把它误称为已可独立分发的完整静态 SDK。
- Python 扩展文件及导入名仍为 `lsm_pybind`，包名仍为 `tinylsm`；构建目标名不必等于扩展入口符号名。
- C++ 命名空间 `tiny_lsm`、`LSM` 类型及 `src/lsm`、`include/lsm` 路径保持。教程链接保留真实 tiny-lsm 地址。
- macOS 下共享库沿用当前 `.so` 后缀，SDK loader 与实际新文件名同步。Windows 安装导入库改用 Xmake 返回的产物路径；此轮不宣称 Windows 已运行验收。

## 前一轮完成

单头文件入口及 C++ 独立接入验收保留于 STATE/AUDIT。前一轮 PLAN 快照：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-naming-rh8v_v6a/previous-PLAN.md`。

## 阶段①验收（2026-11-18）

- 在独立临时源码副本配置 Release；`kopidb_shared`、`example`（包含静态核心 `kopidb`）、`kopidb_pybind` 构建通过。没有切换原仓库的构建模式。
- 安装生成 `include/kopidb/` 与 `lib/libkopidb.so`；库的 install name 和独立 consumer 的依赖均为 `@rpath/libkopidb.so`，没有引用旧库。
- 仅使用新安装头文件/库编译 consumer，清除 DYLD 及 LD_LIBRARY_PATH 后，在阈值0、64下分别执行 write/read 独立进程，4/4通过；覆盖CRUD、批量、提交、回滚、遍历、谓词范围和重开。
- 构建目标引用检查通过；SDK脚本及Python语法检查通过，Python wheel完整验收未开展。Windows未实跑。
- 业务源码/头文件与构建快照一致；本次不重跑全库单元测试。初次验证脚本使用了错误大小写的配置字段，在执行前被断言拒绝；改用实际字段后完成4次运行，不属于数据库失败。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-naming-rh8v_v6a`，含安装树、consumer、配置、运行日志、依赖检查与summary.json。
