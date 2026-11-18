# KopiDB 当前计划

最后更新：2026-11-19。

## 统一完整命名，并交付可独立接入的 C++ SDK

目标：对外使用 `kopidb::LSM`、Python `import kopidb`，构建/库/包名称与项目一致；交付 macOS arm64 预编译 C++ SDK。用户下载解压，在自己的 Xmake 或 CMake 项目中添加一次依赖，再包含 `<lsm/engine.h>` 使用数据库，无需重新编译引擎。

用户已批准按剩余六步实施，每完成一步先汇报，由用户检查后再继续。前三步已由用户检查并授权继续；本轮执行第④步打包脚本，第⑤步真实解压接入及第⑥步文档尚不实施。

已确认包结构：`include/kopidb/`、`lib/libkopidb.so`，以及同放包根目录的 `KopiDBConfig.cmake`、`xmake.lua`、README 和许可说明。接入配置根据自身位置定位文件；引擎继续使用 Xmake 构建。

**进度：67%（4/6 步验收，四舍五入）。** 按用户最新要求，仅统计剩余六步；上一轮构建命名作为已完成前置成果保留。每步通过后等待用户检查，不自动执行下一步。百分比按验收步骤数计算，不代表工时。

| 阶段 | 具体工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① C++ 命名空间 | 65个C++文件的 namespace、using、限定引用，双语README示例 | 全部声明与定义一致，目标重建与定向回归通过，外部程序使用 kopidb::LSM 成功 | 已通过：全目标构建、279项回归及6次外部运行；用户已确认继续 |
| ② Python 包与扩展名称 | 两层包目录、扩展源码/输出/初始化入口、安装及导入引用 | 新包和扩展名称一致，语法/构建/受控导入及CRUD验证通过；不把已有wheel问题算成改名缺陷 | 已通过：扩展构建、名称/语法检查及5/5受控运行；用户已确认继续 |
| ③ SDK 接入配置 | packaging/cpp/KopiDBConfig.cmake、packaging/cpp/xmake.lua | CMake/Xmake用户声明依赖即可获得头文件、库、C++20及运行路径；不重建数据库 | 已通过：安装复制、两种构建工具接入、路径迁移及16次运行；用户已确认继续 |
| ④ 打包脚本 | scripts/package_cpp.sh | 生成带版本、平台、匹配头文件/库、接入配置和许可说明的压缩包；不修改原工作区构建模式 | 已通过：默认/带空格路径打包、包内容检查及压缩失败保留旧包；等待用户检查 |
| ⑤ 解压包验收 | 独立临时目录中的 CMake/Xmake 消费项目 | 异目录解压后可编译并直接运行；无仓库路径或 DYLD 补丁依赖；CRUD、事务、遍历、普通/WiscKey重开通过 | 待开始 |
| ⑥ 双语说明与交付 | README.md、README.zh-CN.md、包内说明、docs/packaging.md | 下载、添加依赖、include步骤与实际产物一致；发布附件准备好，GitHub发布由用户执行 | 待开始 |

## 前置工作：构建名称已完成

- project：`kopidb`；静态核心目标：`kopidb`；共享目标：`kopidb_shared`；Python构建目标：`kopidb_pybind`。
- 共享库：`libkopidb.so`；安装头文件：`include/kopidb/`。
- 内部静态核心仍依赖其他模块，不将它当作已可独立分发的完整静态SDK。

## ① 统一 C++ 命名空间（已完成）

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
| [sdk/kopidb_pybind.cpp](../sdk/kopidb_pybind.cpp) | 11, 12, 13, 23, 24, 25, 37, 42, 44, 45, 46, 47, 51, 52, 53, 54, 55, 67, 70, 72, 74, 76, 78, 81, 83, 85, 88, 89, 90 |
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

## ② 统一 Python 包与内部扩展（已完成）

对外包名为 `kopidb`，内部扩展统一为 `kopidb_pybind`，与现有构建目标同名。Python用户使用 `import kopidb`，不要求直接导入内部扩展。

| 文件/位置（改名前路径，现已完成迁移） | 修改 |
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

### ③ 接入配置（已完成）

- 新增packaging/cpp/KopiDBConfig.cmake：注册`KopiDB::KopiDB`，从配置文件位置确定包根目录，传递include/kopidb、libkopidb.so及C++20要求。
- 新增packaging/cpp/xmake.lua：供使用者导入已经编译好的库，声明头文件、库和运行路径，不添加数据库cpp源码。
- 根xmake.lua的on_install负责将这两份接入配置从packaging/cpp复制到安装根目录；第④步打包脚本调用构建/安装并压缩结果，不重复维护复制规则。分别用独立CMake和Xmake工程验证，并核对解压位置变化后可重新接入。

### ④ 打包（当前步骤）

- 新增scripts/package_cpp.sh：从任意工作目录定位仓库，复制当前工作树构建输入到临时目录，隔离Release构建/安装，读取实际项目版本、依赖信息及最低系统标签，补许可信息后压缩。输出目录可选，默认dist/；.gitignore新增/dist/。
- 打包临时归档写在最终输出目录内，tar写入/清单检查成功后才替换正式包；失败保留旧包并清理临时工作区。
- 新增packaging/cpp/licenses/spdlog-LICENSE：本机安装缓存不保留该完整许可，已从gabime/spdlog的v1.17.0标签核对原文；toml11及bundled fmt的许可取自本次实际安装头文件。依赖升级时同步核对许可资产。
- 包内README暂采用现有项目说明；第⑥步提供packaging/cpp/README.md后脚本自动优先使用专用说明。本步产物不等于已完成发布验收。
- 产物名草案：kopidb-<版本>-macos-arm64.tar.gz。记录实际工具链/系统要求；版本由项目配置读取，不假造发布版本。

### ⑤ 解压验收

- 真正从压缩包解压到新目录，以独立用户项目消费。验证新`kopidb::LSM`入口、CRUD/事务/遍历、普通与WiscKey的跨进程重开。
- 用户程序自行编译；KopiDB只链接现成库。检查动态库确实来自解压包，不能借用仓库build或开发机缓存。

### ⑥ 文档及交付

- 同步两份README的命名、接入配置及下载步骤，保留用户已有知识部分、目录、图标及完整示例；源码构建方法保留为可选说明。第①步即时同步C++命名空间示例，现有构建命令已对应kopidb_shared；只有接入配置及真实压缩包验收后，才写入新的下载接入主流程，不将草案命令描述为已可用。
- 完成包内说明与发布附件准备；不在尚未发布时写假下载链接。GitHub发布由用户执行。
- 用户新增留痕任务安排在本步：新增docs/packaging.md，解释源码构建、安装、压缩、用户接入流程及各文件职责、关键取舍/验证方法；面向维护者，不重复STATE/PLAN进度。当前先不写，不新增第七步。

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

## 第②步完成：Python 包与内部扩展（2026-11-18）

- 用户确认第①步并授权进入本步。外层构建目录及包目录迁移到`sdk/kopidb/kopidb/`；绑定源码改为`sdk/kopidb_pybind.cpp`。
- Python公开包为`kopidb`，内部扩展的源码入口、输出文件名、打包清单和导入统一为`kopidb_pybind`。同步setup发行名/package_data、SDK脚本、测试导入及.gitignore。共9项文件记录，包含5个文件迁移；类型提示文件仅移动、内容不变。
- 独立复核确认改动仅名称替换及一条过时构建注释更新，SDK和有效构建配置无旧名称残留。生成的旧compile_commands.json未手改；需要使用时由Xmake重新生成。
- 隔离Release副本的`kopidb_pybind`及其共享库依赖构建通过；Python AST、bash -n、包名称/文件清单检查通过；nm确认`PyInit_kopidb_pybind`存在且旧入口不存在。
- macOS arm64 / Python3.13.1：用新源码及新库在临时目录组装包，以python -I并显式指定临时包目录验证，不借用全局旧安装。外部清除DYLD、LD_LIBRARY_PATH及PYTHONPATH。1次独立导入，加普通/WiscKey各写入与重开读取，共5/5进程通过；覆盖CRUD、批量、事务提交/回滚、遍历、大值及12字节值。
- 此为受控组装包验证，没有构建或安装wheel，没有修改系统Python环境；已有PythonSDK发布事项仍按STATE/AUDIT处理。本轮未重跑C++单元测试，src/include未改。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-python-naming-_05t1nfo`，含before、changes.json、构建日志、符号/库依赖、组装包、导入脚本及运行日志、summary.json。
- 当前停在第②步，等待用户检查后进入第③步SDK接入配置。

## 第③步完成：SDK 接入配置（2026-11-18）

- 新增packaging/cpp/KopiDBConfig.cmake：SHARED IMPORTED目标KopiDB::KopiDB、相对配置文件定位、Threads依赖、C++20使用要求；macOS设置与实际库一致的IMPORTED_SONAME，消费项目的构建运行路径由CMake生成。
- 新增packaging/cpp/xmake.lua：phony目标kopidb，公开传递头文件、库目录/库名、C++20和运行路径；不添加引擎源码、不修改用户全局配置，使用target_end结束作用域。
- 根xmake.lua的on_install增加两份配置的复制，安装到SDK根目录。安装源是packaging/cpp，未误复制根构建脚本；打包脚本仍待第④步实现。
- 本机Xmake3.1.0探针发现带空格路径的两个参数处理限制：完整库路径拆分、rpath探测失败。最终用linkdirs+links，以及空格路径下保持独立参数的ldflags/shflags处理；保留空字符串避免多消费目标时单组参数被解包导致后续目标丢失。源码、注释及实际多目标构建已复核。原始工具探针：/private/tmp/kopidb-sdk relocated-j0xfb5va。
- 验证平台macOS arm64，CMake4.4.0、Xmake3.1.0。隔离Release构建/安装成功；安装结果复制到两个带空格的新目录，原安装位置移走后分别构建CMake和Xmake项目（各2个binary、1个shared、1个无依赖目标），全部成功。另一个无空格Xmake项目也通过，覆盖标准rpath分支。
- 直接运行清除DYLD和LD_LIBRARY_PATH：14次数据库consumer运行通过（普通/WiscKey、CRUD、批量、事务、遍历/谓词范围及重开），另2次无依赖C++17程序通过，共16/16。两种工具的binary/shared产物均确认指向迁移后的SDK库目录，且消费构建只编译自己的源文件，没有编译数据库源码。
- Xmake公共languages在3.1.0中固定消费目标为C++20；此步不声称保留依赖目标自行设置的更高C++标准。无依赖目标不受影响，后续文档按当前能力表述。
- 引擎业务源码及头文件未修改，不重跑全库单元测试；本步验证目录迁移，尚未生成压缩包，第⑤步仍需验收真实归档产物。验收后仅将注释中的Xmake版本精确为3.1.0，可执行Lua内容与测试快照相同。
- 证据：/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sdk-config-35gjd_er，含源码副本、安装树、迁移后的三个消费工程、构建日志、运行日志、库依赖/运行路径、summary.json。
- 当前停在第③步，等待用户检查后进入第④步打包脚本。

## 第④步完成：自动打包脚本（2026-11-19）

- 新增scripts/package_cpp.sh（可执行，兼容macOS自带Bash3.2）；.gitignore忽略/dist/；新增已核对的packaging/cpp/licenses/spdlog-LICENSE作为必要分发资产。数据库业务源码未改。
- 脚本从自身定位仓库，接受一个可选输出目录；从任意cwd读取当前工作树构建输入到临时副本，不使用git archive，不借用原build/.xmake。执行Release配置/构建/安装，通过Xmake读取项目版本和实际依赖，再检查架构/动态依赖、记录最低系统标签，先压缩到输出目录的临时文件，成功后才替换正式包。
- 项目版本0.0.1产物：dist/kopidb-0.0.1-macos-arm64.tar.gz，517848字节，SHA-256 cc39ae4f218288ff59820adf6384f2dee05a09ce7e088a785e0a0f91c013b710。归档共52项；包含当前头文件、libkopidb.so、两接入配置、项目许可、依赖许可、README、BUILD-INFO，未混入源码/build/.git或旧Python二进制。
- BUILD-INFO记录实际toml11 v4.4.0、spdlog v1.17.0、bundled fmt12.1.0及编译器/SDK信息；库的最低macOS标签实测18.0，构建主机macOS27.2，Apple SDK15.2。这是产物标签，不代表已在所有该版本以上系统测试；第⑥步说明需据实际产物表述系统要求。
- 默认输出、仓库外cwd、带空格的输出及临时路径均打包成功。对包内所有头文件、两接入配置与当前源文件逐字节比较通过。/bin/bash -n及独立静态审查通过。
- 最终压缩写入中注入退出73，已验证旧包SHA保持、残缺归档清理、临时源码工作区清理。首次故障夹具因/var与/private/var别名未匹配，实际未触发故障；统一测试路径后重新验证通过，生产脚本没有为此修改。
- 开始/结束哈希对比确认原仓库源码、.xmake配置和既有构建库未改。没有执行第⑤步消费者程序，也没有重跑数据库单元测试或发布GitHub附件。
- 包内README此时沿用项目说明，第⑥步完善专用package README后脚本自动采用；本步产物不等于最终发布包。用户新增docs/packaging.md仍明确留在第⑥步，当前文件尚未创建。
- 证据：/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-package-check-jj9reryd，含执行日志、归档清单/元信息、路径及失败验证脚本和日志、源码/配置哈希、summary.json。
- 当前停在第④步，等待用户检查后进入第⑤步真实解压接入验收。
