#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

// std::string 可能包含零字节或非 UTF-8 数据。显式返回 bytes，
// 防止 pybind11 把已查到的值当作文本解码；缺失的 key 对应 None。
static py::object bytes_or_none(const std::optional<std::string> &value) {
  if (value)
    return py::bytes(*value);
  return py::none();
}

// 绑定 TwoMergeIterator 迭代器
void bind_TwoMergeIterator(py::module &m) {
  py::class_<kopidb::TwoMergeIterator>(m, "TwoMergeIterator")
      .def("__iter__", [](kopidb::TwoMergeIterator &it) { return &it; })
      .def("__next__", [](kopidb::TwoMergeIterator &it) {
        if (!it.is_valid())
          throw py::stop_iteration();
        auto kv = *it;
        ++it;
        return py::make_tuple(py::bytes(kv.first), py::bytes(kv.second));
      });
}

void bind_Level_Iterator(py::module &m) {
  py::class_<kopidb::Level_Iterator>(m, "Level_Iterator")
      .def("__iter__", [](kopidb::Level_Iterator &it) { return &it; })
      .def("__next__", [](kopidb::Level_Iterator &it) {
        if (!it.is_valid())
          throw py::stop_iteration();
        auto kv = *it;
        ++it;
        return py::make_tuple(py::bytes(kv.first), py::bytes(kv.second));
      });
}

// 绑定 TranContext 事务上下文

// 提前声明 TranContext（如果头文件未包含）
using TranContext = kopidb::TranContext;

// 绑定 TranContext
void bind_TranContext(py::module &m) {
  py::class_<TranContext, std::shared_ptr<TranContext>>(m, "TranContext")
      .def("get_isolation_level", &TranContext::get_isolation_level,
           "Return the isolation level selected when this transaction began.")
      // Python 的 commit() 走正常提交路径；底层故障注入参数只供恢复测试使用。
      .def("commit", [](TranContext &tx) { return tx.commit(); })
      .def("abort", &TranContext::abort)
      .def("get", [](TranContext &tx, const py::bytes &key) {
        return bytes_or_none(tx.get(key));
      }, py::arg("key"))
      .def("remove", [](TranContext &tx, const py::bytes &key) {
        tx.remove(key);
      }, py::arg("key"))
      .def("put", [](TranContext &tx, const py::bytes &key,
                     const py::bytes &value) { tx.put(key, value); },
           py::arg("key"), py::arg("value"));
}

void bind_IsolationLevel(py::module &m) {
  py::enum_<kopidb::IsolationLevel>(m, "IsolationLevel")
      .value("READ_UNCOMMITTED", kopidb::IsolationLevel::READ_UNOP_COMMITTED)
      .value("READ_COMMITTED", kopidb::IsolationLevel::READ_OP_COMMITTED)
      .value("REPEATABLE_READ", kopidb::IsolationLevel::REPEATABLE_READ)
      .value("SERIALIZABLE", kopidb::IsolationLevel::SERIALIZABLE,
             "Experimental: full serializable isolation is not implemented.")
      // 旧 Python 示例使用 C++ 的拼写；两个入口应选择同一个隔离级别。
      .value("READ_UNOP_COMMITTED", kopidb::IsolationLevel::READ_UNOP_COMMITTED)
      .value("READ_OP_COMMITTED", kopidb::IsolationLevel::READ_OP_COMMITTED)
      .export_values();
}

PYBIND11_MODULE(kopidb_pybind, m) {
  // 绑定辅助类
  bind_TwoMergeIterator(m);
  bind_Level_Iterator(m);
  bind_IsolationLevel(m);
  bind_TranContext(m);

  // 主类 LSM
  py::class_<kopidb::LSM>(m, "LSM")
      .def(py::init<const py::str &>(), py::arg("path"))
      // 路径是文本；KV 是字节串。py::bytes 参数在进入函数前检查类型，
      // 转成引擎使用的 std::string 时会保留完整字节长度。
      .def("put", [](kopidb::LSM &db, const py::bytes &key,
                     const py::bytes &value) { db.put(key, value); },
           py::arg("key"), py::arg("value"))
      .def("get", [](kopidb::LSM &db, const py::bytes &key) {
        return bytes_or_none(db.get(key));
      }, py::arg("key"))
      .def("remove", [](kopidb::LSM &db, const py::bytes &key) {
        db.remove(key);
      }, py::arg("key"))
      // 整批转换成功后只调用一次引擎的批量接口，保留原有的整批校验和写入语义。
      .def("put_batch", [](kopidb::LSM &db,
                           const std::vector<std::pair<py::bytes, py::bytes>> &kvs) {
        std::vector<std::pair<std::string, std::string>> entries(kvs.begin(), kvs.end());
        db.put_batch(entries);
      }, py::arg("kvs"))
      .def("get_batch", [](kopidb::LSM &db, const std::vector<py::bytes> &keys) {
        std::vector<std::string> batch(keys.begin(), keys.end());
        py::list results;
        for (const auto &[key, value] : db.get_batch(batch))
          results.append(py::make_tuple(py::bytes(key), bytes_or_none(value)));
        return results;
      }, py::arg("keys"))
      .def("remove_batch", [](kopidb::LSM &db, const std::vector<py::bytes> &keys) {
        std::vector<std::string> batch(keys.begin(), keys.end());
        db.remove_batch(batch);
      }, py::arg("keys"))
      // 迭代器
      .def("begin", &kopidb::LSM::begin, py::arg("tranc_id"),
           "Start an iterator with transaction ID")
      .def("end", &kopidb::LSM::end, "Get end iterator")
      // 事务
      .def("begin_tran", &kopidb::LSM::begin_tran, py::arg("isolation_level"),
           "Start a transaction")
      // 其他方法
      .def("clear", &kopidb::LSM::clear, "Clear all data") // ! Fix bugs
      .def("flush", &kopidb::LSM::flush, "Flush memory table to disk")
      .def("flush_all", &kopidb::LSM::flush_all, "Flush all pending data");
}
