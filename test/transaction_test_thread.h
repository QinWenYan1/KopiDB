#pragma once

#include "lsm/transaction.h"
#include <future>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace kopidb::test {

// 测试线程依次调用 run()：每一步都在同一个工作线程执行，返回结果或抛出
// 异常后，测试才继续。这样可以精确控制两笔事务的交错顺序，同时保留线程归属。
class TransactionTestThread {
public:
  TransactionTestThread() : thread_([this] {
    for (;;) {
      ready_.acquire();
      auto step = std::move(next_);
      if (!step.valid()) return;
      step();
    }
  }) {}

  ~TransactionTestThread() {
    // run() 是同步的，此时没有待执行的步骤。唤醒线程，让它退出。
    ready_.release();
    thread_.join();
  }

  template <typename F> auto run(F action) {
    std::packaged_task<std::invoke_result_t<F &>()> step(std::move(action));
    auto result = step.get_future();
    // 工作线程统一执行无参任务；测试线程通过原任务的 future 取得返回值或异常。
    next_ = std::packaged_task<void()>(std::move(step));
    ready_.release();
    return result.get();
  }

private:
  std::binary_semaphore ready_{0};
  std::packaged_task<void()> next_;
  std::thread thread_;
};

// 需要在多个时刻观察同一笔事务时，创建和后续调用都交给同一个工作线程。
class ThreadOwnedTransaction {
public:
  template <typename Factory> explicit ThreadOwnedTransaction(Factory open)
      : tx_(owner_.run(std::move(open))) {
    if (!tx_) throw std::logic_error("transaction factory returned a null handle");
  }

  template <typename F> auto run(F action) {
    return owner_.run([&] { return action(*tx_); });
  }

private:
  TransactionTestThread owner_;
  std::shared_ptr<TranContext> tx_;
};

} // namespace kopidb::test
