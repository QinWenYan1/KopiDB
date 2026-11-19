# 📊 测试覆盖率

[返回项目 README](../README.zh-CN.md#tools-and-docs)

[`test_coverage.sh`](../test_coverage.sh) 用来检查测试实际执行到了哪些引擎代码。完成一个功能或补充测试后，可以手动运行；正常 `xmake build` 不会自动触发覆盖率统计。

## 📦 准备条件

- macOS、Xmake，以及 Xcode 或 Command Line Tools 提供的 Apple Clang、`llvm-cov`、`llvm-profdata`。
- 仓库源码和构建依赖；首次构建可能需要下载依赖。
- 脚本兼容 macOS 的 Bash 3.2，使用 LLVM 工具直接生成 HTML，不需要 `lcov/genhtml`。

## ▶️ 选择测试并运行

以下命令在仓库根目录执行：

```bash
# 查看参数
bash test_coverage.sh --help

# 只统计一个测试目标
bash test_coverage.sh test_sst

# 合并多个目标的覆盖率
bash test_coverage.sh test_skiplist test_memtable

# 只运行 test_lsm 中的 TwoMerge 相关用例
bash test_coverage.sh test_lsm \
  --filter 'IteratorMvccTest.TwoMerge*:LSMTest.MvccTwoMerge*'

# 打开最近一次成功生成的报告
open coverage-report/index.html
```

**当前无参数调用仅运行 `test_skiplist` 和 `test_memtable`，不是所有模块。** 想查看其他模块，应显式指定目标，或使用下面的全量入口。

```bash
# 包含全部 test_* 目标及其大规模、性能测试，可能耗时很长
bash test_coverage.sh --all
```

`--all` 与显式目标列表不能一起使用。它会从 Xmake 项目中发现全部 `test_*` 二进制目标；当前包括配置、SkipList、MemTable、Block、BlockMeta、文件工具、SST、LSM、Block Cache、Compaction、WAL 和 WiscKey。

TwoMergeIterator 没有单独的测试目标：相关用例位于 `test_lsm` 和 `test_wisckey`。覆盖率以实际执行和链接的代码为准，不要求每个类都拥有同名测试程序。

## 🎯 过滤器与测试范围

- `--filter` 使用 GTest 用例名模式，例如 `SuiteName.TestName`，`*` 可以匹配多个名称。也接受 `--filter=模式`。
- 显式 `--filter` 优先于环境变量 `GTEST_FILTER`；两者都未设置时使用 `*`。
- 同一过滤器会应用到每个选中的目标。只针对某套件时，应同时指定对应测试目标，避免其他目标零匹配。
- 每个选中的目标必须成功退出，并至少有一项测试实际完成且通过。零匹配、测试失败或缺少覆盖率数据都会返回失败。
- 脚本固定执行一轮，清除继承的 GTest 分片、列举及重复执行模式；实际通过、失败、跳过和禁用数量写入结果文件。

## 📂 报告在哪里

报告保存在**仓库目录**的 `coverage-report/`，与调用脚本时的终端目录无关：

| 文件或目录 | 用途 |
| --- | --- |
| `index.html` | 网页入口，点击文件名查看具体源码行 |
| `run-summary.txt` | 本轮目标、过滤器、执行结果和统计范围 |
| `test-results.tsv` | 每个目标的测试数量及退出码 |
| `coverage-summary.txt` | 文件和汇总的行、函数、分支等覆盖率 |
| `logs/`、`gtest/` | 构建、执行、报告日志与 GTest JSON 结果 |
| `profiles/`、`coverage.profdata` | LLVM 原始计数与合并后的数据 |

每次成功运行会替换上一次报告。失败时保留旧报告，并在终端打印本次错误证据的目录；这时旧网页不代表本次运行成功。

## 🔍 如何理解覆盖率

**测试通过率回答“这些断言是否通过”；覆盖率回答“哪些代码被执行过”。** 一组测试全部通过，仍可能没有执行到某个错误处理分支。

报告只统计**所选测试程序已经链接、且带统计信息的 `src/` 和 `include/` 代码**，排除测试本身及第三方库。没有链接进来的文件不会自动作为零覆盖加入分母，因此局部测试的百分比不能称为整个数据库的覆盖率。

先关注未执行的关键逻辑，再根据需要补充有效测试；覆盖率高不证明没有 bug，也不需要为了达到 100% 而机械补测试。使用 `_exit` 或异常终止的子进程可能来不及写出计数，报告不能保证完整反映所有崩溃路径。

## 🛠️ 构建与数据隔离

脚本复制当前工作树源码（包括未提交修改）到临时目录，独立配置 coverage 构建，仅构建所选目标及其依赖。每个测试目标在独立目录运行，并使用仓库 `config.toml` 的副本。

原仓库的 `build/` 和 `.xmake` 保留。成功报告内已嵌入源码，临时源码清理后仍可离线查看。覆盖率程序带有统计开销，其耗时不应作为正常发布版本的性能指标。
