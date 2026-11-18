# KopiDB 当前计划

最后更新：2026-11-18。

## 首发准备：C++ 独立接入

目标：在macOS arm64的独立Release构建中，使用现有lsm_shared目标安装完整头文件和共享库；仓库外程序仅依赖安装产物即可编译，完成公开CRUD、事务与跨进程重开读取。已确认安装缺陷沿用 [AUDIT A17](AUDIT.md#a17)。

**进度：100%（3/3阶段正式验收）。** 用户当前安装实现、独立C++接入及中英文用户说明均已完成；README内容经用户审批后写入。

## 全部阶段

| 阶段 | 具体工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 安装目录修复 | `xmake.lua:117–170` lsm_shared的on_install回调 | 新前缀安装后lib是目录，库保留文件名；重复安装头文件路径不增加include层级 | 已通过：用户版本在全新前缀和重复安装中布局/字节一致 |
| ② 独立C++程序验收 | Release库、安装头文件、仓库外consumer，使用公开LSM/TranContext API | 不包含源码目录/依赖包私有路径，不设置DYLD_LIBRARY_PATH；CRUD、批量/遍历、提交/取消、两个进程写/重开读回通过；普通与WiscKey模式均验证 | 已通过：用户版本Release产物、独立编译及4次写/读进程全部通过 |
| ③ 用户接入说明 | 保留README现有内容，补实际可用的安装/编译命令和配置位置；同步中英文必要说明 | 用户按说明可完成同样接入；标明实际验收平台，不承诺未测平台 | 已通过：中英文同步写入，命令一致、锚点有效，示例已编译运行 |

百分比按正式通过阶段数计算，不代表工时比例。

## 当前发现与改动边界

- 使用现有共享库目标，不重做构建系统或更改API/namespace/库后缀。
- 原安装回调把单个库文件复制到尚不存在的prefix/lib，Xmake将lib当作文件名。install虽然返回成功，实际lib是Mach-O共享库普通文件，没有lib/liblsm_shared.so。
- 草案先mkdir目录，再复制到带完整文件名的目标；头文件复制include/*内容，避免重复安装额外嵌套include。
- 第一轮只交付on_install回调修改。Windows分支保持既有结构，本轮仅验证macOS，不新增Windows支持承诺。
- 不在回调中删除用户已有的错误lib路径；验证使用新的临时安装前缀。
- Python SDK/SERIALIZABLE仍为后续讨论项；CI、新快速回归入口、迭代器重构、回滚清理和VLog GC遵守用户排除约定。

## 已完成的预验证

- 当前源码复制到临时目录配置Release，原仓库Debug配置不变；lsm_shared构建成功。
- 原回调在全新前缀安装退出0，但lib实际为普通Mach-O文件，独立接入无法按预期目录继续。
- 只在临时副本修改回调，新的安装前缀及重复安装布局均通过；原仓库xmake.lua未修改。
- 仓库外consumer通过已安装头文件和库编译；库和程序动态依赖无源码副本构建路径。运行清除DYLD相关变量，普通阈值0及WiscKey阈值64下write/read四个独立进程均退出0，WiscKey文件增长4140B，普通模式VLog保持0B。
- consumer验证普通增改删、批量读取的顺序/缺失/重复项、有序遍历、事务自身读取、提交/取消、4KiB和12B值、flush与重开。此为交付冒烟，不替代全库测试或平台矩阵。
- 配置时遇到Xmake缓存锁/引用文件的沙箱写权限限制，经工具权限审核后配置/安装成功；这不是项目代码缺陷。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-cpp-release-11xze0w6`，含源码副本、原/候选回调、安装布局、consumer源码及编译命令/运行结果。原错误安装前缀保留为证据。

## 上一项完成

A8已完成，100%（2/2阶段），117个不同用例及2次复测通过，共119次执行。详细结果保留于AUDIT A8、STATE及A8完成计划快照。

## 正式验收（2026-11-18）

- 用户修改后的on_install与方案语义一致，示例路径仅为注释，实际安装位置仍由target:installdir取得；助手未代改xmake或业务源码。
- 将用户当前源码完整复制到新的临时目录，独立配置Release并构建lsm_shared，原仓库构建配置不变。
- 使用全新安装前缀执行两次安装：lib是目录，lib/liblsm_shared.so存在；全部安装文件的相对路径与SHA256在两次安装后相同，头文件没有嵌套include。
- 仓库外consumer仅使用安装后的头文件和库成功编译。otool检查仅有预期库ID和系统依赖，无临时源码构建目录依赖；执行清除DYLD相关变量。
- 普通阈值0和WiscKey阈值64各运行write/read两个独立进程，4/4退出0。覆盖CRUD、批量顺序/重复/缺失、有序遍历、事务自身读取、提交/取消、4KiB及12B值、flush和跨进程重开；对应VLog为0B和4140B。
- 源码/头文件/xmake/config执行前后哈希稳定，当前版本已正式验收，不再只是临时草案预验证。未重新运行全库单元测试；未验收Windows/Linux。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-a17-verified-yogc1c_h`，含源码快照/哈希、构建和安装日志、安装文件列表及哈希、consumer源码/编译命令/动态依赖和4次运行结果。

## 文档完成（2026-11-18）

- 用户审批中文版草稿后，要求同步写入README.md与README.zh-CN.md；已仅扩充原“构建与测试”章节，保留原目录锚点、学习资料、完整C++示例和其他正文。
- 新增Release共享库构建/安装、外部程序编译与rpath、配置位置和默认行为、原有模块测试命令。两版3个shell代码块去除注释后完全一致，bash语法检查和所有本地锚点检查通过。
- 当前README完整C++示例已在临时目录使用安装产物编译运行成功（无config时按说明提示并使用默认值）；该示例本轮未改动，没有重复运行全库测试。示例证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-readme-draft-ea7d53y3`。
- 文档核查证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-readme-verified-rmrbqu8m`。Git差异验证只有批准章节变化，既有C++示例字节保持，git diff --check通过。
- 三阶段均完成，本轮C++独立接入收尾；没有自动开始Python SDK或SERIALIZABLE。
