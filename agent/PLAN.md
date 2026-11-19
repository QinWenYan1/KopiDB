# KopiDB 当前计划

最后更新：2026-11-19。

## 跑通覆盖率与普通 SST 可视化工具

目标：开发者手动运行脚本后，可以查看真实测试覆盖率报告，或者查看一份普通 SST 的内部结构。正常 `xmake build` 不自动执行这些脚本。用户已确认上一轮 SDK 已发布；其交付与验证证据保留在 STATE。

用户已批准三个阶段，每阶段完成后汇报并等待检查，再进入下一阶段。阶段①基础链路及用户追加的默认范围扩展均已完成，阶段②普通 SST 可视化已完成，等待用户检查后进入阶段③；README 导航和工具文档路径已整理。业务代码、磁盘格式和已发布附件不修改；本轮不添加 WiscKey/VLog 可视化、CI 或全库覆盖率达标任务。

**进度：67%（2/3 阶段验收通过），阶段①和②已通过，等待最终收尾。** 按通过阶段数计算，不代表工时。

| 阶段 | 文件与工作 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| ① 覆盖率（[A18](AUDIT.md#a18)） | test_coverage.sh；xmake.lua 开头 coverage 配置 | macOS 实际运行单目标/多目标并生成源码 HTML；失败或零匹配不报成功；原构建、配置与业务文件保持 | 已通过 |
| ② 普通 SST 可视化（[A19](AUDIT.md#a19)） | scripts/sst_visualize.py 的 parse_sst 与输出入口，仅在实测需要时修改 | 真实 SSTBuilder 生成文件，多 Block、多版本、墓碑、开/关 Bloom 的解析与已知输入一致 | 已通过 |
| ③ 文档及收尾 | docs/coverage.md；docs/Sst_visualize_tool.md；双语 README 导航及状态记录 | 按文档复跑两个工具，明确输出位置、覆盖范围和普通 SST 支持边界 | 导航与当前行为说明已整理；最终复跑待完成 |

## ① 覆盖率

- [x] macOS coverage 使用 Apple Clang 原生插桩与 llvm-profdata/llvm-cov，不混入 gcov 参数；其他平台保留原覆盖率规则，本轮不扩展平台验收。
- [x] 脚本兼容系统 Bash 3.2，先检查依赖、参数。独立源码副本构建，每个测试目标独立数据目录；不清空原 build，不改原 .xmake。
- [x] 保留 `bash test_coverage.sh [test_target ...]` 入口；无参数默认全部 test_* 模块，精确排除已说明的四项耗时用例。支持过滤条件；显式目标或 `--all` 不应用默认排除。
- [x] 仅统计所选测试实际链接且插桩的引擎 src/include；排除第三方和测试代码。保存选择/过滤条件、GTest 结果、日志和统计口径，不称为整个仓库完整覆盖率。
- [x] profiles 独立命名，合并后生成 coverage-report/index.html；快照清理后网页仍可查看源代码。失败不覆盖之前成功报告。
- [x] 实跑单目标、默认多目标；检查无效目标、零匹配和必要失败路径。百万条用例不属于本轮工具验收；未运行的测试不算通过。

边界：覆盖率依赖进程写出计数，异常退出或 `_exit` 路径可能不完整；本轮不改业务退出语义。覆盖率数值不是正确性证明，也不设 100% 门槛。

### 已采纳的默认范围扩展（已验收）

用户在阶段①验收后要求默认覆盖所有模块。保留上述两目标验收作为历史证据，不能直接把它当作新增范围的验收。

- [x] 无参数默认发现全部测试模块，并明确列出排除的大规模／性能基准用例；`--all` 运行完整集合，显式目标仍可单独选择。
- [x] 根据实际测试代码确定具体排除清单及原因，写入运行摘要和 docs/coverage.md；不能只用“常规测试”概括，也不能因过滤失败用例来宣称成功。
- [x] 运行扩展后的默认集合，核对各目标及源码统计，再同步文档默认行为。脚本、帮助和文档已同步为全部模块常规集合，实际 570/570 通过。

## ② 普通 SST 可视化

- [x] 使用实际普通 SST 的小型样本，验证内容及分段位置。源码对照正确不代替实际解析验收。
- [x] 若当前脚本已正确，保留实现；若不匹配，只修复完成普通 SST 展示所需的逻辑。
- [x] 检查缺失输入/文件提示与生成页面，运行时不修改输入 SST。

## ③ 文档及收尾

- [x] docs/coverage.md 已说明当前手动启动、选目标/过滤、报告口径、路径和耗时范围；默认集合扩展及精确排除清单已同步。
- [x] Sst_visualize_tool.md 已移至 docs/，修正仓库根目录下的命令，说明普通 SST 限制及输入/输出。
- [x] 双语 README 增加目录项及工具文档导航，链接 coverage、SST 可视化和 packaging；不改原知识与 API 示例。
- [ ] 按文档复跑；回写阶段验收证据与使用命令。不扩展为全库审计或发布新版本。

## 阶段①验收（2026-11-19）

- `test_coverage.sh` 已改为 Bash 3.2/macOS LLVM 流程；`xmake.lua` 只调整 coverage 配置，debug/release 规则保留。使用 Xcode 工具链提供 Command Line Tools 的 SDK 路径，编译/链接工具与 xcrun LLVM 配套。
- 首次沙箱执行被用户级依赖缓存文件锁权限拒绝，授权后继续；通用 clang 工具链初测找不到标准库头文件，修正为 Xcode 工具链后实际成功。两次原始日志保留，不把失败说成通过。
- 默认真实运行 SkipList 31/31、MemTable 51/51，共 82/82，零失败、跳过和禁用。只构建所选目标与依赖，未运行百万条测试或 --all 全量。
- 在路径含空格、从仓库外启动的一次性副本中验证 `test_config --filter ConfigTest.BasicTest`，1/1 通过；预设 GTest 分片与重复环境被脚本清除，实际仍只运行一次。
- 未知目标、未知参数、--all 与显式目标冲突均返回失败；零匹配过滤器及一次性副本中的人为失败断言正确退出，原成功报告文件哈希全部保持。人为失败属于脚本错误处理验收，不是数据库现有用例失败。
- 已发现全部 12 个 test_* 目标；只核对发现列表，未声称 --all 全部测试已跑。
- 默认 HTML 共 24 页（23 个源码页＋首页），本地链接全部存在；临时源代码目录已清理，网页仍嵌入源码。report/merge/html 日志无警告；报告仅包含允许的 src/include。
- 本次子集的行覆盖率 58.88%，分母仅为所选程序已链接的插桩引擎代码，不能作为全库覆盖率。`_exit`/异常子进程的计数限制在运行摘要中说明。
- 验收前后 1815 个原业务、测试、config.toml、.xmake、build 及发布归档文件的 SHA-256 和文件清单完全一致；未修改业务代码或现有单元测试。
- 用户可打开 `coverage-report/index.html`；摘要与每目标计数位于 `coverage-report/run-summary.txt`、`test-results.tsv`，日志/JSON/profile 一并保存。
- 完整证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-coverage-stage1-pp23j6zj`（summary.json、原始失败日志、默认报告备份、临时副本探针、前后哈希与 HTML 核查）。阶段②/③尚未开始。

## 阶段①追加验收：默认覆盖全部模块（2026-11-19）

- 用户已采纳默认范围扩展，本轮只改 test_coverage.sh 的目标选择/过滤与摘要，以及 docs/coverage.md；xmake coverage 配置与业务实现不变。
- 默认从 Xmake 发现全部12个 test_*，排除精确四项：LSMTest.BigPersistence、LSMTest.BigPersistence2、CompactTest.TombstoneDefaultConfig、WiscKeyCompactionBenchmarkDeathTest.MeasuresReferenceCompactionCost。排除理由在脚本、帮助、文档、excluded-tests.tsv 和运行摘要中对应；不按模糊名称排除。
- 保留两个十万条 Persistence、小内存持久化/墓碑、WAL并发及普通WiscKey功能回归。默认过滤器合并用户已有负向条件；空字符串仍为零匹配。显式目标/--all 不应用默认排除，仍尊重请求的过滤器。
- 实际12目标570/570通过：Config3、Block37、BlockCache7、BlockMeta6、Compact4、LSM171、MemTable51、SkipList31、SST30、Utils15、WAL148、WiscKey67；零失败/跳过/禁用，另4项显式排除没有执行、不计通过。
- 本轮包括11项名称含TwoMerge的测试，报告实际含engine.cpp与two_merge_iterator.cpp。行覆盖率90.53%，仅统计已链接插桩引擎代码；较旧58.88%的变化来自测试范围扩大，不能宣称业务性能或正确性提高。
- HTML共33页，链接完整，源快照删除后可看源码；报告/合并日志无警告。原业务/测试/.xmake/build/配置/归档共1815文件清单及SHA保持。
- 另用含6个平凡GTest的一次性项目验证脚本契约：default 2/2；--all 6/6；显式target 6/6；default加已有负过滤1/1；空过滤退出1并保留旧报告20文件。使用四个相同排除名的小用例验证选择逻辑，没有运行真实百万级/基准用例，不计入570项数据库测试。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-coverage-expanded-ryzeg2qq`，含summary.json、真实默认日志/报告、contract-probes/summary.json、前后哈希与旧报告备份。
- 覆盖率阶段完整完成，普通SST可视化与最终收尾仍待下一阶段。

## 阶段②验收：普通 SST 可视化（2026-11-19）

- 在独立当前源码副本中构建 SSTBuilder 验收程序，真实生成3份普通文件：Bloom开启/关闭的多块文件（各5 Blocks、9记录）和无Bloom的单块单记录文件。引擎重新open并全版本读取，输入逐条一致。
- 发现并实测[A19](AUDIT.md#a19)：无Bloom时bloom_offset等于footer起点，旧脚本set后按偏移排序，同址顺序受Python哈希种子影响。491B真实文件在seed2错误出现Bloom导航、隐藏Footer；seed0/1/3正常。
- 只修改scripts/sst_visualize.py的四段边界计算，按固定布局取相邻起止点；中文注释含467–491的例子。保留编码、界面和其他解析，不添加WiscKey、CRC或损坏输入处理。
- 3份实际文件×4个哈希种子，12/12 HTML核对通过：全部记录及每块归属、Meta首尾key/偏移、事务范围、Bloom字段/大小、Footer24B与导航；同文件不同种子生成页面完全一致。覆盖墓碑/旧版本、12字节普通值、中文、HTML特殊字符、大于2^53的事务ID，输入文件哈希保持。
- 新增test/test_sst_visualize.py：491B真实SST黄金样本、四seed回归及缺参/缺文件提示，3/3 unittest通过。相同测试指向修复前脚本时，seed2子用例失败，证明测试抓住A19；没有改测试预期来适配缺陷。
- 使用文档补充独立Python回归命令并实跑通过；不将这3项算入C++覆盖率的570项。业务、原测试、.xmake/build、config、发布归档及覆盖率入口共1817个既有文件哈希不变（仅新增Python测试）。
- 可直接打开的3个示例页面放在忽略的sst_html/ordinary-demo-h4871ujq/。WiscKey仍不支持；本轮未运行全库C++测试或修改已发布包。
- 证据：`/var/folders/tc/r_qvpl1d1tqdqf7td2y5chlr0000gn/T/kopidb-sst-visualize-8crqx6go`，包含真实夹具程序/manifest/SST、旧版4seed复现、12次HTML结果、回归前后日志与summary.json。首次临时夹具调用错误接口的编译失败已保留，修正的是夹具，不是引擎。
