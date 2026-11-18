#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

// 绑定 TwoMergeIterator 迭代器
void bind_TwoMergeIterator(py::module &m) {
  py::class_<kopidb::TwoMergeIterator>(m, "TwoMergeIterator")
      .def("__iter__", [](kopidb::TwoMergeIterator &it) { return &it; })
      .def("__next__", [](kopidb::TwoMergeIterator &it) {
        if (!it.is_valid())
          throw py::stop_iteration();
        auto kv = *it;
        ++it;
        return py::make_tuple(kv.first, kv.second);
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
        return py::make_tuple(kv.first, kv.second);
      });
}

// 绑定 TranContext 事务上下文

// 提前声明 TranContext（如果头文件未包含）
using TranContext = kopidb::TranContext;

// 绑定 TranContext
void bind_TranContext(py::module &m) {
  py::class_<TranContext, std::shared_ptr<TranContext>>(m, "TranContext")
      .def("commit", &kopidb::TranContext::commit,
           py::arg("test_fail") = false) // 处理默认参数
      .def("abort", &kopidb::TranContext::abort)
      .def("get", &kopidb::TranContext::get)
      .def("remove", &kopidb::TranContext::remove)
      .def("put", &kopidb::TranContext::put);
}

void bind_IsolationLevel(py::module &m) {
  py::enum_<kopidb::IsolationLevel>(m, "IsolationLevel")
      .value("READ_UNOP_COMMITTED", kopidb::IsolationLevel::READ_UNOP_COMMITTED)
      .value("READ_OP_COMMITTED", kopidb::IsolationLevel::READ_OP_COMMITTED)
      .value("REPEATABLE_READ", kopidb::IsolationLevel::REPEATABLE_READ)
      .value("SERIALIZABLE", kopidb::IsolationLevel::SERIALIZABLE)
      .export_values();
}

PYBIND11_MODULE(kopidb_pybind, m) {
  // 绑定辅助类
  bind_TwoMergeIterator(m);
  bind_Level_Iterator(m);
  bind_TranContext(m);
  bind_IsolationLevel(m);

  // 主类 LSM
  py::class_<kopidb::LSM>(m, "LSM")
      .def(py::init<const std::string &>())
      // 基础操作
      .def("put", &kopidb::LSM::put, py::arg("key"), py::arg("value"),
           "Insert a key-value pair (bytes type)")
      .def("get", &kopidb::LSM::get, py::arg("key"),
           "Get value by key, returns None if not found")
      .def("remove", &kopidb::LSM::remove, py::arg("key"), "Delete a key")
      // 批量操作
      .def("put_batch", &kopidb::LSM::put_batch, py::arg("kvs"),
           "Batch insert key-value pairs")
      .def("remove_batch", &kopidb::LSM::remove_batch, py::arg("keys"),
           "Batch delete keys")
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
