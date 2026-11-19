# KopiDB 当前计划

## Python SDK 首版

用户在 macOS arm64 的 CPython 3.13 虚拟环境中安装一个 wheel，即可 `import kopidb`，用文本或原始字节读写、遍历和执行事务。包的最低 macOS 版本由最终二进制确定。助手实施，每完成一阶段汇报接口变化和验收结果，经用户审批后进入下一阶段。用户负责最终 Release 验收与发布。

**进度为 40%（2/5 个阶段通过技术验收）。** 普通与事务接口已按统一读写约定复验，遍历的两种格式已验证；阶段③的维护操作待验收。下面各组接口先在本地扩展验证，最后用发布附件在独立环境验收。

### ① 数据库与批量读写 · 已通过

- [x] 在 [LSM 绑定](../sdk/kopidb_pybind.cpp) 中保留 `LSM(path: str)`。先处理 `db.put/get/remove`，再处理 `put_batch/get_batch/remove_batch`。新增的 `get_batch` 调用 [C++ 的同名批量接口](../include/lsm/engine.h)，保留输入顺序、重复 key 与缺失项。
- [x] 公开接口接受 `str | bytes`，文本按 UTF-8 写入。默认查询返回文本，传 `as_bytes=True` 返回原始字节；批量和遍历用同一模式处理 key 和 value，缺失值保留 None。无效 UTF-8 的默认读取抛出 UnicodeDecodeError。转换实现在 [Python 接口](../sdk/kopidb/kopidb/api.py)。类型错误抛 `TypeError`，写入空 key 或空 value 抛 `ValueError`。每组绑定同步更新[类型提示](../sdk/kopidb/kopidb/core/__init__.pyi)和 [SDK 测试](../sdk/test_sdk.py)。
- **验收**：写入 `(b"k", b"\xff\x00")` 后，`get_batch([b"k", b"missing", b"k"], as_bytes=True)` 必须给出 `[(b"k", b"\xff\x00"), (b"missing", None), (b"k", b"\xff\x00")]`。更新后点查返回新值，删除后返回 `None`。给 `put_batch` 两项，末项分别传整数或空 value，两次应分别抛 `TypeError` 或 `ValueError`，并且第一次出现的合法 key 仍查不到。

2026-11-20 在 macOS arm64 重新构建 `kopidb_pybind`，使用 CPython 3.13.1 在临时目录加载本地扩展。4 项 CRUD/批量用例及 1 项已有事务用法回归全部通过，包含非 UTF-8 二进制读写、零字节 key 不截断、整批失败时保留旧值且无新增 key。wheel 安装验证安排在阶段⑤。

### ② 显式事务 · 技术验收通过，待用户审批

- [x] `tx.get_isolation_level() -> IsolationLevel` 已绑定，四种枚举已用本地构建验证。
- [x] 在 `bind_IsolationLevel` 中提供 `READ_UNCOMMITTED`、`READ_COMMITTED` 和原有两个拼写的兼容别名；保留 `REPEATABLE_READ` 与实验性的 `SERIALIZABLE`。
- [x] `db.begin_tran(level)` 创建事务。`bind_TranContext` 中的 `tx.put/get/remove` 沿用阶段①的类型及异常约定，业务接口为无参的 `commit() -> bool` 和 `abort() -> bool`。C++ 故障测试参数留给引擎测试。
- **验收**：向已有值的 key 写入不同值，事务内能读到自己的修改；分别验证提交后读到新值、回滚后读到旧值。两个读已提交事务先后创建并暂存修改同一 key，较晚创建的先提交成功，较早创建的再提交应返回 `False`，库中保留成功者的值。两组新旧枚举名对应同一级别。查询级别的测试仅核对返回值。

2026-11-20 使用 CPython 3.13.1 重新加载本机扩展，SDK 测试 9/9 通过（5 项事务、4 项普通与批量读写）。覆盖事务内二进制读写、RU 和 RC 回滚、同 key 写冲突整笔拒绝、隔离级别新旧名及无参提交接口。

2026-11-20 公开接口改为默认返回文本，使用 `as_bytes=True` 读取原始字节。重建本地扩展后，覆盖点查、批量、事务、遍历及旧用法回归的 13 项 SDK 测试全部通过。

### ③ 遍历与维护 · 遍历已验证，维护操作待验收

- [x] 普通遍历使用 `for key, value in db.begin()`，默认 `tranc_id=0`；保留显式版本上限和 `db.end()`。底层提供原始字节，公开遍历默认返回文本对，`begin(as_bytes=True)` 返回字节对。
- [ ] 保留 `flush()`、`flush_all()`、`clear()`。校正类型提示，移除当前 Python 没有绑定的 `lsm_iters_monotony_predicate` 声明。
- **验收**：包含二进制 key/value 的遍历逐项核对，`begin()` 与 `begin(0)` 等价；刷盘后可重开读回；结束事务并释放迭代器后执行 `clear()`，库可继续读写。

### ④ 构建 macOS arm64 wheel · 待实施

- [ ] 复核 [A12 的历史复现](AUDIT.md#a12)。用当前名称检查 [Xmake 的 kopidb_pybind 目标](../xmake.lua) 和 [Python 加载器](../sdk/kopidb/kopidb/core/__init__.py)，让扩展能在安装位置找到包内引擎库。
- [ ] 调整[构建脚本](../sdk/build_python_sdk.sh)和[包配置](../sdk/kopidb/setup.py)，由指定解释器构建，包含扩展、引擎库、类型提示及所需许可文件。
- **验收**：一条命令生成 wheel，核对 wheel 标签及元数据与实际 Python ABI、架构、最低系统版本相符。把包移出仓库后能加载同包的库，无需用户设置动态库搜索路径。

### ⑤ 从用户环境验收并准备 Release · 待实施

- [ ] 在仓库外用全新虚拟环境安装阶段④的实际 wheel，运行前面三组接口测试；另起 Python 进程重开，核对已经提交和回滚的数据。
- [ ] 中英文 README 放安装命令与最短 CRUD 示例，详细用法与打包步骤分别写入[使用指南](../docs/usage.md)与[维护文档](../docs/packaging.md)。
- **验收**：在仓库外从 wheel 文件用 `python -m pip install` 普通安装，清除 `PYTHONPATH` 和 `DYLD_LIBRARY_PATH`；确认 Python 从虚拟环境加载安装包及其原生库。在这个环境运行前三组用例、跨进程重开和 README 示例。用于交付的 wheel 与受测文件的 SHA-256 一致，附中英文 Release 说明，由用户最终验收并在 GitHub 发布。
