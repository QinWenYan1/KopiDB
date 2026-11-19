# C++ SDK 打包与维护

KopiDB 的 C++ SDK 将头文件、已编译的共享库和接入配置一起分发。维护者负责构建引擎；使用者下载后，只需编译自己的程序并链接 SDK。当前打包入口支持 **macOS arm64 / Release**。

## 文件分工

| 文件 | 职责 |
| --- | --- |
| [根 xmake.lua](../xmake.lua) | 构建引擎、测试和示例；`kopidb_shared` 生成共享库，`on_install` 组织 SDK 安装目录。 |
| [scripts/package_cpp.sh](../scripts/package_cpp.sh) | 创建源码快照，调用构建与安装，补齐版本、许可和构建信息，检查并压缩产物。 |
| [packaging/cpp/xmake.lua](../packaging/cpp/xmake.lua) | 供使用者导入预编译库，公开传递头文件、链接和运行路径等配置。 |
| [packaging/cpp/KopiDBConfig.cmake](../packaging/cpp/KopiDBConfig.cmake) | 供使用者通过 `find_package(KopiDB CONFIG REQUIRED)` 获得 `KopiDB::KopiDB` 导入目标。 |
| [packaging/cpp/README.md](../packaging/cpp/README.md) | 随 SDK 分发的接入说明，由打包脚本复制为包内 `README.md`。 |

两份 SDK 接入配置都不编译数据库源码。引擎的构建入口仍是根 Xmake 配置，SDK 中的 CMake 配置并不意味着仓库另有一套 CMake 引擎构建流程。安装时必须复制 `packaging/cpp/xmake.lua`，不能用同名的根配置替代。

## 从源码到发布附件

```text
当前工作树 → 临时源码快照 → Release 共享库 → SDK 安装目录
           → 补版本、许可和 BUILD-INFO → 临时压缩包 → 正式 .tar.gz
```

在仓库根目录执行：

```bash
./scripts/package_cpp.sh                  # 默认输出到 dist/
./scripts/package_cpp.sh "./release files" # 也可指定带空格的输出目录
```

脚本从自身位置查找仓库，因此也可以在其他工作目录调用。传入相对输出目录时，路径相对于调用者的工作目录。运行前需要 Xmake、`rsync`、`tar`、`lipo`、`otool` 和 `xcrun` 等构建及检查工具。

1. **复制输入。** 脚本复制当前工作树的构建输入，包含尚未提交的改动；不使用 `git archive`。临时副本不会复用仓库的 `build/`、`.xmake` 或旧 Python SDK 二进制，因此不会切换原仓库的构建模式。它仍会使用 Xmake 管理的工具链与依赖，这不是完全离线、无缓存的构建。
2. **构建与读取元信息。** 在副本执行 `xmake f -y -p macosx -a arm64 -m release` 和 `xmake build -y kopidb_shared`。临时 Lua 脚本通过 Xmake 查询项目版本、编译器和该目标实际使用的依赖。
3. **安装与补齐文件。** `xmake install -o <安装目录> kopidb_shared` 触发根配置的 `on_install`，复制项目头文件、当前构建的共享库及两份接入配置。打包脚本再补入项目许可、依赖许可、包内说明和 `BUILD-INFO.txt`。
4. **检查与压缩。** 脚本检查必要文件、arm64 架构、动态库依赖及最低 macOS 标签，然后压缩并读取归档目录。全部成功后，将临时归档替换为正式文件。

发布前应确认工作树内容与准备发布的提交一致。构建使用本次复制的输入，脚本不会自动提交改动、创建版本标签或发布 GitHub Release。

## 包内布局与单入口头文件

```text
kopidb-<版本>-macos-arm64/
├── include/kopidb/        # 项目头文件及入口依赖的其他头文件
├── lib/libkopidb.so       # 已编译的 macOS 共享库
├── xmake.lua             # SDK 接入配置
├── KopiDBConfig.cmake    # SDK 接入配置
├── README.md
├── LICENSE
├── licenses/             # 第三方依赖许可
└── BUILD-INFO.txt
```

当前 macOS 产物也使用 `.so` 后缀；它是共享库，不应仅根据后缀推断二进制平台。包名为 `kopidb-<版本>-macos-arm64.tar.gz`，版本来自根配置的 `set_version`，不在脚本内重复维护。

使用者只需要写一个 KopiDB 入口：

```cpp
#include <lsm/engine.h>
```

接入配置会把 `include/kopidb` 加入头文件搜索路径。这个入口继续包含其他项目头文件，因此应保留完整 `include/` 目录。“一个 include”不代表单文件分发或 header-only：程序仍需链接 `libkopidb.so`，运行时也需要找到它。

Xmake 使用者在项目中 `includes("SDK目录")`，再为程序目标添加 `add_deps("kopidb")`；SDK 的 `kopidb` 是只传播配置的 `phony` 目标。CMake 使用者通过 `KopiDB_DIR` 指向 SDK 根目录，查找包后链接 `KopiDB::KopiDB`；该目标是 `SHARED IMPORTED`。两种方式都只编译使用者自己的源文件。

## 路径、运行时查找与语言标准

SDK 不写死维护者的安装前缀：Xmake 从 `os.scriptdir()` 定位自身，CMake 从 `CMAKE_CURRENT_LIST_DIR` 定位自身，然后拼出 `include/kopidb` 和 `lib`。整个 SDK 换一个解压位置后，可以重新配置并构建用户项目。

链接器能找到库，并不代表程序启动时能找到库。当前 macOS 库的 install name 是 `@rpath/libkopidb.so`；Xmake 接入配置显式传递 SDK 库目录的 rpath，CMake 根据导入库位置生成构建运行路径。这样构建出的程序可直接运行，无需依赖开发机的 `DYLD_LIBRARY_PATH`。

这些路径依据配置时的 SDK 位置生成，并非保证已编译的程序可随 SDK 任意移动。移动 SDK 后应重新配置和链接；发布应用自身时也要安排其共享库位置和运行路径。

Xmake 配置保留了针对 **3.1.0** 实测行为的处理，维护时不要只因写法特殊就删除：

- 库目录通过 `add_linkdirs` 传递，库名通过 `add_links("kopidb")` 传递，避免完整库文件路径中的空格被拆开。
- 无空白字符的目录使用 `add_rpathdirs`。路径含空白字符时，3.1.0 的 rpath 自动探测可能拆开路径、漏掉运行路径，因此改用 `-Xlinker` 参数组，把整个目录作为独立参数保留。
- 参数同时加入 `ldflags` 和 `shflags`，分别覆盖可执行目标和共享库目标；`force = true` 跳过失败的参数探测，`expand = false` 保持参数组。
- 参数组后额外的空字符串会被忽略，但能避开 3.1.0 对单组参数的解包问题。移除它时，第一个消费目标可能正常，后续目标却丢失参数，所以验收必须包含多个消费目标。

C++ 标准同样属于接入配置：CMake 的 `cxx_std_20` 表示至少需要 C++20；Xmake 3.1.0 的公开 `set_languages("c++20")` 会将依赖它的目标固定为 C++20，不能承诺保留使用者指定的更高标准。这些要求只传给使用 KopiDB 的目标，不修改无依赖目标的全局配置。线程依赖也由配置传递，分别使用 `pthread` 和 `Threads::Threads`。

## 版本、许可与构建信息

`BUILD-INFO.txt` 记录项目版本、平台、Release 模式、C++ 标准、编译器及其版本、实际依赖版本、构建主机 macOS、Apple SDK、共享库最低 macOS 标签和动态依赖。最低系统标签来自生成的库，发布说明应以该包记录为准；它不能代替在相应系统上实际运行的兼容性验证。

许可文件与本次构建使用的依赖配套：

- 项目 `LICENSE` 复制到包根目录。
- `toml11` 的完整 MIT 许可从实际安装的 `toml.hpp` 提取。
- `spdlog` 的完整许可使用仓库内[已核对的许可文本](../packaging/cpp/licenses/spdlog-LICENSE)；升级版本时需重新核对。
- 使用 spdlog 内置 fmt 时，从实际安装目录复制 `fmt.license.rst`，并从头文件读取 fmt 版本。

收集范围来自 `kopidb_shared` 的实际包依赖，不把测试的 gtest 或 Python 绑定的 pybind11 算入 C++ SDK 分发内容。脚本遇到未知依赖、声明的许可证类型变化或新增传递库依赖会停止，维护者需先补齐收集规则。动态依赖检查也会拒绝未打包的额外动态库，避免把依赖开发机绝对路径的产物发布出去。

## 失败处理与维护验收

源码副本、安装树和中间文件放在本次创建的临时工作目录。待发布归档则先写入最终输出目录中的临时文件，确保最终替换在同一文件系统内完成；`tar` 写入及目录读取成功后，才通过 `mv` 替换同名正式归档。

正式归档替换前发生失败或捕获到中断信号时，退出清理只删除本次临时工作目录和临时归档，保留上一次成功生成的正式包。归档清单可读只是打包检查，不能代替消费者运行验收。

改动打包流程或准备发布时，至少检查：

- [ ] 版本、包名、说明与待发布提交一致；最终附件由当前输入重新生成。
- [ ] 解压检查头文件、库、接入配置、许可和 `BUILD-INFO.txt`，没有混入构建缓存或旧二进制。
- [ ] 用真实压缩包建立独立 CMake 和 Xmake 用户项目，只编译自己的源文件；核对实际头文件及加载库均来自解压目录。
- [ ] 从新位置及带空格目录接入，覆盖多个可执行目标、共享库目标和无依赖目标；清除动态库搜索环境变量后直接运行。
- [ ] 验证 CRUD、事务提交与回滚、批量操作、有序遍历及跨进程重开；普通模式与启用 WiscKey 的模式分别检查。
- [ ] 修改失败处理时注入归档写入失败，确认旧包内容未变、临时归档和工作目录已清理。

## GitHub Release 发布

Tag 是一个确定提交的版本标记；GitHub Release 在这个标记上附加版本说明和可下载的文件。发布前由维护者提交、推送本轮源码与文档，确认准备发布的提交和打包输入一致。脚本会包含未提交改动，因此不能只看包名就认定它对应某个 Git 提交。

以 `0.0.1` 为例，在生成最终归档后准备校验文件；重新打包后也需重新生成校验文件：

```bash
cd dist
shasum -a 256 kopidb-0.0.1-macos-arm64.tar.gz > kopidb-0.0.1-macos-arm64.tar.gz.sha256
shasum -a 256 -c kopidb-0.0.1-macos-arm64.tar.gz.sha256
```

然后由维护者在 [GitHub Releases](https://github.com/QinWenYan1/KopiDB/releases) 操作：

1. 选择 **Draft a new release**，创建或选择与本次版本对应的 tag，例如 `v0.0.1`。
2. 新建 tag 时，**Target** 选择已经包含本轮修改的分支或提交；本项目通常使用 `main`，发布前核对提交。
3. 填写标题、功能和平台要求，上传 `.tar.gz` 与对应 `.sha256` 两个附件。GitHub 自动生成的 **Source code** 文件是源码快照，不包含我们的预编译 SDK。
4. 首个实验版本建议选择 **This is a pre-release**。先保存草稿核对附件，确认后再点击 **Publish release**。
5. 从发布页面重新下载两个附件，放在同一目录运行上述校验命令，并按 README 的接入步骤检查下载结果。

GitHub 界面与选项以[官方发布说明](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository?tool=webui)为准。`dist/` 是本地交付输出，归档通过 Release 附件分发，不需要提交到 Git 仓库。

面向使用者的具体接入步骤见[项目 README](../README.md)和[包内说明](../packaging/cpp/README.md)。实施与验收记录保留在 [STATE](../agent/STATE.md) 和 [PLAN](../agent/PLAN.md)，本页只维护打包设计与检查方法。
