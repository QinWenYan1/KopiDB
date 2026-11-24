# AGENT.md

本文件是 KopiDB 的助手协作入口，适用于参与本项目的编码助手。文件按项目约定命名为 `AGENT.md`；开始协作时先阅读本文件，再按任务读取下列资料。

## 项目与资料入口

KopiDB 是类 RocksDB 的教学级 LSM-tree KV 存储引擎（C++20），沿 [tiny-lsm 教程](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html) 逐模块实现。

| 位置 | 用途 |
| --- | --- |
| `src/`、`include/` | 引擎业务实现，由用户亲手编写。 |
| `test/` | 单元测试，助手可按当前任务补充和运行。 |
| [agent/AUDIT.md](agent/AUDIT.md) | 全局问题账本、证据及处理状态，保留已解决记录。 |
| [agent/STATE.md](agent/STATE.md) | 当前项目状态、已确认设计、全局约定及整体排期。 |
| [agent/PLAN.md](agent/PLAN.md) | 本轮任务、各阶段 TODO 和验收标准。 |
| `agent/skills/<技能名>/SKILL.md` | 可复用的工作方法，按任务选择读取。 |
| `docs/rocksdb/` | RocksDB 源码精读笔记，作为独立学习副线。 |
| [docs/usage.md](docs/usage.md) | 用户接入、配置、完整 API 示例和事务语义。 |
| [docs/lsm-overview.md](docs/lsm-overview.md)、[docs/optimizations.md](docs/optimizations.md) | LSM 背景资料与有测量依据的优化案例；由 README 导航进入。 |
| [docs/coverage.md](docs/coverage.md)、[docs/Sst_visualize_tool.md](docs/Sst_visualize_tool.md) | 手动开发工具的使用方法和适用范围。 |
| [docs/packaging.md](docs/packaging.md) | KopiDB 构建、打包与 SDK 接入设计，面向维护者；使用步骤见 README，进度见 STATE/PLAN。 |

继续 Lab 前先读 STATE 和 PLAN，确认当前阶段及尚未验收的内容。代码与记录不一致时先核对代码，再更新记录；遵循用户在当前会话中的明确要求。

README 是面向使用者的简洁入口：默认 `README.md` 为中文，`README.en.md` 为对应英文首页，`README.zh-CN.md` 只保留旧链接入口。阅读顺序是认识项目、下载接入、最小示例、文档导航、优化简介、学习资料和鸣谢。保留 logo、带图标的目录、必要使用限制，以及 tiny-lsm 代码框架和教程来源；不把内部状态记录放在首页。

修改前先读用户最新版本。首页保留简短 CRUD/事务示例；完整五类 API 单代码块、事务对比与高级说明维护在 `docs/usage.md`，LSM 原理/取舍/场景及论文资料维护在 `docs/lsm-overview.md`，优化机制与实测维护在 `docs/optimizations.md`。迁移有用内容而非直接删除，不再要求所有细节都塞回首页。小节配图标、目录与双语入口同步；变更路径时检查旧链接和已发布 SDK 中的文档链接。

完整 API 示例继续用注释区分普通 CRUD、提交、回滚、批量和遍历，突出调用流程；省略提交结果处理时简短注明，不改变成功／失败语义。优化数字须附场景和测量口径，历史记录不称为本轮重测。内部能力核查、全局约定与阶段验收放 STATE，当前执行放 PLAN，问题事实与复现放 AUDIT，避免重复维护。

README 的普通 KV 示例使用对外 `LSM::put/get/remove`，不要求用户组装 Engine/TranManager、分配事务 ID 或保存内部状态。显式事务示例只展示 `LSM::begin_tran` 与事务句柄的读写、commit/abort，不混入底层组件 API。对外入口尚未接通时简短标明接口示意，不用内部联调代码冒充已可运行的用户入口，也不为文档擅自提前实现后续 Lab。

## Lab 协作流程

开始新任务或重新界定多步骤任务时，使用 [goal-to-plan：先定目标，再拆阶段](agent/skills/goal-to-plan/SKILL.md)。已经确认的目标与计划直接沿用。

开始或继续 Lab 时，先用[计划进度条](agent/skills/plan-progress/SKILL.md)展示总览，再按[逐函数工作流](agent/skills/lab-workflow/SKILL.md#4-一次讲清一个函数)交付本轮函数。用户手写业务代码，助手负责测试与验收。阅读新 Lab、对照作者实现和阶段收尾也沿用该工作流。

主线按 STATE 中已确认的 Lab 路线推进，遵循用户对章节范围的取舍。RocksDB 源码阅读与笔记按用户的学习安排开展，不作为每个 Lab 的强制前置步骤。后续章节排期记录在 STATE；发现的缺陷或重构建议登记 AUDIT，未选中时不打断当前主线。

发现问题、选择问题开始修复或验收回写时，使用 [audit-to-plan：问题先记账，选中再实施](agent/skills/audit-to-plan/SKILL.md)。AUDIT 保存全局问题事实；选中的工作进入 PLAN，原条目保留并更新状态；STATE 保存项目现状与整体排期，通过链接引用问题，避免重复维护。修复任务继续时只需优先读取 PLAN 关联的条目。

用户要求起草／改写 issue，或进入新的修复问题时，使用 [draft-issue：起草可直接发布的 issue](agent/skills/draft-issue/SKILL.md)。在聊天中提供自包含的中文正文，源码位置使用核实过的 GitHub 链接，由用户手动发布；用户要求直接复制时不混入整体进度或教学内容。

## 判断问题与提出修改

评估疑似 bug、检查实现、判断修复是否必要，或设计涉及状态与参数传递的改动时，读取并遵循 [verify-before-fix：先验证，再修复](agent/skills/verify-before-fix/SKILL.md)。

- 先检查实际调用链及已有保障，再判断是否缺少处理。
- 用具体触发场景和代码位置区分必修 bug、可选防护与后续优化；不能把额外要求自动加入当前任务。
- 对照 Lab 和作者实现时保留独立判断，区分已验证事实与推断；资料不可用或结论未验证时直接说明。
- 测试依据接口承诺和已确认需求，不以助手自行增加的要求制造失败。

## 交流与进度记录

- 使用简明中文，先给结论，再解释具体代码和例子。用户判断有误时说明理由，不为了附和而改变结论。
- 定位修改时给出文件、函数及当前行号，避免“原循环后面加上”这类含糊说明。
- 有当前计划时，每轮修改、检查和汇报都调用[计划进度条](agent/skills/plan-progress/SKILL.md)。用聊天正文的引用块、字符长条和完成数显示总进度，仓库用 PLAN 清单记录完成情况。阶段表逐行说明任务、主要改动、完成数/阶段总项数、百分比和状态。具体步骤用小标题，代码紧跟各自的文件位置。只做协作维护时，同样汇报业务计划的实际进度。
- 问题发现与处理状态更新到 AUDIT，当前步骤与验收更新到 PLAN，项目能力与全局约定更新到 STATE。已解决问题保留编号和验收证据；审计完成不代表修复完成。
- 每次进入一个新的修复问题，先在聊天中起草 issue，由用户手动发布；写明问题、准确位置、复现与实际结果、修复范围和验收标准。同一问题内逐函数推进时不重复起草，也不把实际发布 issue 作为继续实现的前置条件。
- 章节定位使用左侧目录编号和名称，必要时补充文章标题及正文小节。例如“目录 5.1.2 事务管理器（文章标题 Lab 5.2），正文 3.2.3 get”；目录 5.2 是尚未开始的 WAL 和崩溃恢复阶段，不能与文章标题 Lab 5.2 混用。

## 验证与常用命令

- 助手按改动范围构建和测试，将实际结果和待验证项记入 PLAN。日常回复用完成进度呈现结果，用户询问时再展开验收细节。需要用户修正的代码直接指出问题和位置。
- 测试可能清理相对路径的数据目录。助手运行测试时，在独立临时目录复制所需配置，并以绝对路径启动已构建的测试程序，避免影响项目数据。

```bash
xmake build test_skiplist          # 构建指定测试目标
xmake run test_skiplist            # 常规测试入口；助手运行时遵守上面的隔离要求
xmake f -m release                 # 切换 release 配置
xmake f -m debug                   # 切回 debug 配置
xmake project -k compile_commands  # 生成 clangd 编译数据库
```

## Git 纪律

- Git 写操作（commit、push、branch、merge 等）由用户执行；助手只使用 status、log、diff 等只读命令。
- 修改后说明变更和验证情况，提交时机与粒度由用户决定。
- `reference-impl` 作为参考分支，不合并进 main。
- 超出本轮授权范围的变更先说明必要性，由用户决定；已有授权的测试与文档维护继续执行。

## 代码风格

- 中文注释，沿用 skeleton 的 `// ?`（提示）与 `// !`（警告）标记。
- snake_case，2 空格缩进，K&R 大括号；保留公开接口及既定拼写，如 `tranc_id`、`preffix`。
- 保留已有 spdlog 埋点，解释逻辑时重点说明为什么需要该判断。

## RocksDB 笔记约定

- 本节仅在处理 `docs/rocksdb/` 笔记任务时适用。既有参考为 `~/Desktop/Lab/rocksdb` 的 `e6a2ee0` 版本，新增源码引用前核对实际版本。
- 源码锚点使用包含完整 commit SHA 和行号的 GitHub 永久链接，不用 main 链接或未核实的 SHA。
- 笔记只记录 RocksDB 知识，不加入 KopiDB 对照章节；原因解释放在相应知识点中。
- 使用 `course-notes-generator` 技能规定的笔记体例，术语采用中文与英文对照；技能不可用时明确说明，不假定能读取其他助手的持久记忆。
- 优先核对本地源码，再用官方资料解释设计；其他检索资料用于定位线索。助手起草，用户精读修改，再由助手检查。
