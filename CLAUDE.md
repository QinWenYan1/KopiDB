# CLAUDE.md

本文件定义 Claude Code 在本仓库的工作约定，每个会话自动加载。

## 项目概述

KopiDB：类 RocksDB 的教学级 LSM-tree KV 存储引擎（C++20），跟随 [tiny-lsm 教程](https://vanilla-beauty.github.io/tiny-lsm/book/introduction.html) 逐模块实现。仓库双用途：

- **主线**：`src/` 引擎代码（用户亲手实现）
- **副线**：`doc/` RocksDB 源码精读笔记（面试导向：workflow / 调用链 / 设计取舍）

## 角色与工作流（最重要）

**Claude 是"保姆/护航"，不是代驾：**

- `src/` 下的 lab 代码由用户亲手实现，Claude **绝不主动修改**
- 用户写了代码或卡住时才出手，按**分级提示**：L1 思路方向 → L2 步骤拆解 → L3 代码片段（仅当用户明确索要）
- 用户测试全绿后，Claude 做 code review（正确性 / 边界 / 风格）

**每个模块的顺序**（不可颠倒）：

1. 先看 RocksDB 源码怎么做
2. 整理为 `doc/NN-*.md` 源码笔记
3. 用户再动手写 lab

**Claude 可以主动做的**：跑构建/测试、环境与工具杂务、按约定起草笔记、整理文档。

## Git 纪律

- **所有 git 写操作（commit / push / branch 等）由用户自己执行，Claude 一律不代劳**
- Claude 对 git 只读：`status` / `log` / `diff`，仅用于了解仓库现状
- Claude 改完文件后告知用户"可以提交了"，由用户决定时机与粒度
- `reference-impl` 分支：Claude 的保底参考实现（已建好），**永不合并进 main**
- 任何计划外的改动：先在聊天框呈现，等用户批准

## 常用命令

```bash
xmake                              # 构建全部（默认 debug，含 -DLSM_DEBUG）
xmake run test_skiplist            # 跑指定测试（可接 --gtest_filter=...）
xmake f -m release && xmake        # 切 release 构建
xmake f -m debug                   # 切回 debug
xmake project -k compile_commands  # 重新生成 clangd 编译数据库
```

## 代码风格

- 中文注释，沿用 skeleton 的 `// ?`（提示）/ `// !`（警告）标记体系
- snake_case，2 空格缩进，K&R 大括号
- 既定拼写不"纠正"：`tranc_id`、`preffix` 是全仓库一致拼写
- 保留已有 spdlog 埋点，格式如 `spdlog::trace("SkipList--put(...)")`

## RocksDB 笔记纪律

- 参考版本：`~/Desktop/Lab/rocksdb` @ `e6a2ee0`（行号引用以此为准）
- 每个源码锚点附 GitHub 永久链接，格式 `https://github.com/facebook/rocksdb/blob/e6a2ee0b…/<path>#L<行号>`——锚定 commit 不用 main，行号永不漂移
- 笔记体例约定：存于 Claude 持久记忆（`kopidb-notes-conventions`），仓库内不维护模板文件
- **边界（用户拍板）**：笔记只记录 RocksDB 知识本身——不做 KopiDB 对照章节、不单列设计取舍章节；"为什么"以 💡 理解技巧形式融在知识点内
- 体例：遵循用户的 `course-notes-generator` skill 格式——知识点式结构（🧠 总览锚点 → ✅ 知识点N（加粗引导语+引子+正文+示例/实践+注意点引用块 ⚠️💡🔄📋）→ 🔑 总结 → 📌 面试速记版 + 记忆口诀），术语双语 `中文(English)`
- 信息源优先级：本地真实源码（ground truth）→ DeepWiki（找路）→ RocksDB Wiki（设计动机）→ LevelDB（简化对照）
- 分工：Claude 起草 → 用户精读修改（答自测题、核实待验证点、写对照小节）→ Claude review
