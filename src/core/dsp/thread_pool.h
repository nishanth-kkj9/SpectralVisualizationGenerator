#pragma once

// Lightweight header-only thread pool.
// ponytail: std::queue + mutex + condition_variable. Sufficient for
// embarrassingly parallel row-chunk work. Replace with taskflow or
// similar if task graph / priority scheduling is needed.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Spectral {

class ThreadPool {
public:
    explicit ThreadPool(int n = 0) {
        if (n <= 0) n = static_cast<int>(std::thread::hardware_concurrency());
        if (n < 1) n = 1;
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    // Non-copyable, non-movable.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submit a void() task. Blocks if pool is shut down.
    void submit(std::function<void()> fn) {
        {
            std::lock_guard lk(mtx_);
            tasks_.push(std::move(fn));
        }
        cv_.notify_one();
    }

    int size() const { return static_cast<int>(workers_.size()); }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace Spectral
