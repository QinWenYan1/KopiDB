# KopiDB 当前计划

最后更新：2026-11-18。

## 统一完整命名，并交付可独立接入的 C++ SDK

目标：对外使用 `kopidb::LSM`、Python `import kopidb`，构建/库/包名称与项目一致；交付 macOS arm64 预编译 C++ SDK。用户下载解压，在自己的 Xmake 或 CMake 项目中添加一次依赖，再包含 `<lsm/engine.h>` 使用数据库，无需重新编译引擎。

用户已批准按剩余六步实施，每完成一步先汇报，由用户检查后再继续。本轮执行第①步 C++ 命名空间，Python包及后续接入/打包尚不改动。

已确认包结构：`include/kopidb/`、`lib/libkopidb.so`，以及同放包根目录的 `KopiDBConfig.cmake`、`xmake.lua`、README 和许可说明。接入配置根据自身位置定位文件；引擎继续使用 Xmake 构建。

**进度：17%（1/6 步验收，四舍五入）。** 按用户最新要求，仅统计剩余六步；上一轮构建命名作为已完成前置成果保留。每步通过后等待用户检查，不自动执行下一步。百分比按验收步骤数计算，不代表工时。

| 阶段 | 具体工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① C++ 命名空间 | 65个C++文件的 namespace、using、限定引用，双语README示例 | 全部声明与定义一致，目标重建与定向回归通过，外部程序使用 kopidb::LSM 成功 | 已通过：全目标构建、279项回归及6次外部运行；等待用户检查 |
| ② Python 包与扩展名称 | 两层包目录、扩展源码/输出/初始化入口、安装及导入引用 | 新包和扩展名称一致，语法/构建/受控导入及CRUD验证通过；不把已有wheel问题算成改名缺陷 | 待开始 |
| ③ SDK 接入配置 | packaging/cpp/KopiDBConfig.cmake、packaging/cpp/xmake.lua | CMake/Xmake用户声明依赖即可获得头文件、库、C++20及运行路径；不重建数据库 | 待开始 |
| ④ 打包脚本 | scripts/package_cpp.sh | 生成带版本、平台、匹配头文件/库、接入配置和许可说明的压缩包；不修改原工作区构建模式 | 待开始 |
| ⑤ 解压包验收 | 独立临时目录中的 CMake/Xmake 消费项目 | 异目录解压后可编译并直接运行；无仓库路径或 DYLD 补丁依赖；CRUD、事务、遍历、普通/WiscKey重开通过 | 待开始 |
| ⑥ 双语说明与交付 | README.md、README.zh-CN.md、包内说明 | 下载、添加依赖、include步骤与实际产物一致；发布附件准备好，GitHub发布由用户执行 | 待开始 |

## 前置工作：构建名称已完成

- project：`kopidb`；静态核心目标：`kopidb`；共享目标：`kopidb_shared`；Python构建目标：`kopidb_pybind`。
- 共享库：`libkopidb.so`；安装头文件：`include/kopidb/`。
- 内部静态核心仍依赖其他模块，不将它当作已可独立分发的完整静态SDK。

## ① 统一 C++ 命名空间（当前步骤）

必须成组修改头文件、实现、测试、示例与Python绑定；不能仅改公开入口头文件。

| 文件组 | 修改位置 | 数量 |
| --- | --- | --- |
| include/ 下命中清单的头文件 | `namespace tiny_lsm`及闭合注释 → `namespace kopidb` | 26 |
| src/ 下命中清单的实现 | 同步命名空间定义；同一文件可能有多个namespace块 | 24 |
| test/test_*.cpp | `using namespace ::tiny_lsm`及显式限定引用 → `kopidb` | 12 |
| example/main.cpp、example/debug.cpp | `using namespace ::tiny_lsm` → `::kopidb` | 2 |
| sdk/lsm_pybind.cpp | 全部`tiny_lsm::类型` → `kopidb::类型`；此步尚不更名扩展模块 | 1 |
| README.md、README.zh-CN.md | C++示例的`using namespace tiny_lsm` → `kopidb` | 2份文档 |

示例定位：include/lsm/engine.h:19、src/lsm/engine.cpp:26、test/test_lsm.cpp:31、sdk/lsm_pybind.cpp:11、README.md:177、README.zh-CN.md:180。以下清单为计划时行号，执行前核对最新文件。

验收：差异审查确认仅命名变化；全量重新构建当前目标（包括测试和绑定），在隔离目录进行相关存储/事务定向回归及已安装库的单头文件接入验证。命名空间改变C++链接符号，库及消费程序一起重建，不混用旧二进制。

### C++ 逐文件清单

| 文件 | 当前命中行 |
| --- | --- |
| [example/debug.cpp](../example/debug.cpp) | 5 |
| [example/main.cpp](../example/main.cpp) | 6 |
| [include/block/block.h](../include/block/block.h) | 29, 110 |
| [include/block/block_cache.h](../include/block/block_cache.h) | 13, 78 |
| [include/block/block_iterator.h](../include/block/block_iterator.h) | 11, 63 |
| [include/block/blockmeta.h](../include/block/blockmeta.h) | 32, 48 |
| [include/config/config.h](../include/config/config.h) | 9, 71 |
| [include/consts.h](../include/consts.h) | 5, 24 |
| [include/iterator/iterator.h](../include/iterator/iterator.h) | 11, 125 |
| [include/logger/logger.h](../include/logger/logger.h) | 5, 10 |
| [include/lsm/compact.h](../include/lsm/compact.h) | 3 |
| [include/lsm/engine.h](../include/lsm/engine.h) | 19, 143 |
| [include/lsm/level_iterator.h](../include/lsm/level_iterator.h) | 8, 42 |
| [include/lsm/transaction.h](../include/lsm/transaction.h) | 16, 190 |
| [include/lsm/two_merge_iterator.h](../include/lsm/two_merge_iterator.h) | 6, 41 |
| [include/memtable/memtable.h](../include/memtable/memtable.h) | 14, 121 |
| [include/skiplist/skiplist.h](../include/skiplist/skiplist.h) | 16, 171 |
| [include/sst/concact_iterator.h](../include/sst/concact_iterator.h) | 10, 47 |
| [include/sst/sst.h](../include/sst/sst.h) | 18, 186 |
| [include/sst/sst_iterator.h](../include/sst/sst_iterator.h) | 12, 96 |
| [include/utils/bloom_filter.h](../include/utils/bloom_filter.h) | 11, 57 |
| [include/utils/cursor.h](../include/utils/cursor.h) | 7, 35 |
| [include/utils/files.h](../include/utils/files.h) | 11, 73 |
| [include/utils/mmap_file.h](../include/utils/mmap_file.h) | 12, 59 |
| [include/utils/std_file.h](../include/utils/std_file.h) | 9, 50 |
| [include/vlog/vlog.h](../include/vlog/vlog.h) | 9, 50 |
| [include/wal/record.h](../include/wal/record.h) | 9, 52 |
| [include/wal/wal.h](../include/wal/wal.h) | 16, 61 |
| [sdk/lsm_pybind.cpp](../sdk/lsm_pybind.cpp) | 11, 12, 13, 23, 24, 25, 37, 42, 44, 45, 46, 47, 51, 52, 53, 54, 55, 67, 70, 72, 74, 76, 78, 81, 83, 85, 88, 89, 90 |
| [src/block/block.cpp](../src/block/block.cpp) | 12, 628 |
| [src/block/block_cache.cpp](../src/block/block_cache.cpp) | 10, 141 |
| [src/block/block_iterator.cpp](../src/block/block_iterator.cpp) | 11, 199 |
| [src/block/blockmeta.cpp](../src/block/blockmeta.cpp) | 8, 136 |
| [src/config/config.cpp](../src/config/config.cpp) | 6, 219 |
| [src/iterator/iterator.cpp](../src/iterator/iterator.cpp) | 7, 230 |
| [src/logger/logger.cpp](../src/logger/logger.cpp) | 4, 14, 28 |
| [src/lsm/engine.cpp](../src/lsm/engine.cpp) | 26, 1495 |
| [src/lsm/level_iterator.cpp](../src/lsm/level_iterator.cpp) | 13, 299 |
| [src/lsm/transation.cpp](../src/lsm/transation.cpp) | 20, 873, 877, 1412 |
| [src/lsm/two_merge_iterator.cpp](../src/lsm/two_merge_iterator.cpp) | 5, 234 |
| [src/memtable/memtable.cpp](../src/memtable/memtable.cpp) | 17, 758 |
| [src/skiplist/skipList.cpp](../src/skiplist/skipList.cpp) | 12, 413 |
| [src/sst/concact_iterator.cpp](../src/sst/concact_iterator.cpp) | 7, 150 |
| [src/sst/sst.cpp](../src/sst/sst.cpp) | 18, 627 |
| [src/sst/sst_iterator.cpp](../src/sst/sst_iterator.cpp) | 12, 448 |
| [src/utils/bloom_filter.cpp](../src/utils/bloom_filter.cpp) | 10, 253 |
| [src/utils/cursor.cpp](../src/utils/cursor.cpp) | 5, 80 |
| [src/utils/files.cpp](../src/utils/files.cpp) | 6, 211 |
| [src/utils/mmap_file.cpp](../src/utils/mmap_file.cpp) | 8, 173 |
| [src/utils/std_file.cpp](../src/utils/std_file.cpp) | 14, 123 |
| [src/vlog/vlog.cpp](../src/vlog/vlog.cpp) | 14, 196 |
| [src/wal/record.cpp](../src/wal/record.cpp) | 10, 333 |
| [src/wal/wal.cpp](../src/wal/wal.cpp) | 18, 541 |
| [test/test_block.cpp](../test/test_block.cpp) | 14 |
| [test/test_block_cache.cpp](../test/test_block_cache.cpp) | 8 |
| [test/test_blockmeta.cpp](../test/test_blockmeta.cpp) | 5, 7, 40 |
| [test/test_compact.cpp](../test/test_compact.cpp) | 14 |
| [test/test_config.cpp](../test/test_config.cpp) | 11 |
| [test/test_lsm.cpp](../test/test_lsm.cpp) | 31 |
| [test/test_memtable.cpp](../test/test_memtable.cpp) | 19 |
| [test/test_skiplist.cpp](../test/test_skiplist.cpp) | 22 |
| [test/test_sst.cpp](../test/test_sst.cpp) | 12 |
| [test/test_utils.cpp](../test/test_utils.cpp) | 16 |
| [test/test_wal.cpp](../test/test_wal.cpp) | 28 |
| [test/test_wisckey.cpp](../test/test_wisckey.cpp) | 42 |

## ② 统一 Python 包与内部扩展

对外包名为 `kopidb`，内部扩展统一为 `kopidb_pybind`，与现有构建目标同名。Python用户使用 `import kopidb`，不要求直接导入内部扩展。

| 文件/位置（当前路径） | 修改 |
| --- | --- |
| sdk/tinylsm/ | 外层构建目录改为sdk/kopidb/；内层tinylsm/包目录改为kopidb/，保留core及类型提示文件布局 |
| sdk/lsm_pybind.cpp:59 | 文件更名sdk/kopidb_pybind.cpp；`PYBIND11_MODULE(lsm_pybind, m)` → `PYBIND11_MODULE(kopidb_pybind, m)` |
| xmake.lua:295、306 | 两个平台分支的add_files同步新绑定源码路径 |
| xmake.lua:300、311 | 扩展文件改为kopidb_pybind.pyd、kopidb_pybind.so；构建目标已正确，无需再改 |
| sdk/tinylsm/setup.py:14、22、37、42 | 包内库目录、扩展文件清单、发行名、package_data键同步kopidb/kopidb_pybind |
| sdk/tinylsm/tinylsm/core/__init__.py:25 | `from kopidb_pybind import LSM, IsolationLevel` |
| sdk/tinylsm/tinylsm/__init__.py:1 | 包名注释更新；`.core`相对导出不必重写 |
| sdk/tinylsm/tinylsm/core/__init__.pyi | 随包移动，保持现有类型声明；不混入已知API类型修正 |
| sdk/build_python_sdk.sh:4、6、7 | 复制目标、进入目录和egg-info名称同步；已有kopidb_pybind构建目标保持 |
| sdk/test_sdk.py:1、3、10 | import及公开调用前缀改为kopidb |
| .gitignore:5–8 | SDK构建、dist、库产物及egg-info忽略路径同步 |

以上文件必须同一阶段完成。扩展文件名、PYBIND11_MODULE和导入名称一致，才能生成/找到正确初始化入口。只迁移有效源码与包文件，不把旧build/dist二进制搬入新包作为验收产物。

验收：Python AST、脚本语法、包文件清单检查通过；新扩展重新构建，确认初始化符号及新包导入、CRUD。使用同一受控环境区分已有A12加载/打包限制与改名新引入的问题；不得仅凭语法检查宣布新包可用，也不要求在纯命名阶段完成整个wheel发布。

## ③–⑥ 随后继续原 SDK 交付步骤

### ③ 接入配置

- 新增packaging/cpp/KopiDBConfig.cmake：注册`KopiDB::KopiDB`，从配置文件位置确定包根目录，传递include/kopidb、libkopidb.so及C++20要求。
- 新增packaging/cpp/xmake.lua：供使用者导入已经编译好的库，声明头文件、库和运行路径，不添加数据库cpp源码。
- 两文件打包时同放根目录。分别用独立CMake和Xmake工程验证。

### ④ 打包

- 新增scripts/package_cpp.sh：隔离Release构建和安装，整理同一版本头文件/库、两份接入配置、说明和所需许可证，再压缩。
- 产物名草案：kopidb-<版本>-macos-arm64.tar.gz。记录实际工具链/系统要求；版本由项目配置读取，不假造发布版本。

### ⑤ 解压验收

- 真正从压缩包解压到新目录，以独立用户项目消费。验证新`kopidb::LSM`入口、CRUD/事务/遍历、普通与WiscKey的跨进程重开。
- 用户程序自行编译；KopiDB只链接现成库。检查动态库确实来自解压包，不能借用仓库build或开发机缓存。

### ⑥ 文档及交付

- 同步两份README的命名、接入配置及下载步骤，保留用户已有知识部分、目录、图标及完整示例；源码构建方法保留为可选说明。第①步即时同步C++命名空间示例，现有构建命令已对应kopidb_shared；只有接入配置及真实压缩包验收后，才写入新的下载接入主流程，不将草案命令描述为已可用。
- 完成包内说明与发布附件准备；不在尚未发布时写假下载链接。GitHub发布由用户执行。

## 范围与既有记录

- `LSM`类名、src/lsm和include/lsm目录、test_lsm及配置中的LSM算法用语保留；公开入口仍是`<lsm/engine.h>`。
- SST/WAL/事务状态编码、业务算法不变，不重写事务和迭代器。历史验收记录与tiny-lsm教程链接保留真实名称。
- Python完整wheel发布与A12已有问题、其他首发排期按STATE处理；纯命名不扩大成PythonSDK全面修复。
- 本轮平台仍限macOS arm64；不自动增加其他平台发布矩阵。

## 前置构建命名验收（2026-11-18）

- 在独立临时源码副本配置 Release；`kopidb_shared`、`example`（包含静态核心 `kopidb`）、`kopidb_pybind` 构建通过。没有切换原仓库的构建模式。
- 安装生成 `include/kopidb/` 与 `lib/libkopidb.so`；库的 install name 和独立 consumer 的依赖均为 `@rpath/libkopidb.so`，没有引用旧库。
- 仅使用新安装头文件/库编译 consumer，清除 DYLD 及 LD_LIBRARY_PATH 后，在阈值0、64下分别执行 write/read 独立进程，4/4通过；覆盖CRUD、批量、提交、回滚、遍历、谓词范围和重开。
- 构建目标引用检查通过；SDK脚本及Python语法检查通过，Python wheel完整验收未开展。Windows未实跑。
- 业务源码/头文件与构建快照一致；本次不重跑全库单元测试。初次验证脚本使用了错误大小写的配置字段，在执行前被断言拒绝；改用实际字段后完成4次运行，不属于数据库失败。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-naming-rh8v_v6a`，含安装树、consumer、配置、运行日志、依赖检查与summary.json。

## 第①步完成：C++ 命名空间（2026-11-18）

- 用户明确授权按六步逐项执行，每完成一步汇报并等待检查。本步已完成，不提前修改Python包/扩展名。
- 65个C++文件及两版README共149处严格词边界替换：`tiny_lsm` → `kopidb`。独立审查与字节对比确认无其他源码/示例逻辑变化；同一文件多个命名空间、绑定类型、测试显式引用均已同步。
- 独立源码副本Release全目标构建成功（包括测试、共享库及Python扩展），安装成功；原仓库配置及用户数据未动。
- 定向回归279/279通过：Config3、SkipList31、MemTable51、Block37、BlockMeta6、Utils15、SST30、BlockCache7、LSM37、WAL46、WiscKey16。没有运行百万条用例或性能benchmark，不宣称全库测试重跑。
- 新安装库的单头文件consumer及两版README示例共3次编译/链接通过；普通/WiscKey各write/read两进程，加两版README运行，共6/6通过。运行清除DYLD及LD_LIBRARY_PATH，使用隔离数据目录。
- README中构建目标/安装目录/库名已在前置工作更新，此步同步C++示例并实际验证；下载接入主流程等真实压缩包通过后由第⑥步更新。
- 检查结束时源码与构建快照一致。证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-namespace-0b6_a3_j`，含before、changes.json、构建日志、测试过滤器/JSON/日志、安装树、consumer及summary.json。
- 当前停在第①步，等待用户检查后进入第②步Python命名。
