#include <axon/executor.hpp>
#include <axon/signal.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <latch>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

class ManualExecutor final : public axon::Executor {
public:
  bool post(axon::Task task) override {
    tasks_.push_back(std::move(task));
    return true;
  }
  void drain() {
    while (!tasks_.empty()) {
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      task();
    }
  }

private:
  std::deque<axon::Task> tasks_;
};

} // namespace

int main() {
  {
    axon::Signal<int> signal;
    int total = 0;
    auto first = signal.connect([&](int value) { total += value; });
    auto once = signal.connect_once([&](int value) { total += value * 10; });
    signal.emit(2);
    signal.emit(3);
    assert(total == 25);
    assert(signal.size() == 1);
    first.disconnect();
    assert(signal.size() == 0);
  }

  {
    ManualExecutor executor;
    axon::Signal<int> signal;
    int original = 0;
    int late = 0;
    auto first = signal.connect([&](int value) { original += value; });
    assert(signal.emit_queued(executor, 4));
    auto second = signal.connect([&](int value) { late += value; });
    executor.drain();
    assert(original == 4);
    assert(late == 0); // queued emission uses the connection snapshot at emit.

    std::stop_source cancelled;
    assert(signal.emit_queued(executor, cancelled.get_token(), 8));
    cancelled.request_stop();
    executor.drain();
    assert(original == 4 && late == 0);
  }

  {
    axon::ThreadPool pool(4);
    axon::Strand strand(pool);
    std::vector<int> order;
    std::latch done(100);
    for (int index = 0; index < 100; ++index) {
      assert(strand.post([&, index] {
        order.push_back(index);
        done.count_down();
      }));
    }
    done.wait();
    assert(order.size() == 100);
    for (int index = 0; index < 100; ++index) assert(order[index] == index);
    strand.shutdown();
    pool.shutdown();
  }

  {
    axon::Signal<> signal;
    int called = 0;
    axon::Connection connection;
    {
      axon::Lifetime owner;
      connection = signal.connect(owner, [&] { ++called; });
      signal.emit();
      assert(called == 1);
    }
    signal.emit();
    assert(called == 1);
  }

  {
    axon::Signal<int> signal;
    axon::ThreadPool pool(2);
    std::atomic<int> value{0};
    std::latch done(1);
    auto connection = signal.connect([&](int input) {
      value.store(input);
      done.count_down();
    });
    assert(signal.emit_queued(pool, 42));
    done.wait();
    assert(value.load() == 42);
    pool.shutdown();
  }

  {
    axon::Signal<> signal;
    int errors = 0;
    signal.set_error_sink([&](std::exception_ptr) { ++errors; });
    auto bad = signal.connect([] { throw 1; });
    auto good = signal.connect([&] { ++errors; });
    signal.emit();
    assert(errors == 2);
  }

  std::cout << "axon_tests: ok\n";
  return 0;
}
